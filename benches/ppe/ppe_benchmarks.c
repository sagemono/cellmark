/*
 * ppe_benchmarks.c - PPE (PowerPC core) benchmark implementations
 *
 * Benchmarks:
 *   VMX FMA   - vmaddfp throughput, 6 independent chains, 4-lane SIMD
 *               loop unrolled 8x to amortize branch overhead (~96% efficiency)
 *   L1 Read   - sequential VMX loads from 28KB (L1-resident) buffer
 *               loads issued ahead of accumulation to hide 7-cycle latency
 *   L2 Read   - sequential VMX loads from 400KB (L2-resident) buffer
 *   L1 Latency - random pointer chase through 14KB (L1-resident) array
 *   L2 Latency - random pointer chase through 256KB (L2-resident) array
 *
 * all results use EMA (a=0.1) for stable display.
 * all benchmarks run synchronously in the PPE main thread.
 * SPUs are stopped while on the PPE page for clean measurements.
 */

#include <altivec.h>
#include <sys/time_util.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stress_common.h"
#include "ppe_benchmarks.h"
#include "pmu.h"

#include <cell/perf/performance.h>

#define PPE_PMU_PROFILE_COUNT      3
#define PPE_PMU_ROTATE_SECONDS  3.0   /* dwell per profile before switching */

static const pmu_event_t g_ppe_pmu_events_issue[] = {
    { CELL_PERF_SIGNAL_PPU_TH0_TWO_PPC_INSTRUCTIONS_COMPLETED,    0, 0, "DUAL"   },
    { CELL_PERF_SIGNAL_PPU_TH0_ISSUE_STALL_DUE_TO_REG_DEPENDENCY, 0, 0, "RAW"    },
    { CELL_PERF_SIGNAL_PPU_TH0_L1_DCACHE_MISS,                    0, 0, "L1MISS" },
    { CELL_PERF_SIGNAL_PPU_TH0_LOAD_HIT_STORE,                    0, 0, "LHS"    },
};

static const pmu_event_t g_ppe_pmu_events_branch[] = {
    { CELL_PERF_SIGNAL_PPU_TH0_BRANCH_INSTRUCTION_COMPLETED,      0, 0, "BR"     },
    { CELL_PERF_SIGNAL_PPU_TH0_BRANCH_MISPREDICTION,              0, 0, "MISPRED"},
    { CELL_PERF_SIGNAL_PPU_TH0_INSTRUCTION_FLUSH,                 0, 0, "IFLUSH" },
    { CELL_PERF_SIGNAL_PPU_TH0_FLUSH_DUE_TO_STORE_QUEUE_FULL,     0, 0, "SQFULL" },
};

static const pmu_event_t g_ppe_pmu_events_mem[] = {
    { CELL_PERF_SIGNAL_L2_HIT,                                    0, 0, "L2HIT"  },
    { CELL_PERF_SIGNAL_L2_MISS,                                   0, 0, "L2MISS" },
    { CELL_PERF_SIGNAL_MIC_YAC0_READ_CMD_DISPATCHED,              0, 0, "XDR_RD" },
    { CELL_PERF_SIGNAL_MIC_YAC0_WRITE_CMD_DISPATCHED,             0, 0, "XDR_WR" },
};

static const pmu_profile_t g_ppe_pmu_profiles[PPE_PMU_PROFILE_COUNT] = {
    { g_ppe_pmu_events_issue,  4, -1, -1, "issue"  },
    { g_ppe_pmu_events_branch, 4, -1, -1, "branch" },
    { g_ppe_pmu_events_mem,    4, -1, -1, "mem"    },
};

static pmu_result_t  g_ppe_pmu_result;
static pmu_history_t g_ppe_pmu_histories[PPE_PMU_PROFILE_COUNT];
static int           g_ppe_pmu_profile_idx     = 0;
static uint64_t      g_ppe_pmu_last_rotate_tb  = 0;
static int           g_ppe_pmu_history_bench   = -1; // which bench filled the history; reset all profiles when this changes
static uint32_t      g_ppe_pmu_log_counter     = 0;  // throttle TTY spam

