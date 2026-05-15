#ifndef EIB_BENCHMARK_H
#define EIB_BENCHMARK_H

#include <stdint.h>

#define EIB_DIR_GET     0   // read partners LS

#define EIB_NUM_SPES        6
#define EIB_WINDOW_SIZE     65536u  // 64 KB DMA window inside partners

typedef struct {
    uint64_t ea_results;
    uint32_t partner_idx;
    uint32_t window_size;
    uint32_t iterations;
    uint32_t direction;
    uint32_t spe_index;
    uint32_t pad[25];
} __attribute__((aligned(128))) eib_params_t;

typedef struct {
    uint64_t total_bytes;
    uint32_t pad[30];
} __attribute__((aligned(128))) eib_results_t;

typedef struct {
    float    pairs_get_mbps;
    uint32_t pairs_runs;
} eib_results_summary_t;

void eib_benchmark_init(uint64_t timebase_freq);

float eib_benchmark_run(int mode);

void eib_run_batch(int bench_id, uint64_t tb_freq);

const eib_results_summary_t *eib_get_results(void);

#endif /* EIB_BENCHMARK_H */