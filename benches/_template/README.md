# Template benchmark

Copy this directory to start a new benchmark:

```
python tools/new_bench.py mybench --category workload
```

That copies `_template/` to `benches/mybench/` and renames identifiers. See [`docs/ADDING_A_BENCHMARK.md`](../../docs ADDING_A_BENCHMARK.md) for the full guide.

## What's here

- `bench.c` - PPU driver. Holds the `bench_module_t` definition that hooks the bench into the engine, plus the lifecycle callbacks (init / tick / render).
- `spu_main.c` - SPU entry point. Receives a params struct, runs a kernel, DMAs results back to PPU. Drop this if the bench is PPU-only.

## What you customise

- The compute kernel itself (in `spu_main.c`, or PPU-side in `bench.c`)
- The params + results structs (cellmark currently keeps them in `bench.c`'s top section)
- The `display_name` and `category` in the `bench_module_t`
- The render block, write whatever your bench wants to show to the UI

## What the engine handles for you

- Spawning + joining the SPU thread group is up to your code; the engine does NOT do that for you. (See `benches/pi/pi_benchmark.c` for a clean batch example, or `benches/burn/burn_benchmark.c` for a continuous-kind example with start/stop lifecycle.)
- Category-level page navigation (L2/R2)
- Input forwarding to `.input` when present
- EMA smoothing of your throughput: do this in `tick` and store smoothed values in your summary struct that `render` reads