/* dcbt prefetch into L1+L2 (th=0). vec_dst is a no-op on cell per ibm, so dcbt is the right primitive */
static inline void prefetch_l1l2(const void *p)
{
    __asm__ __volatile__ ("dcbt 0,%0" : : "r"(p));
}

/* ----------------------------------------------------------------
 * buffer sizes
 * ---------------------------------------------------------------- */

/* L1 buffer: 28KB, leaves room for stack/code in 32KB L1 */
#define L1_BUF_VECS     (28 * 1024 / 16)    /* 1792 x vector float */

/* L2 buffer: 400KB, exceeds 32KB L1, fits in 512KB L2 */
#define L2_BUF_VECS     (400 * 1024 / 16)   /* 25600 x vector float */

/* L1 latency: 3584 entries * 8B (void*) = 28KB - L1-resident */
#define LAT_L1_COUNT    3584

/* L2 latency: 32768 entries * 8B (void*) = 256KB - L2-resident, exceeds L1 */
#define LAT_L2_COUNT    32768

/* batch sizes */
#define VMX_ITERS       (2 * 1024 * 1024)   /* 2M vmaddfp each batch (8x unrolled) */
#define L1_PASSES       512                  /* 512 passes * 28KB = 14MB */
#define L2_PASSES       32                   /* 32 passes * 400KB = 12.5MB */
#define LAT_CHASES      524288               /* 512K chases per batch */
#define FP_SCALAR_ITERS (1 * 1024 * 1024)    /* 1M iters * 8 chains * 8 unroll = 64M fmadd */
#define FXU_ITERS       (2 * 1024 * 1024)    /* 2M iters * 8 chains * 8 unroll = 128M add */

/* EMA smoothing factor (a for new sample) */
#define EMA_ALPHA       0.10f

static vector float __attribute__((aligned(128))) l1_buf[L1_BUF_VECS];
static vector float __attribute__((aligned(128))) l2_buf[L2_BUF_VECS];

/* pointer chase arrays: each entry holds the address of the next entry in the cycle

* inner chase compiles to `ld; bdnz` (2 insns, 4-cycle dependency) instead of the index based `rlwinm; extsw; add; lwz; bdnz` (5 insns, 7 cycle dependency). 
*
* on PPE that is the difference between ~12.5 cycles per chase and ~4 cycles per chase
*/
static void *lat_l1_ptrs[LAT_L1_COUNT] __attribute__((aligned(128)));
static void *lat_l2_ptrs[LAT_L2_COUNT] __attribute__((aligned(128)));

static ppe_results_t g_results;

/*
 * Sattolo cycle: single cycle visiting all N elements
 * differs from fisher-yates: j in [0, i-1] instead of [0, i].
*/
static void build_sattolo(void **buf, int count)
{
    uint32_t seed = 0xDEADBEEF;
    int i, j;
    /* identity start: each slot points to itself */
    for (i = 0; i < count; i++) buf[i] = &buf[i];
    /* shuffle into a single cycle */
    for (i = count - 1; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        j = (int)(seed % (uint32_t)i);   /* j in [0, i-1] */
        {
            void *t = buf[i];
            buf[i] = buf[j];
            buf[j] = t;
        }
    }
}

/* ----------------------------------------------------------------
 * ppe_benchmarks_init - fill buffers with non-trivial data
 * ---------------------------------------------------------------- */
void ppe_benchmarks_init(void)
{
    int i;
    for (i = 0; i < L1_BUF_VECS; i++) {
        l1_buf[i] = (vector float){
            (float)(i + 1), (float)(i + 2),
            (float)(i + 3), (float)(i + 4)};
    }
    for (i = 0; i < L2_BUF_VECS; i++) {
        l2_buf[i] = (vector float){
            (float)(i + 1), (float)(i + 2),
            (float)(i + 3), (float)(i + 4)};
    }
    build_sattolo(lat_l1_ptrs, LAT_L1_COUNT);
    build_sattolo(lat_l2_ptrs, LAT_L2_COUNT);
}

