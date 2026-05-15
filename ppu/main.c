/*
 * main.c - cellmark entry point and main loop
 */

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

#include "stress_common.h"
#include "gcm.h"
#include "spu.h"
#include "ppe_benchmarks.h"
#include "disk_benchmark.h"
#include "dma_benchmark.h"
#include "eib_benchmark.h"
#include "cellmark.h"
#include "render.h"

SYS_PROCESS_PARAM(1001, 0x100000);

int      current_page      = PAGE_CELL;
int      current_mode      = MODE_COMPUTE_SP;
int      current_memtest   = MEMTEST_AUTO_CYCLE;
int      current_ppe_bench = PPE_BENCH_VMX_FMA;
int      current_disk_bench = DISK_BENCH_SEQ_READ;
int      current_dma_bench  = DMA_BENCH_GET;
int      current_eib_bench  = EIB_BENCH_PAIRS;
int      disk_probe_mode   = 0;
int      log_enabled       = 0;
uint32_t memtest_region_size = 0;
uint64_t tb_frequency;
uint64_t tb_start;

static volatile int app_running = 1;
static int          current_compute = COMPUTE_SP_FMA;
static void        *memtest_region  = NULL;

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
    if (ret != 0) {
        printf("init_display failed: %d\n", ret);
        return 1;
    }
    printf("display: %ux%u, local_heap=%p\n", screen_width, screen_height, local_heap_ptr);

    uint32_t dbgfont_local_used = 0;
    ret = init_dbgfont(local_heap_ptr, &dbgfont_local_used);
    if (ret != 0) {
        printf("init_dbgfont failed: 0x%x\n", ret);
        return 1;
    }
    local_heap_ptr = (void *)((uintptr_t)local_heap_ptr + dbgfont_local_used);

    tb_frequency = sys_time_get_timebase_frequency();
    SYS_TIMEBASE_GET(tb_start);

    printf("cellmark: tb_freq = %llu Hz\n", (unsigned long long)tb_frequency);
    printf("cellmark: screen = %ux%u\n", screen_width, screen_height);

    // detect decr or retail 
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

    ppe_benchmarks_init();
    disk_benchmark_init();
    dma_benchmark_init(tb_frequency);
    eib_benchmark_init(tb_frequency);

    current_page    = PAGE_CELL;
    current_mode    = MODE_COMPUTE_SP;
    current_compute = COMPUTE_SP_FMA;

    ret = init_spu();
    if (ret != 0) {
        printf("init_spu failed: 0x%x\n", ret);
        return 1;
    }

    ret = start_spu_stress(current_mode, current_memtest, memtest_region, memtest_region_size, (uint32_t)tb_frequency);
    if (ret != 0) {
        printf("start_spu_stress failed: 0x%x\n", ret);
        return 1;
    }

    printf("cellmark: %d SPEs active, running...\n", num_spes_active);

    cellPadInit(1);

    uint16_t prev_btns1        = 0;
    uint16_t prev_btns2        = 0;
    int      needs_restart     = 0;
    uint32_t last_logged_pass  = 0;

    uint64_t pass_tb_start        = 0;
    uint32_t tracked_pass         = 0;
    uint64_t total_pass_ms_accum  = 0;
    uint32_t pass_count           = 0;

    while (app_running) {
        cellSysutilCheckCallback();

        {
            CellPadData pad_data;
            if (cellPadGetData(0, &pad_data) == CELL_OK && pad_data.len > 0) {
                uint16_t btns1   = pad_data.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
                uint16_t btns2   = pad_data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
                uint16_t pressed1 = btns1 & ~prev_btns1;
                uint16_t pressed2 = btns2 & ~prev_btns2;
                prev_btns1 = btns1;
                prev_btns2 = btns2;

                if ((btns1 & 0x09) == 0x09)
                    app_running = 0;

                if (pressed2 & 0x01) {
                    int np = (current_page - 1 + PAGE_COUNT) % PAGE_COUNT;
                    if (np != current_page) { current_page = np; needs_restart = 1; }
                }
                if (pressed2 & 0x02) {
                    int np = (current_page + 1) % PAGE_COUNT;
                    if (np != current_page) { current_page = np; needs_restart = 1; }
                }

                if (pressed2 & 0x04) {
                    if (current_page == PAGE_CELL) {
                        current_compute = (current_compute - 1 + COMPUTE_COUNT) % COMPUTE_COUNT;
                        current_mode = compute_index_to_mode(current_compute);
                        needs_restart = 1;
                    } else if (current_page == PAGE_PPE) {
                        current_ppe_bench = (current_ppe_bench - 1 + PPE_BENCH_COUNT) % PPE_BENCH_COUNT;
                    } else if (current_page == PAGE_DISK && !disk_probe_mode) {
                        current_disk_bench = (current_disk_bench - 1 + DISK_BENCH_COUNT) % DISK_BENCH_COUNT;
                    } else if (current_page == PAGE_DMA) {
                        current_dma_bench = (current_dma_bench - 1 + DMA_BENCH_COUNT) % DMA_BENCH_COUNT;
                    } else if (current_page == PAGE_EIB) {
                        current_eib_bench = (current_eib_bench - 1 + EIB_BENCH_COUNT) % EIB_BENCH_COUNT;
                    } else {
                        if (current_memtest == MEMTEST_AUTO_CYCLE)
                            current_memtest = MEMTEST_COUNT - 1;
                        else if (current_memtest > 0)
                            current_memtest--;
                        else
                            current_memtest = MEMTEST_AUTO_CYCLE;
                        needs_restart = 1;
                    }
                }

                if (pressed2 & 0x08) {
                    if (current_page == PAGE_CELL) {
                        current_compute = (current_compute + 1) % COMPUTE_COUNT;
                        current_mode = compute_index_to_mode(current_compute);
                        needs_restart = 1;
                    } else if (current_page == PAGE_PPE) {
                        current_ppe_bench = (current_ppe_bench + 1) % PPE_BENCH_COUNT;
                    } else if (current_page == PAGE_DISK && !disk_probe_mode) {
                        current_disk_bench = (current_disk_bench + 1) % DISK_BENCH_COUNT;
                    } else if (current_page == PAGE_DMA) {
                        current_dma_bench = (current_dma_bench + 1) % DMA_BENCH_COUNT;
                    } else if (current_page == PAGE_EIB) {
                        current_eib_bench = (current_eib_bench + 1) % EIB_BENCH_COUNT;
                    } else {
                        if (current_memtest == MEMTEST_AUTO_CYCLE)
                            current_memtest = 0;
                        else if (current_memtest + 1 >= MEMTEST_COUNT)
                            current_memtest = MEMTEST_AUTO_CYCLE;
                        else
                            current_memtest++;
                        needs_restart = 1;
                    }
                }

                if (pressed1 & 0x10) {
                    if (current_page == PAGE_CELL) {
                        current_compute = (current_compute - 1 + COMPUTE_COUNT) % COMPUTE_COUNT;
                        current_mode = compute_index_to_mode(current_compute);
                        needs_restart = 1;
                    } else if (current_page == PAGE_PPE) {
                        current_ppe_bench = (current_ppe_bench - 1 + PPE_BENCH_COUNT) % PPE_BENCH_COUNT;
                    } else if (current_page == PAGE_DISK && !disk_probe_mode) {
                        current_disk_bench = (current_disk_bench - 1 + DISK_BENCH_COUNT) % DISK_BENCH_COUNT;
                    } else if (current_page == PAGE_DMA) {
                        current_dma_bench = (current_dma_bench - 1 + DMA_BENCH_COUNT) % DMA_BENCH_COUNT;
                    } else if (current_page == PAGE_EIB) {
                        current_eib_bench = (current_eib_bench - 1 + EIB_BENCH_COUNT) % EIB_BENCH_COUNT;
                    } else {
                        if (current_memtest == MEMTEST_AUTO_CYCLE)
                            current_memtest = MEMTEST_COUNT - 1;
                        else if (current_memtest > 0)
                            current_memtest--;
                        else
                            current_memtest = MEMTEST_AUTO_CYCLE;
                        needs_restart = 1;
                    }
                }

                if (pressed1 & 0x40) {
                    if (current_page == PAGE_CELL) {
                        current_compute = (current_compute + 1) % COMPUTE_COUNT;
                        current_mode = compute_index_to_mode(current_compute);
                        needs_restart = 1;
                    } else if (current_page == PAGE_PPE) {
                        current_ppe_bench = (current_ppe_bench + 1) % PPE_BENCH_COUNT;
                    } else if (current_page == PAGE_DISK && !disk_probe_mode) {
                        current_disk_bench = (current_disk_bench + 1) % DISK_BENCH_COUNT;
                    } else if (current_page == PAGE_DMA) {
                        current_dma_bench = (current_dma_bench + 1) % DMA_BENCH_COUNT;
                    } else if (current_page == PAGE_EIB) {
                        current_eib_bench = (current_eib_bench + 1) % EIB_BENCH_COUNT;
                    } else {
                        if (current_memtest == MEMTEST_AUTO_CYCLE)
                            current_memtest = 0;
                        else if (current_memtest + 1 >= MEMTEST_COUNT)
                            current_memtest = MEMTEST_AUTO_CYCLE;
                        else
                            current_memtest++;
                        needs_restart = 1;
                    }
                }

                if (pressed2 & 0x10) {
                    log_enabled = !log_enabled;
                    printf("logging %s\n", log_enabled ? "ON" : "OFF");
                }

                if ((pressed2 & 0x40) && current_page == PAGE_DISK) {
                    if (disk_probe_mode) {
                        if (!disk_probes_running() && !disk_is_running())
                            disk_run_probes(tb_frequency);
                    } else {
                        if (!disk_is_running())
                            disk_trigger_bench(current_disk_bench, tb_frequency);
                    }
                }

                if ((pressed2 & 0x80) && current_page == PAGE_DISK)
                    disk_probe_mode = !disk_probe_mode;
            }
        }

        if (needs_restart) {
            needs_restart = 0;
            write_log("mode change");

            if (current_mode == MODE_MEMTEST && current_memtest != MEMTEST_AUTO_CYCLE)
                printf("restarting SPUs: mode=%s test=%s\n",
                       mode_name(current_mode), memtest_name(current_memtest));
            else
                printf("restarting SPUs: mode=%s\n", mode_name(current_mode));

            // only stop if a group is currently running. PAGE_PPE / PAGE_DISK
            // / PAGE_DMA all leave num_spes_active == 0, so re-entering
            // PAGE_CELL from one of them must not double join a dead group
            if (num_spes_active > 0)
                stop_spu_stress();
            SYS_TIMEBASE_GET(tb_start);
            last_logged_pass     = 0;
            tracked_pass         = 0;
            pass_tb_start        = 0;
            pass_count           = 0;
            total_pass_ms_accum  = 0;

            if (current_page == PAGE_CELL) {
                current_mode = compute_index_to_mode(current_compute);
                printf("restarting SPUs: mode=%s\n", mode_name(current_mode));
                ret = start_spu_stress(current_mode, current_memtest, memtest_region, memtest_region_size, (uint32_t)tb_frequency);
                if (ret != 0)
                    printf("restart failed: 0x%x\n", ret);
            } else if (current_page == PAGE_PPE) {
                printf("switched to PPE page: SPUs idle\n");
            } else if (current_page == PAGE_DISK) {
                printf("switched to Disk page: SPUs idle\n");
            } else if (current_page == PAGE_DMA) {
                printf("switched to DMA page: stress group stopped, dma_bench owns SPUs\n");
            } else if (current_page == PAGE_EIB) {
                printf("switched to EIB page: stress group stopped, eib_bench owns SPUs\n");
            } else {
                current_mode = MODE_MEMTEST;
                if (current_memtest != MEMTEST_AUTO_CYCLE)
                    printf("restarting SPUs: mode=%s test=%s\n", mode_name(current_mode), memtest_name(current_memtest));
                else
                    printf("restarting SPUs: mode=%s\n", mode_name(current_mode));
                ret = start_spu_stress(current_mode, current_memtest, memtest_region, memtest_region_size, (uint32_t)tb_frequency);
                if (ret != 0)
                    printf("restart failed: 0x%x\n", ret);
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

        if (current_page == PAGE_PPE)
            ppe_run_batch(current_ppe_bench, tb_frequency);
        if (current_page == PAGE_DMA)
            dma_run_batch(current_dma_bench, tb_frequency);
        if (current_page == PAGE_EIB)
            eib_run_batch(current_eib_bench, tb_frequency);

        {
            CellGcmContextData *ctx = gCellGcmCurrentContext;
            set_render_target();
            cellGcmSetClearColor(ctx, 0xff101010);
            cellGcmSetClearSurface(ctx,
                CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G |
                CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
        }

        render_stats();
        cellDbgFontDrawGcm();
        flip_frame();
    }

    printf("cellmark: stopping...\n");
    write_log("exit");
    stop_spu_stress();

    if (memtest_region)
        free(memtest_region);

    cellPadEnd();
    cellDbgFontConsoleClose(dbg_console);
    cellDbgFontExitGcm();

    printf("cellmark: done, calling sys_process_exit\n");
    sys_process_exit(0);
    return 0;
}