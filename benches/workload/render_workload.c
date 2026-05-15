#include <stdio.h>
#include <math.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "pi_benchmark.h"
#include "fft_benchmark.h"
#include "nbody_benchmark.h"
#include "sysmon.h"
#include "render.h"
#include "cellmark_version.h"

#define PI_REF_DIGITS_PER_SEC   40.0f
#define FFT_REF_POINTS_PER_SEC  184.82e6f
#define NBODY_REF_MPAIRS_PER_SEC  2195.3f

static const char *workload_names[WORKLOAD_COUNT] = {
    "Pi (BBP digit extract)",
    "FFT (radix-2 complex SP, N=1024)",
    "N-body (gravitational, N=4080)",
};

static float pi_score(const pi_results_summary_t *r)
{
    if (r->digits_per_sec <= 0.0f) return 0.0f;
    return r->digits_per_sec / PI_REF_DIGITS_PER_SEC * 100.0f;
}

static float fft_score(const fft_results_summary_t *r)
{
    if (r->points_per_sec <= 0.0f) return 0.0f;
    return r->points_per_sec / FFT_REF_POINTS_PER_SEC * 100.0f;
}

static float nbody_score(const nbody_results_summary_t *r)
{
    if (r->mpairs_per_sec <= 0.0f) return 0.0f;
    return r->mpairs_per_sec / NBODY_REF_MPAIRS_PER_SEC * 100.0f;
}

static void render_pi_block(const pi_results_summary_t *pi)
{
    cellDbgFontConsolePrintf(dbg_console, "[Pi BBP] hex digit extraction, 6-SPE parallel\n");
    cellDbgFontConsolePrintf(dbg_console, "  digit position: %u\n", (unsigned)PI_DEFAULT_START);
    cellDbgFontConsolePrintf(dbg_console, "  batch size:     %u digits (%u per SPE)\n", (unsigned)PI_DIGITS_PER_BATCH, (unsigned)(PI_DIGITS_PER_BATCH / PI_NUM_SPES));
    if (pi->runs > 0u) {
        cellDbgFontConsolePrintf(dbg_console, "  throughput:     %.2f digits/sec\n", pi->digits_per_sec);
        cellDbgFontConsolePrintf(dbg_console, "  batch time:     %.1f ms\n", pi->ms_per_batch);
        cellDbgFontConsolePrintf(dbg_console, "  total computed: %u digits over %u batches\n", (unsigned)pi->total_digits_run, (unsigned)pi->runs);
        cellDbgFontConsolePrintf(dbg_console, "  digits[N..]:    %s\n", pi->last_digits_ascii);
        cellDbgFontConsolePrintf(dbg_console, "  score:          %.1f  (ref=%.0f digits/sec)\n\n", pi_score(pi), PI_REF_DIGITS_PER_SEC);
    } else if (pi->last_err_code != 0) {
        cellDbgFontConsolePrintf(dbg_console, "  FAIL step=%d code=0x%08x\n\n", pi->last_err_step, (unsigned)pi->last_err_code);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "  (warming up)\n\n");
    }
}

static void render_fft_block(const fft_results_summary_t *r)
{
    cellDbgFontConsolePrintf(dbg_console, "[FFT] 1D radix-2 complex SP, N=%u, 6-SPE parallel\n", (unsigned)FFT_N);
    cellDbgFontConsolePrintf(dbg_console, "  batch size:     %u FFTs/SPE x %u SPEs = %u\n", (unsigned)FFT_FFTS_PER_SPE, (unsigned)FFT_NUM_SPES, (unsigned)(FFT_FFTS_PER_SPE * FFT_NUM_SPES));
    cellDbgFontConsolePrintf(dbg_console, "  self-test:      %s  (max bin error %.2e)\n", r->self_test_ok ? "PASS" : "FAIL", (double)r->self_test_max_err);
    if (r->runs > 0u) {
        cellDbgFontConsolePrintf(dbg_console, "  throughput:     %.2f MFFTs/sec  (%.2f Mpoints/sec)\n", r->ffts_per_sec / 1.0e6f, r->points_per_sec / 1.0e6f);
        cellDbgFontConsolePrintf(dbg_console, "  batch time:     %.1f ms\n", r->ms_per_batch);
        cellDbgFontConsolePrintf(dbg_console, "  total computed: %u FFTs over %u batches\n", (unsigned)r->total_ffts_run, (unsigned)r->runs);
        cellDbgFontConsolePrintf(dbg_console, "  score:          %.1f  (ref=%.0f Mpoints/sec)\n\n", fft_score(r), FFT_REF_POINTS_PER_SEC / 1.0e6f);
    } else if (r->last_err_code != 0) {
        cellDbgFontConsolePrintf(dbg_console, "  FAIL step=%d code=0x%08x\n\n", r->last_err_step, (unsigned)r->last_err_code);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "  (warming up)\n\n");
    }
}

