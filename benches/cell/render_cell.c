/*
 * render_cell.c - Cell benchmark page rendering
 *
 * horribly messy file, this is what you get using a TUI
 *
 * render two subviews:
 *   compute view  - per-SPE GFLOPS/GIOPS/GOPS, efficiency vs. stock peak
 *   memtest view  - test progress, pass/fail grid, bandwidth/latency results
 */

#include <stdio.h>
#include <stdint.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "spu.h"
#include "cellmark.h"
#include "cell_pmu.h"
#include "render.h"

// theoretical SPE peak at 3.2 GHz stock clock
float get_peak_ops_stock(void)
{
    float stock_ghz = (float)(STOCK_TB_FREQ * 40) / 1e9f;
    float ops_per_cyc;

    switch (current_mode) {
    case MODE_COMPUTE_DP:    ops_per_cyc = PEAK_OPS_DP_FMA;   break;
    case MODE_COMPUTE_INT:   ops_per_cyc = PEAK_OPS_INT_MUL;  break;
    case MODE_COMPUTE_RECIP: ops_per_cyc = PEAK_OPS_RECIP;    break;
    case MODE_COMPUTE_SHUF:  ops_per_cyc = PEAK_OPS_SHUFFLE;  break;
    case MODE_COMPUTE_DUAL:  ops_per_cyc = PEAK_OPS_DUAL;     break;
    default:                 ops_per_cyc = PEAK_OPS_SP_FMA;   break;
    }

    return stock_ghz * ops_per_cyc * (float)num_spes_active;
}

const char *ops_unit_name(void)
{
    switch (current_mode) {
    case MODE_COMPUTE_SP:
    case MODE_COMPUTE_DP:  return "GFLOPS";
    case MODE_COMPUTE_INT: return "GIOPS";
    default:               return "GOPS";
    }
}

void render_compute_stats(double elapsed)
{
    float total_gops = 0.0f;
    uint32_t total_errors = 0;
    int all_stable = 1;
    int i;
    char buf[256];
    const char *unit = ops_unit_name();

    for (i = 0; i < num_spes_active; i++) {
        if (spe_results[i].verify_errors > 0)
            all_stable = 0;
        total_errors += spe_results[i].verify_errors;
    }

    if (elapsed > 0.01) {
        for (i = 0; i < num_spes_active; i++) {
            total_gops += (float)((double)spe_results[i].total_flops / (elapsed * 1e9));
        }
    }

    float peak_gops  = get_peak_ops_stock();
    float actual_ghz = (float)((double)tb_frequency * 40.0 / 1e9);
    float efficiency = 0.0f;
    if (peak_gops > 0.0f)
        efficiency = (total_gops / peak_gops) * 100.0f;

    cellDbgFontConsolePrintf(dbg_console, "--------------------------------------------\n");

    for (i = 0; i < num_spes_active; i++) {
        float spe_gops = 0.0f;
        if (elapsed > 0.01)
            spe_gops = (float)((double)spe_results[i].total_flops / (elapsed * 1e9));

        snprintf(buf, sizeof(buf),
            "SPE %d: %7.2f %s | Batches: %7u | Err: %u%s",
            i, spe_gops, unit,
            spe_results[i].batches_done,
            spe_results[i].verify_errors,
            (spe_results[i].verify_errors > 0) ? " [FAIL]" : "");
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    }

    cellDbgFontConsolePrintf(dbg_console, "--------------------------------------------\n");

    snprintf(buf, sizeof(buf), "TOTAL: %7.2f / %.1f %s @stock  (%.1f%% vs 3.2GHz)", total_gops, peak_gops, unit, efficiency);
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    snprintf(buf, sizeof(buf), "Clock: %.3f GHz (TB: %u Hz)", actual_ghz, (uint32_t)tb_frequency);
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    snprintf(buf, sizeof(buf), "Errors: %u", total_errors);
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    cellDbgFontConsolePrintf(dbg_console, "\n");

    if (current_mode == MODE_COMPUTE_SP) {
        if (all_stable)
            cellDbgFontConsolePrintf(dbg_console, "Status: STABLE\n");
        else
            cellDbgFontConsolePrintf(dbg_console, "Status: ERRORS DETECTED - UNSTABLE\n");
    } else {
        cellDbgFontConsolePrintf(dbg_console, "Status: RUNNING\n");
    }
    {
        pmu_summary_t s;
        const pmu_profile_t *pp = cell_pmu_get_profile();
        cell_pmu_get_summary(&s);
        if (s.ok && pp) {
            uint64_t total_cyc = s.med_window_us
                * (uint64_t)(tb_frequency * 40 / 1000000);
            int j;
            int pos = snprintf(buf, sizeof(buf), "PMU SPE0 n=%d ~%llums", s.sample_count, (unsigned long long)(s.med_window_us / 1000));
            for (j = 0; j < pp->count && j < PMU_MAX_EVENTS && pos < (int)sizeof(buf); j++) {
                if (j == 3) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s=%u", pp->events[j].label, s.med[j]);
                } else {
                    float pct = total_cyc ? (100.0f * (float)s.med[j] / (float)total_cyc) : 0.0f;
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "  %s=%.1f%%", pp->events[j].label, pct);
                }
            }
            cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
        }
    }

    cellDbgFontConsolePrintf(dbg_console, "\n");
    cellDbgFontConsolePrintf(dbg_console, "  # Benchmark          Pipeline\n");
    cellDbgFontConsolePrintf(dbg_console, "  ----------------------------------------\n");
    for (i = 0; i < COMPUTE_COUNT; i++) {
        uint32_t m = compute_index_to_mode(i);
        const char *arrow = (m == (uint32_t)current_mode) ? ">>" : "  ";
        const char *pipe;
        switch (i) {
        case COMPUTE_SP_FMA:     pipe = "Even (SP float)";    break;
        case COMPUTE_DP_FMA:     pipe = "Even (DP float)";    break;
        case COMPUTE_INT_MUL:    pipe = "Even (integer)";     break;
        case COMPUTE_RECIP:      pipe = "Even (estimate+NR)"; break;
        case COMPUTE_SHUFFLE:    pipe = "Odd  (permute)";     break;
        case COMPUTE_DUAL_ISSUE: pipe = "Even+Odd (max)";     break;
        default:                 pipe = "";                   break;
        }
        snprintf(buf, sizeof(buf), "%s %d %-18s %s", arrow, i + 1, mode_name(m), pipe);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    }
}

