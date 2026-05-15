#include <stdio.h>
#include <string.h>

#include <sys/time_util.h>
#include <cell/perf/performance.h>

#include "stress_common.h"
#include "cellmark.h"
#include "spu.h"
#include "pmu.h"
#include "cell_pmu.h"

#define SAMPLE_INTERVAL_MS  1000
#define SAMPLE_WINDOW_MS     100

static const pmu_event_t s_events[] = {
    { CELL_PERF_SIGNAL_SPU_DUAL_INSTRUCTION_COMPLETED,  0, 0, "DUAL"  },
    { CELL_PERF_SIGNAL_SPU_PIPE0_INSTRUCTION_COMPLETED, 0, 0, "PIPE0" },
    { CELL_PERF_SIGNAL_SPU_PIPE1_INSTRUCTION_COMPLETED, 0, 0, "PIPE1" },
    { CELL_PERF_SIGNAL_SPU_BRANCH_HINT_MISS_PREDICTION, 0, 0, "HMISS" },
};

static const pmu_profile_t s_profile = {
    s_events,
    sizeof(s_events) / sizeof(s_events[0]),
    -1, -1
};

enum { ST_IDLE, ST_MEASURING };

static int           s_state          = ST_IDLE;
static uint64_t      s_state_start_tb = 0;
static pmu_result_t  s_result;
static pmu_history_t s_history;
static int           s_history_kernel = -1;
static uint32_t      s_log_counter    = 0;

static const char *kernel_short_name(int mode_id)
{
    switch (mode_id) {
    case MODE_COMPUTE_SP:    return "spu_sp_fma";
    case MODE_COMPUTE_DP:    return "spu_dp_fma";
    case MODE_COMPUTE_INT:   return "spu_int";
    case MODE_COMPUTE_RECIP: return "spu_recip";
    case MODE_COMPUTE_SHUF:  return "spu_shuf";
    case MODE_COMPUTE_DUAL:  return "spu_dual";
    case MODE_MEMTEST:       return "spu_memtest";
    default:                 return "spu";
    }
}

void cell_pmu_tick(int active, int current_kernel_id)
{
    uint64_t now_tb;
    double   elapsed_ms;

    SYS_TIMEBASE_GET(now_tb);
    elapsed_ms = (double)(now_tb - s_state_start_tb) * 1000.0 / (double)tb_frequency;

    if (!active) {
        if (s_state == ST_MEASURING) {
            (void)pmu_end_and_read(&s_result);   // discard partial
        }
        s_state = ST_IDLE;
        s_state_start_tb = now_tb;
        return;
    }

    if (current_kernel_id != s_history_kernel) {
        memset(&s_history, 0, sizeof(s_history));
        s_history_kernel = current_kernel_id;
        s_log_counter = 0;
    }

    switch (s_state) {
    case ST_IDLE:
        if (elapsed_ms >= (double)(SAMPLE_INTERVAL_MS - SAMPLE_WINDOW_MS)) {
            uint32_t spu_id = spu_get_thread(0);
            if (spu_id != 0
                && pmu_begin_spu(&s_profile, spu_id, 0) == 0) {
                s_state = ST_MEASURING;
                s_state_start_tb = now_tb;
            } else {
                s_state_start_tb = now_tb;
            }
        }
        break;

    case ST_MEASURING:
        if (elapsed_ms >= (double)SAMPLE_WINDOW_MS) {
            (void)pmu_end_and_read(&s_result);
            if (s_result.ok) {
                pmu_history_push(&s_history, &s_result);
                s_log_counter++;
                if ((s_log_counter & 0x7) == 0) {
                    pmu_summary_t s;
                    pmu_history_summary(&s_history, &s);
                    pmu_log_dump_summary(kernel_short_name(current_kernel_id), &s_profile, &s);
                }
            }
            s_state = ST_IDLE;
            s_state_start_tb = now_tb;
        }
        break;
    }
}

void cell_pmu_get_summary(pmu_summary_t *out)
{
    pmu_history_summary(&s_history, out);
}

const pmu_profile_t *cell_pmu_get_profile(void)
{
    return &s_profile;
}

int cell_pmu_get_kernel_id(void)
{
    return s_history_kernel;
}