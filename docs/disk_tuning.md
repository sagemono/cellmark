# Disk I/O tuning on PS3: 5.7 MB/s -> 47 MB/s

This is the writeup of how the disk benchmark went from "this SSHD is broken" to "the drive is fine, we were measuring through too many layers and blaming the wrong one." Most of the journey was diagnostic work, not optimisation. The single biggest gain came from changing ONE number, but the *reason* it worked has more layers than the original hypothesis admitted.

If you only read one section, read [§4 what the probes told us](#4-what-the-probes-told-us). If you want what we got wrong on the way, read [§13 what we don't know](#13-what-we-dont-know).

---

## 1. The starting point

Test platform:

| | |
|---|---|
| Console     | PS3 (CECHA00, original 60 GB fat) |
| Drive       | Seagate SSHD (2.5", 5400 RPM platter + 8 GB NAND cache) |
| Filesystem  | `/dev_hdd0` is **UFS2**, 512-byte sectors |
| Encryption  | Full disk XTS-AES-128 (slim) / AES-CBC-192 (fat) at sector level via the South Bridge ENCDEC device |
| Bus         | SATA-150 (~150 MB/s ceiling) |
| I/O path    | `cellFsRead` -> LV2 -> ENCDEC (South Bridge) -> SATA -> drive |

Filesystem and encryption details from psdevwiki Harddrive and HDD Encryption pages (links in [§14](#14-sources)). Both pages are worth reading before assuming anything about how reads reach the drive on PS3. The **encryption is below the filesystem**: every sector that crosses the SATA bus is encrypted, regardless of API path.

The South Bridge itself is documented separately by Toshiba as the **Super Companion Chip (SCC)** (see [§9](#9-the-os--firmware-ceiling-and-why-we-stopped-here) and [§14](#14-sources) for what that tells us about how I/O bandwidth is allocated inside the chip.)

Initial cellmark results, first cut of the benchmark:

```
Seq Read  64KB    5.7 MB/s
Seq Write 64KB    9.2 MB/s
Rnd 4K Read       0.4 MB/s
Rnd 4K Write      0.9 MB/s
```

Five point seven megabytes per second on sequential read. The same drive on a PC (per the psdevwiki HDD speed tests table) does ~80 MB/s average, ~100 MB/s peak. So we were getting roughly 1/14 of the drive's measured PC throughput, in a system whose internal bus tops out at 150 MB/s. The number was off by an order of magnitude no matter how generously you account for overhead.

What the benchmark looked like at this point:

```c
#define IO_CHUNK   (1 * 1024 * 1024)   // 1 MB per read
#define FILE_SIZE  (128 * 1024 * 1024) // 128 MB test file

cellFsSetIoBuffer(fd, IO_CHUNK, ...);
for (i = 0; i < FILE_SIZE / IO_CHUNK; i++)
    cellFsRead(fd, buf, IO_CHUNK, &nread);
```

Conventional wisdom says big chunks are good. Big chunks should *amortise* per-syscall overhead. We had big chunks. We were getting nothing.

---

## 2. The hypothesis space

Before instrumenting anything, the candidates were:

1. **The file is fragmented across UFS2 blocks.** A 128 MB test file laid out non-contiguously across the partition would force seeks between extents.
2. **Per-syscall LV2 overhead is high.** Each `cellFsRead` involves a hypervisor crossing plus the ENCDEC decrypt path. If there's a fixed cost per call, smaller throughput at any chunk size fits.
3. **The OS is staging through a kernel-side scratch buffer** before the data lands in our user buffer (extra memcpy hidden from us).
4. **`cellFsSetIoBuffer` is misconfigured** and we're getting an unaligned slow path through it.
5. **The SSHD's NAND cache isn't helping a sequential workload.** Per Seagate's product documentation, Adaptive Memory does not promote long sequential data streams to the NAND cache, on the basis that such data does not benefit from flash acceleration. Independent testing (TweakTown, Tech Report - see §14) confirms that sequential workloads bypass the NAND and are served directly from the platters. So if our access pattern looks sequential, we're on platter speed by design, not by accident.
6. **The drive is at an inner-track location** where linear velocity is lower and platter throughput is reduced.

Which hypothesis was actually true couldn't be answered by reading the SDK. We needed measurements.

---

## 3. Building the probe suite

The cellmark "Probe view" was built specifically to disambiguate the hypothesis space above. Each probe is a controlled variant of the same basic operation, and the *differences* between probes tell you which hypothesis fits.

The 13 probes, in their final form:

| ID  | Label             | What it varies                                | What it answers                            |
|-----|-------------------|-----------------------------------------------|--------------------------------------------|
| P0  | `R 16KB plain`    | 16 KB chunks, no IO buffer, no streaming      | Per-call cost at small reads               |
| P1  | `R 64KB plain`    | 64 KB chunks                                  | Mid-range chunk size                       |
| P2  | `R 256KB plain`   | 256 KB chunks                                 | Above the small-chunk regime               |
| P3  | `R 1MB plain`     | 1 MB chunks                                   | Large chunks add fragmentation pressure    |
| P4  | `R 1MB io64K`     | + `cellFsSetIoBuffer` 64 KB pages             | Does an IO buffer help?                    |
| P5  | `R 1MB io1MB`     | + `cellFsSetIoBuffer` 1 MB pages              | Does a *bigger* IO buffer help?            |
| P6  | `R stread 1M/4M`  | `cellFsStRead` streaming API, 4 MB ring       | Does PS3's streaming API win?              |
| P7  | `W 1MB +fsync`    | 1 MB writes, fsync after every chunk          | True media write speed                     |
| P8  | `W 1MB nofsync`   | 1 MB writes, fsync only at end                | OS write-cache speed                       |
| P9  | `W 64KB +fsync`   | 64 KB writes, fsync after every chunk         | Small-write penalty                        |
| P10 | `R+aud 256K`      | 256 KB seq + audio sim (4 KB/call concurrent) | Cost of high-frequency contention          |
| P11 | `R+tex 256K`      | 256 KB seq + texture sim (64 KB random)       | Cost of seek contention                    |
| P12 | `R+both 256K`     | 256 KB seq + audio + texture                  | Realistic game I/O mix                     |

**Key design choices:**

- **All probes use a 32 MB file**, written contiguously by `cellFsAllocateFileAreaWithoutZeroFill`. This holds fragmentation roughly constant across probes (or as close to constant as you can get on a partition you don't control).
- **All probes use the same DMA-capable user buffer**, allocated via `sys_memory_allocate` with `SYS_MEMORY_PAGE_SIZE_1M`. This puts the buffer in the 1 MB-page pool, which is physically pinned and DMA-addressable.
- **Each probe runs an untimed warmup pass first**, then the timed pass. This eliminates "first read is slow because some cache somewhere is empty" as a cross-run variable.

The contention probes (P10–P12) spawn a second PPE thread that hammers the drive in a specific pattern while the main thread runs a 256 KB sequential read. They simulate what a game level load looks like: the level streams sequentially, the audio engine pulls 4 KB voice chunks every ms, the texture system fetches random 64 KB tiles.

---

## 4. What the probes told us

Final probe results on the test SSHD (read row, sorted by what they tell you):

```
P0   R 16KB plain     6.1 MB/s
P1   R 64KB plain    46.0 MB/s        <-- the cliff
P2   R 256KB plain   46.0 MB/s
P3   R 1MB plain     46.0 MB/s
P4   R 1MB io64K     45.5 MB/s        <-- IO buffer slightly slower
P5   R 1MB io1MB     45.6 MB/s
P6   R stread 1M/4M  46.0 MB/s        <-- streaming API: same
P7   W 1MB +fsync    10.7 MB/s        <-- post-fsync = real media speed
P8   W 1MB nofsync   11.0 MB/s        <-- write cache barely helps
P9   W 64KB +fsync    9.8 MB/s
```

**The picture this paints:**

1. **There's a sharp cliff between 16 KB and 64 KB reads.** 6 MB/s vs 46 MB/s, on the same file, same buffer, same everything. *We measured the cliff. We did not isolate the cause.* It could be UFS2 block size on `/dev_hdd0`, ENCDEC dispatch granularity, an LV2 syscall minimum-cost effect that dominates below some threshold, or an OS-side read coalescer. Picking between those would need a different experiment than what we ran.
2. **Above 64 KB, throughput is flat.** 64 KB, 256 KB, 1 MB are all 46 MB/s. So whatever the cliff is, you cross it at 64 KB and there's no further ceiling-extension to be had from larger chunks. 46 MB/s is the steady-state value of *something* downstream of cellFs.
3. **`cellFsSetIoBuffer` is mildly harmful.** P3 (no IO buffer) is 46.0; P4/P5 (with IO buffer) are 45.5/45.6. Tiny but consistent across runs, we re-ran each probe several times before publishing.
4. **`cellFsStRead` doesn't help on a contiguous file.** Streaming is designed to overlap I/O with compute via read-ahead. On a contiguous 32 MB file with our buffer pre-pinned for DMA, there's nothing to overlap *with*. The disk pipeline is already full and the read loop has no compute between calls.
5. **Writes are bottlenecked by media latency, not by the API.** Post-fsync (P7) and no-fsync-until-end (P8) differ by 0.3 MB/s. The write cache exists, but on a 32 MB run with the cache fed at ~11 MB/s, the cache fills before the run ends.

So the original 5.7 MB/s wasn't the fault of `cellFsRead`, the OS, or the SDK in isolation. The original benchmark used 1 MB chunks on a 128 MB file, and *something about that combination*, most likely fragmentation on a non-contiguous 128 MB layout interacting with whatever causes the small-chunk cliff produced the bad number. Using a 32 MB file (which fits contiguously) and 64 KB chunks (above the cliff) made the bad number go away.

The fix was: **smaller file, and 64 KB reads.** What the fix *meant* is partly still open.

---

## 5. Why `cellFsSetIoBuffer` was harmful

`cellFsSetIoBuffer` allocates a kernel-side staging buffer that `cellFsRead` will copy through. The flow becomes:

```
disk -> ENCDEC decrypt -> DMA -> kernel staging buffer -> memcpy -> user buffer
```

Without it, if your user buffer is in physically contiguous DMA-capable memory:

```
disk -> ENCDEC decrypt -> DMA -> user buffer
```

That extra memcpy isn't free. On the PS3, it's roughly 0.5–1.0 MB/s of throughput at our scale. Tiny in absolute terms but consistent and the wrong direction from what you'd intuitively expect from a cache.

The trick is making sure the user buffer is DMA-capable. `malloc` doesn't guarantee that. The fix:

```c
/* page-aligned, physically contiguous, DMA-capable user buffer */
sys_addr_t paddr;
sys_memory_allocate(BUF_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &paddr);
io_buf = (void *)(uintptr_t)paddr;
```

`SYS_MEMORY_PAGE_SIZE_1M` allocates from the 1 MB-page pool, which is physically pinned. `cellFsRead` to this address takes the direct-DMA path; with a `malloc` buffer it takes the staged path.

(FIOS, Sony's production I/O middleware, has an `isNonDMAableMemory()` check in `cell/fios/fios_platform.h` that fires when callers pass non-DMA buffers. Sony's own production middleware cares about exactly this distinction.)

---

## 6. The cold-start problem

After all of the above, runs would still occasionally start at ~11 MB/s and climb to ~46 MB/s after a few seconds. Repeat-running the same benchmark gave 46 MB/s consistently.

Most likely a cache somewhere along the path is empty on a cold start, drive read-ahead, OS-side, or both. This warms up after the first few reads. We didn't isolate which. What's testable is the *fix*: an untimed warmup pass.

```c
/* warmup: read once, throw away the result */
for (i = 0; i < FILE_SIZE / SEQ_IO_SIZE; i++)
    cellFsRead(fd, io_buf, SEQ_IO_SIZE, &nread);

cellFsLseek(fd, 0, CELL_FS_SEEK_SET, &pos);

/* now the timed loop */
SYS_TIMEBASE_GET(t0);
for (i = 0; i < FILE_SIZE / SEQ_IO_SIZE; i++)
    cellFsRead(fd, io_buf, SEQ_IO_SIZE, &nread);
SYS_TIMEBASE_GET(t1);
```

This made the cold/warm divergence disappear. Worth noting: in real workloads (game level loads), you don't get a warmup pass, but real workloads also aren't trying to characterise *steady-state* throughput. For a benchmark, warmup is the correct call.

---

## 7. Random 4K: the syscall round-trip tax

Random 4K read benched at 0.4 MB/s. That's 100 ops/sec. Every op taking ~10 ms. Since the drive's seek time alone is 10–15 ms on a 5400 RPM platter, this is essentially the seek latency floor.

But there was free performance hiding in the API. The original code:

```c
cellFsLseek(fd, offset, CELL_FS_SEEK_SET, &pos);     // syscall 1
cellFsRead(fd, buf, RND_BLOCK_SIZE, &nread);          // syscall 2
```

Two syscalls per random op. 512 ops x 2 syscalls = 1,024 LV2 round-trips. The fix:

```c
cellFsReadWithOffset(fd, offset, buf, RND_BLOCK_SIZE, &nread);  // 1 syscall
```

`cellFsReadWithOffset` is the positional-I/O variant. Seek and read in one syscall. 512 round-trips saved. The benchmark went from 0.4 to 0.6 MB/s. Not earth-shattering in absolute terms, but a 50% relative improvement on something otherwise dominated by seek physics.

`cellFsWriteWithOffset` exists too and gave the same uplift on random 4K writes.

---

## 8. The contention probes and what real game I/O looks like

The headline sequential number is 46 MB/s. The interesting question is: does that hold up when *other things* are reading the disk simultaneously?

P10 spawns an audio simulator: a thread that reads 4 KB chunks in a tight loop, modelling a streaming audio engine pulling voice samples. P11 spawns a texture simulator: 64 KB random reads, modelling a texture streamer fetching tiles. P12 runs both.

Results, while a 256 KB sequential reader runs on the main thread:

```
Baseline (P2)         46.0 MB/s
P10  + audio thread    5.0 MB/s    <-- 89% drop
P11  + texture thread 38.0 MB/s    <-- 17% drop
P12  + both           30.0 MB/s
```

The audio thread is *catastrophic* for the sequential reader, even though both threads are reading sequentially from the same file. The texture thread, despite seeking randomly across the whole file, only knocks 17% off.

This is the most interesting result in the whole investigation. It says, empirically:

> **I/O call frequency dominates I/O bandwidth contention here.**

The audio thread issues `cellFsRead` calls at roughly 64x the rate of the texture thread (4 KB ops vs 64 KB ops, both reading sequentially). Until we read the southbridge documentation (next section), we framed the explanation as "LV2 serialises requests and the queue floods." With the southbridge architecture in hand, the picture is sharper than that, and not just a software queue effect.

---

## 9. The OS / firmware ceiling, and why we stopped here

46 MB/s isn't the platter ceiling. The platter does more. Per psdevwiki's HDD speed tests table, the same family of 5400 RPM 2.5" Seagate / Hitachi / Toshiba drives that PS3s ship with average 60–80 MB/s sequential on a PC, with peaks in the 90–100+ MB/s range. The bus inside the PS3 is SATA-150 (~150 MB/s). So 46 MB/s is well below the drive's measured platter rate and *very* far below the bus.

What 46 MB/s *is* is the steady-state ceiling for sequential reads through the full PS3 stack: `cellFsRead` -> LV2 -> Storage Manager -> ENCDEC decrypt -> SATA -> drive. We confirmed it's the ceiling for this stack three ways:

1. **The chunk-size sweep flatlines above 64 KB.** 64 KB, 256 KB, 1 MB all hit 46 MB/s. There is no chunk size that gets us higher.
2. **`cellFsStRead` (proper streaming) gives the same number.** If the stack had headroom to extract via read-ahead, the streaming API would expose it. It doesn't.
3. **The SSHD's NAND cache won't help this workload anyway.** Per Seagate's product documentation, Adaptive Memory doesn't promote long sequential streams to NAND, sequential reads are served from the platter, not the cache. So expecting the SSHD's flash to lift this number was always a dead end.

### The southbridge architecture explains the contention result

The South Bridge in the PS3 is derived from Toshiba's published **Super Companion Chip (SCC)** design (Hot Chips 17, 2005; Toshiba Review Vol. 61 No. 6, 2006). Comparison of ![retail PS3 southbridge die](https://www.psdevwiki.com/ps3/File:CXD2973GB_Die_Shot_(Stitched).jpg) against Toshiba's published SCC die shot shows congruent floorplan and block placement with at most metal-layer customisation by Sony. Caveat: visual congruence isn't formal proof; what follows describes the published SCC architecture, which the PS3 southbridge is "consistent with" rather than "verified identical to."

The published SCC architecture has a **hierarchical internal bus with explicit QoS**:

| Bus  | Role          | Bandwidth      | Devices on it                              |
|------|---------------|----------------|--------------------------------------------|
| TBUS | Top-level / FlexIO bridge | 2.66 GB/s | bridges to all other buses + PCI/PCIe |
| MBUS | Streaming     | 2.66 GB/s | DDR2, video out, dedicated SDMAC |
| SBUS | **Real-time** | 2.66 GB/s | TS in, IEEE1394, video in, audio in/out |
| HBUS | **Best-effort** | 2.66 GB/s | **ATA, USB, GbE, ENCDEC** |

Three things in this layout matter for our results:

1. **ATA traffic is on HBUS, the best-effort bus, sharing 2.66 GB/s with USB, GbE, and the ENCDEC block itself.** Storage is *deliberately* deprioritised relative to TS-in / audio / video / DMA traffic. This is a hardware design choice, not a bug.
2. **The ENCDEC block sits on HBUS too.** Every encrypted sector heading to or from the drive crosses the same bus that's serving USB and Ethernet, gated by the same arbiter.
3. **The SCC paper specifies that ordering must be enforced for transactions issued by a given I/O controller** i.e. a single source can't pipeline its own commands; multiple commands can only be in flight if they come from *different* I/O controllers, riding different FlexIO channels.

Lined up against the contention probe data, those three facts give a much more grounded story than "LV2 queue flooding." A second PPE thread issuing 4 KB reads doesn't just compete for software-side queue slots, it competes for HBUS arbitration cycles between the ATA controller and the ENCDEC block, with each request strictly ordered relative to its peers from the same source. The audio thread's high call frequency keeps the ATA controller's HBUS slot busy with low-payload transactions; the main thread's larger reads, posted by the same logical I/O source, can't pipeline past them.

Closing the gap between 46 MB/s and the drive's PC-measured ~80 MB/s would require attacking layers below `cellFs`, the encryption path, LV2 storage manager, or the SATA driver. But even then the southbridge HBUS arbiter and per-source ordering are hardware constraints we can't reach from userland. We stopped here because more was no longer extractable from the layer cellmark lives in.

---

## 10. What we didn't try

For completeness, things investigated and rejected:

- **`cellFsAioRead` (async I/O with callback).** Wouldn't help sequential reads. The disk pipeline is already full at 46 MB/s; `aioread` overlaps user computation with I/O, but cellmark's "computation" between reads is one `cellFsRead` per loop iteration with ~200 ns of accounting. No headroom to overlap with.
- **Bypassing `cellFs` entirely?** Going below `cellFs` means talking to the storage stack directly. Undocumented, model-specific, and any throughput it might unlock comes at the cost of being unportable across every PS3 model and CFW. Not worth it for a benchmark. Just an exotic thought.
- **Defragmenting `/dev_hdd0`.** Would help if file fragmentation is part of the cliff story, but isn't accessible from a homebrew app. The PS3 has no exposed defrag tool.

---

## 11. Final geometry

The numbers that ended up in the cellmark code:

```c
#define SEQ_IO_SIZE     (64 * 1024)         /* 64 KB per cellFsRead above the cliff */
#define SEQ_FILE_SIZE   (32 * 1024 * 1024)  /* 32 MB test file fits contiguously */

#define DISK_PROBE_FILE_SZ (32 * 1024 * 1024)
#define DISK_PROBE_COUNT   13

/* DMA-capable user buffer */
sys_memory_allocate(BUF_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &paddr);

/* 16 MB container for cellFsStRead's ring buffer (P6 needs 4 MB,
 * P4/P5 each leak ~4 MB on close ... container exhaustion bug) */
sys_memory_container_create(&g_io_container, 16 * 1024 * 1024);
```

And the API choices:

| Operation       | API used                  | Why                                            |
|-----------------|---------------------------|------------------------------------------------|
| Sequential read | `cellFsRead` (no IO buf)  | Direct DMA path, faster than IO buffer staging |
| Sequential write| `cellFsWrite` + `fsync`   | Reports true media speed, not write cache |
| Random read     | `cellFsReadWithOffset`    | Saves 1 syscall per op vs lseek+read |
| Random write    | `cellFsWriteWithOffset`   | Same |
| Streaming       | `cellFsStRead` (probe only)| Doesn't help, but tested for completeness |

---

## 12. Headline numbers

| Metric          | Before  | After    | Notes                                     |
|-----------------|---------|----------|-------------------------------------------|
| Seq Read 64KB   | 5.7     | 46.9     | 32 MB file, contiguous, 64 KB chunks |
| Seq Write 64KB  | 9.2     | 10.9     | post-fsync, true media speed |
| Rnd 4K Read     | 0.4     | 0.6      | `cellFsReadWithOffset` saves 1 syscall/op |
| Rnd 4K Write    | 0.9     | 1.2      | `cellFsWriteWithOffset` same |

The seq write number didn't move much because it was already correct. 10 MB/s post-fsync is approximately what the drive sustains for fsync-flushed writes. The original "9.2 MB/s" number was right and the original "5.7 MB/s read" was the lie.

---

## 13. What we don't know

Things this investigation surfaced but did **not** answer. Anyone with more time, a DECR, or a willingness to instrument LV2 could pick these up:

- **What is the UFS2 block size on `/dev_hdd0`?** UFS2 block sizes are configurable (typically 4 KB to 64 KB). If it's 64 KB, the cliff at 16->64 KB is partly explained. If it's smaller, the cliff is downstream.
- **What is the cause of the 16 KB -> 64 KB cliff specifically?** The candidates listed in [§4](#4-what-the-probes-told-us) - UFS2 block size, ENCDEC granularity, LV2 syscall fixed cost and OS-side coalescer are all plausible. We measured the cliff but didn't instrument any of those layers.
- **Does the retail PS3 southbridge silicon match the published SCC bus fabric exactly?** Visual die comparison is consistent with same silicon family, but only metal-layer reverse engineering would prove it. Sony may have customised the bus arbiter, ENCDEC block, or HBUS clients vs. the 2005 reference design.
- **Why is the audio-thread contention so much worse than the texture-thread contention?** The HBUS arbitration / per-source ordering interpretation in [§9](#9-the-os--firmware-ceiling-and-why-we-stopped-here) fits the data and is grounded in the SCC paper, but isn't directly verified on retail silicon. Could also be ENCDEC contention, scheduler unfairness, or some combination.
- **Where on the platter does Sony's `/dev_hdd0` partition sit?** This affects linear velocity and therefore platter throughput. We assumed mid-platter. Have not verified.
- **Would these numbers differ on PHAT vs slim consoles?** PHAT uses AES-CBC-192 encryption; slim uses XTS-AES-128. The encryption costs are different per sector. Worth re-running on a slim and comparing.
- **What does the cliff look like on a non-SSHD?** All of our results are on a Seagate SSHD. A pure HDD would presumably show a similar cliff (the SSHD's NAND isn't engaged for this workload anyway), but it's untested.

If anyone runs cellmark and gets meaningfully different numbers than mine, please open an issue! That's the kind of data this writeup exists to attract.

---

## 14. Sources

### PS3 system / filesystem / encryption

- [psdevwiki -  Harddrive](https://www.psdevwiki.com/ps3/Harddrive) - UFS2 GameOS partition, sector size, SATA-150 bus, drive models per PS3 SKU, PC-side speed tests for those drives.
- [psdevwiki - HDD Encryption](https://www.psdevwiki.com/ps3/HDD_Encryption) - XTS-AES-128 / AES-CBC-192 sector-level encryption via ENCDEC in the South Bridge.
- [PS3HDDTool](https://github.com/Pheeeeenom/PS3HDDTool) - 

### Cell SDK

- IBM/Toshiba/Sony Cell Broadband Engine SDK headers `cell/fios/fios_platform.h::isNonDMAableMemory()` for the DMA-capable buffer requirement.

### South Bridge / SCC architecture (relevant to §9)

The PS3 southbridge is derived from Toshiba's published Super Companion Chip. Decapped retail PS3 southbridge die compared against Toshiba's published SCC die shot shows congruent floorplan and block placement.

- Mihara, T., et al. *"[Super Companion Chip with Audio Visual oriented interface for Cell Processor](https://old.hotchips.org/wp-content/uploads/hc_archives/hc17/2_Mon/HC17.S1/HC17.S1T3.pdf)"* Hot Chips 17 Proceedings, Session One, August 2005.
- Mihara, Uno, Maki. *"[Cell を生かす SuperCompanionChip™ / SuperCompanionChip™ Making Optimal Use of Cell Broadband Engine](https://www.global.toshiba/content/dam/toshiba/migration/corp/techReviewAssets/tech/review/2006/06/61_06pdf/a04.pdf)."* Toshiba Review Vol. 61 No. 6, 2006. (Original Japanese + [English](https://web.archive.org/web/20141117101320/http://www.ocpip.org/uploads/documents/SuperCompanionChip_English.pdf) translation both contain the bus-hierarchy diagram and the per-IO ordering specification.)

### Seagate SSHD Adaptive Memory (relevant to §2 hypothesis #5 and §9)

Verifies the claim that Seagate's Adaptive Memory does not promote sequential read streams to the SSHD's NAND cache. Seagate's own wording: long sequential streams "do not benefit from being stored in NAND flash." Independent confirmation:

- [ThePCEnthusiast - Seagate Desktop SSHD Review](https://thepcenthusiast.com/seagate-desktop-sshd-review/) Quotes Seagate: "long, sequential data strings… do not benefit from being stored in NAND flash."
- [TweakTown - Seagate Enterprise Turbo SSHD Review](https://www.tweaktown.com/reviews/5863/seagate-turbo-sshd-enterprise-sshd-review/index.html) "The NAND cache is not used for sequential access."
- [The Tech Report - Seagate Desktop SSHD 2TB Review](https://techreport.com/review/25425/) "Sequential data stays on the platters." Also confirms Adaptive Memory promotion is frequency-gated (per Seagate's explanation to the reviewers).
- [Seagate Marketing Bulletin MB618 SSHD goes the Distance](https://www.seagate.com/files/staticfiles/docs/pdf/en-GB/whitepaper/mb618-solid-state-hybrid-drive-gb.pdf) Seagate's own white paper on Adaptive Memory; frames the rationale positively (promote frequently-used data) rather than as explicit sequential exclusion.
- [StorageReview  Seagate Desktop SSHD Review](https://www.storagereview.com/review/seagate-desktop-sshd-review) Architecture details; uses 5 GB LBA limiting in benchmarks to force "cached" performance. This is implicit acknowledgment that uncached workloads behave like a regular HDD.

---

*Investigation by sagemono, 2026. cellmark v1.0.0.*