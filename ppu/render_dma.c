// render_dma.c - cell DMA bandwidth page rendering
//
// shows aggregate xdr <-> ls throughput across all 6 SPEs. ceiling is the
// 25.6 GB/s xdr dram bus; GET typically lands close to that, PUT
// 10-20% lower because of the xdr write coalescer

#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "dma_benchmark.h"
#include "render.h"

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
        "GET (XDR -> LS)", "PUT (LS -> XDR)"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, "cellmark 1.0.0 by sagemono\n");

    snprintf(buf, sizeof(buf), "Cell DMA BW | %.2f GHz | %02d:%02d:%02d", tb_clock, hours, mins, secs_i);
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

    cellDbgFontConsolePrintf(dbg_console, "  6 SPEs * 1 MB XDR slice  /  16 KB MFC chunks, double-buffered\n");
    cellDbgFontConsolePrintf(dbg_console, "  256 MB transferred per SPE per batch\n\n");

    for (i = 0; i < DMA_BENCH_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n",
            i == current_dma_bench ? "->" : "  ",
            bench_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console,
        "\nL2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n",
        log_enabled ? "ON" : "off");
}