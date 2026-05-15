# cellmark architecture

This document covers how cellmark is organised internally. If you want to add a new benchmark, read [adding_a_benchmark.md](adding_a_benchmark.md) first, it covers the bench-author workflow without all the engine detail. 

This document is for understanding the engine itself.

## 1. Source tree

```
cellmark/
├── ppu/
│   └── main.c                  the entry point. boots the SDK,
│                               allocates the memtest region, calls
│                               cellmark_engine_init, runs the main
│                               loop. Knows nothing about benches.
│
├── engine/                     the bench-agnostic framework
│   ├── cellmark_engine.{c,h}   bench dispatch, navigation state
│   ├── bench_modules.c         Phase-2 staging: all bench_module_t
│   │                           definitions in one place. Phase 5 will
│   │                           distribute these into benches/*/bench.c
│   ├── bench_registry.c        NULL-terminated array of bench_module_t
│   │                           pointers, ordered by category
│   ├── cellmark.h              shared global state (current_*, log_enabled,
│   │                           tb_frequency)
│   ├── render.{c,h}            write_log() + render_*_stats() per page
│   ├── gcm.{c,h}               GCM display init / frame flip
│   ├── spu.{c,h}               SPU thread group lifecycle (start_spu_stress,
│   │                           stop_spu_stress) - used by bench_cell, bench_xdr
│   ├── pmu.{c,h}               libperf wrapper (DECR-only PMU access)
│   ├── cell_pmu.{c,h}          PMU sample-window state machine for the
│   │                           cell stress group (continuous benches)
│   ├── sysmon.{c,h}            thermal/fan/IP/etc. status string
│   └── spu_shared/             SPU code used by multiple benches
│       ├── spu_dma_kernel.c    used by bench_dma + bench_eib
│       └── spu_dual_kernel.S   used by bench_cell + bench_burn
│
├── benches/                    one directory per benchmark
│   ├── _template/              starter skeleton (tools/new_bench.py
│   │                           copies this when scaffolding a new bench)
│   ├── cell/                   compute stress (6 internal modes)
│   ├── xdr/                    (logically grouped here, but the SPU side
│   │                           lives under benches/cell/spu/spu_memtest.c)
│   ├── ppe/                    PPE core benches (VMX, L1/L2, FPU, FXU, SMT)
│   ├── disk/                   storage I/O (HDD/SSD)
│   ├── dma/                    XDR<->LS DMA
│   ├── eib/                    EIB ring bandwidth + atomic + mailbox + branch
│   ├── atomic/                 atomic cache ping-pong
│   ├── mbox/                   mailbox round-trip
│   ├── branch/                 SPE hbrr effectiveness
│   ├── pi/                     Pi BBP digit extraction
│   ├── fft/                    1D radix-2 complex SP FFT, SIMD-batched
│   ├── nbody/                  all-pairs gravitational N-body
│   ├── workload/               composite scoring view (pi + fft + nbody)
│   └── burn/                   all-units saturation burn-in
│
├── include/
│   ├── bench.h                 bench_module_t interface
│   └── stress_common.h         shared types between PPU and SPU
│
├── docs/                       this directory
├── tools/
│   ├── build.py                build driver (Python; replaces build.bat)
│   └── new_bench.py            scaffold a new bench from _template
└── build/                      ALL build outputs
    ├── cellmark.elf
    ├── cellmark.self
    ├── cellmark_decr.elf
    ├── cellmark_decr.self
    ├── obj/<bench>.ppu.o       SPU ELFs embedded as PPU binary objects
    └── spu_elfs/<bench>.elf    SPU ELFs (pre-embed)
```

## 2. The engine

[engine/cellmark_engine.c](../engine/cellmark_engine.c) is ~200 lines of bench-agnostic dispatch. The only thing it knows about is [`include/bench.h`](../include/bench.h)'s `bench_module_t` interface.

### State

```
g_categories[N]   array of (category name, first index in registry, count)
g_num_categories  number of distinct categories
g_current_cat     index into g_categories
g_current_in_cat  index within g_categories[g_current_cat]
```

### Init flow

`cellmark_engine_init(tb_freq, shared_xdr_buffer, shared_xdr_size)`:

1. Stashes the timebase + the optional XDR buffer.
2. Walks the registry, builds the category index.
3. Calls `init(tb_freq)` on every module (skipping NULLs).
4. Calls `start()` on the currently-selected module (index 0,0).