static void __attribute__((noinline)) run_vmx_fma(uint64_t tb_freq)
{
    vector float a0 = (vector float){1.0f, 1.1f, 1.2f, 1.3f};
    vector float a1 = (vector float){1.4f, 1.5f, 1.6f, 1.7f};
    vector float a2 = (vector float){1.8f, 1.9f, 2.0f, 2.1f};
    vector float a3 = (vector float){2.2f, 2.3f, 2.4f, 2.5f};
    vector float a4 = (vector float){2.6f, 2.7f, 2.8f, 2.9f};
    vector float a5 = (vector float){3.0f, 3.1f, 3.2f, 3.3f};
    vector float mul = (vector float){0.9999f, 0.9999f, 0.9999f, 0.9999f};
    vector float add = (vector float){1e-7f,   2e-7f,   3e-7f,   4e-7f  };
    uint64_t t0, t1;
    uint32_t i;

    /* VMX_ITERS must be divisible by 8 for the unrolled loop */
    SYS_TIMEBASE_GET(t0);
    for (i = 0; i < VMX_ITERS; i += 8) {
        /* 8 rounds per iteration, 6 chains each = 48 vmaddfp per loop */
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
    SYS_TIMEBASE_GET(t1);

    /* prevent gcc from removing the loop >:( */
    a0 = vec_add(vec_add(a0, a1), vec_add(a2, a3));
    a0 = vec_add(vec_add(a0, a4), a5);
    __asm__ volatile ("" : "+v" (a0));

    {
        double secs  = (double)(t1 - t0) / (double)tb_freq;
        double flops = (double)VMX_ITERS * 48.0; /* 6 chains * 4 lanes * 2 ops */
        float gflops = (float)(flops / (secs * 1.0e9));
        if (g_results.vmx_gflops == 0.0f) g_results.vmx_gflops = gflops;
        else g_results.vmx_gflops = (1.0f - EMA_ALPHA) * g_results.vmx_gflops + EMA_ALPHA * gflops;
    }
}

/* 
 * l1 read bandwidth: 28kb buffer fits l1, no prefetch needed
 *
 * inner uses a single moving pointer with vec_ld(offset, base) so gcc emits clean lvx with hoisted offsets, avoiding the per `load addi + clrldi` address formation overhead that gcc defaults to when
 * indexing through `l1_buf[i + n]`. loads r issued ahead of the adds to hide the 7 cycle vmx load to use latency
 */
static void __attribute__((noinline)) run_l1_bw(uint64_t tb_freq)
{
    vector float s0 = (vector float){0};
    vector float s1 = (vector float){0};
    vector float s2 = (vector float){0};
    vector float s3 = (vector float){0};
    vector float t0, t1, t2, t3, t4, t5, t6, t7;
    uint64_t ts0, ts1;
    int p, i;

    SYS_TIMEBASE_GET(ts0);
    for (p = 0; p < L1_PASSES; p++) {
        const vector float *base = l1_buf;
        for (i = 0; i < L1_BUF_VECS; i += 8) {
            t0 = vec_ld(0,   base);  t1 = vec_ld(16,  base);
            t2 = vec_ld(32,  base);  t3 = vec_ld(48,  base);
            t4 = vec_ld(64,  base);  t5 = vec_ld(80,  base);
            t6 = vec_ld(96,  base);  t7 = vec_ld(112, base);
            s0 = vec_add(s0, t0);    s1 = vec_add(s1, t1);
            s2 = vec_add(s2, t2);    s3 = vec_add(s3, t3);
            s0 = vec_add(s0, t4);    s1 = vec_add(s1, t5);
            s2 = vec_add(s2, t6);    s3 = vec_add(s3, t7);
            base += 8;
        }
    }
    SYS_TIMEBASE_GET(ts1);

    s0 = vec_add(vec_add(s0, s1), vec_add(s2, s3));
    __asm__ volatile ("" : "+v" (s0));

    {
        double secs  = (double)(ts1 - ts0) / (double)tb_freq;
        double bytes = (double)L1_PASSES * L1_BUF_VECS * 16.0;
        float gbps = (float)(bytes / (secs * 1.0e9));
        if (g_results.l1_gbps == 0.0f) g_results.l1_gbps = gbps;
        else g_results.l1_gbps = (1.0f - EMA_ALPHA) * g_results.l1_gbps + EMA_ALPHA * gbps;
    }
}

/*
 * l2 read bandwidth: 400kb buffer exceeds l1, needs prefetch.
 *
 * same single-pointer pattern as l1 bw, plus a `dcbt` prefetch issued
 * 4 cache lines (512 b) ahead of the current load. ppe l2 hit latency
 * is ~36 cycles; 4 lines ahead at 8 b/cycle issue gives 64 cycles of
 * cover, comfortably overlapping the l2 miss path
 *
 * per IBM tip 5: altivec
 * vec_dst()/dst variants are no-ops on cell. dcbt is the only prefetch primitive that takes effect
*/
static void __attribute__((noinline)) run_l2_bw(uint64_t tb_freq)
{
    vector float s0 = (vector float){0};
    vector float s1 = (vector float){0};
    vector float s2 = (vector float){0};
    vector float s3 = (vector float){0};
    vector float t0, t1, t2, t3, t4, t5, t6, t7;
    uint64_t ts0, ts1;
    int p, i;

    SYS_TIMEBASE_GET(ts0);
    for (p = 0; p < L2_PASSES; p++) {
        const vector float *base = l2_buf;
        const char *prefetch_end = (const char *)l2_buf
                                 + L2_BUF_VECS * 16 - 512;
        for (i = 0; i < L2_BUF_VECS; i += 8) {
            const char *cb = (const char *)base;
            if (cb < prefetch_end) {
                prefetch_l1l2(cb + 512);     /* 4 lines = 512 B ahead */
            }
            t0 = vec_ld(0,   base);  t1 = vec_ld(16,  base);
            t2 = vec_ld(32,  base);  t3 = vec_ld(48,  base);
            t4 = vec_ld(64,  base);  t5 = vec_ld(80,  base);
            t6 = vec_ld(96,  base);  t7 = vec_ld(112, base);
            s0 = vec_add(s0, t0);    s1 = vec_add(s1, t1);
            s2 = vec_add(s2, t2);    s3 = vec_add(s3, t3);
            s0 = vec_add(s0, t4);    s1 = vec_add(s1, t5);
            s2 = vec_add(s2, t6);    s3 = vec_add(s3, t7);
            base += 8;
        }
    }
    SYS_TIMEBASE_GET(ts1);

    s0 = vec_add(vec_add(s0, s1), vec_add(s2, s3));
    __asm__ volatile ("" : "+v" (s0));

    {
        double secs  = (double)(ts1 - ts0) / (double)tb_freq;
        double bytes = (double)L2_PASSES * L2_BUF_VECS * 16.0;
        float gbps = (float)(bytes / (secs * 1.0e9));
        if (g_results.l2_gbps == 0.0f) g_results.l2_gbps = gbps;
        else g_results.l2_gbps = (1.0f - EMA_ALPHA) * g_results.l2_gbps + EMA_ALPHA * gbps;
    }
}

/*
 * l1 / l2 latency: sattolo cycle
 *
 * every entry holds the address of the slot to visit next, so the inner compiles to `ld; bdnz+` (2 insns, 4-cycle dependency = 1 PPE L1 hit latency per chase) 
 * the previous index based version used 5 insns with a 7 cycle dep chain (rlwinm; extsw; add; lwz; bdnz), inflating measured L1 latency to around 12 cycles.
 */
static void __attribute__((noinline)) run_l1_lat(uint64_t tb_freq)
{
    void * volatile sink;
    register void *p = lat_l1_ptrs[0];
    uint64_t ts0, ts1;
    uint32_t i;

    SYS_TIMEBASE_GET(ts0);
    for (i = 0; i < LAT_CHASES; i++)
        p = *(void **)p;
    SYS_TIMEBASE_GET(ts1);

    sink = p; (void)sink;

    {
        double secs = (double)(ts1 - ts0) / (double)tb_freq;
        float lat = (float)(secs * 1.0e9 / LAT_CHASES);
        if (g_results.l1_lat_ns == 0.0f) g_results.l1_lat_ns = lat;
        else g_results.l1_lat_ns = (1.0f - EMA_ALPHA) * g_results.l1_lat_ns + EMA_ALPHA * lat;
    }
}

static void __attribute__((noinline)) run_l2_lat(uint64_t tb_freq)
{
    void * volatile sink;
    register void *p = lat_l2_ptrs[0];
    uint64_t ts0, ts1;
    uint32_t i;

    SYS_TIMEBASE_GET(ts0);
    for (i = 0; i < LAT_CHASES; i++)
        p = *(void **)p;
    SYS_TIMEBASE_GET(ts1);

    sink = p; (void)sink;

    {
        double secs = (double)(ts1 - ts0) / (double)tb_freq;
        float lat = (float)(secs * 1.0e9 / LAT_CHASES);
        if (g_results.l2_lat_ns == 0.0f) g_results.l2_lat_ns = lat;
        else g_results.l2_lat_ns = (1.0f - EMA_ALPHA) * g_results.l2_lat_ns + EMA_ALPHA * lat;
    }
}

static double __attribute__((noinline)) fp_scalar_kernel(uint32_t iters)
{
    volatile double mul_v = 0.9999;
    volatile double add_v = 1.0e-9;
    double mul = mul_v;
    double add = add_v;
    double a0 = 1.01, a1 = 1.02, a2 = 1.03, a3 = 1.04;
    double a4 = 1.05, a5 = 1.06, a6 = 1.07, a7 = 1.08;
    uint32_t i;

    for (i = 0; i < iters; i++) {
        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;

        a0 = a0 * mul + add; a1 = a1 * mul + add;
        a2 = a2 * mul + add; a3 = a3 * mul + add;
        a4 = a4 * mul + add; a5 = a5 * mul + add;
        a6 = a6 * mul + add; a7 = a7 * mul + add;
    }

    a0 += a1 + a2 + a3 + a4 + a5 + a6 + a7;
    __asm__ volatile ("" : "+f"(a0));
    return a0;
}

static void __attribute__((noinline)) run_fp_scalar(uint64_t tb_freq)
{
    uint64_t t0, t1;
    double sink;

    SYS_TIMEBASE_GET(t0);
    sink = fp_scalar_kernel(FP_SCALAR_ITERS);
    SYS_TIMEBASE_GET(t1);
    __asm__ volatile ("" : "+f"(sink));

    {
        double secs = (double)(t1 - t0) / (double)tb_freq;
        double ops  = (double)FP_SCALAR_ITERS * 64.0 * 2.0;
        float gflops = (float)(ops / (secs * 1.0e9));
        if (g_results.fp_scalar_gflops == 0.0f) g_results.fp_scalar_gflops = gflops;
        else g_results.fp_scalar_gflops = (1.0f - EMA_ALPHA) * g_results.fp_scalar_gflops + EMA_ALPHA * gflops;
    }
}

#define FXU_ADD(a, s) __asm__ volatile ("add %0, %0, %1" : "+r"(a) : "r"(s))

static uint64_t __attribute__((noinline)) fxu_kernel(uint32_t iters)
{
    volatile uint64_t s0_v = 1, s1_v = 2, s2_v = 3, s3_v = 4;
    volatile uint64_t s4_v = 5, s5_v = 6, s6_v = 7, s7_v = 8;
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    uint64_t a4 = 0, a5 = 0, a6 = 0, a7 = 0;
    uint64_t s0 = s0_v, s1 = s1_v, s2 = s2_v, s3 = s3_v;
    uint64_t s4 = s4_v, s5 = s5_v, s6 = s6_v, s7 = s7_v;
    uint32_t i;

    for (i = 0; i < iters; i++) {
        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);

        FXU_ADD(a0, s0); FXU_ADD(a1, s1); FXU_ADD(a2, s2); FXU_ADD(a3, s3);
        FXU_ADD(a4, s4); FXU_ADD(a5, s5); FXU_ADD(a6, s6); FXU_ADD(a7, s7);
    }

    a0 += a1 + a2 + a3 + a4 + a5 + a6 + a7;
    __asm__ volatile ("" : "+r"(a0));
    return a0;
}

