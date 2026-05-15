/* render_burn.c - all-units saturation burn-in page
 *
 * live display only. all measurements come from burn_benchmark.c's EMA smoothed summary
 * the page itself doesn't compute anything, it's a window onto the running burn-in
 */

#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "burn_benchmark.h"
#include "sysmon.h"
#include "render.h"
#include "cellmark_version.h"

static void bar(char *buf, size_t bufsz, float frac)
{
    const int width = 20;
    int filled, i;
    int pos = 0;
    if (frac < 0.0f) frac = 0.0f;
    filled = (int)(frac * (float)width + 0.5f);
    if (filled > width) filled = width;
    if (pos < (int)bufsz - 1) buf[pos++] = '[';
    for (i = 0; i < width && pos < (int)bufsz - 2; i++)
        buf[pos++] = (i < filled) ? '#' : '.';
    if (pos < (int)bufsz - 1) buf[pos++] = ']';
    buf[pos] = '\0';
}

void render_burn_stats(double elapsed)
{
    const burn_results_summary_t *r = burn_get_results();
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1.0e9);
    int   hours, mins, secs_i;
    char  buf[256];
    char  bar_buf[64];

    (void)elapsed;

    hours  = (int)r->elapsed_sec / 3600;
    mins   = ((int)r->elapsed_sec % 3600) / 60;
    secs_i = (int)r->elapsed_sec % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, CELLMARK_BANNER "\n");

    snprintf(buf, sizeof(buf), "BURN-IN | %.2f GHz | running %02d:%02d:%02d | %s", tb_clock, hours, mins, secs_i, sysmon_get_status_string());
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    if (r->last_err_code != 0) {
        cellDbgFontConsolePrintf(dbg_console, "FAIL step=%d code=0x%08x\n", r->last_err_step, (unsigned)r->last_err_code);
        cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
        return;
    }

    cellDbgFontConsolePrintf(dbg_console, "All Cell subsystems running simultaneously:\n");
    cellDbgFontConsolePrintf(dbg_console, "  4 SPEs : continuous SP-FMA grind\n");
    cellDbgFontConsolePrintf(dbg_console, "  2 SPEs : continuous XDR streaming DMA (16 MB ring)\n");
    cellDbgFontConsolePrintf(dbg_console, "  PPE TH0: inline VMX FMA between display frames\n\n");

    if (!r->running) {
        cellDbgFontConsolePrintf(dbg_console, "(starting...)\n");
        cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
        return;
    }

    bar(bar_buf, sizeof(bar_buf), r->spe_compute_util);
    snprintf(buf, sizeof(buf), "SPE compute: %6.1f GFLOPS  %s %5.1f%%  (peak 102.4 GFLOPS / 4 SPEs)", r->spe_compute_gflops, bar_buf, r->spe_compute_util * 100.0f);
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    bar(bar_buf, sizeof(bar_buf), r->spe_dma_util);
    snprintf(buf, sizeof(buf), "SPE XDR DMA: %6.2f GB/s    %s %5.1f%%  (peak  ~8.5 GB/s  / 2 SPEs)", r->spe_dma_gbps, bar_buf, r->spe_dma_util * 100.0f);
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    bar(bar_buf, sizeof(bar_buf), r->ppe_vmx_util);
    snprintf(buf, sizeof(buf), "PPE VMX FMA: %6.2f GFLOPS  %s %5.1f%%  (peak ~12.0 GFLOPS / PPE TH0)", r->ppe_vmx_gflops, bar_buf, r->ppe_vmx_util * 100.0f);
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    {
        float frac = r->saturation_score / 100.0f;
        bar(bar_buf, sizeof(bar_buf), frac);
        snprintf(buf, sizeof(buf), "Saturation:  %6.1f       %s  (geomean of 3 utilisations)", r->saturation_score, bar_buf);
        cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
    }

    cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
}