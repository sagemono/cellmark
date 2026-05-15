/* template/bench.c - skeleton benchmark module
 *
 * Demonstrates the minimum needed to register a benchmark with the cellmark engine. 
 *
 * Spawns a 1-SPE thread group per tick, has the SP run an arbitrary kernel for a fixed number of iterations, times it via the SPU decrementer and EMA smooths a throughput score.
 *
 * Steps to customise:
 *   1. Rename TEMPLATE_* macros and identifiers to your bench's name
 *   2. Edit the SPE-side work in spu_main.c
 *   3. Choose how to time it (decrementer, PPU SYS_TIMEBASE_GET, etc.)
 *   4. Update the bench_module_t fields at the bottom (display_name, category, kind)
 *   5. Register the bench: add the module to engine/bench_registry.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/time_util.h>

#include "bench.h"
#include "stress_common.h"

#define TEMPLATE_NUM_SPES        1
#define TEMPLATE_BATCH_ITERATIONS 1000000u

typedef struct {
    uint32_t iterations;
    uint32_t pad[31];
} __attribute__((aligned(128))) template_params_t;

typedef struct {
    uint32_t dec_ticks;     // SPU decrementer delta
    uint32_t pad[31];
} __attribute__((aligned(128))) template_result_t;

typedef struct {
    float    score;         // EMA smoothed
    float    ms_per_batch;
    uint32_t runs;
    int      last_err_code;
} template_summary_t;

extern const char _binary_spu_template_elf_start[];

static uint64_t g_tb_freq = 0;
static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static template_params_t  g_params  __attribute__((aligned(128)));
static template_result_t  g_result  __attribute__((aligned(128)));
static template_summary_t g_summary;

static int template_init(uint64_t tb_freq)
{
    g_tb_freq = tb_freq;
    memset(&g_summary, 0, sizeof(g_summary));
    printf("[template] init OK\n");
    return 0;
}

static void template_tick(uint64_t tb_freq)
{
    sys_spu_thread_group_t            grp;
    sys_spu_thread_t                  th;
    sys_spu_thread_group_attribute_t  grp_attr;
    sys_spu_thread_attribute_t        th_attr;
    sys_spu_thread_argument_t         th_args;
    int  ret, cause, status;
    (void)tb_freq;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_template_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_template_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { g_summary.last_err_code = ret; return; }
        }
        g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(grp_attr);
    sys_spu_thread_group_attribute_name(grp_attr, "template");
    if (sys_spu_thread_group_create(&grp, TEMPLATE_NUM_SPES, 100, &grp_attr) != CELL_OK)
        return;

    memset(&g_params, 0, sizeof(g_params));
    memset(&g_result, 0, sizeof(g_result));
    g_params.iterations = TEMPLATE_BATCH_ITERATIONS;

    sys_spu_thread_attribute_initialize(th_attr);
    sys_spu_thread_attribute_name(th_attr, "template_spe");
    memset(&th_args, 0, sizeof(th_args));
    th_args.arg1 = (uint64_t)(uintptr_t)&g_params;
    th_args.arg2 = (uint64_t)(uintptr_t)&g_result;

    if (sys_spu_thread_initialize(&th, grp, 0, &g_spu_img, &th_attr, &th_args) != CELL_OK)
        goto cleanup;
    if (sys_spu_thread_group_start(grp) != CELL_OK) goto cleanup;
    sys_spu_thread_group_join(grp, &cause, &status);

    if (g_result.dec_ticks > 0) {
        double secs  = (double)g_result.dec_ticks / (double)g_tb_freq;
        float  score = (float)((double)TEMPLATE_BATCH_ITERATIONS / secs / 1.0e6);   // Miters/s
        if (g_summary.runs == 0) g_summary.score = score;
        else g_summary.score = 0.7f * g_summary.score + 0.3f * score;
        g_summary.ms_per_batch = (float)(secs * 1.0e3);
        g_summary.runs++;
    }

cleanup:
    sys_spu_thread_group_destroy(grp);
}

#include <cell/dbgfont.h>
#include "cellmark.h"
extern int dbg_console;

static void template_render(double elapsed)
{
    (void)elapsed;
    cellDbgFontConsolePrintf(dbg_console, "cellmark template benchmark\n\n");
    cellDbgFontConsolePrintf(dbg_console, "  batch iterations: %u\n", (unsigned)TEMPLATE_BATCH_ITERATIONS);
    if (g_summary.runs > 0) {
        cellDbgFontConsolePrintf(dbg_console, "  score:      %.2f Miters/sec\n", g_summary.score);
        cellDbgFontConsolePrintf(dbg_console, "  batch time: %.2f ms\n", g_summary.ms_per_batch);
        cellDbgFontConsolePrintf(dbg_console, "  batches:    %u\n", (unsigned)g_summary.runs);
    } else if (g_summary.last_err_code != 0) {
        cellDbgFontConsolePrintf(dbg_console, "  FAIL code=0x%08x\n", (unsigned)g_summary.last_err_code);
    } else {
        cellDbgFontConsolePrintf(dbg_console, "  (warming up)\n");
    }
}

const bench_module_t bench_template = {
    .id           = "template",
    .display_name = "Template Bench",
    .category     = "workload",         // or "ppe", "fabric", etc.
    .kind         = BENCH_KIND_BATCH,
    .init         = template_init,
    .tick         = template_tick,
    .render       = template_render,
};