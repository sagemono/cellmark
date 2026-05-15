#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "pi_benchmark.h"
#define ERR_IMG_IMPORT  1
#define ERR_GRP_CREATE  2
#define ERR_THR_INIT    3
#define ERR_GRP_START   4
#define ERR_GRP_JOIN    5
#define ERR_DEC_ZERO    6

extern const char _binary_spu_pi_elf_start[];

extern uint8_t pi_hex_digit(uint32_t n);

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;
static int      g_disabled    = 0;

static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static pi_params_t  g_params [PI_NUM_SPES] __attribute__((aligned(128)));
static pi_result_t  g_results[PI_NUM_SPES] __attribute__((aligned(128)));
static pi_results_summary_t g_summary;

static const uint8_t pi_ref_digits[8] = {
    0x2, 0x4, 0x3, 0xf, 0x6, 0xa, 0x8, 0x8
};

static void pi_self_test(void)
{
    int      i, ok = 1;
    uint8_t  got[8];

    for (i = 0; i < 8; i++) {
        got[i] = pi_hex_digit((uint32_t)i);
        if (got[i] != pi_ref_digits[i]) ok = 0;
    }

    if (ok) {
        printf("[pi] self-test PASS: digits[0..7] = %x%x%x%x%x%x%x%x (expected 243f6a88)\n", got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
    } else {
        printf("[pi] self-test FAIL: got %x%x%x%x%x%x%x%x  expected 243f6a88\n", got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
    }
}

void pi_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq     = timebase_freq;
    g_initialized = 1;
    g_disabled    = 0;
    memset(&g_summary, 0, sizeof(g_summary));
    pi_self_test();
}

static void record_failure(int step, int code)
{
    g_summary.last_err_step = step;
    g_summary.last_err_code = code;
    g_disabled = 1;
    printf("[pi] FAIL step=%d code=0x%08x\n", step, (unsigned)code);
}

static char hex_char(uint8_t v)
{
    return (v < 10u) ? (char)('0' + v) : (char)('a' + (v - 10u));
}

void pi_run_batch(uint64_t tb_freq)
{
    sys_spu_thread_group_t              spu_group;
    sys_spu_thread_t                    spu_threads[PI_NUM_SPES];
    sys_spu_thread_group_attribute_t    group_attr;
    sys_spu_thread_attribute_t          thread_attr;
    sys_spu_thread_argument_t           thread_args;
    int      ret, cause, status;
    int      i;
    uint32_t per_spe;
    uint32_t total_digits;
    uint32_t max_dec_ticks = 0;

    (void)tb_freq;

    if (g_disabled)                       return;
    if (!g_initialized || g_tb_freq == 0) return;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_pi_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_pi_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { record_failure(ERR_IMG_IMPORT, ret); return; }
        }       g_image_loaded = 1;
    }

    per_spe      = PI_DIGITS_PER_BATCH / PI_NUM_SPES;
    total_digits = per_spe * PI_NUM_SPES;
    if (per_spe == 0u || total_digits == 0u) return;

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "pi_bbp");
    ret = sys_spu_thread_group_create(&spu_group, PI_NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) { record_failure(ERR_GRP_CREATE, ret); return; }

    memset(g_params,  0, sizeof(g_params));
    memset(g_results, 0, sizeof(g_results));
    for (i = 0; i < (int)PI_NUM_SPES; i++) {
        g_params[i].digit_start = PI_DEFAULT_START;
        g_params[i].digit_count = per_spe;
        g_params[i].spe_index   = (uint32_t)i;
        g_params[i].total_spes  = PI_NUM_SPES;
        g_params[i].ea_results  = (uint64_t)(uintptr_t)g_results;
    }

    for (i = 0; i < (int)PI_NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "pi_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &g_spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) { record_failure(ERR_THR_INIT, ret); goto fail_group; }
    }

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) { record_failure(ERR_GRP_START, ret); goto fail_group; }

    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK) { record_failure(ERR_GRP_JOIN, ret); goto fail_group; }

    sys_spu_thread_group_destroy(spu_group);

    for (i = 0; i < (int)PI_NUM_SPES; i++) {
        if (g_results[i].dec_ticks > max_dec_ticks)
            max_dec_ticks = g_results[i].dec_ticks;
    }
    if (max_dec_ticks == 0u) { record_failure(ERR_DEC_ZERO, 0); return; }
    {
        double secs   = (double)max_dec_ticks / (double)g_tb_freq;
        float  dps    = (float)((double)total_digits / secs);
        float  ms     = (float)(secs * 1.0e3);
        if (g_summary.runs == 0u) {
            g_summary.digits_per_sec = dps;
            g_summary.ms_per_batch   = ms;
        } else {
            g_summary.digits_per_sec = 0.7f * g_summary.digits_per_sec + 0.3f * dps;
            g_summary.ms_per_batch   = 0.7f * g_summary.ms_per_batch   + 0.3f * ms;
        }
        g_summary.total_digits_run += total_digits;
        g_summary.digit_start       = PI_DEFAULT_START;
    }
    {
        char *out = g_summary.last_digits_ascii;
        int   pos = 0;
        for (i = 0; i < (int)PI_NUM_SPES && pos < (int)PI_DIGITS_PER_BATCH; i++) {
            uint32_t k;
            for (k = 0; k < g_results[i].digit_count
                     && pos < (int)PI_DIGITS_PER_BATCH; k++) {
                out[pos++] = hex_char(g_results[i].digits[k]);
            }
        }
        out[pos] = '\0';
    }

    g_summary.runs++;
    return;

fail_group:
    sys_spu_thread_group_destroy(spu_group);
}

const pi_results_summary_t *pi_get_results(void)
{
    return &g_summary;
}