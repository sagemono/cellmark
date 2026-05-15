/*
 * render_ppe.c - PPE core benchmark page rendering
 */

#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "ppe_benchmarks.h"
#include "sysmon.h"
#include "render.h"
#include "cellmark_version.h"

void render_ppe_stats(double elapsed)
{
    const ppe_results_t *r = ppe_get_results();
    float tb_clock  = (float)((double)tb_frequency * 40.0 / 1.0e9);
    float peak_vmx       = (float)(STOCK_TB_FREQ * 40) / 1.0e9f * PEAK_PPE_VMX;
    float peak_fp_scalar = (float)(STOCK_TB_FREQ * 40) / 1.0e9f * PEAK_PPE_FP_SCALAR;
    float peak_fxu       = (float)(STOCK_TB_FREQ * 40) / 1.0e9f * PEAK_PPE_FXU;
    int   hours, mins, secs_i;
    char  buf[256];
    int   i;
    static const char *bench_names[PPE_BENCH_COUNT] = {
        "VMX FMA", "L1 Read BW", "L2 Read BW", "L1 Latency", "L2 Latency",
        "FP Scalar FMA", "FXU Integer Add", "SMT Scaling (TH0+TH1)"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, CELLMARK_BANNER "\n");

    snprintf(buf, sizeof(buf), "PPE Core | %.2f GHz | %02d:%02d:%02d | %s", tb_clock, hours, mins, secs_i, sysmon_get_status_string());
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    if (r->vmx_gflops > 0.0f)
        snprintf(buf, sizeof(buf), "VMX FMA:    %.2f / %.1f GFLOPS  (%.1f%%)", r->vmx_gflops, peak_vmx, r->vmx_gflops / peak_vmx * 100.0f);
    else
        snprintf(buf, sizeof(buf), "VMX FMA:    ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->l1_gbps > 0.0f)
        snprintf(buf, sizeof(buf), "L1 Read BW: %.2f GB/s  (28KB)", r->l1_gbps);
    else
        snprintf(buf, sizeof(buf), "L1 Read BW: ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->l2_gbps > 0.0f)
        snprintf(buf, sizeof(buf), "L2 Read BW: %.2f GB/s  (400KB)", r->l2_gbps);
    else
        snprintf(buf, sizeof(buf), "L2 Read BW: ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->l1_lat_ns > 0.0f)
        snprintf(buf, sizeof(buf), "L1 Latency: %.1f ns  (14KB chase)", r->l1_lat_ns);
    else
        snprintf(buf, sizeof(buf), "L1 Latency: ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->l2_lat_ns > 0.0f)
        snprintf(buf, sizeof(buf), "L2 Latency: %.1f ns  (256KB chase)", r->l2_lat_ns);
    else
        snprintf(buf, sizeof(buf), "L2 Latency: ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->fp_scalar_gflops > 0.0f)
        snprintf(buf, sizeof(buf), "FP Scalar:  %.2f / %.1f GFLOPS  (%.1f%%)  double fmadd", r->fp_scalar_gflops, peak_fp_scalar, r->fp_scalar_gflops / peak_fp_scalar * 100.0f);
    else
        snprintf(buf, sizeof(buf), "FP Scalar:  ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->fxu_gops > 0.0f)
        snprintf(buf, sizeof(buf), "FXU Add:    %.2f / %.1f GOPS    (%.1f%%)  int64 add",  r->fxu_gops, peak_fxu,  r->fxu_gops / peak_fxu * 100.0f);
    else
        snprintf(buf, sizeof(buf), "FXU Add:    ---");
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->smt_runs > 0) {
        snprintf(buf, sizeof(buf), "SMT same:   %.2f GFLOPS  scale=%.2fx  (FP+FP, one FPU pipe)", r->smt_same_aggr_gflops, r->smt_same_scaling);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
        snprintf(buf, sizeof(buf), "SMT cross:  %.2f GFLOPS + %.2f GOPS  scale=%.2fx  (FP|FXU)", r->smt_cross_fp_gflops, r->smt_cross_fxu_gops, r->smt_cross_scaling);
        cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "SMT:        --- (select 'SMT Scaling' to run)\n\n");
    }
    {
        pmu_summary_t s;
        const pmu_profile_t *pp = ppe_get_pmu_profile();
        ppe_get_pmu_summary(&s);
        if (s.ok && pp) {
            int j;
            int pos = snprintf(buf, sizeof(buf), "PMU [%s] n=%d ~%llums max(min)", pp->name ? pp->name : "?", s.sample_count, (unsigned long long)(s.med_window_us / 1000));
            for (j = 0; j < pp->count && j < PMU_MAX_EVENTS && pos < (int)sizeof(buf); j++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s=%u(%u)", pp->events[j].label, s.mx[j], s.mn[j]);
            }
            cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
        }
    }

    for (i = 0; i < PPE_BENCH_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n", i == current_ppe_bench ? "->" : "  ", bench_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console,
        "\nL2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n",
        log_enabled ? "ON" : "off");
}