static void __attribute__((noinline)) run_fxu(uint64_t tb_freq)
{
    uint64_t t0, t1;
    uint64_t sink;

    SYS_TIMEBASE_GET(t0);
    sink = fxu_kernel(FXU_ITERS);
    SYS_TIMEBASE_GET(t1);
    __asm__ volatile ("" : "+r"(sink));

    {
        double secs = (double)(t1 - t0) / (double)tb_freq;
        double ops  = (double)FXU_ITERS * 64.0;   /* 64 adds per iter */
        float gops  = (float)(ops / (secs * 1.0e9));
        if (g_results.fxu_gops == 0.0f) g_results.fxu_gops = gops;
        else g_results.fxu_gops = (1.0f - EMA_ALPHA) * g_results.fxu_gops + EMA_ALPHA * gops;
    }
}

#define SMT_FP_ITERS    (2u * 1024u * 1024u)
#define SMT_FXU_ITERS   (1280u * 1024u)

#define SMT_CMD_NONE    0u
#define SMT_CMD_FP      1u
#define SMT_CMD_FXU     2u
#define SMT_CMD_EXIT    0xFFu

typedef struct {
    volatile uint32_t cmd;
    volatile uint32_t go;
    volatile uint32_t done;
    volatile uint32_t pad0;
    volatile uint64_t t_start;
    volatile uint64_t t_end;
    uint32_t          iters;
    uint32_t          pad1[23];
} __attribute__((aligned(128))) smt_worker_state_t;

