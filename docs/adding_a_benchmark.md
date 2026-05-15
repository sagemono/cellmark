# Adding a benchmark to cellmark

This is the practical guide for adding a new benchmark, what files to reate, what functions to write, how to register your bench with the engine. For a deeper look at the engine itself, see [architecture.md](architecture.md)

## TL;DR

```bash
python tools/new_bench.py mybench --category workload
# edit benches/mybench/bench.c, benches/mybench/spu_main.c
# add to tools/build.py (3 places) and engine/bench_registry.c (2 places)
python tools/build.py retail
# run build/cellmark.self, your bench is on the workload page
```

## What is a benchmark?

A cellmark benchmark is a `bench_module_t` (defined in [`include/bench.h`](../include/bench.h)) plus the implementation behind its callbacks. Each module has:

- An `id` - short snake_case identifier, used for logging and match-with-symbol-name (the SPU ELF symbol is `_binary_spu_<id>_elf_start`).
- A `display_name` - what shows up in the UI.
- A `category` - pages in the UI are grouped by category. Common values: `cell`, `ppe`, `disk`, `dma`, `fabric`, `workload`, `burn`.
- A `kind` - `BENCH_KIND_BATCH` (tick runs to completion each iter) or `BENCH_KIND_CONTINUOUS` (start spawns long-lived work, stop joins).
- Lifecycle callbacks - `init`, `start`, `tick`, `stop`, `render`, `cleanup`, `input`. All optional, NULL means "do nothing".

## Step-by-step

### 1. Scaffold

```bash
python tools/new_bench.py raytracer --category workload
```

Creates `benches/raytracer/` from the `_template/` directory with
files:

- `bench.c` - PPU driver. The `bench_module_t` definition lives at the bottom; the lifecycle callbacks are above it.
- `spu_main.c` - SPU entry point. Receives a params struct, runs a kernel, DMAs results back. Drop with `--no-spu` if PPE-only.
- `README.md` - bench-specific notes.

### 2. Implement the SPU kernel

`benches/raytracer/spu_main.c` template:

```c
int main(uint64_t arg_params_ea, uint64_t arg_result_ea, uint64_t arg3, uint64_t arg4)
{
    /* params in */
    mfc_get(&ls_params, arg_params_ea, sizeof(ls_params), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    uint32_t t0 = spu_readch(SPU_RdDec);

    /* !!! YOUR KERNEL HERE !!! */
    for (uint32_t i = 0; i < ls_params.iterations; i++) {
      // ray-triangle intersection, AES round, FFT butterfly, whatever
    }

    uint32_t t1 = spu_readch(SPU_RdDec);
    ls_result.dec_ticks = t0 - t1;

    mfc_put(&ls_result, arg_result_ea, sizeof(ls_result), TAG_RESULT, 0, 0);
    dma_wait(TAG_RESULT);
    return 0;
}
```

If you have additional SPU kernel files (`.c` or `.S`), drop them alongside `spu_main.c`. Convention: prefix with `spu_` so they're visually grouped.

### 3. Implement the PPU driver

`benches/raytracer/bench.c` does:
- `init` - one-shot setup at app start
- `tick` - spawn the SPE thread group, join, read results, update EMA-smoothed score
- `render` - paint the UI block via `cellDbgFontConsolePrintf`

Look at simple examples:
- [`benches/pi/pi_benchmark.c`](../benches/pi/pi_benchmark.c) - batch benchmark with PPU-side self-test at init.
- [`benches/burn/burn_benchmark.c`](../benches/burn/burn_benchmark.c) - continuous benchmark with start/stop spawning persistent SPE group.

### 4. Register with the build system

Edit [`tools/build.py`](../tools/build.py) in two places:

```python
# Add the SPU bench's source list
SPU_BENCHES = {
    ...
    'raytracer': ['benches/raytracer/spu_main.c',
                  'benches/raytracer/spu_intersect.c'],
}

# Add the PPU sources (bench driver + any .c render block)
PPU_SOURCES = [
    ...
    'benches/raytracer/bench.c',
]
```

If your SPU kernel needs `libm` (for cos/sin/sqrt at init), also add:

```python
SPU_LIBS = {
    ...
    'raytracer': ['-lm'],
}
```

### 5. Register with the engine

Edit [`engine/bench_registry.c`](../engine/bench_registry.c):

```c
extern const bench_module_t bench_raytracer;

const bench_module_t *const cellmark_bench_registry[] = {
    ...,
    &bench_raytracer,
    0,
};
```

The order matters for UI navigation: modules are grouped by category in their declaration order, and categories appear in the order they first show up in this array.

### 6. Build and test

```bash
python tools/build.py retail        # or 'decr' for DECR PMU access
```

Move `build/cellmark.self` (or `cellmark_decr.self`) to your PS3. The new bench appears under its category page. L2/R2 navigates between categories, L1/R1 navigates within a category (if multiple modules share one).

## Common patterns

### Batch benchmark with EMA-smoothed score

(Pi, FFT, N-body all follow this pattern.)

```c
static struct {
    float    score;          /* EMA smoothed */
    float    ms_per_batch;
    uint32_t runs;
} g_summary;

static void mybench_tick(uint64_t tb_freq) {
    /* spawn SPE group, run K iterations, join */
    ...
    if (g_result.dec_ticks > 0) {
        double secs = (double)g_result.dec_ticks / (double)tb_freq;
        float score = (float)(iters / secs / 1.0e6);   /* Miters/s */
        if (g_summary.runs == 0) g_summary.score = score;
        else g_summary.score = 0.7f * g_summary.score + 0.3f * score;
        g_summary.ms_per_batch = (float)(secs * 1.0e3);
        g_summary.runs++;
    }
}
```

