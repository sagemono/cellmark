#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>
#include <sys/time_util.h>
#include <sys/sys_time.h>

#include <cell/gcm.h>
#include <cell/sysmodule.h>
#include <cell/pad.h>
#include <sysutil/sysutil_sysparam.h>

#include <sys/syscall.h>

#include "stress_common.h"
#include "gcm.h"
#include "spu.h"
#include "cellmark.h"
#include "cellmark_engine.h"
#include "render.h"
#include "pmu.h"
#include "cell_pmu.h"
#include "disk_benchmark.h"   /* DISK_BENCH_SEQ_READ etc. */

SYS_PROCESS_PARAM(1001, 0x100000);

int      current_page      = PAGE_CELL;
int      current_mode      = MODE_COMPUTE_SP;
int      current_memtest   = MEMTEST_AUTO_CYCLE;
int      current_ppe_bench = PPE_BENCH_VMX_FMA;
int      current_disk_bench = DISK_BENCH_SEQ_READ;
int      current_dma_bench  = DMA_BENCH_GET;
int      current_eib_bench  = EIB_BENCH_PAIRS;
int      current_workload   = WORKLOAD_PI;
int      current_compute    = COMPUTE_SP_FMA;
int      disk_probe_mode    = 0;
int      log_enabled        = 0;
uint16_t pad_held1          = 0;
uint16_t pad_held2          = 0;
uint8_t  pad_lx             = 128, pad_ly = 128;
uint8_t  pad_rx             = 128, pad_ry = 128;
uint32_t memtest_region_size = 0;
uint64_t tb_frequency;
uint64_t tb_start;

static volatile int app_running = 1;
static void        *memtest_region = NULL;

static void sysutil_callback(uint64_t status, uint64_t param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        app_running = 0;
}

double get_elapsed_seconds(void)
{
    uint64_t now;
    SYS_TIMEBASE_GET(now);
    return (double)(now - tb_start) / (double)tb_frequency;
}

static uint64_t read_lv1_nclk_hz(void)
{
    system_call_8(10, 1ULL, 0x62650000ULL, 0ULL, 0x6e636c6b00000000ULL, 0ULL, 0ULL, 0ULL, 91ULL);
    if (p1 != 0) return 0;
    return p2;
}

