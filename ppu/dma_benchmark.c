#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "dma_benchmark.h"

#define NUM_SPES                6
#define DMA_PER_SPE_WINDOW      (1024 * 1024) 
#define DMA_BATCH_ITERATIONS    16384

extern const char _binary_spu_dma_elf_start[];

static uint64_t      g_tb_freq = 0;
static int           g_initialized = 0;

static dma_params_t  g_params [NUM_SPES] __attribute__((aligned(128)));
static dma_results_t g_results[NUM_SPES] __attribute__((aligned(128)));

static void *g_xdr_region    = NULL;
static size_t g_xdr_region_size = 0;

// imported once, reused for every batch
// importing per call leaks a kernel SPU image slot each time, and the pool runs dry at 71 calls
static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

void dma_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq = timebase_freq;
    g_initialized = 1;
}

static int ensure_xdr_region(void)
{
    if (g_xdr_region) return 0;

    g_xdr_region_size = NUM_SPES * DMA_PER_SPE_WINDOW;
    g_xdr_region = memalign(128, g_xdr_region_size);
    if (!g_xdr_region) return -1;

    memset(g_xdr_region, 0xA5, g_xdr_region_size);
    return 0;
}

float dma_benchmark_run(int direction)
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
    if (ensure_xdr_region() != 0)         return 0.0f;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_dma_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_dma_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) return 0.0f;
        }
        g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "dma_bw");
    ret = sys_spu_thread_group_create(&spu_group, NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) return 0.0f;

    memset(g_params,  0, sizeof(g_params));
    memset(g_results, 0, sizeof(g_results));
    for (i = 0; i < NUM_SPES; i++) {
        g_params[i].ea_results     = (uint64_t)(uintptr_t)&g_results[i];
        g_params[i].ea_test_region = (uint64_t)(uintptr_t)g_xdr_region + (uint64_t)i * DMA_PER_SPE_WINDOW;
        g_params[i].window_size    = DMA_PER_SPE_WINDOW;
        g_params[i].iterations     = DMA_BATCH_ITERATIONS;
        g_params[i].direction      = (uint32_t)direction;
        g_params[i].spe_index      = (uint32_t)i;
    }

    for (i = 0; i < NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "dma_bw_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &g_spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) goto fail_group;
    }

    SYS_TIMEBASE_GET(t0);

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) goto fail_group;

    // SPEs do their iteration count and exit... join blocks until all 6 have exited or the group is force terminated
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

static dma_results_summary_t g_summary;

void dma_run_batch(int bench_id, uint64_t tb_freq)
{
    float now;
    int   dir;

    (void)tb_freq;

    if (bench_id == DMA_BENCH_PUT) dir = DMA_DIR_PUT;
    else                           dir = DMA_DIR_GET;

    now = dma_benchmark_run(dir);
    if (now <= 0.0f) return;

    if (dir == DMA_DIR_GET) {
        if (g_summary.get_runs == 0) g_summary.get_mbps = now;
        else g_summary.get_mbps = 0.7f * g_summary.get_mbps + 0.3f * now;
        g_summary.get_runs++;
    } else {
        if (g_summary.put_runs == 0) g_summary.put_mbps = now;
        else g_summary.put_mbps = 0.7f * g_summary.put_mbps + 0.3f * now;
        g_summary.put_runs++;
    }
}

const dma_results_summary_t *dma_get_results(void)
{
    return &g_summary;
}