static smt_worker_state_t g_smt __attribute__((aligned(128)));
static sys_ppu_thread_t   g_smt_thread;
static int                g_smt_thread_alive = 0;

static void smt_worker_entry(uint64_t arg)
{
    (void)arg;
    for (;;) {
        while (!g_smt.go) { sys_timer_usleep(50); }
        __asm__ volatile ("lwsync" : : : "memory");

        if (g_smt.cmd == SMT_CMD_EXIT) break;

        SYS_TIMEBASE_GET(g_smt.t_start);
        if (g_smt.cmd == SMT_CMD_FP) {
            volatile double sink = fp_scalar_kernel(g_smt.iters);
            (void)sink;
        } else if (g_smt.cmd == SMT_CMD_FXU) {
            volatile uint64_t sink = fxu_kernel(g_smt.iters);
            (void)sink;
        }
        SYS_TIMEBASE_GET(g_smt.t_end);

        __asm__ volatile ("lwsync" : : : "memory");
        g_smt.go = 0;
        g_smt.done = 1;
    }
    sys_ppu_thread_exit(0);
}

static int smt_ensure_worker(void)
{
    int ret;
    if (g_smt_thread_alive) return 0;

    memset((void *)&g_smt, 0, sizeof(g_smt));

    ret = sys_ppu_thread_create(&g_smt_thread, smt_worker_entry, 0, 100, 65536, SYS_PPU_THREAD_CREATE_JOINABLE, "cellmark_smt");
    if (ret != CELL_OK) {
        printf("[smt] sys_ppu_thread_create failed: 0x%x\n", ret);
        return ret;
    }
    g_smt_thread_alive = 1;
    return 0;
}