### Continuous benchmark with start/stop

(Burn-in follows this pattern.)

```c
static int g_active = 0;

static void mybench_start(void) {
    /* spawn long-lived SPE group, return immediately */
    sys_spu_thread_group_create(&g_group, NUM_SPES, 100, &group_attr);
    /* ... initialise, start_group ... */
    g_active = 1;
}

static void mybench_tick(uint64_t tb_freq) {
    if (!g_active) return;
    /* sample shared progress counters that the SPEs DMA back to
       PPU-side EAs in their own loop */
    ...
}

static void mybench_stop(void) {
    if (!g_active) return;
    /* signal SPEs to exit via shared flag, join */
    g_stop_flag[0] = 1;
    sys_spu_thread_group_join(g_group, &cause, &status);
    sys_spu_thread_group_destroy(g_group);
    g_active = 0;
}
```

### PPU-side self-test at init

For BBP-style algorithms where the PPU can validate the SPU's output against a known-correct reference computed in C, do it in `init`:

```c
static int mybench_init(uint64_t tb_freq) {
    /* compute a small known case on the PPU using the same kernel
       source (compile the SPE kernel for PPU too via build.py) */
    extern uint32_t mybench_compute_reference(uint32_t input);
    uint32_t got = mybench_compute_reference(KNOWN_INPUT);
    if (got != KNOWN_EXPECTED) {
        printf("[mybench] self-test FAIL: got %u expected %u\n",
               got, KNOWN_EXPECTED);
        return -1;   /* disables the bench */
    }
    printf("[mybench] self-test PASS\n");
    return 0;
}
```

Pi BBP does this, see [`benches/pi/pi_benchmark.c`](../benches/pi/pi_benchmark.c). To compile a SPU kernel for the PPU side, add it to `PPU_SOURCES` in `tools/build.py`:

```python
PPU_SOURCES = [
    ...
    'benches/pi/pi_benchmark.c',
    'benches/pi/spu_pi_kernel.c',   # cross-compiled for PPU self-test
]
```

The kernel must use only portable C constructs (no SPU intrinsics).

### Multi-variant bench (uses input callback)

For benches with sub-modes selected by L1/R1 (PPE has 8, EIB has 7), own the variant state in a module-local static and handle button events in `.input`:

```c
static int current_variant = 0;

static void mybench_input(uint16_t pressed1, uint16_t pressed2) {
    int dir = 0;
    if ((pressed2 & 0x04) || (pressed1 & 0x10)) dir = -1;  /* L1 or D-pad <- */
    if ((pressed2 & 0x08) || (pressed1 & 0x40)) dir = +1;  /* R1 or D-pad -> */
    if (dir != 0) current_variant = (current_variant + dir + N) % N;
}

static void mybench_tick(uint64_t tb_freq) {
    /* dispatch on current_variant */
    switch (current_variant) {
        case 0: mybench_run_a(tb_freq); break;
        case 1: mybench_run_b(tb_freq); break;
        ...
    }
}
```

The engine forwards button events to `.input` when the bench is the only module in its category, or for any non-navigation buttons.

## Things to know

### SPU ELF symbol naming

When `build.py` embeds the SPU ELF as a PPU object, the symbol prefix is derived from the SPU bench's key in the `SPU_BENCHES` dict. The prefix is `_binary_spu_<key>_elf_start`. Your PPU `bench.c` references this symbol as an `extern const char[]`.

### Shared SPU code

If multiple benches need the same SPU helper (e.g., a DMA double buffering kernel), put it in [`engine/spu_shared/`](../engine/spu_shared/) and reference it from each bench's `SPU_BENCHES` source list. Existing examples:

- `engine/spu_shared/spu_dma_kernel.c` - used by `dma` and `eib`
- `engine/spu_shared/spu_dual_kernel.S` - used by `stress` (cell) and `burn`

### Resources owned by main.c

`ppu/main.c` allocates one large XDR working buffer (the "memtest region", up to 384 MB) and hands it to the engine. Benches that need a big shared XDR buffer (currently only `burn`) fetch it via:

```c
extern void *cellmark_engine_shared_xdr_buffer(uint32_t *size_out);
```

This avoids redundant memalign of multi-MB buffers in tight DECR heap.

### What the engine does NOT do for you

- It does not manage your SPU thread groups. Each bench creates, starts, joins, and destroys its own groups.
- It does not allocate result buffers. Each bench owns its own PPU-side state.
- It does not enforce timing or thread safety. If your `tick()` function spawns SPEs while another bench's group is running, cellmark will EINVAL on group create, so make sure your bench's `start()` claims the chip cleanly.
- It does not handle PMU profile rotation. If your bench wants to use libperf on DECR, look at how `benches/ppe/ppe_benchmarks.c` rotates through three 4-event profiles.

## Calibrating a score baseline

cellmark's composite workload score is normalised against stock 3.2 GHz PS3 measurements. If you add a workload-category bench:

1. Build the bench, run it on stock hardware
2. Measure the score after EMA stabilises (~10 batches)
3. Bake that value into your bench's score normalisation (look at `benches/workload/render_workload.c` for `PI_REF_DIGITS_PER_SEC` / `FFT_REF_POINTS_PER_SEC` / `NBODY_REF_MPAIRS_PER_SEC` as examples)

This way score = 100 on stock, OC'd consoles read above, etc.