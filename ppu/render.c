/*
* render.c - top level render dispatch and file logging
*/

#include <stdio.h>
#include <stdint.h>

#include <cell/dbgfont.h>

#include "stress_common.h"
#include "gcm.h"
#include "spu.h"
#include "cellmark.h"
#include "render.h"

void render_stats(void)
{
    double elapsed = get_elapsed_seconds();
    int    hours   = (int)elapsed / 3600;
    int    mins    = ((int)elapsed % 3600) / 60;
    int    secs    = (int)elapsed % 60;
    float  tb_clock = (float)((double)tb_frequency * 40.0 / 1e9);
    char   buf[256];

    cellDbgFontConsoleClear(dbg_console);

    // ppe plus disk pages have their own renderers that also clear
    if (current_page == PAGE_PPE) {
        render_ppe_stats(elapsed);
        return;
    }
    if (current_page == PAGE_DISK) {
        if (disk_probe_mode) render_disk_probe_view(elapsed);
        else                 render_disk_stats(elapsed);
        return;
    }
    if (current_page == PAGE_DMA) {
        render_dma_stats(elapsed);
        return;
    }
    if (current_page == PAGE_EIB) {
        render_eib_stats(elapsed);
        return;
    }

    cellDbgFontConsolePrintf(dbg_console, "cellmark 1.0.0 by sagemono\n");

    if (current_mode == MODE_MEMTEST && current_memtest != MEMTEST_AUTO_CYCLE) {
        snprintf(buf, sizeof(buf), "%s [%s] | %dSPE %.2fGHz | %02d:%02d:%02d", mode_name(current_mode), memtest_name(current_memtest), num_spes_active, tb_clock, hours, mins, secs);
    } else {
        snprintf(buf, sizeof(buf), "%s | %dSPE %.2fGHz | %02d:%02d:%02d", mode_name(current_mode), num_spes_active, tb_clock, hours, mins, secs);
    }
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);

    if (mode_is_compute(current_mode))
        render_compute_stats(elapsed);
    else if (current_mode == MODE_MEMTEST)
        render_memtest_stats(elapsed);

    cellDbgFontConsolePrintf(dbg_console,
        "L2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n",
        log_enabled ? "ON" : "off");
}

void write_log(const char *reason)
{
    if (!log_enabled) return;

    FILE *fp = fopen(LOG_PATH, "a");
    if (!fp) {
        printf("log: cannot open %s\n", LOG_PATH);
        return;
    }

    double elapsed = get_elapsed_seconds();
    int hours = (int)elapsed / 3600;
    int mins  = ((int)elapsed % 3600) / 60;
    int secs  = (int)elapsed % 60;
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1e9);
    int i;

    fprintf(fp, "--- %s [%02d:%02d:%02d] ---\n", reason, hours, mins, secs);
    fprintf(fp, "Mode: %s | SPEs: %d | Clock: %.2f GHz\n",
            mode_name(current_mode), num_spes_active, tb_clock);

    if (current_mode == MODE_MEMTEST) {
        uint32_t total_errors = 0;
        uint64_t total_bytes  = 0;
        for (i = 0; i < num_spes_active; i++) {
            total_errors += spe_results[i].memtest_errors;
            total_bytes  += spe_results[i].memtest_bytes;
        }
        fprintf(fp, "Region: %u MB | Pass: %u | Test: %s\n", memtest_region_size / (1024 * 1024), spe_results[0].memtest_pass, memtest_name(spe_results[0].current_test));
        fprintf(fp, "Tested: %llu MB | Errors: %u\n", (unsigned long long)(total_bytes / (1024 * 1024)), total_errors);

        for (i = 0; i < num_spes_active; i++) {
            fprintf(fp, "  SPE%d: pass=%u bytes=%llu MB errors=%u\n",
                    i, spe_results[i].memtest_pass,
                    (unsigned long long)(spe_results[i].memtest_bytes / (1024*1024)),
                    spe_results[i].memtest_errors);
            if (spe_results[i].memtest_errors > 0) {
                fprintf(fp,
                    "    first err @+0x%08X exp:0x%08X got:0x%08X bit:%u\n",
                    spe_results[i].memtest_err_addr,
                    spe_results[i].memtest_err_exp,
                    spe_results[i].memtest_err_got,
                    spe_results[i].memtest_err_bit);
            }
        }

        if (spe_results[0].bw_read_mbps > 0) {
            fprintf(fp, "BW sync R:%u W:%u MB/s | pipe R:%u W:%u MB/s\n",
                    spe_results[0].bw_read_mbps,
                    spe_results[0].bw_write_mbps,
                    spe_results[0].bw_pipe_read_mbps,
                    spe_results[0].bw_pipe_write_mbps);
            fprintf(fp, "Latency (16KB DMA) R:%u ns W:%u ns\n",
                    spe_results[0].lat_read_ns,
                    spe_results[0].lat_write_ns);
        }

        if (spe_results[0].lat_micro_read_ns > 0) {
            fprintf(fp, "Latency (128B) Read  avg:%u min:%u max:%u ns\n",
                    spe_results[0].lat_micro_read_ns,
                    spe_results[0].lat_read_ns,
                    spe_results[0].bw_read_mbps);
            fprintf(fp, "Latency (128B) Write avg:%u min:%u max:%u ns\n",
                    spe_results[0].lat_micro_write_ns,
                    spe_results[0].lat_write_ns,
                    spe_results[0].bw_write_mbps);
        }

        if (spe_results[0].last_pass_ms > 0) {
            fprintf(fp, "Pass time: last %u ms  avg %u ms  min %u ms  max %u ms\n", spe_results[0].last_pass_ms, spe_results[0].avg_pass_ms, spe_results[0].min_pass_ms, spe_results[0].max_pass_ms);
        }

        fprintf(fp, "Tests passed: 0x%04X | failed: 0x%04X\n", spe_results[0].test_pass_mask, spe_results[0].test_fail_mask);
    } else {
        float total_gops   = 0.0f;
        uint32_t total_err = 0;
        for (i = 0; i < num_spes_active; i++) {
            total_err += spe_results[i].verify_errors;
            if (elapsed > 0.01)
                total_gops += (float)((double)spe_results[i].total_flops / (elapsed * 1e9));
        }
        float peak = get_peak_ops_stock();
        fprintf(fp, "%s: %.2f / %.1f %s peak (%.1f%% eff)\n", mode_name(current_mode), total_gops, peak, ops_unit_name(), peak > 0.0f ? (total_gops / peak) * 100.0f : 0.0f);
        fprintf(fp, "Errors: %u\n", total_err);

        for (i = 0; i < num_spes_active; i++) {
            float gf = 0.0f;
            if (elapsed > 0.01)
                gf = (float)((double)spe_results[i].total_flops / (elapsed * 1e9));
            fprintf(fp, "  SPE%d: %.2f %s batches=%u errors=%u\n", i, gf, ops_unit_name(), spe_results[i].batches_done, spe_results[i].verify_errors);
        }
    }

    fprintf(fp, "\n");
    fclose(fp);
    printf("log: wrote to %s (%s)\n", LOG_PATH, reason);
}