#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "dma_benchmark.h"
#include "sysmon.h"
#include "render.h"
#include "cellmark_version.h"

// xdr dram bus ceiling, single channel. agg ceiling for the 6-SPE batch
#define XDR_PEAK_GBPS   25.6f

void render_dma_stats(double elapsed)
{
    const dma_results_summary_t *r = dma_get_results();
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1.0e9);
    int   hours, mins, secs_i;
    char  buf[256];
    int   i;
    static const char *bench_names[DMA_BENCH_COUNT] = {
        "GET (XDR -> LS)",
        "PUT (LS -> XDR)",
        "Size sweep (256 B .. 16 KB)"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, CELLMARK_BANNER "\n");

    snprintf(buf, sizeof(buf), "Cell DMA BW | %.2f GHz | %02d:%02d:%02d | %s", tb_clock, hours, mins, secs_i, sysmon_get_status_string());
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    if (r->get_runs > 0) {
        float gbps = r->get_mbps / 1000.0f;
        snprintf(buf, sizeof(buf), "GET (XDR -> LS):  %6.2f GB/s  (%.1f%% of %.1f GB/s)  runs=%u", gbps, gbps / XDR_PEAK_GBPS * 100.0f, XDR_PEAK_GBPS, r->get_runs);
    } else {
        snprintf(buf, sizeof(buf), "GET (XDR -> LS):  ---");
    }
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (r->put_runs > 0) {
        float gbps = r->put_mbps / 1000.0f;
        snprintf(buf, sizeof(buf), "PUT (LS -> XDR):  %6.2f GB/s  (%.1f%% of %.1f GB/s)  runs=%u", gbps, gbps / XDR_PEAK_GBPS * 100.0f, XDR_PEAK_GBPS, r->put_runs);
    } else {
        snprintf(buf, sizeof(buf), "PUT (LS -> XDR):  ---");
    }
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    cellDbgFontConsolePrintf(dbg_console, "  6 SPEs * 1 MB XDR slice  /  16 KB MFC chunks, 8-deep ring per SPE\n");
    cellDbgFontConsolePrintf(dbg_console, "  256 MB transferred per SPE per batch\n\n");

    {
        const dma_sweep_summary_t *sw = dma_get_sweep_summary();
        int any = 0;
        for (i = 0; i < DMA_SWEEP_COUNT; i++) if (sw->runs[i]) { any = 1; break; }
        if (any) {
            cellDbgFontConsolePrintf(dbg_console, "  GET chunk-size sweep:\n");
            cellDbgFontConsolePrintf(dbg_console, "    chunk    GB/s   %%XDR  runs\n");
            for (i = 0; i < DMA_SWEEP_COUNT; i++) {
                uint32_t cs = dma_sweep_chunk_sizes[i];
                char     cs_label[16];
                if (cs < 1024u) snprintf(cs_label, sizeof(cs_label), "%4u B", (unsigned)cs);
                else            snprintf(cs_label, sizeof(cs_label), "%4u KB", (unsigned)(cs / 1024u));

                if (sw->runs[i] > 0) {
                    float gbps = sw->mbps[i] / 1000.0f;
                    snprintf(buf, sizeof(buf), "    %-6s %6.2f  %4.0f%%  %u%s", cs_label, gbps, gbps / XDR_PEAK_GBPS * 100.0f, sw->runs[i], i == sw->active_idx && current_dma_bench == DMA_BENCH_SIZESWEEP ? "  <-" : "");
                } else {
                    snprintf(buf, sizeof(buf), "    %-6s   ---     ---  -%s", cs_label, i == sw->active_idx && current_dma_bench == DMA_BENCH_SIZESWEEP ? "  <-" : "");
                }
                cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            }
            cellDbgFontConsolePrintf(dbg_console, "\n");
        }
    }
    {
        pmu_summary_t s;
        const pmu_profile_t *pp = dma_get_pmu_profile();
        dma_get_pmu_summary(&s);
        if (s.ok && pp) {
            int j;
            int pos = snprintf(buf, sizeof(buf), "PMU SPE0 n=%d ~%llums", s.sample_count, (unsigned long long)(s.med_window_us / 1000));
            for (j = 0; j < pp->count && j < PMU_MAX_EVENTS && pos < (int)sizeof(buf); j++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s=%u", pp->events[j].label, s.med[j]);
            }
            cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
        }
    }

    for (i = 0; i < DMA_BENCH_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n", i == current_dma_bench ? "->" : "  ", bench_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
}