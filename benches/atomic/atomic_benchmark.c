#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "atomic_benchmark.h"
#define NUM_SPES   2

extern const char _binary_spu_atomic_elf_start[];

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;

static atomic_params_t g_params[NUM_SPES] __attribute__((aligned(128)));

// shared atomic line, 128 B aligned, in heap so its a real EA
static uint8_t        *g_atomic_line     = NULL;
static sys_spu_image_t g_spu_img;
static int             g_image_loaded    = 0;

void atomic_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq     = timebase_freq;
    g_initialized = 1;
}

static int ensure_line(void)
{
    if (g_atomic_line) return 0;
    g_atomic_line = memalign(128, 128);
    if (!g_atomic_line) return -1;
    memset(g_atomic_line, 0, 128);
    return 0;
}

float atomic_benchmark_run(uint32_t iterations_per_spe)
{
    sys_spu_thread_group_t              spu_group;
    sys_spu_thread_t                    spu_threads[NUM_SPES];
    sys_spu_thread_group_attribute_t    group_attr;
    sys_spu_thread_attribute_t          thread_attr;
    sys_spu_thread_argument_t           thread_args;
    uint64_t t0, t1;
    int      ret, i, cause, status;

    if (!g_initialized || g_tb_freq == 0) return 0.0f;
    if (ensure_line() != 0)               return 0.0f;

    // reset the shared counter
    // SPE 0 has parity 0 and goes first
    *(volatile uint32_t *)g_atomic_line = 0;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_atomic_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_atomic_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) return 0.0f;
        }       g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "atomic_bw");
    ret = sys_spu_thread_group_create(&spu_group, NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) return 0.0f;

    memset(g_params, 0, sizeof(g_params));
    for (i = 0; i < NUM_SPES; i++) {
        g_params[i].ea_atomic  = (uint64_t)(uintptr_t)g_atomic_line;
        g_params[i].iterations = iterations_per_spe;
        g_params[i].spe_index  = (uint32_t)i;
    }

    for (i = 0; i < NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "atomic_spe");

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

    sys_spu_thread_group_destroy(spu_group);

    {
        // total putllc rounds = sum of both SPEs completed counts. each successful round transfers the line once between caches, so total transfers = total rounds
        uint32_t total_rounds = g_params[0].iterations + g_params[1].iterations;
        double   secs         = (double)(t1 - t0) / (double)g_tb_freq;
        if (secs <= 0.0 || total_rounds == 0) return 0.0f;

        return (float)((secs * 1.0e9) / (double)total_rounds);
    }

fail_group:
    sys_spu_thread_group_destroy(spu_group);
    return 0.0f;
}

static atomic_results_summary_t g_summary;

#define ATOMIC_ITERATIONS_PER_SPE   (256u * 1024u) // 50 - 100ms a batch

void atomic_run_batch(uint64_t tb_freq)
{
    float ns;

    (void)tb_freq;

    ns = atomic_benchmark_run(ATOMIC_ITERATIONS_PER_SPE);
    if (ns <= 0.0f) return;

    if (g_summary.runs == 0) g_summary.ns_per_bounce = ns;
    else g_summary.ns_per_bounce = 0.7f * g_summary.ns_per_bounce + 0.3f * ns;
    g_summary.rounds_per_sec_M = 1000.0f / g_summary.ns_per_bounce;
    g_summary.runs++;
}

const atomic_results_summary_t *atomic_get_results(void)
{
    return &g_summary;
}