static void smt_dispatch_worker(uint32_t cmd, uint32_t iters)
{
    g_smt.cmd    = cmd;
    g_smt.iters  = iters;
    g_smt.done   = 0;
    g_smt.t_start= 0;
    g_smt.t_end  = 0;
    __asm__ volatile ("lwsync" : : : "memory");
    g_smt.go = 1;
}

static void smt_wait_worker(void)
{
    while (!g_smt.done) { __asm__ volatile (""); }
    __asm__ volatile ("lwsync" : : : "memory");
}

static void __attribute__((noinline)) run_smt_scaling(uint64_t tb_freq)
{
    uint64_t mt0, mt1;
    double   m_secs, w_secs, span_secs;
    double   fp_ops_one  = (double)SMT_FP_ITERS  * 64.0 * 2.0;
    double   fxu_ops_one = (double)SMT_FXU_ITERS * 64.0;
    float    fp_solo, fxu_solo;

    if (smt_ensure_worker() != 0) return;

    SYS_TIMEBASE_GET(mt0);
    {
        volatile double sink = fp_scalar_kernel(SMT_FP_ITERS);
        (void)sink;
    }
    SYS_TIMEBASE_GET(mt1);
    fp_solo = (float)(fp_ops_one / (((double)(mt1 - mt0) / (double)tb_freq) * 1.0e9));

    SYS_TIMEBASE_GET(mt0);
    {
        volatile uint64_t sink = fxu_kernel(SMT_FXU_ITERS);
        (void)sink;
    }
    SYS_TIMEBASE_GET(mt1);
    fxu_solo = (float)(fxu_ops_one / (((double)(mt1 - mt0) / (double)tb_freq) * 1.0e9));

    smt_dispatch_worker(SMT_CMD_FP, SMT_FP_ITERS);
    SYS_TIMEBASE_GET(mt0);
    {
        volatile double sink = fp_scalar_kernel(SMT_FP_ITERS);
        (void)sink;
    }
    SYS_TIMEBASE_GET(mt1);
    smt_wait_worker();
    {
        uint64_t s0 = (mt0 < g_smt.t_start) ? mt0 : g_smt.t_start;
        uint64_t e0 = (mt1 > g_smt.t_end)   ? mt1 : g_smt.t_end;
        span_secs = (double)(e0 - s0) / (double)tb_freq;
        m_secs    = (double)(mt1 - mt0) / (double)tb_freq;
        w_secs    = (double)(g_smt.t_end - g_smt.t_start) / (double)tb_freq;
    }
    {
        float aggr_gflops = (float)((2.0 * fp_ops_one) / (span_secs * 1.0e9));
        if (g_results.smt_same_aggr_gflops == 0.0f)
            g_results.smt_same_aggr_gflops = aggr_gflops;
        else
            g_results.smt_same_aggr_gflops = (1.0f - EMA_ALPHA) * g_results.smt_same_aggr_gflops + EMA_ALPHA * aggr_gflops;
        if (fp_solo > 0.0f)
            g_results.smt_same_scaling = g_results.smt_same_aggr_gflops / fp_solo;
        (void)m_secs; (void)w_secs;
    }

    smt_dispatch_worker(SMT_CMD_FXU, SMT_FXU_ITERS);
    SYS_TIMEBASE_GET(mt0);
    {
        volatile double sink = fp_scalar_kernel(SMT_FP_ITERS);
        (void)sink;
    }
    SYS_TIMEBASE_GET(mt1);
    smt_wait_worker();

    m_secs = (double)(mt1 - mt0) / (double)tb_freq;
    w_secs = (double)(g_smt.t_end - g_smt.t_start) / (double)tb_freq;
    {
        float fp_cross  = (float)(fp_ops_one  / (m_secs * 1.0e9));
        float fxu_cross = (float)(fxu_ops_one / (w_secs * 1.0e9));
        if (g_results.smt_cross_fp_gflops == 0.0f) g_results.smt_cross_fp_gflops = fp_cross;
        else g_results.smt_cross_fp_gflops = (1.0f - EMA_ALPHA) * g_results.smt_cross_fp_gflops + EMA_ALPHA * fp_cross;
        if (g_results.smt_cross_fxu_gops == 0.0f) g_results.smt_cross_fxu_gops = fxu_cross;
        else g_results.smt_cross_fxu_gops = (1.0f - EMA_ALPHA) * g_results.smt_cross_fxu_gops + EMA_ALPHA * fxu_cross;

        if (fp_solo > 0.0f && fxu_solo > 0.0f) {
            float fp_ret  = g_results.smt_cross_fp_gflops / fp_solo;
            float fxu_ret = g_results.smt_cross_fxu_gops  / fxu_solo;
            g_results.smt_cross_scaling = fp_ret + fxu_ret;
        }
    }

    g_results.smt_runs++;
}

