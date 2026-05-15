#ifndef NBODY_BENCHMARK_H
#define NBODY_BENCHMARK_H

#include <stdint.h>

#define NBODY_N             4080u
#define NBODY_NUM_SPES      6u
#define NBODY_BODIES_PER_SPE (NBODY_N / NBODY_NUM_SPES)
#define NBODY_ITERATIONS    50u

typedef struct {
    uint32_t spe_index;
    uint32_t total_spes;
    uint32_t n_bodies;
    uint32_t iterations;
    uint64_t ea_results;
    uint32_t pad[26];
} __attribute__((aligned(128))) nbody_params_t;

typedef struct {
    uint32_t i_bodies_done;
    uint32_t dec_ticks;
    float    checksum;
    uint32_t pad[29];
} __attribute__((aligned(128))) nbody_result_t;

typedef struct {
    float    mpairs_per_sec;
    float    ms_per_batch;
    float    last_checksum;
    uint32_t total_pairs_run;
    uint32_t runs;
    int      last_err_step;
    int      last_err_code;
} nbody_results_summary_t;

#ifndef __SPU__
void nbody_benchmark_init(uint64_t timebase_freq);
void nbody_run_batch(uint64_t tb_freq);
const nbody_results_summary_t *nbody_get_results(void);
#endif

#endif /* NBODY_BENCHMARK_H */