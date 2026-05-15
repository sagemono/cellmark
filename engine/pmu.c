// pmu.c - DECR libperf wrapper. retail builds compile to no-ops

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pmu.h"

static int u32_cmp(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

static int u64_cmp(const void *a, const void *b)
{
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

void pmu_history_push(pmu_history_t *h, const pmu_result_t *r)
{
    int i;
    if (!h || !r || !r->ok) return;
    for (i = 0; i < PMU_MAX_EVENTS; i++)
        h->samples[h->head][i] = r->values[i];
    h->window_us[h->head] = r->window_us;
    h->head = (h->head + 1) % PMU_HISTORY_DEPTH;
    if (h->filled < PMU_HISTORY_DEPTH) h->filled++;
}

void pmu_history_summary(const pmu_history_t *h, pmu_summary_t *out)
{
    uint32_t col[PMU_HISTORY_DEPTH];
    uint64_t wcol[PMU_HISTORY_DEPTH];
    int n, i, j;

    memset(out, 0, sizeof(*out));
    if (!h) return;
    n = h->filled;
    if (n <= 0) { out->ok = 0; return; }
    out->sample_count = n;

    for (j = 0; j < PMU_MAX_EVENTS; j++) {
        for (i = 0; i < n; i++)
            col[i] = h->samples[i][j];
        qsort(col, n, sizeof(uint32_t), u32_cmp);
        out->mn[j]  = col[0];
        out->med[j] = col[n / 2];
        out->mx[j]  = col[n - 1];
    }
    for (i = 0; i < n; i++) wcol[i] = h->window_us[i];
    qsort(wcol, n, sizeof(uint64_t), u64_cmp);
    out->med_window_us = wcol[n / 2];
    out->ok = 1;
}

void pmu_log_dump_summary(const char *bench_name, const pmu_profile_t *profile, const pmu_summary_t *s)
{
    int i;
    if (!s || !s->ok || !profile) return;
    printf("[PMU] %-12s n=%d ~%llums", bench_name, s->sample_count, (unsigned long long)(s->med_window_us / 1000));
    for (i = 0; i < profile->count && i < PMU_MAX_EVENTS; i++) {
        printf(" %s=%u(min=%u p50=%u)", profile->events[i].label, s->mx[i], s->mn[i], s->med[i]);
    }
    printf("\n");
}

#ifdef CELLMARK_DECR

#include <sys/sys_time.h>
#include <cell/perf/performance.h>

static int           g_initialized = 0;
static int           g_active      = 0;
static system_time_t g_t0          = 0;
static const pmu_profile_t *g_active_profile = NULL;

int pmu_init(void)
{
    if (g_initialized) return 0;
    // libperf has no separate init, cellPerfAddCBEpm sets up per window
    // we just record that the wrapper is ready to be used
    printf("[PMU] libperf wrapper init (libperf v%04x)\n", cellPerfGetVersion());
    g_initialized = 1;
    return 0;
}

int pmu_is_active(void)
{
    return g_active;
}

static void pmu_fill_signals(CellPerfCBEpmSetup *setup, const pmu_profile_t *profile)
{
    int i;
    for (i = 0; i < profile->count && i < PMU_MAX_EVENTS; i++) {
        setup->signal[i].name        = profile->events[i].signal_name;
        setup->signal[i].setup       = CELL_PERF_SIGNAL_SETUP_ENABLE | CELL_PERF_SIGNAL_SETUP_POSITIVE | CELL_PERF_SIGNAL_SETUP_CYCLE;
        setup->signal[i].targetSpuId = profile->events[i].target_slot;
        setup->signal[i].mask        = profile->events[i].mask;
    }
}

int pmu_begin(const pmu_profile_t *profile)
{
    CellPerfCBEpmSetup setup;
    int ret;

    if (!g_initialized || !profile || profile->count <= 0) return -1;
    if (g_active) {
        return -1;
    }

    memset(&setup, 0, sizeof(setup));
    setup.conf = CELL_PERF_SETUP_CONF_NO_TRACE_MODE | CELL_PERF_SETUP_CONF_BUFFER_TYPE_NA | CELL_PERF_SETUP_CONF_COUNTER32;
    setup.interval = 0;

    pmu_fill_signals(&setup, profile);

    (void)0;

    ret = cellPerfAddCBEpm(&setup);
    if (ret < 0) {
        printf("[PMU] cellPerfAddCBEpm failed: 0x%08x\n", ret);
        return ret;
    }

    cellPerfStart();
    g_t0 = sys_time_get_system_time();
    g_active = 1;
    g_active_profile = profile;
    return 0;
}

int pmu_begin_spu(const pmu_profile_t *profile, uint32_t spu_thread_id_a, uint32_t spu_thread_id_b)
{
    CellPerfCBEpmSetup setup;
    int ret;

    if (!g_initialized || !profile || profile->count <= 0) return -1;
    if (g_active) return -1;

    if (spu_thread_id_a == 0) return -1;

    memset(&setup, 0, sizeof(setup));
    setup.conf = CELL_PERF_SETUP_CONF_NO_TRACE_MODE | CELL_PERF_SETUP_CONF_BUFFER_TYPE_NA | CELL_PERF_SETUP_CONF_COUNTER32;
    setup.interval = 0;

    pmu_fill_signals(&setup, profile);

    setup.spuTraceTarget[0].type     = CELL_PERF_SPU_TARGET_TYPE_THREAD_ID;
    setup.spuTraceTarget[0].threadId = (sys_spu_thread_t)spu_thread_id_a;

    setup.spuTraceTarget[1].type     = CELL_PERF_SPU_TARGET_TYPE_THREAD_ID;
    setup.spuTraceTarget[1].threadId = (sys_spu_thread_t)
        (spu_thread_id_b ? spu_thread_id_b : spu_thread_id_a);

    ret = cellPerfAddCBEpm(&setup);
    if (ret < 0) {
        printf("[PMU] cellPerfAddCBEpm(spu) failed: 0x%08x\n", ret);
        return ret;
    }

    cellPerfStart();
    g_t0 = sys_time_get_system_time();
    g_active = 1;
    g_active_profile = profile;
    return 0;
}

int pmu_end_and_read(pmu_result_t *out)
{
    CellPerfCBEpmCounter counters;
    system_time_t t1;
    int ret;
    int i;

    if (!out) return -1;
    out->ok = 0;
    memset(out->values, 0, sizeof(out->values));
    out->window_us = 0;

    if (!g_active) return -1;

    t1 = sys_time_get_system_time();
    cellPerfStop();
    out->window_us = (uint64_t)(t1 - g_t0);

    memset(&counters, 0, sizeof(counters));
    ret = cellPerfReadAndResetCBEpmCounter(&counters);

    cellPerfDeleteCBEpm();
    g_active = 0;
    g_active_profile = NULL;

    if (ret < 0) {
        printf("[PMU] cellPerfReadAndResetCBEpmCounter failed: 0x%08x\n", ret);
        return ret;
    }

    for (i = 0; i < PMU_MAX_EVENTS; i++)
        out->values[i] = counters.counter32[i];
    out->ok = 1;
    return 0;
}

void pmu_log_dump(const char *bench_name, const pmu_profile_t *profile, const pmu_result_t *r)
{
    int i;
    if (!r->ok) return;
    printf("[PMU] %-16s window=%6llu us", bench_name, (unsigned long long)r->window_us);
    for (i = 0; i < profile->count && i < PMU_MAX_EVENTS; i++) {
        printf(" %s=%u", profile->events[i].label, r->values[i]);
    }
    printf("\n");
}

#else /* !CELLMARK_DECR */

int  pmu_init(void) { return 0; }
int  pmu_is_active(void) { return 0; }

int  pmu_begin(const pmu_profile_t *profile)
{
    (void)profile;
    return 0;
}

int  pmu_begin_spu(const pmu_profile_t *profile, uint32_t spu_thread_id_a, uint32_t spu_thread_id_b)
{
    (void)profile; (void)spu_thread_id_a; (void)spu_thread_id_b;
    return 0;
}

int  pmu_end_and_read(pmu_result_t *out)
{
    if (out) {
        memset(out->values, 0, sizeof(out->values));
        out->window_us = 0;
        out->ok = 0;
    }
    return 0;
}

void pmu_log_dump(const char *bench_name, const pmu_profile_t *profile, const pmu_result_t *r)
{
    (void)bench_name; (void)profile; (void)r;
}

#endif // CELLMARK_DECR