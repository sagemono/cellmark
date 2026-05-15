#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "fft_benchmark.h"

#define ERR_IMG_IMPORT  1
#define ERR_GRP_CREATE  2
#define ERR_THR_INIT    3
#define ERR_GRP_START   4
#define ERR_GRP_JOIN    5
#define ERR_DEC_ZERO    6

extern const char _binary_spu_fft_elf_start[];

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;
static int      g_disabled    = 0;

static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static fft_params_t g_params [FFT_NUM_SPES] __attribute__((aligned(128)));
static fft_result_t g_results[FFT_NUM_SPES] __attribute__((aligned(128)));
static fft_results_summary_t g_summary;

void fft_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq     = timebase_freq;
    g_initialized = 1;
    g_disabled    = 0;
    memset(&g_summary, 0, sizeof(g_summary));
    printf("[fft] init OK (SPE side per batch impulse validation)\n");
}

static void record_failure(int step, int code)
{
    g_summary.last_err_step = step;
    g_summary.last_err_code = code;
    g_disabled = 1;
    printf("[fft] FAIL step=%d code=0x%08x\n", step, (unsigned)code);
}

void fft_run_batch(uint64_t tb_freq)
{
    sys_spu_thread_group_t              spu_group;
    sys_spu_thread_t                    spu_threads[FFT_NUM_SPES];
    sys_spu_thread_group_attribute_t    group_attr;
    sys_spu_thread_attribute_t          thread_attr;
    sys_spu_thread_argument_t           thread_args;
    int      ret, cause, status;
    int      i;
    uint32_t max_dec_ticks = 0;
    uint32_t total_ffts;
    float    worst_err = 0.0f;

    (void)tb_freq;

    if (g_disabled)                       return;
    if (!g_initialized || g_tb_freq == 0) return;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_fft_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_fft_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { record_failure(ERR_IMG_IMPORT, ret); return; }
        }
        g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "fft");
    ret = sys_spu_thread_group_create(&spu_group, FFT_NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) { record_failure(ERR_GRP_CREATE, ret); return; }

    memset(g_params,  0, sizeof(g_params));
    memset(g_results, 0, sizeof(g_results));
    for (i = 0; i < (int)FFT_NUM_SPES; i++) {
        g_params[i].spe_index   = (uint32_t)i;
        g_params[i].total_spes  = FFT_NUM_SPES;
        g_params[i].ffts_to_run = FFT_FFTS_PER_SPE;
        g_params[i].ea_results  = (uint64_t)(uintptr_t)g_results;
    }

    for (i = 0; i < (int)FFT_NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "fft_spe");

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

    for (i = 0; i < (int)FFT_NUM_SPES; i++) {
        if (g_results[i].dec_ticks > max_dec_ticks)
            max_dec_ticks = g_results[i].dec_ticks;
        if (g_results[i].first_fft_max_err > worst_err)
            worst_err = g_results[i].first_fft_max_err;
    }
    if (max_dec_ticks == 0u) { record_failure(ERR_DEC_ZERO, 0); return; }

    if (worst_err > g_summary.self_test_max_err)
        g_summary.self_test_max_err = worst_err;
    g_summary.self_test_ok = (g_summary.self_test_max_err < 1.0e-3f);

    total_ffts = FFT_FFTS_PER_SPE * FFT_NUM_SPES;
    {
        double secs    = (double)max_dec_ticks / (double)g_tb_freq;
        float  fps     = (float)((double)total_ffts / secs);
        float  pts     = (float)((double)(total_ffts * FFT_N) / secs);
        float  ms      = (float)(secs * 1.0e3);
        if (g_summary.runs == 0u) {
            g_summary.ffts_per_sec   = fps;
            g_summary.points_per_sec = pts;
            g_summary.ms_per_batch   = ms;
        } else {
            g_summary.ffts_per_sec   = 0.7f * g_summary.ffts_per_sec   + 0.3f * fps;
            g_summary.points_per_sec = 0.7f * g_summary.points_per_sec + 0.3f * pts;
            g_summary.ms_per_batch   = 0.7f * g_summary.ms_per_batch   + 0.3f * ms;
        }
        g_summary.total_ffts_run += total_ffts;
    }

    g_summary.runs++;
    return;

fail_group:
    sys_spu_thread_group_destroy(spu_group);
}

const fft_results_summary_t *fft_get_results(void)
{
    return &g_summary;
}