#ifndef MBOX_BENCHMARK_H
#define MBOX_BENCHMARK_H

#include <stdint.h>

typedef struct {
    float    ns_per_roundtrip;
    float    ns_one_way;
    float    rounds_per_sec_M;
    uint32_t runs;
    int      last_err_step;
    int      last_err_code;
    uint32_t completed_iters;
} mbox_results_summary_t;

void mbox_benchmark_init(uint64_t timebase_freq);

float mbox_benchmark_run(uint32_t iterations);

void mbox_run_batch(uint64_t tb_freq);

const mbox_results_summary_t *mbox_get_results(void);

#endif /* MBOX_BENCHMARK_H */