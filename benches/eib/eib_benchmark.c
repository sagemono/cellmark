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
#include "pmu.h"

#include <cell/perf/performance.h>

static const pmu_event_t g_eib_pmu_events[] = {
    { CELL_PERF_SIGNAL_EIB_DATA_RING_0_WAS_IN_USE, 0, 0, "RING0" },
    { CELL_PERF_SIGNAL_EIB_DATA_RING_1_WAS_IN_USE, 0, 0, "RING1" },
    { CELL_PERF_SIGNAL_EIB_DATA_RING_2_WAS_IN_USE, 0, 0, "RING2" },
    { CELL_PERF_SIGNAL_EIB_DATA_RING_3_WAS_IN_USE, 0, 0, "RING3" },
};

static const pmu_profile_t g_eib_pmu_profile = {
    g_eib_pmu_events,
    sizeof(g_eib_pmu_events) / sizeof(g_eib_pmu_events[0]),
    -1, -1
};

static pmu_result_t  g_eib_pmu_result;
static pmu_history_t g_eib_pmu_history;
static int           g_eib_pmu_history_mode = -1;
static uint32_t      g_eib_pmu_log_counter = 0;

#define NUM_SPES                EIB_NUM_SPES
#define EIB_BATCH_ITERATIONS    16384
#define EIB_QUEUE_DEPTH         8

extern const char _binary_spu_eib_elf_start[];

static uint64_t g_tb_freq = 0;
static int      g_initialized = 0;

static eib_params_t  g_params [NUM_SPES] __attribute__((aligned(128)));
static eib_results_t g_results[NUM_SPES] __attribute__((aligned(128)));

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
    case EIB_BENCH_HOTSPOT_SWEEP:
    case EIB_BENCH_HOTSPOT:
        return 0;
    case EIB_BENCH_PAIRS:
    default:
        // 0<->1, 2<->3, 4<->5: 3 disjoint pairs, ideally land on 3 different EIB ring slots
        return i ^ 1;
    }
}

static eib_sweep_summary_t g_sweep_summary = { {0}, {0}, 0 };
static int      g_sweep_idx          = 0;
static uint32_t g_sweep_batches_here = 0;
#define EIB_SWEEP_BATCHES_PER_VALUE  4

static eib_nxn_summary_t g_nxn_summary;
static int      g_nxn_idx          = 0;
static uint32_t g_nxn_batches_here = 0;
#define EIB_NXN_BATCHES_PER_VALUE  4
#define EIB_NXN_TOTAL_PAIRS        30
static void nxn_pair_for_idx(int idx, int *src, int *dst)
{
    int s = idx / (EIB_NXN_SPES - 1);
    int d = idx % (EIB_NXN_SPES - 1);
    if (d >= s) d++;
    *src = s;
    *dst = d;
}

