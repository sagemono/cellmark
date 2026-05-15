#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "branch_benchmark.h"

#define NUM_SPES   1

#define ERR_IMG_IMPORT  1
#define ERR_GRP_CREATE  2
#define ERR_THR_INIT    3
#define ERR_GRP_START   4
#define ERR_GRP_JOIN    5
#define ERR_DEC_ZERO    6

extern const char _binary_spu_branch_elf_start[];

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;
static int      g_disabled    = 0;

static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static branch_params_t g_params __attribute__((aligned(128)));

static branch_results_summary_t g_summary;

void branch_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq     = timebase_freq;
    g_initialized = 1;
    g_disabled    = 0;
    memset(&g_summary, 0, sizeof(g_summary));
}

static void record_failure(int step, int code)
{
    g_summary.last_err_step = step;
    g_summary.last_err_code = code;
    g_disabled = 1;
    printf("[branch] FAIL step=%d code=0x%08x\n", step, (unsigned)code);
}

float branch_benchmark_run(uint32_t iterations)
{
    sys_spu_thread_group_t              spu_group;
    sys_spu_thread_t                    spu_thread;
    sys_spu_thread_group_attribute_t    group_attr;
    sys_spu_thread_attribute_t          thread_attr;
    sys_spu_thread_argument_t           thread_args;
    int      ret, cause, status;

    if (g_disabled)                       return 0.0f;
    if (!g_initialized || g_tb_freq == 0) return 0.0f;
    if (iterations == 0)                  return 0.0f;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_branch_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_branch_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { record_failure(ERR_IMG_IMPORT, ret); return 0.0f; }
        }
        g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "branch_bw");
    ret = sys_spu_thread_group_create(&spu_group, NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) { record_failure(ERR_GRP_CREATE, ret); return 0.0f; }

    memset(&g_params, 0, sizeof(g_params));
    g_params.iterations = iterations;

    sys_spu_thread_attribute_initialize(thread_attr);
    sys_spu_thread_attribute_name(thread_attr, "branch_spe");

    memset(&thread_args, 0, sizeof(thread_args));
    thread_args.arg1 = (uint64_t)(uintptr_t)&g_params;

    ret = sys_spu_thread_initialize(&spu_thread, spu_group, 0, &g_spu_img, &thread_attr, &thread_args);
    if (ret != CELL_OK) { record_failure(ERR_THR_INIT, ret); goto fail_group; }

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) { record_failure(ERR_GRP_START, ret); goto fail_group; }

    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK) { record_failure(ERR_GRP_JOIN, ret); goto fail_group; }

    sys_spu_thread_group_destroy(spu_group);

    if (g_params.hinted_dec_ticks == 0 || g_params.unhinted_dec_ticks == 0) {
        record_failure(ERR_DEC_ZERO, 0);
        return 0.0f;
    }

    {
        float ticks_per_pclk = 40.0f;   // PPE clock per decrementer tick
        float h_cyc = (float)g_params.hinted_dec_ticks   * ticks_per_pclk / (float)iterations;
        float u_cyc = (float)g_params.unhinted_dec_ticks * ticks_per_pclk / (float)iterations;
        if (g_summary.runs == 0) {
            g_summary.hinted_cyc_per_iter   = h_cyc;
            g_summary.unhinted_cyc_per_iter = u_cyc;
        } else {
            g_summary.hinted_cyc_per_iter   = 0.7f * g_summary.hinted_cyc_per_iter   + 0.3f * h_cyc;
            g_summary.unhinted_cyc_per_iter = 0.7f * g_summary.unhinted_cyc_per_iter + 0.3f * u_cyc;
        }
        g_summary.mispredict_penalty_cyc = g_summary.unhinted_cyc_per_iter - g_summary.hinted_cyc_per_iter;
        g_summary.speedup = (g_summary.hinted_cyc_per_iter > 0.0f) ? g_summary.unhinted_cyc_per_iter / g_summary.hinted_cyc_per_iter : 0.0f;
        g_summary.runs++;
        return h_cyc;
    }

fail_group:
    sys_spu_thread_group_destroy(spu_group);
    return 0.0f;
}

#define BRANCH_ITERATIONS_PER_BATCH   (1u << 20)   // 1M branches per pass

void branch_run_batch(uint64_t tb_freq)
{
    (void)tb_freq;
    (void)branch_benchmark_run(BRANCH_ITERATIONS_PER_BATCH);
}

const branch_results_summary_t *branch_get_results(void)
{
    return &g_summary;
}