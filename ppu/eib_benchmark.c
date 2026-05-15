#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "eib_benchmark.h"

#define NUM_SPES                EIB_NUM_SPES
#define EIB_BATCH_ITERATIONS    16384

extern const char _binary_spu_eib_elf_start[];

static uint64_t g_tb_freq = 0;
static int      g_initialized = 0;

static eib_params_t  g_params [NUM_SPES] __attribute__((aligned(128)));
static eib_results_t g_results[NUM_SPES] __attribute__((aligned(128)));

// imported once, reused
// same deal as the dma benchmark
static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

void eib_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq = timebase_freq;
    g_initialized = 1;
}

static int partner_for(int mode, int i)
{
    switch (mode) {
    case EIB_BENCH_PAIRS:
    default:
        // 0<->1, 2<->3, 4<->5: 3 disjoint pairs, ideally land on 3 different EIB ring slots
        return i ^ 1;
    }
}

float eib_benchmark_run(int mode)
{
    sys_spu_thread_group_t   spu_group;
    sys_spu_thread_t         spu_threads[NUM_SPES];
    sys_spu_thread_group_attribute_t group_attr;
    sys_spu_thread_attribute_t        thread_attr;
    sys_spu_thread_argument_t         thread_args;
    uint64_t t0, t1;
    uint64_t total_bytes = 0;
    int      ret, i, cause, status;

    if (!g_initialized || g_tb_freq == 0) return 0.0f;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_eib_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_eib_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) return 0.0f;
        }
        g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "eib_bw");
    ret = sys_spu_thread_group_create(&spu_group, NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) return 0.0f;

    memset(g_params,  0, sizeof(g_params));
    memset(g_results, 0, sizeof(g_results));
    for (i = 0; i < NUM_SPES; i++) {
        g_params[i].ea_results  = (uint64_t)(uintptr_t)&g_results[i];
        g_params[i].partner_idx = (uint32_t)partner_for(mode, i);
        g_params[i].window_size = EIB_WINDOW_SIZE;
        g_params[i].iterations  = EIB_BATCH_ITERATIONS;
        g_params[i].direction   = EIB_DIR_GET;
        g_params[i].spe_index   = (uint32_t)i;
    }

    for (i = 0; i < NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "eib_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &g_spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) goto fail_group;
    }

    SYS_TIMEBASE_GET(t0);

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) goto fail_group;

    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK) goto fail_group;

    SYS_TIMEBASE_GET(t1);

    for (i = 0; i < NUM_SPES; i++)
        total_bytes += g_results[i].total_bytes;

    sys_spu_thread_group_destroy(spu_group);

    {
        double secs = (double)(t1 - t0) / (double)g_tb_freq;
        if (secs <= 0.0) return 0.0f;
        return (float)((double)total_bytes / (secs * 1.0e6));
    }

fail_group:
    sys_spu_thread_group_destroy(spu_group);
    return 0.0f;
}

static eib_results_summary_t g_summary;

void eib_run_batch(int bench_id, uint64_t tb_freq)
{
    float now;
    int   mode;

    (void)tb_freq;

    switch (bench_id) {
    case EIB_BENCH_PAIRS:
    default:                mode = EIB_BENCH_PAIRS; break;
    }

    now = eib_benchmark_run(mode);
    if (now <= 0.0f) return;

    if (mode == EIB_BENCH_PAIRS) {
        if (g_summary.pairs_runs == 0) g_summary.pairs_get_mbps = now;
        else g_summary.pairs_get_mbps = 0.7f * g_summary.pairs_get_mbps + 0.3f * now;
        g_summary.pairs_runs++;
    }
}

const eib_results_summary_t *eib_get_results(void)
{
    return &g_summary;
}