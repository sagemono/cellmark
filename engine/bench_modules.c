#include <stdio.h>
#include <stdint.h>

#include <cell/dbgfont.h>

#include "bench.h"
#include "cellmark.h"
#include "cellmark_engine.h"
#include "stress_common.h"
#include "render.h"
#include "spu.h"
#include "sysmon.h"
#include "gcm.h"

#include "ppe_benchmarks.h"
#include "disk_benchmark.h"
#include "dma_benchmark.h"
#include "eib_benchmark.h"
#include "atomic_benchmark.h"
#include "mbox_benchmark.h"
#include "branch_benchmark.h"
#include "pi_benchmark.h"
#include "fft_benchmark.h"
#include "nbody_benchmark.h"
#include "burn_benchmark.h"
#include "cellmark_version.h"

extern void *cellmark_engine_shared_xdr_buffer(uint32_t *size_out);

static int      g_cell_active = 0;
static uint32_t g_cell_tb_hz  = 0;

static int cell_init(uint64_t tb_freq) {
    g_cell_tb_hz = (uint32_t)tb_freq;
    return 0;
}
static void cell_start(void) {
    uint32_t sz; void *buf = cellmark_engine_shared_xdr_buffer(&sz);
    int mode = compute_index_to_mode((uint32_t)current_compute);
    if (start_spu_stress(mode, MEMTEST_AUTO_CYCLE, buf, sz, g_cell_tb_hz) == 0)
        g_cell_active = 1;
}
static void cell_stop(void) {
    if (g_cell_active) { stop_spu_stress(); g_cell_active = 0; }
}
static void render_cellxdr_header(double elapsed) {

    int hours = (int)elapsed / 3600;
    int mins  = ((int)elapsed % 3600) / 60;
    int secs  = (int)elapsed % 60;
    float tb_clock = (float)((double)tb_frequency * 40.0 / 1.0e9);
    const char *sm = sysmon_get_status_string();
    char buf[256];

    cellDbgFontConsoleClear(dbg_console);
    cellDbgFontConsolePrintf(dbg_console, CELLMARK_BANNER "\n");
    if (current_mode == MODE_MEMTEST && current_memtest != MEMTEST_AUTO_CYCLE) {
        snprintf(buf, sizeof(buf), "%s [%s] | %dSPE %.2fGHz | %02d:%02d:%02d | %s", mode_name(current_mode), memtest_name(current_memtest), num_spes_active, tb_clock, hours, mins, secs, sm);
    } else {
        snprintf(buf, sizeof(buf), "%s | %dSPE %.2fGHz | %02d:%02d:%02d | %s", mode_name(current_mode), num_spes_active, tb_clock, hours, mins, secs, sm);
    }
    cellDbgFontConsolePrintf(dbg_console, "%s\n", buf);
}
static void render_cellxdr_footer(void) {
    cellDbgFontConsolePrintf(dbg_console,
        "L2/R2:page L1/R1:bench TRI:log[%s] | SEL+START:exit\n",
        log_enabled ? "ON" : "off");
}

static void cell_render(double elapsed) {
    render_cellxdr_header(elapsed);
    render_compute_stats(elapsed);
    render_cellxdr_footer();
}
static void cell_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir == 0) return;
    cell_stop();
    current_compute = (current_compute + dir + COMPUTE_COUNT) % COMPUTE_COUNT;
    current_mode    = compute_index_to_mode(current_compute);
    cell_start();
}

const bench_module_t bench_cell = {
    .id           = "cell_compute",
    .display_name = "Cell Compute Stress",
    .category     = "cell",
    .kind         = BENCH_KIND_CONTINUOUS,
    .init = cell_init, .start = cell_start, .stop = cell_stop,
    .render = cell_render, .input = cell_input,
};

static int      g_xdr_active = 0;
static uint32_t g_xdr_tb_hz  = 0;

