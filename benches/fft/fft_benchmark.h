#ifndef FFT_BENCHMARK_H
#define FFT_BENCHMARK_H

#include <stdint.h>

#define FFT_N               1024u
#define FFT_LOG2_N          10u

#define FFT_NUM_SPES        6u
#define FFT_BATCH_LANES     4u
#define FFT_FFTS_PER_SPE    32u

typedef struct {
    uint32_t spe_index;
    uint32_t total_spes;
    uint32_t ffts_to_run;
    uint32_t pad0;
    uint64_t ea_results;
    uint32_t pad[26];
} __attribute__((aligned(128))) fft_params_t;

typedef struct {
    uint32_t ffts_done;
    uint32_t dec_ticks;
    float    first_fft_max_err;
    uint32_t pad[29];
} __attribute__((aligned(128))) fft_result_t;

typedef struct {
    float    points_per_sec;
    float    ffts_per_sec;
    float    ms_per_batch;
    uint32_t total_ffts_run;
    uint32_t runs;
    int      last_err_step;
    int      last_err_code;
    int      self_test_ok;
    float    self_test_max_err;
} fft_results_summary_t;

#ifndef __SPU__
void fft_benchmark_init(uint64_t timebase_freq);
void fft_run_batch(uint64_t tb_freq);
const fft_results_summary_t *fft_get_results(void);
#endif

#endif /* FFT_BENCHMARK_H */