#ifndef DMA_BENCHMARK_H
#define DMA_BENCHMARK_H

#include <stdint.h>

#include "pmu.h"

#define DMA_DIR_GET     0
#define DMA_DIR_PUT     1

#define DMA_ST_IDLE     0
#define DMA_ST_RUNNING  1
#define DMA_ST_DONE     2
#define DMA_ST_ERROR    3

typedef struct {
    uint64_t ea_results;
    uint64_t ea_test_region;
    uint32_t window_size;
    uint32_t iterations;
    uint32_t direction;
    uint32_t spe_index;
    uint32_t queue_depth;
    uint32_t chunk_size;  
} __attribute__((aligned(128))) dma_params_t;

typedef struct {
    uint64_t total_bytes;
    uint32_t status;
    uint32_t pad[29];
} __attribute__((aligned(128))) dma_results_t;

typedef struct {
    float    get_mbps;
    float    put_mbps;
    uint32_t get_runs;
    uint32_t put_runs;
} dma_results_summary_t;

#define DMA_SWEEP_COUNT             10
extern const uint32_t                dma_sweep_chunk_sizes[DMA_SWEEP_COUNT];

typedef struct {
    float    mbps[DMA_SWEEP_COUNT];
    uint32_t runs[DMA_SWEEP_COUNT];
    int      active_idx;
} dma_sweep_summary_t;

const dma_sweep_summary_t *dma_get_sweep_summary(void);

void dma_benchmark_init(uint64_t timebase_freq);

float dma_benchmark_run(int direction, uint32_t chunk_size, uint32_t iterations);

void dma_run_batch(int bench_id, uint64_t tb_freq);

const dma_results_summary_t *dma_get_results(void);

void                 dma_get_pmu_summary(pmu_summary_t *out);
const pmu_profile_t *dma_get_pmu_profile(void);

#endif /* DMA_BENCHMARK_H */