static int xdr_init(uint64_t tb_freq) {
    g_xdr_tb_hz = (uint32_t)tb_freq;
    return 0;
}
static void xdr_start(void) {
    uint32_t sz; void *buf = cellmark_engine_shared_xdr_buffer(&sz);
    if (start_spu_stress(MODE_MEMTEST, current_memtest, buf, sz, g_xdr_tb_hz) == 0)
        g_xdr_active = 1;
}
static void xdr_stop(void) {
    if (g_xdr_active) { stop_spu_stress(); g_xdr_active = 0; }
}
static void xdr_render(double elapsed) {
    render_cellxdr_header(elapsed);
    render_memtest_stats(elapsed);
    render_cellxdr_footer();
}
static void xdr_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir == 0) return;
    xdr_stop();
    if (current_memtest == MEMTEST_AUTO_CYCLE) {
        current_memtest = (dir < 0) ? (MEMTEST_COUNT - 1) : 0;
    } else {
        int next = current_memtest + dir;
        if (next < 0 || next >= MEMTEST_COUNT) current_memtest = MEMTEST_AUTO_CYCLE;
        else current_memtest = next;
    }
    xdr_start();
}

const bench_module_t bench_xdr = {
    .id           = "xdr_memtest",
    .display_name = "XDR Memtest",
    .category     = "xdr",
    .kind         = BENCH_KIND_CONTINUOUS,
    .init = xdr_init, .start = xdr_start, .stop = xdr_stop,
    .render = xdr_render, .input = xdr_input,
};

static int ppe_init(uint64_t tb_freq) { (void)tb_freq; ppe_benchmarks_init(); return 0; }
static void ppe_tick(uint64_t tb_freq) { ppe_run_batch(current_ppe_bench, tb_freq); }
static void ppe_render(double e) { render_ppe_stats(e); }
static void ppe_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir != 0)
        current_ppe_bench = (current_ppe_bench + dir + PPE_BENCH_COUNT) % PPE_BENCH_COUNT;
}

const bench_module_t bench_ppe = {
    .id           = "ppe",
    .display_name = "PPE Core Benchmarks",
    .category     = "ppe",
    .kind         = BENCH_KIND_BATCH,
    .init = ppe_init, .tick = ppe_tick, .render = ppe_render, .input = ppe_input,
};

extern void disk_run_probes(uint64_t tb_freq);
extern int  disk_is_running(void);
extern int  disk_probes_running(void);
extern void disk_trigger_bench(int bench_id, uint64_t tb_freq);

static int  g_disk_tb_hz_lo = 0;
static int  g_disk_tb_hz_hi = 0;
static uint64_t disk_tb_hz(void) {
    return ((uint64_t)(uint32_t)g_disk_tb_hz_hi << 32) | (uint32_t)g_disk_tb_hz_lo;
}

static int disk_init(uint64_t tb_freq) {
    disk_benchmark_init();
    g_disk_tb_hz_lo = (int)(uint32_t)(tb_freq & 0xffffffffu);
    g_disk_tb_hz_hi = (int)(uint32_t)(tb_freq >> 32);
    return 0;
}
static void disk_render(double e) {
    if (disk_probe_mode) render_disk_probe_view(e);
    else                 render_disk_stats(e);
}
static void disk_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir != 0 && !disk_probe_mode)
        current_disk_bench = (current_disk_bench + dir + DISK_BENCH_COUNT) % DISK_BENCH_COUNT;
    if (p2 & 0x80) disk_probe_mode = !disk_probe_mode;
    if (p2 & 0x40) {
        uint64_t tb = disk_tb_hz();
        if (disk_probe_mode) {
            if (!disk_probes_running() && !disk_is_running()) disk_run_probes(tb);
        } else {
            if (!disk_is_running()) disk_trigger_bench(current_disk_bench, tb);
        }
    }
}

const bench_module_t bench_disk = {
    .id           = "disk",
    .display_name = "Storage I/O",
    .category     = "disk",
    .kind         = BENCH_KIND_BATCH,
    .init = disk_init, .render = disk_render, .input = disk_input,
};

