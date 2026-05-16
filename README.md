# cellmark

A stress test and benchmark suite for the PlayStation 3.

cellmark started as a way to verify Cell BE / XDR overclock stability against memtest86+ style patterns and a deterministic FMA accumulator. It has since grown into a fuller hardware characterisation suite covering the PPE core, all 6 SPEs, the EIB ring fabric, the XDR memory subsystem, and the disk path. 

Both retail CFW and DECR-1000A development-kit build variants are supported. The DECR build links `libperf` for direct CBE Performance Monitor access on dev hardware.

![A preview of cellmark](assets/cellmark.gif)

---

## What's in this repo

```
ppu/main.c                entry point

engine/                   bench-agnostic framework + shared services
  cellmark_engine.{c,h}   bench module dispatch
  bench_modules.c         all bench_module_t entries
  bench_registry.c        the canonical NULL-terminated module array
  spu.{c,h}, gcm.{c,h}    SPU thread group lifecycle, GCM display
  pmu.{c,h}, cell_pmu.{c,h}  libperf wrapper + cell-side sampling
  sysmon.{c,h}, render.{c,h} status string + render helpers
  spu_shared/             SPU code used by multiple benches

benches/<name>/           one self-contained directory per benchmark
  _template/              copy-source for new benches (see adding_a_benchmark.md)
  cell/    ppe/    disk/    dma/    eib/    atomic/    mbox/    branch/
  pi/      fft/    nbody/   workload/   burn/   mandelbrot/

include/                  public headers
  bench.h                 bench_module_t interface
  stress_common.h         PPU/SPE shared types

tools/
  build.py                build driver (replaces build.bat)
  new_bench.py            scaffold a new bench from _template/

docs/
  cell_tuning.md          optimization bible + measurement bible
  architecture.md         how cellmark is structured internally
  adding_a_benchmark.md   guide for extending cellmark
  ppe_tuning.md, spu_tuning.md, disk_tuning.md  subsystem deep dives

build/                    all build outputs land here (gitignored)
```

For a deeper look at the engine internals see [docs/architecture.md](docs/architecture.md);
to add your own benchmark, [docs/adding_a_benchmark.md](docs/adding_a_benchmark.md).

---

## What cellmark actually does (full app)

Nine pages, switched with **L2/R2** on a controller. L1/R1 cycle the
variant within a page.

### 1. Cell - SPU compute (all 6 SPEs in parallel)

| Mode          | Pipeline           | Peak (3.2 GHz) | Purpose                            |
|---------------|--------------------|----------------|------------------------------------|
| SP FMA Stress | Even (SP float)    | 153.6 GFLOPS   | OC stability, deterministic verify |
| DP FMA Stress | Even (DP float)    | 13.1 GFLOPS    | DP throughput (7-cycle stall) |
| Int Multiply  | Even (`mpya`)      | 153.6 GIOPS    | Integer pipeline |
| Recip/Rsqrt   | Even (estimate+NR) | 76.8 GOPS      | Newton-Raphson refinement |
| Shuffle Storm | Odd (`shufb`)      | 76.8 GOPS      | Odd pipeline |
| Dual-Issue    | Even+Odd           | 230.4 GOPS     | Theoretical max |

SP FMA stress verifies a deterministic accumulator chain at intervals; if a single bit drifts, the SPE reports an error and the run is flagged unstable (unlikely for this to happen). Other modes track throughput only.

### 2. PPE - PowerPC core benchmarks (PPE0 thread, SPUs idle)

- **VMX FMA** - `vmaddfp` throughput, 6 chains x 8x unrolled (~96% of 8 GFLOPS/GHz peak)
- **L1 / L2 read bandwidth** - sequential VMX loads, load-ahead pattern hides 7-cycle latency
- **L1 / L2 latency** - Pointer chase using Sattolo's algorithm, true load-to-use ns

### 3. XDR - memtest86+ style memory test (15 patterns)

