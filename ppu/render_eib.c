#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "eib_benchmark.h"
#include "render.h"

#define EIB_PEAK_GBPS   204.8f
#define EIB_DIR_PEAK_GBPS  25.6f
#define EIB_PAIR_PEAK_GBPS  (2.0f * EIB_DIR_PEAK_GBPS)

void render_eib_stats(double elapsed)
{
    const eib_results_summary_t *r = eib_get_results();
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1.0e9);
    int   hours, mins, secs_i;
    char  buf[256];
    int   i;
    static const char *bench_names[EIB_BENCH_COUNT] = {
        "Pairs (0<->1, 2<->3, 4<->5)"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, "cellmark 1.0.0 by sagemono\n");

    snprintf(buf, sizeof(buf), "Cell EIB BW | %.2f GHz | %02d:%02d:%02d", tb_clock, hours, mins, secs_i);
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    if (r->pairs_runs > 0) {
        float gbps     = r->pairs_get_mbps / 1000.0f;
        float per_pair = gbps / 3.0f;
        float per_dir  = per_pair / 2.0f;  // each pair runs two flosw

        snprintf(buf, sizeof(buf), "Pairs GET (LS<->LS):  %6.2f GB/s aggregate   runs=%u", gbps, r->pairs_runs);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        snprintf(buf, sizeof(buf), "  per-pair: %.2f GB/s (%.0f%% of %.1f bidirectional ceiling)", per_pair, per_pair / EIB_PAIR_PEAK_GBPS * 100.0f, EIB_PAIR_PEAK_GBPS);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        snprintf(buf, sizeof(buf), "  per-dir:  %.2f GB/s (%.0f%% of %.1f single-ring ceiling)", per_dir, per_dir / EIB_DIR_PEAK_GBPS * 100.0f, EIB_DIR_PEAK_GBPS);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        snprintf(buf, sizeof(buf), "  aggregate: %.1f%% of %.1f GB/s EIB sustained max", gbps / EIB_PEAK_GBPS * 100.0f, EIB_PEAK_GBPS);
        cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "Pairs GET (LS<->LS):  ---\n\n");
    }

    for (i = 0; i < EIB_BENCH_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n",
            i == current_eib_bench ? "->" : "  ",
            bench_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
}