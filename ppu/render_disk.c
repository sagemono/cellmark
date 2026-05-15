/*
 * render_disk.c - disk io benchmark page rendering
 *
 * 2 sub views:
 *   bench  - sequential and random 4K read/write results
 *   probe  - live results from the 13 probe diagnostic suite
 */

#include <stdio.h>

#include <cell/dbgfont.h>

#include "gcm.h"
#include "cellmark.h"
#include "disk_benchmark.h"
#include "render.h"

void render_disk_probe_view(double elapsed)
{
    const disk_probe_t *pr = disk_get_probes();
    int cur     = disk_probe_current();
    int running = disk_probes_running();
    int hours, mins, secs_i;
    char buf[128];
    int i;

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, "cellmark 1.0.0 by sagemono\n");
    cellDbgFontConsolePrintf(dbg_console, "Disk I/O Probe Suite | 32MB | %02d:%02d:%02d\n\n", hours, mins, secs_i);

    for (i = 0; i < DISK_PROBE_COUNT; i++) {
        const char *arrow = (running && i == cur) ? "->" : "  ";
        switch (pr[i].state) {
        case PROBE_ST_RUNNING:
            snprintf(buf, sizeof(buf), "%s %s  running...", arrow, pr[i].label);
            break;
        case PROBE_ST_DONE:
            snprintf(buf, sizeof(buf), "%s %s  %5.1f MB/s", arrow, pr[i].label, pr[i].mbps);
            break;
        case PROBE_ST_ERROR:
            snprintf(buf, sizeof(buf), "%s %s  ERROR", arrow, pr[i].label);
            break;
        default: /* PENDING */
            snprintf(buf, sizeof(buf), "%s %s  ---", arrow, pr[i].label);
            break;
        }
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    }

    cellDbgFontConsolePrintf(dbg_console, "\n");
    if (running)
        cellDbgFontConsolePrintf(dbg_console, "running probe %d of %d...\n", cur + 1, DISK_PROBE_COUNT);
    else
        cellDbgFontConsolePrintf(dbg_console, DISK_PROBE_COUNT > 0 && pr[DISK_PROBE_COUNT-1].state == PROBE_ST_DONE ? "all probes complete\n" : "X: run all probes\n");

    cellDbgFontConsolePrintf(dbg_console, "SQR:bench view  X:%s  L2/R2:page\n", running ? "running..." : "run all probes");
}

void render_disk_stats(double elapsed)
{
    const disk_bench_state_t *ds = disk_get_state();
    int hours, mins, secs_i;
    char buf[256];
    int i;
    static const char *bench_names[DISK_BENCH_COUNT] = {
        "Seq  Read 64KB", "Seq Write 64KB",
        "Rnd4K Read    ", "Rnd4K Write   "
    };
    static const char *status_str[] = {
        "---", "preparing...", "running...", "", "ERROR"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, "cellmark 1.0.0 by sagemono\n");
    cellDbgFontConsolePrintf(dbg_console, "Disk I/O | %02d:%02d:%02d | 32MB file\n\n", hours, mins, secs_i);

    for (i = 0; i < DISK_BENCH_COUNT; i++) {
        const disk_result_t *r = &ds->results[i];
        int is_selected = (i == current_disk_bench);
        int is_active   = is_selected && disk_is_running();
        const char *arrow = is_selected ? "->" : "  ";

        if (is_active) {
            snprintf(buf, sizeof(buf), "%s %s  %s", arrow, bench_names[i], status_str[r->status < 5 ? r->status : 2]);
        } else if (r->status == DISK_STATUS_DONE && r->mbps > 0.0f) {
            snprintf(buf, sizeof(buf), "%s %s  %.1f MB/s", arrow, bench_names[i], r->mbps);
        } else if (r->status == DISK_STATUS_ERROR) {
            snprintf(buf, sizeof(buf), "%s %s  ERROR (0x%x)", arrow, bench_names[i], (unsigned)r->last_error);
        } else {
            snprintf(buf, sizeof(buf), "%s %s  ---", arrow, bench_names[i]);
        }

        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    }

    cellDbgFontConsolePrintf(dbg_console, "\npost-fsync | X:%s\n", disk_is_running() ? "running..." : "run selected bench");
    cellDbgFontConsolePrintf(dbg_console, "L2/R2:page L1/R1:bench SQR:probes TRI:log[%s]\n", log_enabled ? "ON" : "off");
}