// per test pass/fail/running indicator
static const char *test_status_str(uint32_t id, uint32_t pass_mask, uint32_t fail_mask, uint32_t cur_test)
{
    if (fail_mask & (1u << id)) return "FAIL";
    if (pass_mask & (1u << id)) return "PASS";
    if (id == cur_test)         return "RUN.";
    return "    ";
}

void render_memtest_stats(double elapsed)
{
    uint32_t total_errors = 0;
    uint64_t total_bytes  = 0;
    int all_pass = 1;
    int i;
    char buf[256];

    uint32_t cur_test   = spe_results[0].current_test;
    uint32_t cur_pass   = spe_results[0].memtest_pass;
    uint32_t cur_pct    = spe_results[0].progress_pct;
    uint32_t pass_mask  = spe_results[0].test_pass_mask;
    uint32_t fail_mask  = spe_results[0].test_fail_mask;
    uint32_t done_count = spe_results[0].tests_completed;

    (void)elapsed;

    for (i = 0; i < num_spes_active; i++) {
        total_errors += spe_results[i].memtest_errors;
        total_bytes  += spe_results[i].memtest_bytes;
        if (spe_results[i].memtest_errors > 0)
            all_pass = 0;
        pass_mask &= spe_results[i].test_pass_mask;
        fail_mask |= spe_results[i].test_fail_mask;
    }

    if (cur_test < MEMTEST_COUNT) {
        char bar[41];
        uint32_t fill = cur_pct * 2 / 5;   // 40 char bar
        uint32_t j;
        for (j = 0; j < 40; j++)
            bar[j] = (j < fill) ? '#' : '-';
        bar[40] = '\0';

        snprintf(buf, sizeof(buf), "Pass %u | Test %u/%u: %s", cur_pass, done_count + 1, MEMTEST_COUNT, memtest_name(cur_test));
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        snprintf(buf, sizeof(buf), "[%s] %u%%", bar, cur_pct);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

        uint32_t slice_size = memtest_region_size / MAX_SPES;
        snprintf(buf, sizeof(buf), "Pattern: 0x%08X | Region: %u MB (%u MB/SPE x %d)", spe_results[0].current_pattern, memtest_region_size / (1024 * 1024), slice_size / (1024 * 1024), num_spes_active);
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "Pass %u | Idle\n", cur_pass);
        cellDbgFontConsolePrintf(dbg_console, "\n\n");
    }

    cellDbgFontConsolePrintf(dbg_console, "\n");

    cellDbgFontConsolePrintf(dbg_console, "  # Test              Status  # Test              Status\n");
    cellDbgFontConsolePrintf(dbg_console, "  --------------------------------------------------""----------------------------\n");
    for (i = 0; i < MEMTEST_COUNT; i += 2) {
        const char *s0 = test_status_str(i, pass_mask, fail_mask, cur_test);
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "  %2d %-16s [%s]", i + 1, memtest_name(i), s0);
        if (i + 1 < MEMTEST_COUNT) {
            const char *s1 = test_status_str(i + 1, pass_mask, fail_mask, cur_test);
            while (pos < 28) buf[pos++] = ' ';
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%2d %-16s [%s]", i + 2, memtest_name(i + 1), s1);
        }
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    }

    cellDbgFontConsolePrintf(dbg_console, "\n");

    cellDbgFontConsolePrintf(dbg_console,"  SPE  Pass  Tested      Errors\n");
    cellDbgFontConsolePrintf(dbg_console,"  ---------------------------------\n");
    for (i = 0; i < num_spes_active; i++) {
        snprintf(buf, sizeof(buf), "   %d    %2u   %5u MB    %u%s", i, spe_results[i].memtest_pass, (uint32_t)(spe_results[i].memtest_bytes / (1024 * 1024)), spe_results[i].memtest_errors, (spe_results[i].memtest_errors > 0) ? "  <<<" : "");
        cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
    }

    cellDbgFontConsolePrintf(dbg_console, "\n");

    // latency test: dedicated display
    if (cur_test == MEMTEST_LATENCY ||
        (current_memtest == MEMTEST_LATENCY && spe_results[0].lat_micro_read_ns > 0)) {
        uint32_t kprobes = spe_results[0].current_pattern;
        cellDbgFontConsolePrintf(dbg_console, "  Samples: %uK probes (%u passes)\n", kprobes, cur_pass);
        cellDbgFontConsolePrintf(dbg_console, "  Read  latency:  avg %5u ns   min %5u ns   max %5u ns\n", spe_results[0].lat_micro_read_ns, spe_results[0].lat_read_ns, spe_results[0].bw_read_mbps);
        cellDbgFontConsolePrintf(dbg_console, "  Write latency:  avg %5u ns   min %5u ns   max %5u ns\n", spe_results[0].lat_micro_write_ns, spe_results[0].lat_write_ns, spe_results[0].bw_write_mbps);
    } else if (spe_results[0].bw_pipe_read_mbps > 0) {
        cellDbgFontConsolePrintf(dbg_console, "  Bandwidth (sync)     Read: %5u MB/s    Write: %5u MB/s\n", spe_results[0].bw_read_mbps, spe_results[0].bw_write_mbps);
        cellDbgFontConsolePrintf(dbg_console, "  Bandwidth (pipe)     Read: %5u MB/s    Write: %5u MB/s\n", spe_results[0].bw_pipe_read_mbps, spe_results[0].bw_pipe_write_mbps);
        cellDbgFontConsolePrintf(dbg_console, "  Latency (16KB DMA)   Read: %5u ns      Write: %5u ns\n", spe_results[0].lat_read_ns, spe_results[0].lat_write_ns);
    }

    cellDbgFontConsolePrintf(dbg_console, "\n");

    if (spe_results[0].last_pass_ms > 0) {
        cellDbgFontConsolePrintf(dbg_console, "  Pass time: last %ums  avg %ums  min %ums  max %ums\n", spe_results[0].last_pass_ms, spe_results[0].avg_pass_ms, spe_results[0].min_pass_ms, spe_results[0].max_pass_ms);
    }

    snprintf(buf, sizeof(buf),"  Total tested: %llu MB | Total errors: %u", (unsigned long long)(total_bytes / (1024 * 1024)), total_errors);
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (spe_results[0].status == STATUS_DONE)
        cellDbgFontConsolePrintf(dbg_console, "  Status: %s\n", all_pass ? "ALL TESTS PASSED" : "ERRORS FOUND");
    else
        cellDbgFontConsolePrintf(dbg_console, "  Status: %s\n", all_pass ? "TESTING..." : "ERRORS DETECTED");

    if (total_errors > 0) {
        cellDbgFontConsolePrintf(dbg_console, "\n");
        for (i = 0; i < num_spes_active; i++) {
            if (spe_results[i].memtest_errors > 0) {
                snprintf(buf, sizeof(buf), "  SPE%d ERR @+0x%08X exp:0x%08X got:0x%08X bit:%u", i, spe_results[i].memtest_err_addr, spe_results[i].memtest_err_exp, spe_results[i].memtest_err_got, spe_results[i].memtest_err_bit);
                cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
            }
        }
    }
}