15 sub-tests cycle automatically across **all 6 SPEs in parallel**, partitioning XDR into per-SPE slices. Allocates as much XDR as the OS will give.

Sequential, Walking 1/0, Checkerboard, Random, Moving Inversions, Bank Hammer, R/W Turnaround, Bandwidth, Own Address, Modulo-N, Block Move, Bit Fade, BW Pipelined, Coherency, Latency.

### 4. Disk I/O - HDD/SSD/SSHD characterisation

Sequential 64KB and random 4K read/write on a 32 MB test file (writes are post-`fsync` these are true media latency, not write-cache). A 13-probe diagnostic suite separates per-call LV2 overhead from FAT cluster fragmentation, exercises `cellFsSetIoBuffer` and `cellFsStRead`, and finishes with three contention probes that simulate a real game's I/O mix (audio + texture streaming + sequential level load). See [docs/disk_tuning.md](docs/disk_tuning.md) for what those probes actually told us.

### 5. Cell DMA BW - aggregate XDR <-> LS bandwidth

All 6 SPEs in parallel pulling (GET) or pushing (PUT) 16 KB MFC chunks against XDR through a double-buffered pipeline. Each SPE owns a 1 MB XDR slice so they don't fight for the same cache lines. Headline numbers on stock 3.2 GHz: **GET ~22.8 GB/s, PUT ~22.1 GB/s**, both within ~13% of the 25.6 GB/s XDR1 DRAM ceiling. Stresses the EIB-to-MIC path, the bottleneck is DRAM, not the EIB or the MFC.

### 6. Fabric - EIB / atomic / mailbox / branch hint

Cell's interconnect characterised six ways:

- **Pairs**: 3 disjoint SPE<->SPE LS-to-LS flows via the SDK fixed peer-LS EA mapping. Aggregate **~115 GB/s** across 6 SPEs (74% of per-direction ring ceiling).
- **Hotspot (5→1)**: many-to-one gather into one SPE's LS, **~23.7 GB/s** 93% of the 25.6 GB/s LS port ceiling).
- **Hotspot sweep** (N=1..5): proves the destination LS port is the bottleneck *immediately* at N=1; adding readers splits the same pool.
- **NxN matrix** (30 pairs): resolves the EIB physical topology - proves logical SPE IDs 0–5 sit on the ring in numerical order.
- **Atomic cache** (`getllar`/`putllc` ping-pong): **150 ns/bounce** between two SPEs, the locking fabric latency floor.
- **Mailbox** (PPE<->SPE with lv2 event queue): **11.6 μs round-trip** - ~75x slower than atomic cache, dominated by lv2 syscall overhead.
- **SPE branch hint** (`hbrr`): 17 vs 30 cyc/iter on a tight 12-cyc loop body; 1.71x speedup for correctly hinted backward branches.

All of these characterise different parts of the same EIB fabric and were published first on retail/DECR PS3 silicon as part of cellmark. See [docs/cell_tuning.md](docs/cell_tuning.md) §3 for the full architectural analysis.

### 7. Workload - real-workload benchmarks with scoring

Cinebench-style: each workload gets a normalised score (stock 3.2 GHz =
100), composite = geometric mean across all active workloads.

- **Pi BBP** - Bailey-Borwein-Plouffe hex digit extraction at position N=10000. Realistic "DP scalar number theory" workload, deliberately showcases Cell's *weakness* (1.2% of DP peak; SPU is wrong-shaped for branchy DP code). 40 digits/sec at stock.
- **FFT** - 1D radix-2 complex SP FFT, N=1024, SIMD batched across 4 vector lanes (CellBuzz/FFTC inner-loop techniques). 185 Mpoints/sec at stock; 4.3x over the scalar baseline, ~6% of SP-FMA peak.
- **N-body** - all-pairs gravitational, N=4080, 50 iterations/batch, hardware rsqrte. SPU's *strength* workload: 2195 Mpairs/sec at stock = **27% of SP-FMA peak** (~42 GFLOPS aggregate).

### 8. Burn-In - all-units saturation

