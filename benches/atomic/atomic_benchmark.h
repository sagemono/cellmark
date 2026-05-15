#ifndef ATOMIC_BENCHMARK_H
#define ATOMIC_BENCHMARK_H

#include <stdint.h>

typedef struct {
    uint64_t ea_atomic;
    uint32_t iterations;
    uint32_t spe_index;
    uint32_t pad[28];
} __attribute__((aligned(128))) atomic_params_t;

typedef struct {
    float    ns_per_bounce;
    float    rounds_per_sec_M;
    uint32_t runs;
} atomic_results_summary_t;

void atomic_benchmark_init(uint64_t timebase_freq);

float atomic_benchmark_run(uint32_t iterations_per_spe);

void atomic_run_batch(uint64_t tb_freq);

const atomic_results_summary_t *atomic_get_results(void);

#endif /* ATOMIC_BENCHMARK_H */