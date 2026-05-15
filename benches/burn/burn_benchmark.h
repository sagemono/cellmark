#ifndef BURN_BENCHMARK_H
#define BURN_BENCHMARK_H

#include <stdint.h>

#define BURN_NUM_SPES           6u
#define BURN_NUM_COMPUTE_SPES   4u
#define BURN_NUM_DMA_SPES       2u
#define BURN_ROLE_COMPUTE       0u
#define BURN_ROLE_DMA           1u
#define BURN_DMA_BUFFER_BYTES   (16u * 1024u * 1024u)
#define BURN_DMA_CHUNK_BYTES    (16u * 1024u)

typedef struct {
    uint32_t spe_index;
    uint32_t role;                 
    uint32_t pad0;
    uint32_t pad1;
    uint64_t ea_stop_flag;
    uint64_t ea_progress;
    uint64_t ea_dma_buffer;
    uint32_t pad[22];
} __attribute__((aligned(128))) burn_params_t;

typedef struct {
    uint64_t counter;
    uint64_t pad[15];
} __attribute__((aligned(128))) burn_progress_t;

typedef struct {
    float    spe_compute_gflops;
    float    spe_dma_gbps;
    float    ppe_vmx_gflops;
    float    spe_compute_util;
    float    spe_dma_util;
    float    ppe_vmx_util;
    float    saturation_score;
    double   elapsed_sec;
    uint32_t running;
    int      last_err_step;
    int      last_err_code;
} burn_results_summary_t;

#ifndef __SPU__
void burn_benchmark_init(uint64_t timebase_freq, void *dma_buffer, uint32_t dma_buffer_size);
void burn_start(void);
void burn_stop(void);
void burn_tick(uint64_t tb_freq);
const burn_results_summary_t *burn_get_results(void);
#endif

#endif /* BURN_BENCHMARK_H */