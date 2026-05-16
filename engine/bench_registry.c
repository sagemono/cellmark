#include "bench.h"

extern const bench_module_t bench_cell;
extern const bench_module_t bench_xdr;
extern const bench_module_t bench_ppe;
extern const bench_module_t bench_disk;
extern const bench_module_t bench_dma;
extern const bench_module_t bench_fabric;
extern const bench_module_t bench_workload;
extern const bench_module_t bench_burn;
extern const bench_module_t bench_mandelbrot;

const bench_module_t *const cellmark_bench_registry[] = {
    &bench_cell,
    &bench_xdr,
    &bench_ppe,
    &bench_disk,
    &bench_dma,
    &bench_fabric,
    &bench_workload,
    &bench_burn,
    &bench_mandelbrot,
    0,
};