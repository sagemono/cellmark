// dma_benchmark.h - aggregate xdr <-> ls dma bandwidth benchmark

#ifndef DMA_BENCHMARK_H
#define DMA_BENCHMARK_H

#include <stdint.h>

#define DMA_DIR_GET     0   /* XDR -> LS */
#define DMA_DIR_PUT     1   /* LS -> XDR */

#define DMA_ST_IDLE     0
#define DMA_ST_RUNNING  1
#define DMA_ST_DONE     2
#define DMA_ST_ERROR    3

typedef struct {
    uint64_t ea_results;        /* where SPE writes its results */
    uint64_t ea_test_region;    /* base of this SPEs XDR slice */
    uint32_t window_size;       /* bytes. MUST be a power of 2, >= 32 KB */
    uint32_t iterations;        /* number of 16 KB chunks per batch */
    uint32_t direction;         /* DMA_DIR_GET / DMA_DIR_PUT */
    uint32_t spe_index;         /* 0..5 */
    uint32_t pad[2];
} __attribute__((aligned(128))) dma_params_t;

typedef struct {
    uint64_t total_bytes;       /* bytes transferred this batch */
    uint32_t status;            /* DMA_ST_* */
    uint32_t pad[29];           /* round to 128 bytes */
} __attribute__((aligned(128))) dma_results_t;

typedef struct {
    float    get_mbps;
    float    put_mbps;
    uint32_t get_runs;
    uint32_t put_runs;
} dma_results_summary_t;

void dma_benchmark_init(uint64_t timebase_freq);

float dma_benchmark_run(int direction);

void dma_run_batch(int bench_id, uint64_t tb_freq);

const dma_results_summary_t *dma_get_results(void);

#endif /* DMA_BENCHMARK_H */