static int  dma_init(uint64_t tb_freq) { dma_benchmark_init(tb_freq); return 0; }
static void dma_tick(uint64_t tb_freq) { dma_run_batch(current_dma_bench, tb_freq); }
static void dma_render(double e) { render_dma_stats(e); }
static void dma_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir != 0)
        current_dma_bench = (current_dma_bench + dir + DMA_BENCH_COUNT) % DMA_BENCH_COUNT;
}

const bench_module_t bench_dma = {
    .id           = "dma",
    .display_name = "XDR DMA",
    .category     = "dma",
    .kind         = BENCH_KIND_BATCH,
    .init = dma_init, .tick = dma_tick, .render = dma_render, .input = dma_input,
};

static int eib_init(uint64_t tb_freq) {
    eib_benchmark_init(tb_freq);
    atomic_benchmark_init(tb_freq);
    mbox_benchmark_init(tb_freq);
    branch_benchmark_init(tb_freq);
    return 0;
}
static void eib_tick(uint64_t tb_freq) {
    if (current_eib_bench == EIB_BENCH_ATOMIC)       atomic_run_batch(tb_freq);
    else if (current_eib_bench == EIB_BENCH_MBOX)    mbox_run_batch(tb_freq);
    else if (current_eib_bench == EIB_BENCH_BRANCH)  branch_run_batch(tb_freq);
    else                                              eib_run_batch(current_eib_bench, tb_freq);
}
static void eib_render(double e) { render_eib_stats(e); }
static void eib_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir != 0)
        current_eib_bench = (current_eib_bench + dir + EIB_BENCH_COUNT) % EIB_BENCH_COUNT;
}

const bench_module_t bench_fabric = {
    .id           = "fabric",
    .display_name = "EIB / Atomic / Mbox / Branch",
    .category     = "fabric",
    .kind         = BENCH_KIND_BATCH,
    .init = eib_init, .tick = eib_tick, .render = eib_render, .input = eib_input,
};

static int wl_init(uint64_t tb_freq) {
    pi_benchmark_init(tb_freq);
    fft_benchmark_init(tb_freq);
    nbody_benchmark_init(tb_freq);
    return 0;
}
static void wl_tick(uint64_t tb_freq) {
    if      (current_workload == WORKLOAD_PI)    pi_run_batch(tb_freq);
    else if (current_workload == WORKLOAD_FFT)   fft_run_batch(tb_freq);
    else if (current_workload == WORKLOAD_NBODY) nbody_run_batch(tb_freq);
}
static void wl_render(double e) { render_workload_stats(e); }
static void wl_input(uint16_t p1, uint16_t p2) {
    int dir = 0;
    if ((p2 & 0x04) || (p1 & 0x10)) dir = -1;
    if ((p2 & 0x08) || (p1 & 0x40)) dir = +1;
    if (dir != 0)
        current_workload = (current_workload + dir + WORKLOAD_COUNT) % WORKLOAD_COUNT;
}

const bench_module_t bench_workload = {
    .id           = "workload",
    .display_name = "Real Workloads",
    .category     = "workload",
    .kind         = BENCH_KIND_BATCH,
    .init = wl_init, .tick = wl_tick, .render = wl_render, .input = wl_input,
};

static int burn_init_module(uint64_t tb_freq) {
    uint32_t sz; void *buf = cellmark_engine_shared_xdr_buffer(&sz);
    burn_benchmark_init(tb_freq, buf, sz);
    return 0;
}
static void burn_start_module(void) { burn_start(); }
static void burn_tick_module(uint64_t tb_freq) { burn_tick(tb_freq); }
static void burn_stop_module(void) { burn_stop(); }
static void burn_render_module(double e) { render_burn_stats(e); }

const bench_module_t bench_burn = {
    .id           = "burn",
    .display_name = "All-Units Saturation Burn-In",
    .category     = "burn",
    .kind         = BENCH_KIND_CONTINUOUS,
    .init = burn_init_module, .start = burn_start_module,
    .tick = burn_tick_module, .stop = burn_stop_module,
    .render = burn_render_module,
};