Continuously saturates every Cell subsystem at once: **4 SPEs running dual-issue max-heat compute** (saturates even+odd pipes every cycle), **2 SPEs streaming XDR DMA**, **PPE TH0 VMX FMA** inline between display frames. Reports live SPE compute GFLOPS, XDR GB/s, PPE VMX GFLOPS, and a composite saturation score. Leave it running for a few hours to find thermal issues. The "how much of Cell are you actually using" view.

### 9. Render - SPE Mandelbrot with RSX display

The first cellmark bench that puts pixels on screen instead of just text. All 6 SPEs compute a 1920x1080 (or fallback) ARGB Mandelbrot frame straight into XDR; RSX's **NV3089 2D image scaler** blits MAIN->LOCAL onto the active scanout buffer over FlexIO. Two-pack interleaved SIMD inner loop (8 pixels per outer iter; the Michael Kohn naken_asm trick applied through GCC intrinsics) gets the iteration cost to **~7.75 instructions per 4-pack iter**, close to the SPU dual-issue ceiling. **Cardioid + period-2 bulb** analytic test pre-traps lanes guaranteed in-set, skipping the iteration loop entirely for the bulk of the heart-shape interior. View-change detection caches the last computed frame so static viewing drops the SPE batch and runs at vsync 60 fps with the cached buffer.

Interactive: left stick pans, right stick Y zooms, X/O step zoom, d-pad LEFT/RIGHT cycles palett (rainbow, fire, ocean, grayscale, electric), d-pad UP/DOWN rotates hue, Square cycles max_iter 64 4096, L1 resets the view.

If the 9 MB 1080p buffer won't allocate (tight retail heap), the bench walks a resolution fallback ladder and uses RSX bilinear (NV3089 FOH) to upscale whatever fits onto the full screen, so you still get fullscreen output. Exercises subsystems no other bench touches: the RSX 2D transfer engine, IO MMU mapping (`cellGcmMapMainMemory`), and the FlexIO main-mem -> VRAM path.

---

## Running it

The compiled package is on the [Releases](../../releases) page.

1. Transfer `cellmark.pkg` to the PS3 (FTP, USB, `/dev_hdd0/packages/`, etc.)
2. Install via Package Manager (`★ Install Package Files`)

**Controls:**

| Button                | Action                                  |
|-----------------------|-----------------------------------------|
| **L2 / R2**           | Previous / next page (category)         |
| **L1 / R1 / D-pad**   | Previous / next benchmark within page   |
| **X (Cross)**         | Run selected disk bench / probe suite   |
| **Square**            | Toggle disk bench / probe view          |
| **Triangle**          | Toggle file logging                     |
| **SELECT + START**    | Exit                                    |

On the Render page the Mandelbrot bench takes over the analog sticks and d-pad for pan/zoom/palette/hue. The global L2/R2 page navigation still works to leave.

File logging appends to `/dev_hdd0/game/CELLMARK0/USRDIR/cellmark.log` on every mode change, every memtest pass, and on exit. Useful for long stability runs where you want a paper trail of what passed before something hung.

## Building from source

```bash
python tools/build.py retail      # build/cellmark.elf + cellmark.self (CFW)
python tools/build.py decr        # build/cellmark_decr.elf + .self (ProDG/Target Manager)
python tools/build.py clean       # remove build/
python tools/build.py spu_fft     # rebuild just one SPU bench (faster iter)

python make_pkg.py --variant retail   # build/cellmark.pkg (NPDRM-signed)
python make_pkg.py --variant decr     # build/cellmark_decr.pkg
```

Requires the Cell SDK 3.0 toolchain at `C:\usr\local\cell` (or override
via `CELL_SDK` env var). Python 3.7+ for the build driver.

### Adding a benchmark

```bash
python tools/new_bench.py raytracer --category workload
# scaffolds benches/raytracer/ from _template/, prints registration steps
```

See [docs/adding_a_benchmark.md](docs/adding_a_benchmark.md) for the
full guide.

---

## Compatibility

