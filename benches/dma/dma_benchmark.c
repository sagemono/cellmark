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
#include "pmu.h"

#include <cell/perf/performance.h>

static const pmu_event_t g_dma_pmu_events[] = {
    { CELL_PERF_SIGNAL_SPU_DUAL_INSTRUCTION_COMPLETED,  0, 0, "DUAL"  },
    { CELL_PERF_SIGNAL_SPU_PIPE0_INSTRUCTION_COMPLETED, 0, 0, "PIPE0" },
    { CELL_PERF_SIGNAL_SPU_TAG_COMPLETION_WAIT,         0, 0, "TAGW"  },
    { CELL_PERF_SIGNAL_SPU_CHANNEL_21_STALL,            0, 0, "CH21"  },
};

static const pmu_profile_t g_dma_pmu_profile = {
    g_dma_pmu_events,
    sizeof(g_dma_pmu_events) / sizeof(g_dma_pmu_events[0]),
    0, -1
};

static pmu_result_t  g_dma_pmu_result;
static pmu_history_t g_dma_pmu_history;
static int           g_dma_pmu_history_dir = -1;
static uint32_t      g_dma_pmu_log_counter = 0;

#define NUM_SPES                6
#define DMA_PER_SPE_WINDOW      (1024 * 1024)
#define DMA_BATCH_ITERATIONS    16384
#define DMA_CHUNK_SIZE_DEFAULT  16384u

#define DMA_QUEUE_DEPTH         8

extern const char _binary_spu_dma_elf_start[];

static uint64_t      g_tb_freq = 0;
static int           g_initialized = 0;

static dma_params_t  g_params [NUM_SPES] __attribute__((aligned(128)));
static dma_results_t g_results[NUM_SPES] __attribute__((aligned(128)));

static void *g_xdr_region    = NULL;
static size_t g_xdr_region_size = 0;

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

const uint32_t dma_sweep_chunk_sizes[DMA_SWEEP_COUNT] = {
    32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u
};

#define DMA_SWEEP_TARGET_BYTES_PER_SPE   (64u * 1024u * 1024u)
#define DMA_SWEEP_MAX_ITERATIONS         (1u * 1024u * 1024u)

static dma_sweep_summary_t g_sweep_summary = { {0}, {0}, 0 };
static int      g_sweep_idx          = 0;
static uint32_t g_sweep_batches_here = 0;
#define DMA_SWEEP_BATCHES_PER_VALUE  4

float dma_benchmark_run(int direction, uint32_t chunk_size, uint32_t iterations)
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
        g_params[i].iterations     = iterations;
        g_params[i].direction      = (uint32_t)direction;
        g_params[i].spe_index      = (uint32_t)i;
        g_params[i].queue_depth    = DMA_QUEUE_DEPTH;
        g_params[i].chunk_size     = chunk_size;
    }

    for (i = 0; i < NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "dma_bw_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &g_spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) goto fail_group;
    }

    pmu_begin_spu(&g_dma_pmu_profile, (uint32_t)spu_threads[0], 0);
    SYS_TIMEBASE_GET(t0);

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) {
        pmu_end_and_read(&g_dma_pmu_result);
        goto fail_group;
    }

    // SPEs do their iteration count and exit... join blocks until all 6 have exited or the group is force terminated
    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK) {
        pmu_end_and_read(&g_dma_pmu_result);
        goto fail_group;
    }

    SYS_TIMEBASE_GET(t1);
    pmu_end_and_read(&g_dma_pmu_result);

    // push to history; reset on direction OR chunk_size switch since
    // each combination is a distinct workload
    if (g_dma_pmu_result.ok) {
        int key = (direction << 24) | (int)chunk_size;
        static int last_key = -1;
        if (key != last_key) {
            memset(&g_dma_pmu_history, 0, sizeof(g_dma_pmu_history));
            g_dma_pmu_history_dir = direction;
            g_dma_pmu_log_counter = 0;
            last_key = key;
        }
        pmu_history_push(&g_dma_pmu_history, &g_dma_pmu_result);

        g_dma_pmu_log_counter++;
        if ((g_dma_pmu_log_counter & 0x3) == 0) {
            pmu_summary_t s;
            char tag[32];
            pmu_history_summary(&g_dma_pmu_history, &s);
            snprintf(tag, sizeof(tag), "%s_%uB",
                     direction == DMA_DIR_PUT ? "dma_put" : "dma_get",
                     (unsigned)chunk_size);
            pmu_log_dump_summary(tag, &g_dma_pmu_profile, &s);
        }
    }

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
    float    now;
    int      dir;
    uint32_t chunk;
    uint32_t iters;

    (void)tb_freq;

    if (bench_id == DMA_BENCH_SIZESWEEP) {
        chunk = dma_sweep_chunk_sizes[g_sweep_idx];
        iters = DMA_SWEEP_TARGET_BYTES_PER_SPE / chunk;
        if (iters > DMA_SWEEP_MAX_ITERATIONS) iters = DMA_SWEEP_MAX_ITERATIONS;
        if (iters < (uint32_t)DMA_QUEUE_DEPTH * 2u) iters = (uint32_t)DMA_QUEUE_DEPTH * 2u;

        g_sweep_summary.active_idx = g_sweep_idx;

        now = dma_benchmark_run(DMA_DIR_GET, chunk, iters);
        if (now > 0.0f) {
            float *slot = &g_sweep_summary.mbps[g_sweep_idx];
            uint32_t runs = g_sweep_summary.runs[g_sweep_idx];
            if (runs == 0) *slot = now;
            else           *slot = 0.7f * *slot + 0.3f * now;
            g_sweep_summary.runs[g_sweep_idx] = runs + 1u;
        }

        g_sweep_batches_here++;
        if (g_sweep_batches_here >= DMA_SWEEP_BATCHES_PER_VALUE) {
            g_sweep_batches_here = 0;
            g_sweep_idx = (g_sweep_idx + 1) % DMA_SWEEP_COUNT;
        }
        return;
    }

    if (bench_id == DMA_BENCH_PUT) dir = DMA_DIR_PUT;
    else                           dir = DMA_DIR_GET;

    now = dma_benchmark_run(dir, DMA_CHUNK_SIZE_DEFAULT, DMA_BATCH_ITERATIONS);
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

const dma_sweep_summary_t *dma_get_sweep_summary(void)
{
    return &g_sweep_summary;
}

const dma_results_summary_t *dma_get_results(void)
{
    return &g_summary;
}

void dma_get_pmu_summary(pmu_summary_t *out)
{
    pmu_history_summary(&g_dma_pmu_history, out);
}

const pmu_profile_t *dma_get_pmu_profile(void)
{
    return &g_dma_pmu_profile;
}