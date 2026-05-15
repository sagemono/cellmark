/*
 * render_ppe.c - PPE core benchmark page rendering
 */

#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "ppe_benchmarks.h"
#include "render.h"

void render_ppe_stats(double elapsed)
{
    const ppe_results_t *r = ppe_get_results();
    float tb_clock  = (float)((double)tb_frequency * 40.0 / 1.0e9);
    float peak_vmx  = (float)(STOCK_TB_FREQ * 40) / 1.0e9f * PEAK_PPE_VMX;
    int   hours, mins, secs_i;
    char  buf[256];
    int   i;
    static const char *bench_names[PPE_BENCH_COUNT] = {
        "VMX FMA", "L1 Read BW", "L2 Read BW", "L1 Latency", "L2 Latency"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, "cellmark 1.0.0 by sagemono\n");

    snprintf(buf, sizeof(buf), "PPE Core | %.2f GHz | %02d:%02d:%02d", tb_clock, hours, mins, secs_i);
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
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    for (i = 0; i < PPE_BENCH_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n", i == current_ppe_bench ? "->" : "  ", bench_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console,
        "\nL2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n",
        log_enabled ? "ON" : "off");
}