| | |
|---|---|
| **Console**   | PS3 with CFW/HEN (retail or DEX) |
| **Firmware**  | Tested on Evilnat 4.92. Should work on any modern CFW. |
| **Best on**   | All. Though if you are chasing OC numbers, pre CECH-2000 systems are the best, but there are exceptions even among 2000 series (same motherboard revision, diferent part number or manufacturer) |

Different Cell process nodes (90 nm / 65 nm / 45 nm) hit different stability ceilings, and most non-trivial overclocks require overvolting. Use at your own risk! See [cell-xdr-overclocking](https://github.com/sagemono/cell-xdr-overclocking) for the hardware side.

---

## Why this exists

This was built alongside [cell-xdr-overclocking](https://github.com/sagemono/cell-xdr-overclocking) as a way to verify CELL/XDR overclock stability and measure how much performance an OC actually unlocks. The compute kernels tell you whether the SPEs are doing math correctly at the new clock; the memtest catches XDR errors that only appear under sustained load; the disk page exists because the stock PS3 disk benchmarks were terrible and an SSHD upgrade turned out to be bottlenecked entirely on the OS, not the drive.

If you find a configuration that fails here that worked in something else, open an issue! That's exactly the data this was built to surface.

---

## Credits and sources

- **Catherine H. Crawford, Paul Henning, Michael Kistler, Cornell Wright** - [Accelerating Computing with the Cell Broadband Engine Processor](https://dl.acm.org/doi/10.1145/1366230.1366234)
- **Michael Kistler, Michael Perrone, Fabrizio Petrini** - [CELL MULTIPROCESSOR COMMUNICATION NETWORK: BUILT FOR SPEED](https://users.cecs.anu.edu.au/~Alistair.Rendell/hons09/ieeemicro-cell.pdf)
- **David A. Bader, Virat Agarwal, Seunghwa Kang** - [Computing discrete transforms on the Cell Broadband Engine](https://davidbader.net/publication/2009-bak/2009-bak.pdf)
- **Farshad Khunjush, Nikitas J. Dimopoulos** - [Extended Characterization of DMA Transfers on the Cell BE Processor](https://ieeexplore.ieee.org/document/4536190)
- **David A. Bader, Virat Agarwal** - [FFTC: Fastest Fourier Transform for the IBM Cell Broadband Engine](https://davidbader.net/publication/2007-ba/2007-ba.pdf) and [source code](https://github.com/Bader-Research/CellBuzz/tree/main/fft)
- **David A. Bader , Virat Agarwal, Kamesh Madduri, Seunghwa Kang** - [High performance combinatorial algorithm design on the Cell Broadband Engine processor](https://davidbader.net/publication/2007-bamk/2007-bamk.pdf)
- **David A. Bader, Sulabh Patel** - [High Performance MPEG-2 Software Decoder on the Cell Broadband Engine](https://ieeexplore.ieee.org/document/4536234) and [source code](https://github.com/Bader-Research/CellBuzz/tree/main/mpeg2)
- **Olaf Lubeck, Michael Lang, Ram Srinivasan, Greg Johnson** - [Implementation and performance modeling of deterministic particle transport (Sweep3D) on the IBM Cell/B.E.](https://dl.acm.org/doi/abs/10.1155/2009/784153)
- **Jakub Kurzak, Jack Dongarra** - [Implementation of the Mixed-Precision High Performance LINPACK Benchmark on the CELL Processor](https://www.netlib.org/lapack/lawnspdf/lawn177.pdf)
- **Arnd Bergmann** - [Linux on Cell Broadband Engine status update](https://www.kernel.org/doc/ols/2007/ols2007v1-pages-21-28.pdf)
- **Daniel A. Brokenshire** - [Maximizing the power of the Cell Broadband Engine processor: 25 tips to optimal application performance](https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/25tips.pdf)
- **David A. Bader, Seunghwa Kang** - [Optimizing JPEG2000 Still Image Encoding on the Cell Broadband Engine](https://ieeexplore.ieee.org/document/4625836) and [source code](https://github.com/Bader-Research/CellBuzz/tree/main/jpeg2000)
- **Daniel Jiménez-González Xavier Martorell, Alex Ramírez** - [Performance Analysis of Cell Broadband Engine for High Memory Bandwidth Applications](https://ieeexplore.ieee.org/document/4211037)
- **Luke Cico, Robert Cooper, Jon Greene, Michael Pepe** - [Performance Benchmarks and Programmability of the IBM/Sony/Toshiba Cell Broadband Engine Processor](https://archive.ll.mit.edu/HPEC/agendas/proc06/Day3/05_Cico_Pres.pdf)
- **Jacob Johnson** - [POWER EFFICIENCY AND SCALING OF THE CELL BROADBAND ENGINE](https://etda.libraries.psu.edu/files/final_submissions/2582)
- **Hauser, Jochem H., Cambier Jean-Luc, Surampudi Surya, Gollnick Torsten** - [Programming the IBM Cell Broadband Engine a general parallelization strategy](https://apps.dtic.mil/sti/pdfs/ADA525908.pdf)
- **Filip Blagojevic, Alexandros Stamatakis, Christos D. Antonopoulos, Dimitrios S. Nikolopoulos** - [RAxML-Cell: Parallel Phylogenetic Tree Inference on the Cell Broadband Engine](https://ieeexplore.ieee.org/document/4227995)
- **Pieter Bellens, Josep M. Perez, Felipe Cabarcas, Alex Ramirez, Rosa M. Badia, Jesus Labarta** - [CellSs: Scheduling techniques to better exploit memory hierarchy](https://dl.acm.org/doi/abs/10.1155/2009/561672)
- **Michael Gschwind, Fred G. Gustavson, Jan Prins** - [High Performance Computing with the Cell Broadband Engine](https://dl.acm.org/doi/10.1155/2009/979236)
- **Michael Kistler, John Gunnels, Daniel Brokenshire, Brad Benton** - [Programming the Linpack benchmark for the IBM PowerXCell 8i processor](https://dl.acm.org/doi/10.1155/2009/401691)
- **B.C. Vishwas, Abhishek Gadia and Mainak Chaudhuri** - [Implementing a parallel matrix factorization library on the cell broadband engine](https://dl.acm.org/doi/abs/10.1155/2009/710321)
- **Michael Gschwind** - [The Cell Broadband Engine: Exploiting Multiple Levels of Parallelism in a Chip Multiprocessor](https://courses.grainger.illinois.edu/ece511/fa2008/papers/cell.pdf)
- **Samuel Williams, John Shalf, Leonid Oliker, Shoaib Kamil, Parry Husbands, Katherine Yelick** - [The potential of the cell processor for scientific computing](https://dl.acm.org/doi/10.1145/1128022.1128027)
- **Sándor Héman, Niels Nes, Marcin Zukowski, Peter Boncz** - V[ectorized Data Processing on the Cell Broadband Engine](https://ir.cwi.nl/pub/12426/12426B.pdf)
- **Nascar1243, villahed94, gypsy, RGBeter, NGX, DoublesAdvocate** - Testing, feedback and moral support.
- **Sony / IBM / Toshiba** - Cell Broadband Engine Programming Handbook (v1.1, April 2007) and the SPU instruction timing reference; the PPE VMX latencies in `ppe_benchmarks.c` come from there.
- **memtest86+** - The test methodology used in the `spu_memtest.c` follows the standard pattern set: Walking 1/0, Checkerboard, Moving Inversions, Bank Hammer, Bit Fade, etc. The patterns themselves are decades-old academic memory-testing techniques; this is an independent SPU/DMA implementation of them, but credit where it's due.
- **The PS3 developer community** - `ps3py`, `scetool`, and decades of patient reverse engineering.

--- 

## License

MIT License

---

## Author

[sagemono](https://github.com/sagemono) - design, implementation, and the days lost to figuring out why `cellFsRead` was returning 9 MB/s.