void ppe_run_batch(int bench_id, uint64_t tb_freq)
{
    sys_ppu_thread_t self;
    int old_prio = -1, ret;

    if (sys_ppu_thread_get_id(&self) == CELL_OK) {
        ret = sys_ppu_thread_get_priority(self, &old_prio);
        if (ret == CELL_OK)
            sys_ppu_thread_set_priority(self, 100);
        else
            old_prio = -1;
    }

    if (bench_id != g_ppe_pmu_history_bench) {
        memset(g_ppe_pmu_histories, 0, sizeof(g_ppe_pmu_histories));
        g_ppe_pmu_history_bench = bench_id;
        g_ppe_pmu_log_counter   = 0;
        g_ppe_pmu_profile_idx   = 0;
        SYS_TIMEBASE_GET(g_ppe_pmu_last_rotate_tb);
    }
    {
        uint64_t now_tb;
        double   since_s;
        SYS_TIMEBASE_GET(now_tb);
        since_s = (double)(now_tb - g_ppe_pmu_last_rotate_tb) / (double)tb_freq;
        if (since_s >= PPE_PMU_ROTATE_SECONDS) {
            const pmu_profile_t *outgoing = &g_ppe_pmu_profiles[g_ppe_pmu_profile_idx];
            pmu_summary_t s;
            static const char *bench_short[PPE_BENCH_COUNT] = {
                "vmx_fma", "l1_bw", "l2_bw", "l1_lat", "l2_lat",
                "fp_scalar", "fxu", "smt"
            };
            const char *bn = (bench_id >= 0 && bench_id < PPE_BENCH_COUNT) ? bench_short[bench_id] : "ppe";
            char tag[40];
            pmu_history_summary(&g_ppe_pmu_histories[g_ppe_pmu_profile_idx], &s);
            snprintf(tag, sizeof(tag), "%s/%s", bn, outgoing->name ? outgoing->name : "?");
            pmu_log_dump_summary(tag, outgoing, &s);

            g_ppe_pmu_profile_idx = (g_ppe_pmu_profile_idx + 1) % PPE_PMU_PROFILE_COUNT;
            g_ppe_pmu_last_rotate_tb = now_tb;
        }
    }

    pmu_begin(&g_ppe_pmu_profiles[g_ppe_pmu_profile_idx]);

    switch (bench_id) {
    case PPE_BENCH_VMX_FMA:   run_vmx_fma(tb_freq);   break;
    case PPE_BENCH_L1_BW:     run_l1_bw(tb_freq);     break;
    case PPE_BENCH_L2_BW:     run_l2_bw(tb_freq);     break;
    case PPE_BENCH_L1_LAT:    run_l1_lat(tb_freq);    break;
    case PPE_BENCH_L2_LAT:    run_l2_lat(tb_freq);    break;
    case PPE_BENCH_FP_SCALAR: run_fp_scalar(tb_freq); break;
    case PPE_BENCH_FXU:       run_fxu(tb_freq);       break;
    case PPE_BENCH_SMT:       run_smt_scaling(tb_freq); break;
    }

    pmu_end_and_read(&g_ppe_pmu_result);

    if (old_prio >= 0)
        sys_ppu_thread_set_priority(self, old_prio);

    if (g_ppe_pmu_result.ok) {
        pmu_history_push(&g_ppe_pmu_histories[g_ppe_pmu_profile_idx], &g_ppe_pmu_result);
        g_ppe_pmu_log_counter++;
    }
}

const ppe_results_t *ppe_get_results(void)
{
    return &g_results;
}

const pmu_result_t *ppe_get_pmu_result(void)
{
    return &g_ppe_pmu_result;
}

const pmu_profile_t *ppe_get_pmu_profile(void)
{
    return &g_ppe_pmu_profiles[g_ppe_pmu_profile_idx];
}

void ppe_get_pmu_summary(pmu_summary_t *out)
{
    pmu_history_summary(&g_ppe_pmu_histories[g_ppe_pmu_profile_idx], out);
}