### Main loop integration

`ppu/main.c` calls four engine entry points each frame:

```c
cellmark_engine_handle_input(pressed1, pressed2);  /* pad event */
...
cellmark_engine_tick(tb_freq);                      /* per-frame work */
...
cellmark_engine_render(get_elapsed_seconds());      /* UI paint */
```

And one at shutdown:

```c
cellmark_engine_shutdown();
```

### Navigation

```
L2 / R2  -> previous / next category (page)
L1 / R1  -> previous / next module within category, OR forwarded
            to the current bench's input() callback if only one module
            is in this category
D-pad <- / ->  -> same as L1 / R1 (alternate binding)
TRI      -> NOT forwarded to engine; main.c handles log toggle directly
SEL+START  -> NOT forwarded; main.c handles exit directly
```

On category switch the engine calls `stop()` on the outgoing module then `start()` on the incoming one, that's how continuous benches (cell stress, burn-in) cleanly release their SPU thread groups before the next bench claims the chip.

## 3. bench_module_t lifecycle

```
                  ┌─── app start ───┐
                  │                 │
                  │   init(tb_freq) │   one-shot setup,
                  │                 │   return 0 to enable bench
                  │                 │
            ┌─────┤   start()       │   bench just became selected;
            │     │                 │   spawn long-lived threads /
            │     │                 │   allocate per-run resources
            │     │                 │
            │     │   tick(tb_freq) │   called every main loop iter
   user     │     │   tick(tb_freq) │   - BATCH: do measurement, return
   stays    │     │   ...           │   - CONTINUOUS: sample background
   here ────┤     │                 │
            │     │   render(t)     │   paint UI on every frame
            │     │   render(t)     │
            │     │   ...           │
            │     │                 │
            │     │   input(b1,b2)  │   pressed-this-frame buttons
            │     │                 │   that the engine didn't consume
            │     │                 │
   user     │     │   stop()        │   bench is being deselected;
   leaves ──┘     │                 │   join threads, free per-run
                  │                 │
                  │   cleanup()     │   app exit; free init resources
                  │                 │
                  └─────────────────┘
```

All callbacks are optional (NULL = noop). The engine never violates the order shown, `tick` is never called without a preceding `start`, `stop` is always matched to a `start`, `cleanup` is at-most-once.

## 4. Build system

[`tools/build.py`](../tools/build.py) replaces the old `build.bat`. It:

1. Compiles each SPU bench (`SPU_BENCHES` dict) into `build/spu_elfs/<name>.elf`
2. `objcopy`s each SPU ELF into a PPU binary object, exposing `_binary_spu_<name>_elf_start/end/size` symbols. Lands in `build/obj/spu_<name>.ppu.o`.
3. Compiles + links all PPU sources (`PPU_SOURCES`) plus the embedded SPU objects + dbgfont + sysmodule stubs into `build/cellmark.elf` (or `cellmark_decr.elf`).
4. Runs `make_fself` to produce `build/cellmark.self` (or `cellmark_decr.self`). Both variants are fake-signed; retail CFW and DECR/ProDG accept this.

Variant differences:
- `retail`: no `-DCELLMARK_DECR=1`, no libperf link.
- `decr`: defines `CELLMARK_DECR=1`, links `-lperf`. PMU is available on this variant only.

Per-bench SPU compile flags:
- All benches use `-O3 -funroll-loops` by default
- Exception: `stress` uses `-O2` (preserved from the original build.bat; changing it would alter compute mode numbers)
- Library overrides: `fft` links `-lm` for cos/sin in the twiddle table

The SPU compile gets these include paths:
```
-I <SPU_SDK>/include
-I include
-I engine
-I engine/spu_shared
-I <each source's parent directory>
```

The PPU compile gets:
```
-I <PPU_SDK>/include
-I include
-I ppu
-I engine
-I benches/<each-bench>
```

## 5. Things still to do

- Phase 5: `build.py` could auto-discover benches from `benches/*/` and auto-generate `bench_registry.c` by scanning for `bench_*` symbols. The hand-edited registry would go away.
- Splitting `bench_modules.c` into per-bench `benches/<name>/bench.c`. Each module's definition co-located with its kernel.
- Splitting multi-variant benches (PPE has 8 sub-benches, EIB has 7) into one `bench_module_t` per variant. Currently they're page-level modules with internal variant cycling via `input()`.