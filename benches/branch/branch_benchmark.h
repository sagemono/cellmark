#ifndef BRANCH_BENCHMARK_H
#define BRANCH_BENCHMARK_H

#include <stdint.h>

typedef struct {
    uint32_t iterations;
    uint32_t hinted_dec_ticks;
    uint32_t unhinted_dec_ticks;
    uint32_t pad[29];
} __attribute__((aligned(128))) branch_params_t;

typedef struct {
    float    hinted_cyc_per_iter;
    float    unhinted_cyc_per_iter;
    float    mispredict_penalty_cyc;
    float    speedup;
    uint32_t runs;
    int      last_err_step;
    int      last_err_code;
} branch_results_summary_t;

void branch_benchmark_init(uint64_t timebase_freq);

float branch_benchmark_run(uint32_t iterations);

void branch_run_batch(uint64_t tb_freq);

const branch_results_summary_t *branch_get_results(void);

#endif /* BRANCH_BENCHMARK_H */