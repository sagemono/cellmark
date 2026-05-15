# PS3 Cell BE: Measurement & Optimization

A first-published characterization of the PS3's Cell Broadband Engine at the silicon level, with optimization guidance derived from on-hardware measurements. All numbers measured on retail and DECR PS3 hardware at stock 3.2 GHz using [cellmark](../README.md).

Companion documents carry deeper investigation notes for individual subsystems: [`ppe_tuning.md`](ppe_tuning.md), [`spu_tuning.md`](spu_tuning.md), [`disk_tuning.md`](disk_tuning.md).

## How to use this document

- **Developers** writing Cell / PS3 code: jump to [§0.6 Quick Reference](#06-quick-reference) for the optimization cheatsheet, then follow the cross-refs back to the section that proves each bullet.
- **Researchers (nerds)** studying Cell architecture: [§0.1–0.5](#part-0-methodology) cover methodology, PMU infrastructure and the bimodality caveat; [Parts 1–4](#part-1-ppe-powerpc-element) organize the findings by component (PPE, SPE, EIB, XDR) with measurement tables, PMU corroboration, and references to the IBM CBE Programming Handbook (CBE-PH).

---

## TL;DR

- **EIB SPE<->SPE pure-fabric bandwidth**: aggregate **115 GB/s** across 3 disjoint LS-to-LS pairs (74% of the per-direction 25.6 GB/s ring ceiling, 56% of the 204.8 GB/s nominal sustained EIB max). PMU shows **all 4 rings busy 66–77%** of the time - the 26% gap from nominal isn't ring imbalance, it's arbitration / half-ring overhead. [§3.1](#31-aggregate-spe-spe-bandwidth-pairs)

- **EIB hot-spot (5 readers -> SPE 0)**: aggregate **23.72 GB/s** into a single LS port = **93% of the 25.6 GB/s single-direction ceiling**. Ring breakdown reveals the EIB's physical topology directly: **RING2=47%, RING3=49% busy; RING0=1.5%, RING1=1.4% busy.** Two rings carry essentially all the traffic, two are idle. [§3.2](#32-hot-spot-saturation-5---1)

- **Hot-spot reader-count sweep**: saturation reached at **N=1 (23.58 GB/s = 92% of LS port)**. Adding readers does not increase aggregate; it divides the same pool. [§3.3](#33-reader-count-sweep---saturation-at-n1)

- **NxN matrix resolves the SPE physical layout**: logical SPE IDs 0–5 sit on the EIB ring in *numerical order* - CW goes 0->1->2->3->4->5, CCW the reverse. First published confirmation of the PS3's logical-to-physical SPE mapping on the EIB. [§3.4](#34-nxn-matrix---physical-topology)

- **Atomic-cache ping-pong (2-SPE `getllar`/`putllc`)**: **150 ns per line bounce, 6.65M rounds/sec**. Comparable to inter-core cache ping-pong on modern x86 (~50–100 ns) at much newer silicon / higher clock. [§3.5](#35-atomic-cache-fabric)

- **Mailbox PPE<->SPE round-trip**: **11.6 μs per round-trip, 5.8 μs one-way**. **~40x slower than the atomic-cache fabric** - mailbox is for occasional control events, not hot-loop signaling. [§3.6](#36-mailbox--event-queue)

- **PPE scalar FPU**: **4.93 GFLOPS** double-precision `fmadd` (77% of 6.4 GFLOPS peak; single FPU pipe, 7-cyc latency, 8 chains). [§1.1.2](#112-fp-scalar-fma-double)

- **PPE FXU integer**: **3.00 GOPS** uint64 `add` (94% of 3.2 GOPS peak; single FXU pipe). **The PPE has one FXU, not two** - CBE-PH §3.1 rules out intra-unit dual-issue. [§1.1.3](#113-fxu-integer-add)

- **SPE branch hint (`hbrr`) effectiveness**: **17 cyc/iter hinted, 30 cyc/iter unhinted, 13 cyc mispredict penalty, 1.71x speedup** on a tight 12-cycle backward loop. [§2.2](#22-branch-hint-hbrr-effectiveness)

- **PPE SMT scaling (TH0 + TH1)**: **1.30x aggregate**. Counter-intuitively, **same-unit (FPU+FPU) ties cross-unit (FPU+FXU)** - both score 1.30x. Same-unit hits **6.10 GFLOPS = 95% of FPU peak**. Cell PPE dispatch is single-issue per cycle across threads - POWER5/x86 SMT intuition doesn't apply. [§1.3](#13-smt-scaling-th0--th1)

- **Real-workload scoring**: stock 3.2 GHz PS3 baselines = score 100. **Pi BBP** (DP scalar number theory, hostile to SPE): 40 digits/sec at N=10000, 1.2% of DP peak. **FFT** (SIMD batched 4-lane radix-2 SP, FFTC-style inner butterfly): 185 Mpoints/sec, 6% of SP-FMA peak. Composite = geometric mean. [§5](#part-5-real-workload-benchmarks)

- **DMA chunk-size sweep (256B -> 16 KB)**: no knee. All chunk sizes hit ~22 GB/s aggregate. MFC issue rate is not the bottleneck. [§2.3.1](#231-chunk-size-sweep-negative-result)

- **XDR<->LS aggregate DMA**: GET 22.83 GB/s, PUT 22.13 GB/s on stock 3.2 GHz. PMU shows the SPE kernel is **90% TAGW-bound** - DMA-latency-limited, not bandwidth-limited from the SPE side. [§4.1](#41-aggregate-xdr-ls-bandwidth)

- **SP FMA SPU compute**: 88% dual-issue rate sustained. **DP FMA**: `DUAL=17` absolute but `PIPE0=PIPE1=36M` - direct PMU evidence that DFMA's 6-cyc even-pipe stall puts the two pipes out of phase. First time measured on retail/DECR PS3 silicon. [§2.1](#21-compute-pipelines--dual-issue)

- **Thermal/fan readout works on retail CFW only.** DECR kernel filters the syscalls by per-process privilege. [§6.1](#61-decr-thermalfan-readout-blocked)

---

# Part 0: Methodology

## 0.1 Hardware under test

- Retail PS3 (CECHB00 with CFW) and DECR-1000A development kit
- 6 user-accessible SPEs (Cell die has 8; 1 disabled at fabrication for yield, 1 reserved for the hypervisor)
- 256 MB XDR1 main memory (25.6 GB/s nominal ceiling)
- PPE: 32 KB I+D L1 (8-way), 512 KB unified L2 (8-way)
- SPE: 256 KB local store, 4 SP/2 DP FMA pipes
- LS port: 25.6 GB/s single-direction, 51.2 GB/s bidirectional
- EIB: 4 rings, 16B/cyc/ring at EIB clock = PPE clock / 2
- Stock clock: 3.2 GHz PPE/SPE, 79.8 MHz timebase
- Game OS / LV2 kernel under user-mode application execution

## 0.2 Build variants

`build.py retail` produces `cellmark.self` - fake-signed NPDRM, no libperf, runs on any CFW PS3 via `/dev_hdd0/game/CELLMARK0/USRDIR/EBOOT.BIN`.

`build.py decr` produces `cellmark_decr.elf` - defines `CELLMARK_DECR=1`, links `-lperf` for full PMU access. Loaded via ProDG Target Manager on a DECR development kit.

`make_pkg.py --variant decr` produces `cellmark_decr.pkg` with a distinct `TITLE_ID=CELLMARKD` so retail and DECR packages can coexist on the same console without overwriting each other's XMB slot.

DECR signing uses the same retail NPDRM auth-id (`0x1010000001000003`) - the alternate auth-ids in the Program Authority ID table are all path-locked to `/dev_flash/...` and can't be loaded from `/dev_hdd0/game/...`.

## 0.3 lv1 nclk read - accurate timebase on unpatched firmware

`sys_time_get_timebase_frequency()` is [stuck](https://github.com/sagemono/cell-xdr-overclocking/blob/main/lv0-lv1-timebase-analysis.md#2-root-cause) at the stock 79.8MHz value on firmware whose timebase patch hasn't been applied. An overclocked 3.857GHz chip still reports 79.8MHz timebase, making every wall-clock-derived metric in cellmark wrong by `(actual_clock / stock_clock)`.

Fix in [`ppu/main.c`](../ppu/main.c):

```c
system_call_8(10, 1ULL, 0x62650000ULL, 0ULL, 0x6e636c6b00000000ULL, 0ULL, 0ULL, 0ULL, 91ULL);
// p1 = status, p2 = nclk_hz
```

Syscall 10 = `sys_lv1_call` (lv1 passthrough). Hypercall 91 = `lv1_get_repository_node_value`. Path = `be.0.nclk` - the lv1 repository node holding the actual chip n-clock in Hz, regardless of LV2's view. [VshFpsCounter](https://github.com/sagemono/VshFpsCounter) uses the same path for its FPS overlay clock readout.

If lv1 returns a value that disagrees with `sys_time_get_timebase_frequency()`, the lv1-derived `nclk / 40` is used as the true timebase. If lv1 access fails, fall back to the SDK syscall.

## 0.4 PMU infrastructure (`pmu.h`/`pmu.c`)

Compiled into all builds; on retail every entry is a no-op so call sites don't need ifdefs. On DECR (`#ifdef CELLMARK_DECR`) it wraps libperf's NO_TRACE_MODE counter-only path:

```c
pmu_begin(profile);                    // PPE-only events
pmu_begin_spu(profile, spu_a, spu_b);  // SPU-targeted via thread ID
// ... kernel runs ...
pmu_end_and_read(&result);
```

**libperf hardware limits**: 4 32-bit counters per pass, at most 2 signal groups simultaneously. To cover more events the profiles are *rotated* every ~3 seconds wall-clock, each profile keeping its own rolling history. This is the same pattern game studios used internally with SN Tuner.

**SPU PMU gotcha**: `pmu_begin_spu` must populate **both** `spuTraceTarget[0]` and `[1]` even when only one is used; libperf returns EINVAL if either slot is left at default `type=0, threadId=0`. Use thread IDs from `sys_spu_thread_initialize`, not `LOGICAL_ID` - the latter fails with EINVAL on DECR (libperf can't infer group context in NO_TRACE_MODE).

**COUNTER32 slot mapping**: in NO_TRACE_MODE, `counter32[i]` reads from `signal[i]` (slots 0,1,2,3), **not** from `signal[i*2]` (slots 0,2,4,6) as a cascade theory would suggest. Empirically verified by swapping events and watching which slots came back zero. Not documented in the SDK headers available.

## 0.5 The bimodal PPE PMU caveat

PPE PMU readings are **bimodal** across batches. The PMU counts events core-wide during the 8 ms wall-clock window regardless of which thread is running. When LV2 preempts the kernel for a syscall handler / sysutil callback / interrupt, that handler also generates events but at a different rate than the kernel, making the counter values vary 30–100x between samples.

The cellmark display uses **MAX of a 32-sample rolling window** as the "least-interrupted run" indicator. MIN reflects the worst preemption case; MAX is closest to a clean kernel measurement. Median is meaningless for a bimodal distribution.

SPE-side PMU is unimodal (LV2 doesn't preempt SPE threads the same way), so the SPU bench display shows median directly.

## 0.6 Quick Reference

The optimization cheatsheet. Each bullet points to the section that proves it.

**PPE compute** - [§1](#part-1-ppe-powerpc-element)
- The PPE has **one of each** execution unit (FXU, FPU, VSU, LSU, BXU). Dual-issue is *inter-unit*, never two of the same. [§1.1.4](#114-the-ppe-has-one-fxu-surprise)
- Use **8+ independent FMA chains** to hide the 7-cycle FPU latency. With fewer chains, throughput drops linearly. [§1.1.2](#112-fp-scalar-fma-double)
- VMX FMA caps at ~47% of nominal peak on this platform (lv2 overhead); scalar FPU hits 77%, FXU hits 94%. Wider units have worse efficiency ceilings on this silicon. [§1.1.5](#115-per-cycle-issue-summary)
- For SMT: put **identical work on both threads**, not different work. Cross-unit doesn't double; same-unit hits 95% of unit peak. [§1.3](#13-smt-scaling-th0--th1)

**PPE cache** - [§1.2](#12-cache-hierarchy)
- L1 latency is 4 cyc when you use pointer-chase (`p = *(void**)p`), ~12 cyc when you use indexed addressing, gcc emits a 5-insn formation otherwise. [§1.2.2](#122-latency)
- `dcbt 0, base+512` (4 lines ahead) covers the ~36-cyc L2 hit latency for streaming reads. **`vec_dst()` is a no-op on Cell** per IBM tip 5, `dcbt` is the only effective prefetch primitive.

**SPE compute** - [§2](#part-2-spe-synergistic-processing-element)
- **SP FMA sustains 88% dual-issue.** Even pipe + odd pipe in lockstep is the goal; unrolled FMA + branch hits this. [§2.1.1](#211-sp-fma---88-dual-issue)
- **DP FMA cannot dual-issue with itself.** The 6-cyc even-pipe stall forces even and odd pipes out of phase. Interleave with SP/branch work if you need both pipes active during DP loops. [§2.1.2](#212-dp-fma---out-of-phase-pipes)
- **`hbrr` every backward branch in tight loops.** 1.71x speedup on a 12-cyc body; 3.6x on a 5-cyc body. Penalty for unhinted = 13 cyc full flush. Even hinted, taken branches cost ~5 cyc fetch redirect. [§2.2](#22-branch-hint-hbrr-effectiveness)
- **2-deep double-buffer DMA is sufficient.** Deeper queues don't help, XDR is the bottleneck. [§2.3.2](#232-queue-depth-negative-result)

**EIB / inter-SPE** - [§3](#part-3-eib-element-interconnect-bus)
- A single SPE can extract ~23.6 GB/s from another's LS port (92% of ceiling). **Many-to-one gather doesn't increase aggregate**, it just splits the same pool. [§3.3](#33-reader-count-sweep---saturation-at-n1)
- Logical SPE IDs map to **adjacent physical ring positions**. Producer -> `(src + 1) mod 6` is the cheapest CW neighbor. The opposite SPE (`(src + 3) mod 6`) is most expensive, the arbiter splits routing. [§3.4](#34-nxn-matrix---physical-topology)
- **Use atomic cache (`getllar`/`putllc`) for hot SPE<->SPE signaling.** 150 ns/bounce. Locks, atomic counters, work-queue heads. [§3.5](#35-atomic-cache-fabric)
- **Don't use mailbox for hot signaling.** 11.6 μs/round-trip, lv2-syscall-dominated. Use only for setup, error events, low-frequency control. [§3.6](#36-mailbox--event-queue)
- PPE<->SPE bootstrap: mailbox is fine (latency irrelevant once). PPE -> SPE periodic poke: signal notification or polled-flag DMA. SPE <-> SPE coordination: atomic cache always wins.

**XDR memory** - [§4](#part-4-xdr-memory)
- **6 SPEs saturate XDR at 22 GB/s.** Don't over-engineer per-SPE DMA; any reasonable 8-deep ring at any chunk size 256 B–16 KB hits the same ceiling. [§4.1](#41-aggregate-xdr-ls-bandwidth)
- Even single-SPE DMA approaches XDR ceiling with a deep enough queue; the bottleneck moves between SPE issue rate and XDR depending on active SPE count. [§4.2](#42-the-11-gap-from-nominal-xdr)

---

# Part 1: PPE (PowerPC Element)

## 1.1 Single-thread compute pipes

### 1.1.1 VMX FMA (vector)

`vmaddfp` throughput, 6 independent chains, 4-lane SIMD, 8x unrolled inner loop. Theoretical peak: 1 vmaddfp/cyc x 4 lanes x 2 ops = 8 FLOPS/cyc = 25.6 GFLOPS at 3.2 GHz.

| Metric | Value |
|---|---|
| Measured | **~12 GFLOPS** |
| Peak | 25.6 GFLOPS |
| Efficiency | **47%** |

The 53% gap is the **platform ceiling**, not the code. On lv2/Game OS, single-thread VMX FMA on the PPE caps near 50% of the documented peak regardless of how clean the kernel is. Disassembly shows no microcoded ops, no rc=1 forms, no spills. ProDG analysis suggests VSU result-port conflicts with the FPU that don't appear in IBM's nominal numbers.

This 47% ceiling is part of why Cell intentionally pushes vectorised work to the SPEs: 6 SPEs hit ~150 GFLOPS SP combined, > 12x the PPE-VMX ceiling.

### 1.1.2 FP Scalar FMA (double)

8 independent `a = a*mul + add` chains, 8x unrolled = 64 `fmadd` per outer iter, total 64M fmadd per batch. `volatile` source operands force the mul/add constants to be loaded once and prevent constant folding. `-mfused-madd` causes gcc to emit one `fmadd` (not separate `fmul`+`fadd`) per source expression.

| Metric | Value |
|---|---|
| Measured | **4.93 GFLOPS** |
| Peak | 6.4 GFLOPS (1 fmadd x 2 ops x 3.2 GHz) |
| Efficiency | **77%** |

The 23% gap is the PPE FPU's 7-cycle FMA latency vs the in-order single-issue pipe: 8 independent chains barely cover the latency window, and there's residual cost from loop control and branch overhead.

### 1.1.3 FXU Integer Add

8 independent uint64 accumulators with distinct strides, 8x unrolled = 64 adds per outer iter. **Forced via inline asm** because integer adds are associative. Without inline asm, gcc collapses `a0 += s0; a0 += s0; ...` into `a0 += s0 * 64` and the loop becomes nearly empty:

```c
#define FXU_ADD(a, s) __asm__ volatile ("add %0, %0, %1" : "+r"(a) : "r"(s))
```

| Metric | Value |
|---|---|
| Measured | **3.00 GOPS** |
| Peak | 3.2 GOPS (1 add x 3.2 GHz) |
| Efficiency | **94%** |

### 1.1.4 The "PPE has ONE FXU" surprise

A common misconception is that the Cell PPE is a "PowerPC 970 with SMT2". It isn't. The PPE was specifically *simplified* from PPC970 to keep die area for the SPEs. CBE-PH §3.1 documents the issue rules:

> "Two instructions can be issued in the same cycle from different
>  execution units."

The execution units are FXU, FPU, LSU, VSU (VMX), BXU, and a few support units, **one of each**. So PPE dual-issue is *inter-unit* (e.g., FXU add + FPU fmadd in the same cycle), never *intra-unit* (two adds in the same cycle). This is why FXU peak is 1.0 GOPS/GHz, not the 2.0 GOPS/GHz that a PPC970-style dual-FXU core would give.

In contrast, the PPC970 had two integer units that could dual-issue two adds per cycle.

### 1.1.5 Per-cycle issue summary

| Bench | Op type | Peak per cycle | Measured |
|---|---|---|---|
| VMX FMA | 1 vmaddfp (4 SP lanes x 2 ops) | 8 FLOPS | ~3.75 (47%) |
| FP Scalar FMA | 1 fmadd x 2 ops | 2 FLOPS | 1.54 (77%) |
| FXU Add | 1 add | 1 op | 0.94 (94%) |

The wider the unit, the worse the efficiency ceiling on this silicon. FXU at 94% saturates a simple in-order add stream with full dependency-chain unrolling. Scalar FP at 77% is limited by latency-hiding chain count. VMX at 47% is the platform ceiling.

## 1.2 Cache hierarchy

The PPE bench page measures L1/L2 read bandwidth and pointer-chase latency. Full results are in the cellmark runtime UI; key findings:

### 1.2.1 Bandwidth

L1 (28 KB working set) and L2 (400 KB) read bandwidth measured with a single moving pointer + `vec_ld(offset, base)` so gcc emits clean `lvx` with hoisted offsets, avoiding the per-`load addi + clrldi` address formation overhead gcc defaults to when indexing through `buf[i + n]`. Loads are issued ahead of the adds to hide the 7-cycle VMX load-to-use latency.

L2 path adds `dcbt 0, base+512` (4 cache lines = 512B ahead of current load). PPE L2 hit latency is ~36 cycles; 4 lines ahead at 8B/cycle issue gives 64 cycles of cover, comfortably overlapping the L2 miss path.

Per IBM Tip 5 (Altivec): `vec_dst()`/`dst` variants are **no-ops on
Cell**. `dcbt` is the only effective prefetch primitive.

### 1.2.2 Latency

Pointer chase using **Sattolo cycle** (single cycle visiting all N elements) over `void *` next-pointers. Compiles to `ld; bdnz` (2 insns, 4-cycle dependency = 1 PPE L1 hit latency per chase). The previous index-based version used 5 insns with a 7-cycle dependency chain (`rlwinm; extsw; add; lwz; bdnz`), inflating measured L1 latency to ~12 cycles.

PMU corroboration in §1.4 below: `l2_lat` profile shows 390K L1 misses in a 5 ms window for the 256 KB L2 chase = 14M cycles spent in L2-fill (390K x 36 cyc) = **87% of cycles miss-bound**, exactly what a 256 KB pointer chase should produce.

### 1.2.3 The 5-insn vs 2-insn pointer-chase difference

This is the kind of detail that easily moves a measured "L1 latency" number by 3x. The two compile paths:

Indexed (bad):
```
rlwinm  r9,r10,2,...     ; index *= 4
extsw   r9,r9            ; sign-ext
add     r8,r9,r4         ; addr = base + index
lwz     r10,0(r8)        ; load
bdnz+   loop             ; loop control
```

Pointer-chained (good):
```
lwz     r10,0(r10)       ; p = *p
bdnz+   loop             ; loop control
```

GCC chose `lwz` over `ld` because the BSS pointers fit in 32 bits. Same L1 latency, harmless in practice.

## 1.3 SMT scaling (TH0 + TH1)

The PPE is 2-way SMT. Conventional intuition from POWER5 / x86 SMT designs: **cross-unit kernels should scale near 2x because both threads can dispatch in the same cycle to different execution units.** Same-unit kernels should flatline at ~1x because they fight for one pipe.

The Cell PPE breaks this intuition. Two sub-tests were run back-to-back on the same SPU-idle PPU bench page, with a persistent worker PPU thread at priority 100 (matching the master's elevated priority) that sleeps on `sys_timer_usleep(50)` while idle so the inline solo baselines run on a truly empty TH1:

| Pair | Aggregate | Scaling vs solo |
|------|-----------|-----------------|
| **Same-unit** (FPU + FPU) | 6.10 GFLOPS | 1.30x |
| **Cross-unit** (FPU + FXU) | 3.55 GFLOPS + 1.54 GOPS | 1.30x |

Single-thread baselines on the same kernels (taken inline, worker asleep): FP scalar 4.69 GFLOPS, FXU 3.00 GOPS.

### Why same-unit wins

The PPE silicon dispatches **one thread per cycle**, alternating between TH0 and TH1 by default (TSRL[TP] favours equal weight; problem-state code can't change it, see [`ppe_tuning.md` §4.5](ppe_tuning.md)). Each thread therefore gets ~50% of dispatch bandwidth when both are busy.

So in cross-unit:
- TH0's FPU gets dispatched ~0.5 fmadd/cyc -> 3.2 GFLOPS theoretical
- TH1's FXU gets dispatched ~0.5 add/cyc -> 1.6 GOPS theoretical
- Each unit is **half-utilised** because the other half of its potential cycles belong to the other thread, which isn't using it.
- Measured: 3.55 GFLOPS + 1.54 GOPS, very close to theoretical.

In same-unit:
- Both threads dispatch into the FPU at 0.5 fmadd/cyc each
- The FPU pipe is fed every cycle (alternating threads) -> ~1.0 fmadd/cyc sustained throughput
- 1.0 fmadd/cyc x 3.2 GHz x 2 ops = 6.4 GFLOPS peak
- Measured: 6.10 GFLOPS = **95.3% of FPU peak**

Single-thread FP scalar only hits 4.69 GFLOPS (73% of peak) because one thread alone can't fully hide the 7-cycle fmadd latency even with 8 chains, the second thread's chains fill the gap.

### Implications

- **SMT on Cell PPE caps at ~1.3x total throughput**, not 1.8–2x, for any practical workload. The published "two-way SMT" capability is technically real but the silicon dispatch policy throws away most of the benefit you'd get on other SMT designs.
- **Same-unit pairing is not a penalty** here, it can be a *win* when the bottleneck is the unit's latency hiding rather than its throughput. Counter-intuitively, the right way to use PPE SMT for compute is to put the same kind of work on both threads.
- **Cross-unit SMT does not double throughput**. The 50/50 dispatch alternation means each thread runs at ~half its solo rate, summed back to ~the same aggregate as one fully-dispatched thread.
- Cinebench-style aggregate scoring should use **single-thread numbers** for PPE compute (or apply a flat ~1.3x SMT factor); modelling cross- vs same-unit separately is not warranted on this silicon.

## 1.4 PPE PMU profile

4 events from groups `PPU_IU_1` + `PPU_XU` (both TH0):

- `DUAL` - `TWO_PPC_INSTRUCTIONS_COMPLETED` (IPC ceiling indicator)
- `RAW`  - `ISSUE_STALL_DUE_TO_REG_DEPENDENCY` (register-dep stalls)
- `L1MISS` - `L1_DCACHE_MISS` (D-cache miss count)
- `LHS`  - `LOAD_HIT_STORE` (LHS hazards, the in-order PPE killer)

### What the data shows

| Bench | DUAL max | RAW max | L1MISS max | Notes |
|---|---|---|---|---|
| vmx_fma  | 6.1M (24%) | 5.9M (23%) | ~3.5K  | FMA loop, L1MISS ~zero (register-resident) |
| l1_bw    | 1.35M (35%) | 471K (12%) | ~45K | L1-resident, mild miss noise |
| l2_bw    | 1.18M (37%) | 728K (23%) | ~22K | L2-resident, more miss activity |
| l1_lat   | 327K       | 14K       | ~3.5K  | Tight pointer chase, fits L1 |
| l2_lat   | 330K       | 6.5M      | **390K** | 256 KB chase blows L1 - 87% miss-bound |

`L1MISS` correctly scales across the cache hierarchy, `l2_lat` shows. 390K misses in 5 ms, ≈ 14M cycles spent in L2-fill (every miss x ~36 cyc L2 latency) ~87% of cycles miss-bound, exactly what a 256KB pointer chase should produce.

The profile *rotates* every 3 seconds through three sub-profiles: `issue`, `branch`, `mem`. Each maintains its own rolling history so all 12 events are visible over time, working around the 4-counter / 2-group hardware limit.

---

# Part 2: SPE (Synergistic Processing Element)

## 2.1 Compute pipelines & dual-issue

`PAGE_CELL` runs SPE compute kernels in a forever-loop, so the per-batch `pmu_begin/end` pattern doesn't apply. `cell_pmu.c` runs a state machine called from the main loop: 900 ms idle -> 100 ms libperf window on SPE 0 -> read -> idle, with per-mode history reset.

Profile: `DUAL`, `PIPE0`, `PIPE1`, `BRANCH_HINT_MISS_PREDICTION`. SPE samples have <1% variance (no LV2 preemption on SPE side), so MIN ≈ MAX ≈ p50, the display just shows median as a clean number.

### 2.1.1 SP FMA - 88% dual-issue

```
DUAL  ≈ 278M (88% of cycles dual-issuing)
PIPE0 ≈ 278M
PIPE1 ≈ 278M
HMISS ≈ 11
```

PIPE0 = PIPE1 = DUAL, both pipes retire on the same cycle nearly every cycle. **88% dual-issue is the saturated case**: the unrolled FMA + branch / counter ops fill both pipes essentially every cycle. The `MAX` figure of ~324M is the rare uninterrupted run hitting effectively 100%.

### 2.1.2 DP FMA - out-of-phase pipes

```
DUAL   = 17 (yes, seventeen, absolute)
PIPE0 ≈ 36M
PIPE1 ≈ 36M
HMISS  ≈ 0
```

DUAL is essentially zero, but PIPE0 and PIPE1 are both at 36M. **The pipes retire on different cycles, never simultaneously.**

This is the architectural fingerprint of DFMA's 6-cycle even-pipe stall (SPU-ISA §10): the dfma forces a 7-cycle gap between successive even-pipe retirements, and the branches/counter ops slot into the odd-pipe holes between dfma retirements. PIPE0 and PIPE1 are out of phase. Total retirement = 72M / 320M = 22.5%, which matches the 79% efficiency seen at the GFLOPS level.

The IBM Cell SDK docs warn DP FMA can't dual-issue with itself, but I'm not aware of any prior published PMU measurement showing it directly on retail PS3 hardware. This is a genuinely new datapoint.

## 2.2 Branch hint (`hbrr`) effectiveness

The SPU has no dynamic branch predictor, the default static prediction is **predict-not-taken**. Every taken backward branch in a loop is therefore a mispredict unless the program inserts an `hbrr` (hint branch relative) instruction at least 8 cycles before the branch, telling the fetch unit which target to load.

### Measurement

One SPE runs two back-to-back kernels measured with the SPU decrementer. Both kernels are identical except the hinted version has an `hbrr` at the top of the loop body:

```asm
.balign 16
.Lloop:
    hbrr  .Lbranch, .Lloop     ; (hinted version only)
    lnop ; lnop ; lnop ; lnop ; lnop
    ai    $40, $40, -1
    lnop ; lnop ; lnop ; lnop ; lnop
.Lbranch:
    brnz  $40, .Lloop
```

10 `lnop`s give the 8+ cycle separation the hint hardware needs. 1M iterations per pass, decrementer at the timebase clock (79.8 MHz); cycles per iter = (decrementer ticks x 40) / iterations.

### Results (stock 3.2 GHz)

| Version | Cycles/iter | Notes |
|---|---|---|
| **Hinted** (`hbrr`) | **17** | loop body floor (~12 odd-pipe) + ~5 cyc redirect |
| **Unhinted** (no hint) | **30** | hinted floor + full pipeline flush per iter |
| **Penalty** (= unhinted − hinted) | **13** | the actual mispredict cost |
| **Speedup** | **1.71x** | for this body size |

### Interpretation

The 13-cycle mispredict penalty is in the expected range for an 18-stage SPU pipeline that resolves branches near the back of the pipe but not at the very end. CBE-PH quotes "18–20 cycles" as the worst case; the 13 reflects partial overlap of body fetch with the mispredict resolution.

**Hinted ≠ free.** Even with a correct hint, taken branches cost ~5 cycles of fetch redirect. The hint converts the *full flush* (13-cycle penalty) into a *partial redirect* (no penalty visible in the measured 17 − 12 = 5 cyc residual overhead). The cost of the branch is non-zero either way - it's just much smaller with a hint.

### Speedup depends on body size

For a tighter loop (5-cyc body minimum, the absolute floor), the same 13-cyc penalty would give:

| Body cycles | Hinted total | Unhinted total | Speedup |
|---|---|---|---|
| 5  | 5  | 18 | **3.6x** |
| 12 | 17 | 30 | 1.71x (measured) |
| 20 | 25 | 38 | 1.52x |
| 50 | 55 | 68 | 1.24x |

The same 13-cycle absolute penalty becomes a smaller *relative* overhead as the loop body grows. Tightly-unrolled SPU kernels see the biggest wins from `hbrr`; loops with heavy bodies care less.

### Why this matters

Most SPU compute kernels - FMA loops, vector blends and DMA pipelines have loop counters and backward branches. Without `hbrr`, every iteration pays the 13-cycle penalty; with `hbrr`, the penalty drops to ~5 cycles. The PPE has dynamic branch prediction (PowerPC branch predictor) so it doesn't need an `hbrr` equivalent. The SPU's hint-based scheme is a die-area tradeoff: the SPU saves the predictor's storage and logic, and pushes the responsibility onto the compiler.

cellmark's existing SPU kernels (`spu_fma_kernel.S`, etc.) all use `hbrr` at the top of their main loops, these benchmarks would lose the 1.71x speedup without it.

## 2.3 SPE-side DMA

The DMA bench (`PAGE_DMA`) runs all 6 SPEs in parallel pulling/pushing between LS and XDR. Results in §4, this section covers the SPE-side **programming patterns** and the negative results that fell out of the investigation.

### 2.3.1 Chunk-size sweep (negative result)

Sweep mode cycles GET chunk size through 256, 512, 1024, 2048, 4096, 8192, 16384B, four batches per value. Per-batch byte budget held constant at 64 MB/SPE so the wall-clock window is comparable across all sizes.

PMU window times from the per-chunk TTY log:

| Chunk | Window | Aggregate GB/s |
|-------|--------|----------------|
|   256B | ~17 ms | ~22.0 |
|   512B | ~17 ms | ~22.0 |
|  1024B | ~17 ms | ~22.0 |
|  2048B | ~17 ms | ~22.0 |
|  4096B | ~17 ms | ~22.0 |
|  8192B | ~17 ms | ~22.0 |
| 16384B | ~17 ms | ~22.0 |

(`64 MB x 6 SPEs / 17 ms ≈ 22 GB/s`. All bucket windows are identical to within sample noise.)

**No knee.** Throughput is invariant across the entire 256B -> 16 KB range, there's no chunk size at which MFC issue rate becomes the bottleneck within this sweep. With 6 SPEs each running an 8-deep ring, the *aggregate* MFC issue rate (~30 ns per `mfc_get` channel write x 6 x 8 in-flight ≈ tens of GB/s of issue capacity even at 256B) comfortably exceeds the 25.6 GB/s XDR ceiling at every size in the sweep. The XDR DRAM bus is the bottleneck, and the SPE-side queues self-throttle.

To find the issue-rate knee you'd need to either (a) drop to chunk sizes <=128B (likely below MFC pipeline efficiency), or (b) run the sweep with fewer active SPEs so the aggregate issue rate is no longer sufficient. Both are open follow-up experiments.

### 2.3.2 Queue depth (negative result)

Original speculation: "queue more outstanding DMAs per SPE, CH21 should climb, TAGW should drop, throughput should approach 25.6 GB/s." Now tested with a proper 8-deep ring kernel (tags 16..23, 128 KB LS for ring buffers, `queue_depth` parameter plumbed PPU->SPU).

Result: **no change**. PMU is identical at 2-deep and 8-deep:

| | 2-deep | 8-deep |
|---|---|---|
| TAGW% (GET) | 90.6% | 90.6% |
| TAGW% (PUT) | 93.5% | 89.5% |
| CH21 | 0 | 0 |
| Window | ~70 ms | ~70 ms |
| GB/s | 22.83 | 22.83 |

The original prediction was wrong. The XDR DRAM channel was already saturated at 2-deep. TAGW% being high doesn't mean the queue is too shallow, it means the SPE is waiting for the next slot in a fully-saturated XDR pipeline. 6 SPEs x 2 in-flight = 12 outstanding DMAs is already enough to keep XDR's 25.6 GB/s pipeline full; adding more in-flight per SPE just means each one waits longer for its turn. Wait *fraction* stays constant.

The 8-deep kernel was kept (it's the cleaner code), but the throughput ceiling for this workload is genuinely XDR-bound.

### 2.3.3 What the PMU tells us

SPU_MON profile on logical SPE 0 of the DMA bench group, 100% reproducible:

| | DUAL | PIPE0 | TAGW | CH21 |
|---|---|---|---|---|
| GET (70 ms ≈ 224M cyc) | 49K | 107K | **203M (90.6%)** | **0** |
| PUT (73 ms ≈ 234M cyc) | 49K | 107K | **210M (93.5%)** | **0** |

`TAGW` = cycles waiting for an MFC tag completion (in-flight DMA to finish). `CH21` = cycles stalled writing to channel 21 (DMA cmd queue when full).

**The kernel is 90–93% DMA-latency-bound, not bandwidth-bound from the SPE side.** With the 2-deep double-buffer (`A` and `B` chunks alternating), the SPE has at most 2 DMAs in flight. The instant one completes,the SPE issues the next and immediately blocks on tag completion. The MFC's 16-deep cmd queue is **never** full (CH21=0), meaning the SPE could issue commands much faster than completions arrive, but won't, because the kernel's structure limits it to 2 outstanding.

The 22 GB/s aggregate is set by: **6 SPEs x in-flight-DMA-count x per-chunk-completion-time**

---

# Part 3: EIB (Element Interconnect Bus)

The EIB is Cell's on-die interconnect, 4 rings (2 CW + 2 CCW), 16B/cyc each at EIB clock = PPE clock / 2. The atomic-cache fabric and mailbox event path run on separate channels off the same physical bus structure.

## 3.1 Aggregate SPE<->SPE bandwidth (PAIRS)

### Measurement

`PAGE_EIB` runs all 6 SPEs in three disjoint pairs (0<->1, 2<->3, 4<->5). Each SPE DMA-reads 64 KB windows from its partner's local store via the SDK-fixed peer-LS EA mapping:

```
peer_ls_ea = SYS_SPU_THREAD_BASE_LOW + partner_idx * SYS_SPU_THREAD_OFFSET
           = 0xF0000000 + partner_idx * 0x100000
```

The MFC routes the DMA based on the high address bits, no LS-EA discovery handshake needed. Pure EIB traffic; the MIC and XDR are never touched.

### Results (stock 3.2 GHz)

```
Pairs GET (LS<->LS):  114.73 GB/s aggregate
  per-pair: 37.80 GB/s (74% of 51.2 GB/s bidirectional ceiling)
  per-dir:  18.90 GB/s (74% of 25.6 GB/s single-ring ceiling)
  aggregate: 56.0% of 204.8 GB/s EIB sustained max
```

Each pair runs **two** simultaneous flows (i->j AND j->i), so per-pair is counted bidirectionally. ~37.8 GB/s/pair is plausible against 51.2 GB/s nominal because the EIB has 2 rings going in each direction.

### PMU ring breakdown

`EIB_DARB_1` group, 4x `EIB_DATA_RING_X_WAS_IN_USE` counters:

| Ring | Median busy cycles (14 ms window) | % of EIB cycles |
|------|-----------------------------------|-----------------|
| RING0 | 14.8M | **66%** |
| RING1 | 16.6M | **74%** |
| RING2 | 15.6M | **70%** |
| RING3 | 17.2M | **77%** |
| Avg   |       | **72%** |

(EIB cycles = window_us x tb_freq x 20, since EIB clock = PPE clock / 2)

### Interpretation

The 72% average ring-busy figure matches the 74% per-direction efficiency seen at the GB/s level. **The 26% inefficiency is NOT because some rings are idle**, all 4 are saturated 66–77% of the time. The headroom is in:

- Ring arbitration overhead (CBE-PH §1.7.3: arbiter denies any transfer >half-ring on a given direction, forcing it onto the opposite-direction ring or the queue)
- Command-vs-data phase gaps (a "ring busy" cycle includes both grant and data phases)
- Inter-master scheduling delays at the EIB ramps

Ring imbalance is small (15% spread RING0 lowest to RING3 highest), so the EIB scheduler distributes flows reasonably across rings under PAIRS load.

To my knowledge no public benchmark has previously published a per-ring EIB utilization breakdown. The IBM Cell BE Programming Handbook (§1.7.2) quotes 204.8 GB/s sustained / 307.2 GB/s instantaneous for the fully-populated 8-SPE Cell, but real measurements on the PS3's 6-SPE config are nonexistent.

## 3.2 Hot-spot saturation (5 -> 1)

### Measurement

Same SPE kernel as PAIRS, but the partner mapping flattens: SPEs 1–5 all target SPE 0's LS via the peer-LS EA. SPE 0 itself is spawned with `iterations=0` so its kernel returns immediately and its MFC is free to service the 5 inbound flows.

### Results (stock 3.2 GHz)

```
Hotspot (5 -> SPE 0):  23.72 GB/s aggregate
  per-reader: 4.74 GB/s (23.7 GB/s into target LS port)
  LS port:    93% of 25.6 GB/s single-direction ceiling
```

### PMU ring breakdown - the publishable finding

`EIB_DARB_1` group, 56 ms PMU window (89.6M EIB cycles):

| Ring | Median busy cycles | % of EIB cycles |
|------|--------------------|-----------------|
| RING0 | 1.39M | **1.5%** |
| RING1 | 1.27M | **1.4%** |
| RING2 | 42.0M | **46.9%** |
| RING3 | 44.2M | **49.3%** |

**Only two of four rings carry the traffic.** This is the cleanest possible demonstration of the EIB's "two rings per direction" physical topology. Under PAIRS, traffic flows in both directions and all four rings sit at 66–77% busy. Under HOTSPOT, traffic is unidirectional (5 sources -> 1 sink), so the EIB arbiter routes everything onto the two rings whose direction points toward SPE 0's ramp. The other two rings, which would have to carry data the wrong way, get nothing.

### LS-port ceiling

23.72 GB/s = 93% of the 25.6 GB/s single-direction LS port ceiling
quoted by CBE-PH §1.7.3. The remaining 7% is:

- Inter-master arbitration at SPE 0's MFC, which cycles between requests from the 5 readers (CBE-PH §7.5.6)
- Half-ring policy denials when a reader's physically-farther path would require >½ ring traversal in the active direction; the arbiter forces those onto the opposing-direction ring path back through the queue, which shows up as the residual 1.5% on RING0/RING1
- Grant/data phase gaps on the two active rings (each is only ~47–49% busy despite carrying all the traffic, they have plenty of capacity, the bottleneck is at the destination LS port)

For program design: a master/worker pattern where the master gathers from N workers is fundamentally limited by the master's LS port (~25 GB/s). A peer-to-peer pattern (workers exchange with each other in pairs) can scale to ~115 GB/s aggregate. The 5x scaling gap between the two patterns is now PMU-measured rather than theoretical.

## 3.3 Reader-count sweep - saturation at N=1

Sweep mode walks the active-reader count N through 1, 2, 3, 4, 5 (SPEs `1..N` read from SPE 0; the rest are idle), four batches per value, EWMA per slot. Each reader does 256 MB of DMA per batch from SPE 0's LS.

### Results

| N readers | Aggregate | Per-reader | % of LS port |
|-----------|-----------|------------|--------------|
| 1 | 23.58 GB/s | 23.58 GB/s | **92%** |
| 2 | 23.78 GB/s | 11.89 GB/s | 93% |
| 3 | 23.86 GB/s |  7.95 GB/s | 93% |
| 4 | 23.76 GB/s |  5.94 GB/s | 93% |
| 5 | 23.71 GB/s |  4.75 GB/s | 93% |

### Interpretation

The aggregate is **flat** across N, 23.6–23.9 GB/s independent of reader count. Per-reader bandwidth scales as aggregate/N. **The destination LS port is the bottleneck immediately at N=1.** A single SPE can extract 23.58 GB/s out of another SPE's LS, hitting 92% of the 25.6 GB/s single-direction ceiling on its own. Additional readers just split the same pool fairly.

Practical consequence: a many-to-one gather pattern doesn't degrade *aggregate* throughput beyond what a one-to-one transfer already gives, it just multiplexes that same fixed bandwidth across more consumers. The LS port arbitration is fair (no reader is starved or privileged).

### Per-N ring distribution - dynamic routing exposed

Median ring busy% from the per-N PMU log:

| N | RING0 | RING1 | RING2 | RING3 | Active pair |
|---|-------|-------|-------|-------|-------------|
| 1 | 0.7%  | **54%** | 1.7%  | **60%** | RING1 + RING3 |
| 2 | 0.8%  | **43%** | 1.4%  | **60%** | RING1 + RING3 |
| 3 | 0.8%  | **36%** | 1.4%  | **63%** | RING1 + RING3 |
| 4 | 1.3%  |  1.4%   | **33%** | **64%** | RING2 + RING3 |
| 5 | 1.5%  |  1.5%   | **47%** | **49%** | RING2 + RING3 |

The active *pair* changes between N=3 and N=4. With 1–3 readers the EIB arbiter routes everything through RING1 + RING3; at 4–5 readers it switches to RING2 + RING3. RING3 is in the "preferred direction" pair for all N, but its partner ring flips.

This is direct PMU evidence of dynamic per-pattern ring routing. The rings have fixed physical directions, and the arbiter picks the shorter-hop ring out of each direction-pair based on which readers are active. At N<=3 the active readers sit on one side of SPE 0's ramp on the silicon; at N>=4 readers are on the opposite side, forcing the arbiter onto a different ring pair to keep traversal short.

## 3.4 NxN matrix - physical topology

All 30 ordered SPE pairs measured in isolation: only `src` reads from `dst`'s LS, all other SPEs idle. Each pair classified by which EIB direction-pair carried the traffic, rings 0+2 (clockwise) versus rings 1+3 (counter-clockwise). If one direction's combined ring busy% exceeds 2x the other's, the pair is labeled `CW` or `CCW`; otherwise `SPL` (arbiter routed roughly equally in both directions).

### Result

```
src\dst   0    1    2    3    4    5
   0:     -   CW   CW   SPL  CCW  CCW
   1:    CCW   -   CW   CW   SPL  CCW
   2:    CCW  CCW   -   CW   CW   SPL
   3:    SPL  CCW  CCW   -   CW   CW
   4:    CW   SPL  CCW  CCW   -   CW
   5:    CW   CW   SPL  CCW  CCW   -
```

Per-pair aggregate: 22.97–23.60 GB/s, average 23.42 GB/s. Single-reader saturation of the destination LS port confirmed at every pair.

### The topology

The matrix is **perfectly rotationally symmetric**. For every source SPE
the same pattern holds:

- The 2 SPEs at logical distance +1, +2 (mod 6) use the CW direction-pair.
- The 2 SPEs at logical distance −1, −2 (mod 6) use the CCW direction-pair.
- The 1 SPE at logical distance +3 (mod 6, i.e. directly across the ring) is SPL equidistant in both directions, so the arbiter routes roughly half each way.

**This proves the logical SPE IDs 0–5 are the physical positions on the EIB ring, in numerical order.** The CW direction physically traverses the ring as 0->1->2->3->4->5->0; CCW goes 0->5->4->3->2->1->0.

```
                    EIB ring (CW)
       ╔════════════════════════════════════════╗
       ║                                        ║
       ║   SPE0  ──►  SPE1  ──►  SPE2  ──►      ║
       ║                                        ║
       ║      ◄──  SPE5  ◄──  SPE4  ◄──  SPE3   ║
       ║                                        ║
       ╚════════════════════════════════════════╝
                   EIB ring (CCW)
```

### Why this matters

On the PS3, 2 of the Cell's 8 physical SPEs are disabled (one yield, one hypervisor-reserved). The 6 SPEs exposed to applications have logical IDs 0–5, but until now there was no public confirmation that these logical IDs map directly to *adjacent* physical ring positions. The NxN matrix's clean rotational symmetry is direct PMU evidence that they do. The SDK / Lv-2 numbers the available SPEs in physical-ring order without gaps in the visible window.

For program design this means:
- Communication cost on the EIB grows with logical-ID distance.
- A producer -> consumer flow that targets `(src + 1) mod 6` is the cheapest possible (closest CW neighbor); `(src − 1) mod 6` is the cheapest CCW neighbor.
- The opposite SPE (`(src + 3) mod 6`) is the most expensive target; the arbiter splits its traffic across both directions, doubling the ring-busy footprint per unit of data.

For master/worker patterns where the master gathers from N workers, placing the master at SPE 0 and workers at 1, 2, 3, 4, 5 means workers at distance ±1 and ±2 cost roughly the same (CW or CCW near-neighbor), but the worker at SPE 3 (distance 3) costs roughly 2x the ring time of the others. The reader-count sweep's RING2<->RING3 partner-flip between N=3 and N=4 (§3.3) now makes sense: at N=3, all readers (SPE 1, 2, 3) sit on the CW arc and one direction-pair suffices; adding SPE 4 and SPE 5 (CCW arc) forces the arbiter to also engage the CCW rings, appearing in PMU as a partner-ring switch.

## 3.5 Atomic-cache fabric

The atomic cache is a separate, dedicated 8-entry-per-SPE structure (CBE-PH §8) used by `getllar` / `putllc` reservation-based atomic ops. It bypasses the main MFC DMA queue and the EIB data rings, atomic traffic uses the atomic-cache fabric directly. Until now, nobody had published a measurement of its latency on retail PS3 silicon.

### Measurement

Two SPEs share a single 128B cache-aligned EA. A 4-byte counter sits at offset 0. Each SPE polls the counter via `getllar`; when its turn parity matches, it increments and does `putllc`. Strict turn-by-turn alternation means reservation losses are extremely rare and each successful `putllc` represents one full line handoff through the atomic cache fabric.

```c
while (successful < target) {
    mfc_getllar(buf, ea_atomic, 0, 0);
    (void)mfc_read_atomic_status();
    val = *(volatile uint32_t *)buf;
    if ((val & 1) != my_parity) continue;        /* spin */
    *(volatile uint32_t *)buf = val + 1;
    mfc_putllc(buf, ea_atomic, 0, 0);
    if (mfc_read_atomic_status() & MFC_PUTLLC_STATUS)
        continue;                                /* lost reservation - retry */
    successful++;
}
```

The PPU times the full 2-SPE thread-group join over 256K rounds per SPE (512K total bounces) and reports `ns/bounce = total_time / total_rounds`.

### Result (stock 3.2 GHz)

```
Atomic-cache:  150 ns/bounce   6.65M rounds/sec
```

### Interpretation

**150 ns per bounce** = the time for one 128B atomic line to transit from one SPE's atomic cache to another's, including the snoop / invalidate of the source cache. This is comparable to inter-core cache ping-pong on modern x86 (~50–100 ns, but on much newer silicon and at 4–5 GHz). For 2006 Cell silicon at 3.2 GHz it's a pretty competitive number.

For program design:

- A spin-lock acquire/release cycle across two SPEs is bounded by ~2x this number (one bounce to acquire, one to release) = **~300 ns per fully-contended critical section** on the atomic cache. Fine for coarse-grain work, costly for tight loops.
- A producer->consumer work-queue handoff is one bounce per item, the fabric can sustain **6.6M items/sec** between two SPEs at a single atomic head pointer. Bandwidth-wise, that's 128B x 6.6M = 850 MB/s of atomic traffic between two SPEs, much lower than EIB pair bandwidth (~38 GB/s per pair) but the per-op overhead is fixed.
- The atomic cache is the right primitive for SPE<->SPE control plane (mutexes, work counters, ring-buffer indices). Doing the same with DMA + spin-on-LS-poll would cost >=600 ns per round-trip and burn EIB ring cycles to boot.

The atomic-cache fabric is the under-documented half of Cell's inter-SPE communication story. Most public discussion focuses on EIB ring bandwidth; the atomic path is mentioned in IBM docs as "fast and dedicated" with no concrete latency number for the PS3 silicon. This benchmark fills that gap.

## 3.6 Mailbox + event queue

The mailbox is the dedicated PPE<->SPE control channel. A hardware FIFO on each SPU's local store interface. There are three mailboxes per SPU:

- `SPU_RdInMbox` (channel 29, 4-deep) - PPE writes, SPE reads
- `SPU_WrOutMbox` (channel 28, 1-deep) - SPE writes, PPE polls via MMIO
  (not accessible to thread-mode SPUs)
- `SPU_WrOutIntrMbox` (channel 30, 1-deep) - SPE writes, raises an
  interrupt routed to a connected event queue on PPU

For thread-mode SPUs (which is everything in cellmark), the SPE->PPE direction must use the *interrupting* outbox plus an lv2 event queue, there's no syscall to directly read the non-interrupting outbox.

### Measurement

1 SPE thread tightly loops `spu_read_in_mbox -> sys_spu_thread_send_event`. PPE loops `sys_spu_thread_write_spu_mb -> sys_event_queue_receive` and times the steady-state window over 4096 iterations. The PPU-side mailbox write is a lv2 syscall (~1-2 μs); the SPE-side `send_event` is also a syscall under the hood (it must pack the SPE thread's port into the intr_mbox value and route via the event subsystem); lv2 schedules the event delivery into the queue.

```c
// SPE (linked with -lsputhread):
for (i = 0; i < n; i++) {
    uint32_t v = spu_read_in_mbox();
    sys_spu_thread_send_event(SPU_PORT, (v + 1) & 0x00FFFFFF, 0);
}

// PPE:
sys_event_queue_create(&eq, ..., SYS_EVENT_QUEUE_LOCAL, 8);
sys_spu_thread_connect_event(thread, eq, SYS_SPU_THREAD_EVENT_USER, SPU_PORT);
sys_spu_thread_group_start(group);
for (i = 0; i < n; i++) {
    sys_spu_thread_write_spu_mb(thread, val);
    sys_event_queue_receive(eq, &event, timeout);
    val = (uint32_t)(event.data2 & 0x00FFFFFF);
}
```

### Result (stock 3.2 GHz)

```
Mailbox PPE<->SPE:  11600 ns roundtrip   5800 ns one-way   0.086M rounds/sec
```

### Interpretation

11.6 μs per round-trip is consistent with **~4 lv2 syscall dispatches plus 2 channel waits plus event-queue scheduling** in the loop hot path. This is the unavoidable cost a thread-mode PS3 program pays for explicit PPE<->SPE control plane signaling.

For comparison across Cell's inter-unit signaling paths:

| Path | Round-trip | Use case |
|------|-----------|----------|
| **Atomic cache** (`getllar`/`putllc`) | 300 ns (2x150 ns) | locks, atomic counters |
| **EIB ring DMA** (small chunk) | ~600–800 ns | bulk data, work queues |
| **Mailbox + event queue** | **11600 ns** | low-frequency control events |

The mailbox is **40x slower than atomic cache** for a comparable ping-pong. The disparity is entirely lv2-syscall overhead on the PPE side plus event-queue scheduling, the underlying intr_mbox hardware is single-cycle, but the software path to access it from thread-mode programs requires multiple kernel dispatches per direction.

### Practical implication

Don't use mailbox for hot signaling. The standard programming patterns:

- **PPE -> SPE bootstrap** (one-shot, "here's your params EA"): mailbox is fine, latency is irrelevant for setup
- **SPE -> SPE coordination** (work queue heads, completion flags): atomic cache (`getllar`/`putllc`) at 150 ns per transfer
- **PPE -> SPE periodic poke** (frame boundaries, mode changes): signal notification or a polled flag in EA via DMA
- **SPE -> PPE rare events** (errors, completion of long-running batches): mailbox is acceptable; 12 μs is negligible if the SPE just spent milliseconds doing real work

cellmark's own SPE spawn/join cycles for the DMA and EIB benches go through this same path implicitly (the thread group join is event-based), which is why batch overhead in those benches is dominated by spawn time rather than the bench kernel itself for short iteration counts.

### Bare-metal floor (not measured, design-only)

The mailbox hardware itself is single-cycle. A bench that uses `sys_raw_spu_create` + direct MMIO writes to `SPU_In_MBox` / `SPU_Out_MBox` (no lv2 calls on the hot path) would estimate the silicon floor at **~300–800 ns per round-trip**, roughly 15–30x faster than the 11.6 μs measured thread-mode cost. The full implementation (raw-SPU bootstrap, manual LS load via `sys_raw_spu_image_load`, MMIO polling loop) was prototyped but the PS3 only has 6 user-accessible SPUs total, and reserving one for raw mode forces every existing 6-SPE thread group (compute, EIB pairs, DMA aggregate) to drop to 5 SPEs and re-measure. The trade-off wasn't worth disrupting the other benches, so the silicon floor is documented here as a predicted bound; the 11.6 μs is the realistic cost any thread-mode PS3 program actually pays.

---

# Part 4: XDR Memory

## 4.1 Aggregate XDR<->LS bandwidth

### Results (stock 3.2 GHz)

```
GET (XDR -> LS):  22.83 GB/s aggregate (89% of 25.6 GB/s XDR1 ceiling)
PUT (LS -> XDR):  22.13 GB/s aggregate (86%)
```

Both within ~13% of the published XDR1 DRAM ceiling. PUT being only ~3% behind GET is interesting, I originally predicted "10–20% lower for PUT due to write coalescer latency" but the actual delta is much smaller.

PMU corroboration and the queue-depth / chunk-size details are in [§2.3.1–2.3.3](#231-chunk-size-sweep-negative-result). Those numbers characterize the SPE-side programming patterns; the aggregate ceiling here is set by XDR DRAM, not anything SPE-side.

## 4.2 The 11% gap from nominal XDR

22.83 / 25.6 ≈ 89%. The 11% gap from the published XDR1 ceiling is
explained by:

- DRAM refresh cycles (~2–3% on XDR1)
- Bank conflict probability (6 SPEs hitting independent 1 MB slices = some unavoidable bank overlap)
- MIC scheduling overhead (read/write coalescer reset latency)

None of those are addressable from the SPE side. The 22 GB/s sustained
is the practical ceiling for any 6-SPE XDR-streaming kernel on retail
PS3 silicon.

---

# Part 5: Real-workload benchmarks

The measurements in Parts 1–4 characterize Cell at the *silicon* level, peak compute rates, fabric bandwidth, latency floors. Part 5 covers *application-level* workloads designed to exercise specific subsets of the silicon under realistic algorithmic load. Each workload reports a score normalised to a stock 3.2 GHz PS3 baseline (stock = 100); the composite cellmark score is the geometric mean across active workloads.

## 5.1 Pi (BBP hex digit extraction)

### Algorithm

Bailey-Borwein-Plouffe digit-extraction. Computes hex digit at
position n+1 of π directly (no need to compute prior digits) via:

```
S_j(n) = frac( sum_{k=0..n} [16^(n-k) mod (8k+j)] / (8k+j) ) + frac( sum_{k>n} [16^(n-k) / (8k+j)] ) 

digit_{n+1} = floor( 16 * frac(4*S_1 - 2*S_4 - S_5 - S_6) )
```

Each digit position is independent -> trivially parallel across 6 SPEs. Cost per digit grows linearly with N (the position): O(N log N) double-precision ops dominated by `16^p mod m` modular exponentiation.

### Implementation

`spu_pi_kernel.c`. Uses double precision throughout (SP would lose significance past ~10^4 digit positions). The SPE has no hardware DP divide, so each `mod_pow16` call precomputes `1/m` once and uses multiplication thereafter, eliminating O(log p) divides per call.

PPU-side self-test at init: `pi_hex_digit(0..7)` is compared against the known first 8 hex digits of π (`243f6a88`). PASS confirms the algorithm is correct; the SPU compiles the same source so a PPU pass implies SPU pass too.

### Measurement at stock 3.2 GHz

Computing 30 digits per batch at position N=10000 (5 per SPE):

| Metric | Value |
|---|---|
| Throughput | **39.99 digits/sec** |
| Batch wall time | 750 ms |
| Aggregate DP rate | ~0.14 GFLOPS |
| % of SPE DP peak | **~1.2%** |
| Stock baseline (score 100) | 40.0 digits/sec |

### Interpretation

Pi BBP is a poor fit for SPE silicon:

- **No hardware DP divide** - emulated via software, ~30 cycles each.
- **No SIMD utilisation** - BBP is scalar number-theory code.
- **DP FMA out-of-phase stall** (§2.1.2) caps SPE DP throughput at ~1.83 GFLOPS regardless.
- **Branchy** - `if (p & 1)` in `mod_pow16` can't be branch-hinted cleanly.
- **Int<->double conversions** inside `dfloor_pos` are slow on SPE (each requires a pipeline-stalling type conversion intrinsic).

So 1.2% of DP peak is the realistic floor for naive DP scalar code on SPE. Real-CPU comparison at position N=10000: a modern Apple M5 / Ryzen core hits ~5000–10000 digits/sec (probably even more) single-threaded. This is **100–250x faster than 6 PS3 SPEs combined**. That's mostly 2006 silicon vs 2026 silicon plus algorithm-vs-architecture mismatch; it's a deliberate inclusion in cellmark precisely because it *doesn't* showcase Cell's strengths, so the composite score reflects "general-purpose code", not just the best case.

## 5.2 FFT (1D radix-2 complex SP, SIMD batched)

### Algorithm

Cooley-Tukey radix-2 decimation-in-time, single-precision complex, SoA layout (separate real/imag arrays). FFT_N = 1024 points, log2(N) = 10 stages. Cost = (N/2) x log2(N) x 5 SP flops = 25,600 flops per FFT.

### Implementation

`spu_fft_kernel.c`. SIMD batched: 4 logical FFTs run in lockstep across the 4 lanes of `vector float`. Each butterfly is one set of vector ops:

```c
vp_imim = spu_mul(vw_im, vb_im);
vp_imre = spu_mul(vw_im, vb_re);
vtmp_re = spu_msub(vw_re, vb_re, vp_imim);   // w_re*b_re - w_im*b_im
vtmp_im = spu_madd(vw_re, vb_im, vp_imre);   // w_re*b_im + w_im*b_re
re[i] = spu_add(va_re, vtmp_re);
im[i] = spu_add(va_im, vtmp_im);
re[j] = spu_sub(va_re, vtmp_re);
im[j] = spu_sub(va_im, vtmp_im);
```

Twiddle factors `w_re`, `w_im` are scalar table lookups broadcast across lanes via `spu_splats` sidesteps the strided-gather problem that single-FFT-vectorized FFTs face.

This inner-loop SIMD pattern is adopted from **FFTC** (Bader, Agarwal, Petrini, *et al.*, "FFTC: Fastest Fourier Transform for the IBM Cell Broadband Engine", CellBuzz 2007). FFTC's *parallelism model*, one big FFT distributed across 8 SPUs with hardcoded log2(8)=3-stage inter-SPE sync, does not port cleanly to PS3 (only 6 SPUs accessible; log2(6) is not an integer, so the radix-2 sync network has no clean equivalent). cellmark uses a different parallelism model: **6 SPUs x 4 lanes = 24 independent logical FFTs in parallel, zero inter-SPE traffic**. Same inner-loop techniques, different outer-loop topology.

### Self-validation

Each batch's first SIMD-pass uses an impulse input (`re[0]=1`, rest zero) on lane 0. Expected output: `re[k]=1` for all k, `im[k]=0` for all k. The SPE computes max bin error vs expectation and ships it back. PPU surfaces worst-of-six. This validates the *actual SIMD code path* every batch (not just a PPU-side scalar reference of the algorithm).

### Measurement at stock 3.2 GHz

32 FFTs per SPE per batch (8 SIMD-batches x 4 lanes), 6 SPEs:

| Metric | Value |
|---|---|
| Throughput | **184.82 Mpoints/sec** (0.18 MFFTs/sec) |
| Batch wall time | 1.1 ms |
| Aggregate SP rate | ~9.3 GFLOPS |
| % of SPE SP-FMA peak | **~6%** |
| Self-test max error | 0.00 (bit-exact for impulse) |
| Stock baseline (score 100) | 184.82 Mpoints/sec |

### Why only 6% of peak

The butterfly inner has **8 even-pipe ops in a 3-deep dependency chain** (mul -> fma -> add/sub). The 4-lane SIMD widens each op but doesn't shorten the chain; SP FMA has 6-cycle latency and 1-cycle throughput, so the chain takes ~18 cycles to drain per butterfly even with perfect pipelining. The early stages (m_half = 1, 2) have tiny inner-loop trip counts, so loop-control overhead and small-N effects dominate them. Scalar twiddle loads + splats add latency that the inner loop can't hide.

The FFTC paper hits ~70 GFLOPS aggregate on 8 SPUs (8 GFLOPS/SPU = 31% of peak) by:
- Vectorising *within* one FFT (4 butterflies of the same FFT processed in parallel), which gives stride-1 twiddle loads.
- Combining early-stage radix-2 butterflies into radix-4/8 to amortise the loop overhead.
- Using DMA double-buffering since their FFTs span multiple SPUs.

The cellmark FFT could close half the gap with similar techniques. The 4.3x speedup over the scalar baseline (43 Mpoints/sec -> 184) is a good return on a single SIMD pass; further optimization is deferred to a future revision since the current number is a valid, validated workload measurement.

### Real-CPU comparison

A modern desktop FFTW or MKL single-thread N=1024 FFT hits ~5–10 GFLOPS = ~200–400 Mpoints/sec. 6 PS3 SPEs at 184 Mpoints/sec is **~50–100% of a single modern core**, which is a much more flattering ratio than Pi BBP shows. SIMD-friendly dense FMA workloads are Cell's wheelhouse.

## 5.3 Composite cellmark score

Geometric mean across active workloads:

```
composite = (score_1 * score_2 * ... * score_N) ^ (1/N)
```

Each `score_k = (measured_k / ref_stock_k) * 100`, where `ref_stock_k` is the stock 3.2 GHz baseline for workload k. So at stock, every workload reads ~100 and the composite ~100 too. An overclocked / underclocked / patched-firmware console reads above or below 100 in proportion to its delta from stock.

Geometric mean is the standard Cinebench-style aggregate: it penalises any single very-low score (one failing workload tanks the composite) and rewards uniform improvement across all workloads. Future workload additions (AES, SHA, prime sieve) will each contribute a single normalised score to the same composite.

---

# Part 6: Limitations & known gaps

## 6.1 DECR thermal/fan readout blocked

`sys_game_get_temperature` (syscall 383) and `sys_sm_get_fan_policy` (syscall 409) are filtered by **per-process privilege** on the DECR LV2 kernel. Even when the SDK function call (PRX-imported via sysPrxForUser) is used instead of the raw syscall, the kernel returns ENOSYS (`0x80010003`) for npdrm-app processes.

webman/webMAN MOD's thermal/fan overlay works because it's injected into **vshmain** at runtime by Cobra/HEN's plugin loader. The webman PRX inherits vshmain's process auth (`0x10700005FF000001`, `vsh.self`), which clears the privilege check.

cellmark can't be elevated this way without changing its deployment model. The realistic fix is an LV1 hcall fallback (LV1 sits below LV2's process privilege table, same path used for nclk), but the exact LV1 thermal node paths or SM service hcalls aren't pinned down in the SDK docs.

For now: cellmark's `sysmon.c` gracefully omits the thermal/fan field on DECR. Run webman's overlay alongside cellmark for the live thermal data.

## 6.2 LV2 PPU thread affinity API doesn't exist

There's no Cell OS Lv-2 syscall to bind a PPU thread to TH0 vs TH1 on the PPE's SMT2. The scheduler decides; then observe whichever HW thread the kernel happens to land on. This is part of why the PPE PMU data is bimodal even with the same kernel run repeatedly.

A TH0+TH1 split profile that summed both threads was attempted; it didn't help because the DECR kernel evidently runs cellmark's PPE thread predominantly on TH0 (TH1 events were always zero). The bimodality is from LV2 preemption, not SMT scheduling.

See [§1.3](#13-smt-scaling-th0--th1) for what the SMT bench *can* show about TH0+TH1 dispatch, using lv2's natural scheduling onto both HW threads rather than affinity to expose the dispatch arbitration.

## 6.3 libperf is DECR-only, no retail fallback

`libperf` goes through lv1 hypercalls that retail lv2 refuses (`CELL_PERF_ERROR_LV1_NO_PRIVILEGE`). `libgcm_pm` (RSX PMU) and the SN Tuner sidechannel (`libprof_stub.a`) are similarly restricted to DECR.

This is documented in `Performance_Monitor-Overview_e.htm §2.1` and the DECR capability table: "Performance monitoring functions" is "Supported on DECR-1000/1000A and DECR-1400 development mode" only.

cellmark's PMU integration is therefore DECR-only by design. Retail builds get all the wall-clock-derived numbers but no PMU readouts.

## 6.4 Auth-id elevation isn't a viable path

The full PAID table makes it clear: every privileged auth-id is path-locked to `/dev_flash/...`. The kernel checks both auth-id and install path; an NPDRM SELF in `/dev_hdd0/game/.../EBOOT.BIN` can only legitimately use the npdrm-game auth-id. There's no "elevated DECR auth-id" to sign with that would escape the privilege filter, the alternative auth-ids would simply fail to load.

## 6.5 Raw-SPU unavailable due to 6-SPE constraint

`sys_raw_spu_create` could give direct MMIO access to an SPU (bypassing lv2 mailbox overhead, dropping PPE<->SPE round-trip from 11.6 μs to ~300–800 ns). The full bootstrap path was prototyped.

The blocker is that `sys_spu_initialize(MAX_SPES=6, 1)` requests 6 thread + 1 raw = 7 SPUs but PS3 only exposes 6. Reserving one for raw mode forces every 6-SPE thread group (compute, EIB pairs, DMA aggregate) to drop to 5 SPEs and re-measure. The trade-off wasn't worth disrupting the other benches.

The mailbox bare-metal floor is therefore documented as a *predicted bound* (§3.6); the measured 11.6 μs is the realistic cost any thread-mode PS3 program pays.

---

# Part 7: Future work

## Bandwidth side

- **Single-SPE saturation curve for XDR** - How many in-flight DMAs does a single SPE need to saturate XDR? Likely 4–8 deep given the latency.

## Compute side

- **PPE branch mispredict + sync fence cost** - currently uncovered; meaningful for understanding the PPE in-order pipeline penalties.

- **SPE inter-pipe stall map** - extend the DP FMA pipe-phase finding to cover all stall categories: load-use, channel-read, branch-redirect. The PMU events are there; a dedicated bench can isolate each.

## Workload pipeline (deferred from §5)

Additional real-workload benchmarks to extend the composite score's
coverage. Each is sized to ~200 ms / batch at stock 3.2 GHz so the
EMA rolls cleanly.

- **SGEMM** - canonical Cell demo workload. Dense SP matrix multiply with XDR streaming + double-buffered DMA pipeline. IBM published ~150 GFLOPS SP on full 8-SPU Cell BE; 6-SPU PS3 should land around 100–110 GFLOPS sustained when tuned. Hits SP-FMA + XDR + DMA overlap simultaneously. Reproducible against published Cell papers.

- **Mandelbrot deep zoom with RSX rendering** - real-time visual demo. Each SPE computes a screen tile, RSX rasterises and presents. Mandelbrot iteration uses DP at deep zoom (hits the SPU DP out-of-phase pipe stall from §2.1.2). First cellmark workload to actually use the RSX, this wires up libgcm draw calls, a colormap, and swapchain presentation. Score: pixels/sec at fixed zoom depth, or effective FPS for a fixed render path.

- **AES-128 bit-sliced** - pure crypto, 4 blocks per SPU per cycle via bit-slicing on the 4-wide vector lanes. Score: MB/sec encrypted. Real workload comparable to AES-NI on modern CPUs (none on PS3 of course). Tests bitops + LS sbox table lookups.

- **SHA-256** - bit shift / rotate / add heavy. 6-SPE parallel hashing of independent message streams. Score: MB/sec. Different ALU mix from AES (more ROL/ROR, more deps).

- **Prime sieve (segmented Eratosthenes)** - integer + memory bandwidth heavy. Each SPE sieves a segment of [N, N+block), composite results DMA'd out. Score: primes/sec.

- **Raytracer** - compute-heavy, scene data streamed from XDR. SPE does ray-triangle intersection on tiles of pixels. Score: rays/sec. Exercises DMA + SP FMA + branch-heavy code (visibility culling).

- **FFTC proper port** - port the CellBuzz/FFTC code faithfully to 6 SPUs. Needs sync-topology redesign since FFTC hardcodes 8 SPUs with log2(8)=3-stage radix-2 sync. Targets the paper's headline ~150 GFLOPS aggregate (scaled to ~110 for 6SPUs). Different parallelism model from §5.2 (one large FFT distributed), would complement the existing multi-FFT bench.

- **JPEG2000 / MPEG2 / zlib** - also in the CellBuzz reference suite. Useful for cross-validation against published Cell benchmark numbers from 2007.

- **Wavelet transform** - signal-processing primitive, similar structure to FFT but different memory access pattern. Useful for image-coding workloads.

## DECR-specific

- **LV1 thermal/fan path** - research the exact LV1 hcall + repository node paths (or SM service ID), wire as a DECR fallback when the lv2 syscalls ENOSYS. Lets cellmark show thermals on DECR without webman.

- **libgcm_pm RSX PMU** - wire an RSX-side bench page (vertex throughput, fragment fill, ROP rate, GDDR3/5 BW). Requires actually issuing GCM work from cellmark, so paired with adding a draw-call benchmark.

- **SN Tuner sidechannel** - `libprof_stub.a` lets Tuner.exe attach via DECI3 and drive the PMU remotely. Currently linked but unused; could expose cellmark's bench windows as bookmarks for deeper analysis.

## Scoring & workloads

- **Real workload kernels** - pi calculation (Bailey-Borwein-Plouffe or Chudnovsky on SPEs), prime sieve, AES/SHA on SPEs, FFT. Each exercises a different subset of the silicon and gives publishable application-level numbers anchored to the architectural findings here.

- **Cinebench-style aggregate score** - geometric mean across PPE compute, SPE compute, EIB fabric BW, fabric latency, memory BW. One number per console for cross-OC comparisons.

---

# References

- IBM **Cell Broadband Engine Programming Handbook v1.11** (May 2008) - EIB ring architecture (§1.7), SPU pipeline rules (§3), MFC commands (§7), DFMA latency tables (§3.2)
- IBM **Cell Broadband Engine Architecture v1.01** (Oct 2006) - PMU hardware spec, atomic cache (§8), MFC (§17)
- IBM **SPU ISA Reference Manual v1.2** (Jan 2007) - instruction latency tables, dual-issue rules
- Sony **Performance_Monitor-Overview_e.htm** (Cell SDK 3.0) - libperf semantics, 2-group simultaneous limit, NO_TRACE_MODE counter-only path
- Sony **libperf-Reference_e.htm** - full event list, signal group constants, `CellPerfCBEpmSetup` struct layout
- Sony **Reference_System_DECR1400-Setup_e.htm** §2 capability table - retail-vs-DECR PMU availability matrix
- **psdevwiki - Program Authority ID** - the PAID table confirming auth-id path-lock policy
- **psdevwiki - CELL_BE / Thermal** - thermal sensor architecture, syscon service IDs, syscall 383/409 documentation
- Kistler, Perrone, Petrini - **CELL MULTIPROCESSOR COMMUNICATION NETWORK:BUILT FOR SPEED** (IEEE Micro, 2006) - academic EIB performance reference
- Bader, Agarwal, Petrini, *et al.* - **FFTC: Fastest Fourier Transform for the IBM Cell Broadband Engine** (CellBuzz, 2007) - source for the SIMD butterfly inner-loop techniques used in §5.2. cellmark's FFT workload adopts FFTC's inner-loop SIMD pattern but uses a different parallelism model (6 SPUs x 4 lanes independent FFTs vs FFTC's single FFT distributed across 8 SPUs with hardcoded sync). Reference code lives under `docs/cell_code_examples/CellBuzz-main/fft/`.
- webMAN MOD source (`cpursx.h`, `fancontrol.h`) - reference implementation showing thermal/fan readout patterns from a vsh-injected context