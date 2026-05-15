#ifndef PI_BENCHMARK_H
#define PI_BENCHMARK_H

#include <stdint.h>

#define PI_MAX_DIGITS_PER_SPE   240u
#define PI_NUM_SPES             6u

#define PI_DEFAULT_START        10000u

#define PI_DIGITS_PER_BATCH     30u

typedef struct {
    uint32_t digit_start;
    uint32_t digit_count;  
    uint32_t spe_index;    
    uint32_t total_spes;
    uint64_t ea_results;     
    uint32_t pad[26];
} __attribute__((aligned(128))) pi_params_t;

typedef struct {
    uint32_t digit_start;
    uint32_t digit_count;
    uint32_t dec_ticks;
    uint32_t pad0;
    uint8_t  digits[PI_MAX_DIGITS_PER_SPE];
    uint8_t  pad1[8];
} __attribute__((aligned(128))) pi_result_t;

typedef struct {
    float    digits_per_sec;
    float    ms_per_batch;
    uint32_t total_digits_run;
    uint32_t digit_start;
    uint32_t runs;
    int      last_err_step;
    int      last_err_code;
    char     last_digits_ascii[PI_DIGITS_PER_BATCH + 8];
} pi_results_summary_t;

#ifndef __SPU__
void pi_benchmark_init(uint64_t timebase_freq);
void pi_run_batch(uint64_t tb_freq);
const pi_results_summary_t *pi_get_results(void);
#endif

#endif /* PI_BENCHMARK_H */