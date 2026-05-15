#include <stdio.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "eib_benchmark.h"
#include "atomic_benchmark.h"
#include "mbox_benchmark.h"
#include "branch_benchmark.h"
#include "sysmon.h"
#include "render.h"
#include "cellmark_version.h"

#define EIB_PEAK_GBPS         204.8f
#define EIB_DIR_PEAK_GBPS      25.6f
#define EIB_PAIR_PEAK_GBPS    (2.0f * EIB_DIR_PEAK_GBPS)
#define EIB_LS_PORT_PEAK_GBPS  25.6f

void render_eib_stats(double elapsed)
{
    const eib_results_summary_t *r = eib_get_results();
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1.0e9);
    int   hours, mins, secs_i;
    char  buf[256];
    int   i;
    static const char *bench_names[EIB_BENCH_COUNT] = {
        "Pairs (0<->1, 2<->3, 4<->5)",
        "Hotspot (1..5 -> SPE 0)",
        "Hotspot reader sweep (1..5 -> SPE 0)",
        "N x N matrix (30 ordered SPE pairs)",
        "Atomic cache ping-pong (2 SPE getllar/putllc)",
        "Mailbox ping-pong (PPE <-> SPE)",
        "SPE branch hint (hbrr) effectiveness"
    };

    hours  = (int)elapsed / 3600;
    mins   = ((int)elapsed % 3600) / 60;
    secs_i = (int)elapsed % 60;

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, CELLMARK_BANNER "\n");

    snprintf(buf, sizeof(buf), "Cell EIB BW | %.2f GHz | %02d:%02d:%02d | %s", tb_clock, hours, mins, secs_i, sysmon_get_status_string());
    cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);

    if (r->pairs_runs > 0) {
        float gbps     = r->pairs_get_mbps / 1000.0f;
        float per_pair = gbps / 3.0f;
        float per_dir  = per_pair / 2.0f;

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

    if (r->hotspot_runs > 0) {
        float gbps       = r->hotspot_mbps / 1000.0f;
        float per_reader = gbps / 5.0f;

        snprintf(buf, sizeof(buf), "Hotspot (5 -> SPE 0): %6.2f GB/s aggregate   runs=%u", gbps, r->hotspot_runs);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        snprintf(buf, sizeof(buf), "  per-reader: %.2f GB/s (%.1f GB/s into target LS port)", per_reader, gbps);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        snprintf(buf, sizeof(buf), "  LS port:    %.0f%% of %.1f GB/s single-direction ceiling", gbps / EIB_LS_PORT_PEAK_GBPS * 100.0f, EIB_LS_PORT_PEAK_GBPS);
        cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "Hotspot (5 -> SPE 0): ---\n\n");
    }
    {
        const eib_sweep_summary_t *sw = eib_get_sweep_summary();
        int any = 0;
        for (i = 0; i < EIB_SWEEP_COUNT; i++) if (sw->runs[i]) { any = 1; break; }
        if (any) {
            cellDbgFontConsolePrintf(dbg_console, "  Hot-spot reader sweep (N readers -> SPE 0):\n");
            cellDbgFontConsolePrintf(dbg_console, "    N    aggregate   per-reader   %%LSport  runs\n");
            for (i = 0; i < EIB_SWEEP_COUNT; i++) {
                int readers = i + 1;
                if (sw->runs[i] > 0) {
                    float gbps    = sw->mbps[i] / 1000.0f;
                    float per_rdr = gbps / (float)readers;
                    snprintf(buf, sizeof(buf), "    %d   %6.2f GB/s  %5.2f GB/s   %4.0f%%   %u%s", readers, gbps, per_rdr, gbps / EIB_LS_PORT_PEAK_GBPS * 100.0f, sw->runs[i], i == sw->active_idx && current_eib_bench == EIB_BENCH_HOTSPOT_SWEEP ? "  <-" : "");
                } else {
                    snprintf(buf, sizeof(buf), "    %d     ---          ---       ---   -%s", readers, i == sw->active_idx && current_eib_bench == EIB_BENCH_HOTSPOT_SWEEP ? "  <-" : "");
                }
                cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            }
            cellDbgFontConsolePrintf(dbg_console, "\n");
        }
    }
    {
        const eib_nxn_summary_t *m = eib_get_nxn_summary();
        int any = 0, s, d;
        for (s = 0; s < EIB_NXN_SPES && !any; s++)
            for (d = 0; d < EIB_NXN_SPES; d++)
                if (m->runs[s][d]) { any = 1; break; }
        if (any) {
            float mn = 1.0e30f, mx = 0.0f, sum = 0.0f;
            int   cnt = 0;
            cellDbgFontConsolePrintf(dbg_console, "  N x N EIB src->dst per-pair GB/s (* = current):\n");
            cellDbgFontConsolePrintf(dbg_console, "  src\\dst    0      1      2      3      4      5\n");
            for (s = 0; s < EIB_NXN_SPES; s++) {
                int pos = snprintf(buf, sizeof(buf), "       %d:", s);
                for (d = 0; d < EIB_NXN_SPES; d++) {
                    int active = (s == m->active_src && d == m->active_dst && current_eib_bench == EIB_BENCH_NXN);
                    if (s == d) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "    -  ");
                    } else if (m->runs[s][d] > 0 && m->mbps[s][d] > 0.0f) {
                        float g = m->mbps[s][d] / 1000.0f;
                        pos += snprintf(buf + pos, sizeof(buf) - pos, " %5.2f%c", g, active ? '*' : ' ');
                        if (g < mn) mn = g;
                        if (g > mx) mx = g;
                        sum += g; cnt++;
                    } else {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "  ...  ");
                    }
                }
                cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            }
            if (cnt > 0) {
                snprintf(buf, sizeof(buf), "    summary GB/s: min=%.2f  avg=%.2f  max=%.2f", mn, sum / (float)cnt, mx);
                cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            }
            cellDbgFontConsolePrintf(dbg_console, "\n");
        }
    }
    cellDbgFontConsolePrintf(dbg_console, "  16 KB MFC chunks, 8-deep ring per reader SPE\n\n");
    {
        const atomic_results_summary_t *ar = atomic_get_results();
        if (ar->runs > 0) {
            snprintf(buf, sizeof(buf), "Atomic-cache:  %.1f ns/bounce   %.2f M rounds/sec   runs=%u", ar->ns_per_bounce, ar->rounds_per_sec_M, ar->runs);
            cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            cellDbgFontConsolePrintf(dbg_console, "  2 SPEs ping-pong 128 B atomic line via getllar/putllc\n\n");
        }
    }
    {
        const mbox_results_summary_t *mr = mbox_get_results();
        if (mr->runs > 0) {
            snprintf(buf, sizeof(buf), "Mailbox PPE<->SPE:  %.0f ns roundtrip   %.0f ns one-way   %.2f M rounds/sec   runs=%u", mr->ns_per_roundtrip, mr->ns_one_way, mr->rounds_per_sec_M, mr->runs);
            cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            cellDbgFontConsolePrintf(dbg_console, "  PPE writes in-mbox, SPE reads + writes intr_mbox, PPE waits on event queue\n\n");
        } else if (mr->last_err_step != 0) {
            static const char *step_names[] = {
                "?", "image_import", "group_create", "thread_init", "queue_create",
                "connect_event", "group_start", "write_mb", "recv_event"
            };
            int s = mr->last_err_step;
            const char *n = (s >= 1 && s <= 8) ? step_names[s] : "?";
            snprintf(buf, sizeof(buf), "Mailbox PPE<->SPE:  FAILED step=%d (%s)  code=0x%08x  completed=%u/4096", s, n, (unsigned)mr->last_err_code, (unsigned)mr->completed_iters);
            cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
        }
    }
    {
        const branch_results_summary_t *br = branch_get_results();
        if (br->runs > 0) {
            snprintf(buf, sizeof(buf), "Branch hint:        hinted=%.2f cyc/iter   unhinted=%.2f cyc/iter   penalty=%.1f cyc   speedup=%.2fx   runs=%u", br->hinted_cyc_per_iter, br->unhinted_cyc_per_iter, br->mispredict_penalty_cyc, br->speedup, br->runs);
            cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            cellDbgFontConsolePrintf(dbg_console, "  Tight backward branch loop on one SPE; hbrr at loop top vs no hint (default predict-not-taken)\n\n");
        } else if (br->last_err_step != 0) {
            snprintf(buf, sizeof(buf), "Branch hint:        FAILED step=%d  code=0x%08x", br->last_err_step, (unsigned)br->last_err_code);
            cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
        }
    }
    {
        pmu_summary_t s;
        const pmu_profile_t *pp = eib_get_pmu_profile();
        eib_get_pmu_summary(&s);
        if (s.ok && pp) {
            uint64_t total_eib_cyc = s.med_window_us * (uint64_t)(tb_frequency * 20 / 1000000);
            int j;
            int pos = snprintf(buf, sizeof(buf), "PMU EIB n=%d ~%llums", s.sample_count, (unsigned long long)(s.med_window_us / 1000));
            for (j = 0; j < pp->count && j < PMU_MAX_EVENTS && pos < (int)sizeof(buf); j++) {
                float pct = total_eib_cyc ? (100.0f * (float)s.med[j] / (float)total_eib_cyc) : 0.0f;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s=%.0f%%", pp->events[j].label, pct);
            }
            cellDbgFontConsolePrintf(dbg_console, "%s\n\n", buf);
        }
    }

    for (i = 0; i < EIB_BENCH_COUNT; i++) {
        cellDbgFontConsolePrintf(dbg_console, "%s %s\n", i == current_eib_bench ? "->" : "  ", bench_names[i]);
    }

    cellDbgFontConsolePrintf(dbg_console, "\nL2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n", log_enabled ? "ON" : "off");
}