int main(void)
{
    int ret;

    ret = cellSysmoduleLoadModule(CELL_SYSMODULE_GCM_SYS);
    printf("sysmodule GCM_SYS: 0x%x\n", ret);
    ret = cellSysmoduleLoadModule(CELL_SYSMODULE_FS);
    printf("sysmodule FS: 0x%x\n", ret);
    ret = cellSysmoduleLoadModule(CELL_SYSMODULE_IO);
    printf("sysmodule IO: 0x%x\n", ret);

    cellSysutilRegisterCallback(0, sysutil_callback, NULL);

    ret = init_display();
    if (ret != 0) { printf("init_display failed: %d\n", ret); return 1; }
    printf("display: %ux%u, local_heap=%p\n", screen_width, screen_height, local_heap_ptr);

    uint32_t dbgfont_local_used = 0;
    ret = init_dbgfont(local_heap_ptr, &dbgfont_local_used);
    if (ret != 0) { printf("init_dbgfont failed: 0x%x\n", ret); return 1; }
    local_heap_ptr = (void *)((uintptr_t)local_heap_ptr + dbgfont_local_used);

    /* timebase: prefer the lv1 nclk read; fall back to SDK */
    {
        uint64_t reported_tb = sys_time_get_timebase_frequency();
        uint64_t lv1_nclk    = read_lv1_nclk_hz();
        uint64_t derived_tb  = lv1_nclk / 40ULL;
        if (derived_tb > 0 && derived_tb != reported_tb) {
            tb_frequency = derived_tb;
            printf("cellmark: SDK tb=%llu Hz disagrees with lv1 nclk=%llu Hz\n", (unsigned long long)reported_tb, (unsigned long long)lv1_nclk);
            printf("cellmark: using lv1-derived tb=%llu Hz (chip clock = %.3f GHz)\n", (unsigned long long)tb_frequency, lv1_nclk / 1.0e9);
        } else {
            tb_frequency = reported_tb;
            if (lv1_nclk > 0)
                printf("cellmark: tb_freq = %llu Hz (lv1 nclk agrees, %.3f GHz chip)\n", (unsigned long long)tb_frequency, lv1_nclk / 1.0e9);
            else
                printf("cellmark: tb_freq = %llu Hz (lv1 nclk read unavailable)\n", (unsigned long long)tb_frequency);
        }
    }
    SYS_TIMEBASE_GET(tb_start);
    printf("cellmark: screen = %ux%u\n", screen_width, screen_height);

    /* memtest region (used by cell stress, xdr memtest, and burn-in) */
    {
        static const uint32_t try_mb[] = { 384, 256, 192, 128, 64, 32, 0 };
        int ti;
        for (ti = 0; try_mb[ti] > 0; ti++) {
            uint32_t size = try_mb[ti] * 1024 * 1024;
            size &= ~((uint32_t)(MAX_SPES * DMA_MAX_SIZE) - 1);
            void *p = memalign(128, size);
            if (p) {
                memtest_region      = p;
                memtest_region_size = size;
                memset(p, 0, size);
                printf("cellmark: memtest region at %p (%u MB)\n", p, size / (1024 * 1024));
                break;
            }
            printf("cellmark: %u MB alloc failed, trying smaller\n", try_mb[ti]);
        }
        if (!memtest_region)
            printf("cellmark: WARNING - memtest region alloc failed\n");
    }

    pmu_init();

    ret = init_spu();
    if (ret != 0) { printf("init_spu failed: 0x%x\n", ret); return 1; }

    cellmark_engine_init(tb_frequency, memtest_region, memtest_region_size);

    cellPadInit(1);

    uint16_t prev_btns1        = 0;
    uint16_t prev_btns2        = 0;
    uint32_t last_logged_pass  = 0;
    uint64_t pass_tb_start     = 0;
    uint32_t tracked_pass      = 0;
    uint64_t total_pass_ms_accum = 0;
    uint32_t pass_count        = 0;

    while (app_running) {
        cellSysutilCheckCallback();

        {
            CellPadData pad_data;
            if (cellPadGetData(0, &pad_data) == CELL_OK && pad_data.len > 0) {
                uint16_t btns1   = pad_data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
                uint16_t btns2   = pad_data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
                uint16_t pressed1 = btns1 & ~prev_btns1;
                uint16_t pressed2 = btns2 & ~prev_btns2;
                prev_btns1 = btns1; prev_btns2 = btns2;
                pad_held1 = btns1; pad_held2 = btns2;
                pad_lx = pad_data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
                pad_ly = pad_data.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
                pad_rx = pad_data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X];
                pad_ry = pad_data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y];

                if ((btns1 & 0x09) == 0x09)
                    app_running = 0;

                if (pressed2 & 0x10) {
                    log_enabled = !log_enabled;
                    printf("logging %s\n", log_enabled ? "ON" : "OFF");
                }

                cellmark_engine_handle_input(pressed1, pressed2);
            }
        }

        if (current_mode == MODE_MEMTEST && num_spes_active > 0) {
            uint32_t cur_pass = spe_results[0].memtest_pass;
            if (cur_pass != tracked_pass) {
                uint64_t now_tb;
                SYS_TIMEBASE_GET(now_tb);
                if (tracked_pass > 0 && pass_tb_start > 0) {
                    uint64_t delta   = now_tb - pass_tb_start;
                    uint32_t pass_ms = (uint32_t)(delta * 1000 / tb_frequency);
                    spe_results[0].last_pass_ms = pass_ms;
                    if (pass_count == 0 || pass_ms < spe_results[0].min_pass_ms)
                        spe_results[0].min_pass_ms = pass_ms;
                    if (pass_ms > spe_results[0].max_pass_ms)
                        spe_results[0].max_pass_ms = pass_ms;
                    pass_count++;
                    total_pass_ms_accum += pass_ms;
                    spe_results[0].avg_pass_ms =
                        (uint32_t)(total_pass_ms_accum / pass_count);
                }
                pass_tb_start = now_tb;
                tracked_pass  = cur_pass;
            }
            if (cur_pass > last_logged_pass && cur_pass > 0) {
                last_logged_pass = cur_pass;
                write_log("pass complete");
            }
        }

        cellmark_engine_tick(tb_frequency);

        {
            const char *cat = cellmark_engine_current_category();
            int cell_active = (strcmp(cat, "cell") == 0 || strcmp(cat, "xdr") == 0) && num_spes_active > 0;
            cell_pmu_tick(cell_active, current_mode);
        }

        {
            CellGcmContextData *ctx = gCellGcmCurrentContext;
            set_render_target();
            cellGcmSetClearColor(ctx, 0xff101010);
            cellGcmSetClearSurface(ctx, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
        }

        cellmark_engine_render(get_elapsed_seconds());
        cellDbgFontDrawGcm();
        flip_frame();
    }

    printf("cellmark: stopping...\n");
    write_log("exit");
    cellmark_engine_shutdown();

    if (memtest_region) free(memtest_region);

    cellPadEnd();
    cellDbgFontConsoleClose(dbg_console);
    cellDbgFontExitGcm();

    printf("cellmark: done, calling sys_process_exit\n");
    sys_process_exit(0);
    return 0;
}