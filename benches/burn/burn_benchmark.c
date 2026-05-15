#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <altivec.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "burn_benchmark.h"

#define ERR_BUFFER_ALLOC    1
#define ERR_IMG_IMPORT      2
#define ERR_GRP_CREATE      3
#define ERR_THR_INIT        4
#define ERR_GRP_START       5

#define BURN_PEAK_SPE_GFLOPS    102.4f
#define BURN_PEAK_DMA_GBPS      8.5f
#define BURN_PEAK_PPE_GFLOPS    12.0f

#define VMX_BURST_ITERS         200000u
#define VMX_FLOPS_PER_BURST     ((uint64_t)VMX_BURST_ITERS * 48ULL)

#define EMA_ALPHA               0.15f

extern const char _binary_spu_burn_elf_start[];

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;

static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static sys_spu_thread_group_t g_group;
static sys_spu_thread_t       g_threads[BURN_NUM_SPES];
static int                    g_group_active = 0;

static burn_params_t   g_params  [BURN_NUM_SPES] __attribute__((aligned(128)));
static burn_progress_t g_progress[BURN_NUM_SPES] __attribute__((aligned(128)));
static uint32_t        g_stop_flag[32]           __attribute__((aligned(128)));

static void    *g_dma_buffer    = NULL;
static uint32_t g_dma_buffer_sz = 0;

static burn_results_summary_t g_summary;

static uint64_t g_last_compute_counter = 0;
static uint64_t g_last_dma_counter     = 0;
static uint64_t g_burn_start_tb        = 0;
static uint64_t g_last_tick_tb         = 0;

void burn_benchmark_init(uint64_t timebase_freq, void *dma_buffer, uint32_t dma_buffer_size)
{
    g_tb_freq     = timebase_freq;
    g_dma_buffer  = dma_buffer;
    g_dma_buffer_sz = (dma_buffer_size >= BURN_DMA_BUFFER_BYTES) ? BURN_DMA_BUFFER_BYTES : dma_buffer_size;

    if (!g_dma_buffer || g_dma_buffer_sz < BURN_DMA_CHUNK_BYTES) {
        printf("[burn] init: NO DMA buffer (got %p, %u bytes) - burn-in will be disabled\n", g_dma_buffer, (unsigned)dma_buffer_size);
        g_initialized = 0;
        return;
    }
    g_initialized = 1;
    memset(&g_summary, 0, sizeof(g_summary));
    printf("[burn] init OK (4 compute SPEs + 2 DMA SPEs + PPE VMX, dma_buf=%p sz=%u MB)\n", g_dma_buffer, (unsigned)(g_dma_buffer_sz / (1024u * 1024u)));
}

const burn_results_summary_t *burn_get_results(void)
{
    return &g_summary;
}

static void record_failure(int step, int code)
{
    g_summary.last_err_step = step;
    g_summary.last_err_code = code;
    printf("[burn] FAIL step=%d code=0x%08x\n", step, (unsigned)code);
}

void burn_start(void)
{
    int      ret, i;

    if (!g_initialized || g_group_active) return;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_burn_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_burn_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { record_failure(ERR_IMG_IMPORT, ret); return; }
        }
        g_image_loaded = 1;
    }

    if (!g_dma_buffer || g_dma_buffer_sz < BURN_DMA_CHUNK_BYTES) {
        record_failure(ERR_BUFFER_ALLOC, 0);
        return;
    }

    memset(g_progress, 0, sizeof(g_progress));
    memset(g_stop_flag, 0, sizeof(g_stop_flag));
    g_last_compute_counter = 0;
    g_last_dma_counter     = 0;
    memset(&g_summary, 0, sizeof(g_summary));

    memset(g_params, 0, sizeof(g_params));
    for (i = 0; i < (int)BURN_NUM_SPES; i++) {
        g_params[i].spe_index    = (uint32_t)i;
        g_params[i].role         = (i < (int)BURN_NUM_COMPUTE_SPES) ? BURN_ROLE_COMPUTE : BURN_ROLE_DMA;
        g_params[i].ea_stop_flag = (uint64_t)(uintptr_t)g_stop_flag;
        g_params[i].ea_progress  = (uint64_t)(uintptr_t)&g_progress[i];
        g_params[i].ea_dma_buffer= (uint64_t)(uintptr_t)g_dma_buffer;
    }

    {
        sys_spu_thread_group_attribute_t group_attr;
        sys_spu_thread_attribute_t       thread_attr;
        sys_spu_thread_argument_t        thread_args;

        sys_spu_thread_group_attribute_initialize(group_attr);
        sys_spu_thread_group_attribute_name(group_attr, "burn");
        ret = sys_spu_thread_group_create(&g_group, BURN_NUM_SPES, 100, &group_attr);
        if (ret != CELL_OK) { record_failure(ERR_GRP_CREATE, ret); return; }

        for (i = 0; i < (int)BURN_NUM_SPES; i++) {
            sys_spu_thread_attribute_initialize(thread_attr);
            sys_spu_thread_attribute_name(thread_attr, "burn_spe");

            memset(&thread_args, 0, sizeof(thread_args));
            thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

            ret = sys_spu_thread_initialize(&g_threads[i], g_group, i, &g_spu_img, &thread_attr, &thread_args);
            if (ret != CELL_OK) {
                record_failure(ERR_THR_INIT, ret);
                sys_spu_thread_group_destroy(g_group);
                return;
            }
        }

        ret = sys_spu_thread_group_start(g_group);
        if (ret != CELL_OK) {
            record_failure(ERR_GRP_START, ret);
            sys_spu_thread_group_destroy(g_group);
            return;
        }
    }

    g_group_active = 1;
    g_summary.running = 1;
    SYS_TIMEBASE_GET(g_burn_start_tb);
    g_last_tick_tb = g_burn_start_tb;
    printf("[burn] 6-SPE group running (4 compute + 2 DMA), PPE VMX engaged\n");
}