float eib_benchmark_run(int mode, int active_readers, int src, int dst)
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
        g_params[i].window_size = EIB_WINDOW_SIZE;
        g_params[i].iterations  = EIB_BATCH_ITERATIONS;
        g_params[i].direction   = EIB_DIR_GET;
        g_params[i].spe_index   = (uint32_t)i;
        g_params[i].queue_depth = EIB_QUEUE_DEPTH;

        if (mode == EIB_BENCH_NXN) {
            g_params[i].partner_idx = (uint32_t)dst;
            if (i != src) g_params[i].iterations = 0;
        } else {
            g_params[i].partner_idx = (uint32_t)partner_for(mode, i);
            if (mode == EIB_BENCH_HOTSPOT || mode == EIB_BENCH_HOTSPOT_SWEEP) {
                int idle = (i == 0) || (i > active_readers);
                if (idle) g_params[i].iterations = 0;
            }
        }
    }

    for (i = 0; i < NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "eib_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &g_spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) goto fail_group;
    }

    pmu_begin(&g_eib_pmu_profile);
    SYS_TIMEBASE_GET(t0);

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) {
        pmu_end_and_read(&g_eib_pmu_result);
        goto fail_group;
    }

    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK) {
        pmu_end_and_read(&g_eib_pmu_result);
        goto fail_group;
    }

    SYS_TIMEBASE_GET(t1);
    pmu_end_and_read(&g_eib_pmu_result);

    if (g_eib_pmu_result.ok) {
        int key;
        if (mode == EIB_BENCH_NXN) key = (mode << 16) | (src << 8) | dst;
        else                       key = (mode << 16) | (active_readers & 0xff);
        if (key != g_eib_pmu_history_mode) {
            memset(&g_eib_pmu_history, 0, sizeof(g_eib_pmu_history));
            g_eib_pmu_history_mode = key;
            g_eib_pmu_log_counter = 0;
        }
        pmu_history_push(&g_eib_pmu_history, &g_eib_pmu_result);

        g_eib_pmu_log_counter++;
        if ((g_eib_pmu_log_counter & 0x3) == 0) {
            pmu_summary_t s;
            char tag[32];
            pmu_history_summary(&g_eib_pmu_history, &s);
            if (mode == EIB_BENCH_NXN)
                snprintf(tag, sizeof(tag), "eib_nxn_%d_%d", src, dst);
            else if (mode == EIB_BENCH_HOTSPOT_SWEEP)
                snprintf(tag, sizeof(tag), "eib_hot_n%d", active_readers);
            else if (mode == EIB_BENCH_HOTSPOT)
                snprintf(tag, sizeof(tag), "eib_hotspot");
            else
                snprintf(tag, sizeof(tag), "eib_pairs");
            pmu_log_dump_summary(tag, &g_eib_pmu_profile, &s);
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

static eib_results_summary_t g_summary;

static void rings_label(const pmu_result_t *r, char out[4])
{
    uint64_t cw  = (uint64_t)r->values[0] + (uint64_t)r->values[2];
    uint64_t ccw = (uint64_t)r->values[1] + (uint64_t)r->values[3];
    const char *s;
    if (cw  > 2u * ccw) s = "CW ";
    else if (ccw > 2u * cw)  s = "CCW";
    else                     s = "SPL";
    out[0] = s[0]; out[1] = s[1]; out[2] = s[2]; out[3] = '\0';
}

void eib_run_batch(int bench_id, uint64_t tb_freq)
{
    float now;

    (void)tb_freq;

    if (bench_id == EIB_BENCH_NXN) {
        int src, dst;
        nxn_pair_for_idx(g_nxn_idx, &src, &dst);
        g_nxn_summary.active_src = src;
        g_nxn_summary.active_dst = dst;

        now = eib_benchmark_run(EIB_BENCH_NXN, 0, src, dst);
        if (now > 0.0f) {
            float   *slot = &g_nxn_summary.mbps[src][dst];
            uint32_t runs = g_nxn_summary.runs[src][dst];
            if (runs == 0) *slot = now;
            else           *slot = 0.7f * *slot + 0.3f * now;
            g_nxn_summary.runs[src][dst] = runs + 1u;
            if (g_eib_pmu_result.ok)
                rings_label(&g_eib_pmu_result, g_nxn_summary.rings[src][dst]);
        }

        g_nxn_batches_here++;
        if (g_nxn_batches_here >= EIB_NXN_BATCHES_PER_VALUE) {
            g_nxn_batches_here = 0;
            g_nxn_idx = (g_nxn_idx + 1) % EIB_NXN_TOTAL_PAIRS;
        }
        return;
    }

    if (bench_id == EIB_BENCH_HOTSPOT_SWEEP) {
        int readers = g_sweep_idx + 1;
        g_sweep_summary.active_idx = g_sweep_idx;

        now = eib_benchmark_run(EIB_BENCH_HOTSPOT_SWEEP, readers, 0, 0);
        if (now > 0.0f) {
            float   *slot = &g_sweep_summary.mbps[g_sweep_idx];
            uint32_t runs = g_sweep_summary.runs[g_sweep_idx];
            if (runs == 0) *slot = now;
            else           *slot = 0.7f * *slot + 0.3f * now;
            g_sweep_summary.runs[g_sweep_idx] = runs + 1u;
        }

        g_sweep_batches_here++;
        if (g_sweep_batches_here >= EIB_SWEEP_BATCHES_PER_VALUE) {
            g_sweep_batches_here = 0;
            g_sweep_idx = (g_sweep_idx + 1) % EIB_SWEEP_COUNT;
        }
        return;
    }

    if (bench_id == EIB_BENCH_HOTSPOT) {
        now = eib_benchmark_run(EIB_BENCH_HOTSPOT, 5, 0, 0);
        if (now <= 0.0f) return;
        if (g_summary.hotspot_runs == 0) g_summary.hotspot_mbps = now;
        else g_summary.hotspot_mbps = 0.7f * g_summary.hotspot_mbps + 0.3f * now;
        g_summary.hotspot_runs++;
        return;
    }

    now = eib_benchmark_run(EIB_BENCH_PAIRS, 0, 0, 0);
    if (now <= 0.0f) return;
    if (g_summary.pairs_runs == 0) g_summary.pairs_get_mbps = now;
    else g_summary.pairs_get_mbps = 0.7f * g_summary.pairs_get_mbps + 0.3f * now;
    g_summary.pairs_runs++;
}

const eib_nxn_summary_t *eib_get_nxn_summary(void)
{
    return &g_nxn_summary;
}

const eib_sweep_summary_t *eib_get_sweep_summary(void)
{
    return &g_sweep_summary;
}

const eib_results_summary_t *eib_get_results(void)
{
    return &g_summary;
}

void eib_get_pmu_summary(pmu_summary_t *out)
{
    pmu_history_summary(&g_eib_pmu_history, out);
}

const pmu_profile_t *eib_get_pmu_profile(void)
{
    return &g_eib_pmu_profile;
}