static void render_nbody_block(const nbody_results_summary_t *r)
{
    cellDbgFontConsolePrintf(dbg_console, "[N-body] all-pairs gravitational, N=%u, 6-SPE parallel\n", (unsigned)NBODY_N);
    cellDbgFontConsolePrintf(dbg_console, "  iterations/batch: %u\n", (unsigned)NBODY_ITERATIONS);
    cellDbgFontConsolePrintf(dbg_console, "  bodies/SPE:       %u\n", (unsigned)NBODY_BODIES_PER_SPE);
    if (r->runs > 0u) {
        cellDbgFontConsolePrintf(dbg_console, "  throughput:       %.1f Mpairs/sec\n", r->mpairs_per_sec);
        cellDbgFontConsolePrintf(dbg_console, "  batch time:       %.1f ms\n", r->ms_per_batch);
        cellDbgFontConsolePrintf(dbg_console, "  checksum:         %.4e  (deterministic; stable across batches)\n", (double)r->last_checksum);
        cellDbgFontConsolePrintf(dbg_console, "  total computed:   %u Mpairs over %u batches\n", (unsigned)r->total_pairs_run, (unsigned)r->runs);
        cellDbgFontConsolePrintf(dbg_console, "  score:            %.1f  (ref=%.0f Mpairs/sec)\n\n", nbody_score(r), NBODY_REF_MPAIRS_PER_SEC);
    } else if (r->last_err_code != 0) {
        cellDbgFontConsolePrintf(dbg_console, "  FAIL step=%d code=0x%08x\n\n", r->last_err_step, (unsigned)r->last_err_code);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "  (warming up)\n\n");
    }
}

void render_workload_stats(double elapsed)
{
    const pi_results_summary_t    *pi    = pi_get_results();
    const fft_results_summary_t   *fft   = fft_get_results();
    const nbody_results_summary_t *nbody = nbody_get_results();
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1.0e9);
    int   hours, mins, secs_i;
    char  buf[256];
    int   i;

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, CELLMARK_BANNER "\n");

    snprintf(buf, sizeof(buf), "Real Workloads | %.2f GHz | %02d:%02d:%02d | %s", tb_clock, hours, mins, secs_i, sysmon_get_status_string());
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    if (current_workload == WORKLOAD_PI)         render_pi_block(pi);
    else if (current_workload == WORKLOAD_FFT)   render_fft_block(fft);
    else if (current_workload == WORKLOAD_NBODY) render_nbody_block(nbody);

    {
        float scores[WORKLOAD_COUNT];
        int   active = 0;
        double prod  = 1.0;

        scores[WORKLOAD_PI]    = pi_score(pi);
        scores[WORKLOAD_FFT]   = fft_score(fft);
        scores[WORKLOAD_NBODY] = nbody_score(nbody);
        for (i = 0; i < WORKLOAD_COUNT; i++) {
            if (scores[i] > 0.0f) {
                prod *= (double)scores[i];
                active++;
            }
        }
        if (active > 0) {
            float composite = (float)pow(prod, 1.0 / (double)active);
            cellDbgFontConsolePrintf(dbg_console, "Composite score: %.1f  (geometric mean of %d workload%s)\n", composite, active, active == 1 ? "" : "s");
            cellDbgFontConsolePrintf(dbg_console, "  Pi: %.1f   FFT: %.1f   N-body: %.1f\n\n", scores[WORKLOAD_PI], scores[WORKLOAD_FFT], scores[WORKLOAD_NBODY]);
        } else {
            cellDbgFontConsolePrintf(dbg_console, "Composite score: --- (waiting for first batch)\n\n");
        }
    }

    for (i = 0; i < WORKLOAD_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n", i == current_workload ? "->" : "  ", workload_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page L1/R1:workload TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
}