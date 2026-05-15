#ifndef PMU_H
#define PMU_H

#include <stdint.h>

#define PMU_MAX_EVENTS  4

typedef struct {
    uint32_t    signal_name;
    uint32_t    target_slot;    // 0 or 1; only relevant for SPC group signals
    uint32_t    mask;           // dma_tag_mask / spu_event_mask, 0 = unused
    const char *label;          // short display label
} pmu_event_t;

typedef struct {
    const pmu_event_t *events;
    int                count;
    int                spe_logical_a;   // -1 = unused; logical SPE id within current group bound to slot 0
    int                spe_logical_b;   // -1 = unused; bound to slot 1
    const char        *name;            // optional short label for the UI when rotating profiles ("issue", "branch", "mem"). NULL = unlabelled
} pmu_profile_t;

typedef struct {
    uint32_t values[PMU_MAX_EVENTS];
    uint64_t window_us;             // wall time the counters were live
    int      ok;                    // 0 = uninitialised / failed
} pmu_result_t;

int  pmu_init(void);
int  pmu_begin(const pmu_profile_t *profile);
int  pmu_begin_spu(const pmu_profile_t *profile, uint32_t spu_thread_id_a, uint32_t spu_thread_id_b);
int  pmu_end_and_read(pmu_result_t *out);
void pmu_log_dump(const char *bench_name, const pmu_profile_t *profile, const pmu_result_t *r);
int  pmu_is_active(void);

#define PMU_HISTORY_DEPTH   32

typedef struct {
    uint32_t samples[PMU_HISTORY_DEPTH][PMU_MAX_EVENTS];
    uint64_t window_us[PMU_HISTORY_DEPTH];
    int      head;
    int      filled;
} pmu_history_t;

typedef struct {
    uint32_t mn[PMU_MAX_EVENTS];
    uint32_t med[PMU_MAX_EVENTS];   // p50
    uint32_t mx[PMU_MAX_EVENTS];
    uint64_t med_window_us;
    int      sample_count;
    int      ok;
} pmu_summary_t;

void pmu_history_push(pmu_history_t *h, const pmu_result_t *r);
void pmu_history_summary(const pmu_history_t *h, pmu_summary_t *out);

void pmu_log_dump_summary(const char *bench_name, const pmu_profile_t *profile, const pmu_summary_t *s);

#endif // PMU_H