void burn_stop(void)
{
    int cause, status;

    if (!g_group_active) return;

    g_stop_flag[0] = 1u;

    sys_spu_thread_group_join(g_group, &cause, &status);
    sys_spu_thread_group_destroy(g_group);

    g_group_active = 0;
    g_summary.running = 0;
    printf("[burn] stopped\n");
}

static void __attribute__((noinline)) vmx_burst(void)
{
    vector float a0 = { 1.0f, 1.1f, 1.2f, 1.3f };
    vector float a1 = { 1.4f, 1.5f, 1.6f, 1.7f };
    vector float a2 = { 1.8f, 1.9f, 2.0f, 2.1f };
    vector float a3 = { 2.2f, 2.3f, 2.4f, 2.5f };
    vector float a4 = { 2.6f, 2.7f, 2.8f, 2.9f };
    vector float a5 = { 3.0f, 3.1f, 3.2f, 3.3f };
    vector float mul = { 0.9999f, 0.9999f, 0.9999f, 0.9999f };
    vector float add = { 1.0e-7f, 2.0e-7f, 3.0e-7f, 4.0e-7f };
    uint32_t i;

    for (i = 0; i < VMX_BURST_ITERS; i += 8) {
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
        a0 = vec_madd(a0, mul, add); a1 = vec_madd(a1, mul, add);
        a2 = vec_madd(a2, mul, add); a3 = vec_madd(a3, mul, add);
        a4 = vec_madd(a4, mul, add); a5 = vec_madd(a5, mul, add);
    }
    a0 = vec_add(vec_add(a0, a1), vec_add(a2, a3));
    a0 = vec_add(a0, vec_add(a4, a5));
    __asm__ volatile ("" : "+v"(a0));  // who else but quagmire?
}

void burn_tick(uint64_t tb_freq)
{
    uint64_t t_pre_burst, t_post_burst;
    uint64_t compute_now = 0, dma_now = 0;
    int      i;

    if (!g_group_active) return;

    for (i = 0; i < (int)BURN_NUM_COMPUTE_SPES; i++)
        compute_now += g_progress[i].counter;
    for (i = (int)BURN_NUM_COMPUTE_SPES; i < (int)BURN_NUM_SPES; i++)
        dma_now += g_progress[i].counter;

    SYS_TIMEBASE_GET(t_pre_burst);
    vmx_burst();
    SYS_TIMEBASE_GET(t_post_burst);

    {
        double dt_inter_tick = (double)(t_post_burst - g_last_tick_tb) / (double)tb_freq;
        double dt_burst      = (double)(t_post_burst - t_pre_burst) / (double)tb_freq;
        if (dt_inter_tick < 1.0e-4 || dt_burst < 1.0e-5) {
            g_last_tick_tb = t_post_burst;
            return;
        }

        uint64_t d_compute = compute_now - g_last_compute_counter;
        uint64_t d_dma     = dma_now     - g_last_dma_counter;
        g_last_compute_counter = compute_now;
        g_last_dma_counter     = dma_now;

        float spe_gf  = (float)((double)d_compute / (dt_inter_tick * 1.0e9));
        float spe_gbs = (float)((double)d_dma     / (dt_inter_tick * 1.0e9));
        float ppe_gf  = (float)((double)VMX_FLOPS_PER_BURST / (dt_burst * 1.0e9));

        if (g_summary.spe_compute_gflops == 0.0f) {
            g_summary.spe_compute_gflops = spe_gf;
            g_summary.spe_dma_gbps       = spe_gbs;
            g_summary.ppe_vmx_gflops     = ppe_gf;
        } else {
            g_summary.spe_compute_gflops = (1.0f - EMA_ALPHA) * g_summary.spe_compute_gflops + EMA_ALPHA * spe_gf;
            g_summary.spe_dma_gbps       = (1.0f - EMA_ALPHA) * g_summary.spe_dma_gbps       + EMA_ALPHA * spe_gbs;
            g_summary.ppe_vmx_gflops     = (1.0f - EMA_ALPHA) * g_summary.ppe_vmx_gflops     + EMA_ALPHA * ppe_gf;
        }

        g_summary.spe_compute_util = g_summary.spe_compute_gflops / BURN_PEAK_SPE_GFLOPS;
        g_summary.spe_dma_util     = g_summary.spe_dma_gbps       / BURN_PEAK_DMA_GBPS;
        g_summary.ppe_vmx_util     = g_summary.ppe_vmx_gflops     / BURN_PEAK_PPE_GFLOPS;

        {
            float u1 = g_summary.spe_compute_util; if (u1 > 1.5f) u1 = 1.5f;
            float u2 = g_summary.spe_dma_util;     if (u2 > 1.5f) u2 = 1.5f;
            float u3 = g_summary.ppe_vmx_util;     if (u3 > 1.5f) u3 = 1.5f;
            double prod = (double)u1 * (double)u2 * (double)u3;
            if (prod > 0.0) {
                double gm = pow(prod, 1.0 / 3.0);
                g_summary.saturation_score = (float)(gm * 100.0);
            }
        }

        g_summary.elapsed_sec = (double)(t_post_burst - g_burn_start_tb) / (double)g_tb_freq;
        g_last_tick_tb = t_post_burst;
    }
}