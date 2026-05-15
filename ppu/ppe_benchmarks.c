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
#include <stdint.h>
#include <string.h>

#include "stress_common.h"
#include "ppe_benchmarks.h"

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

/*
 * vmx fma: 6 independent vmaddfp chains, 8x unrolled.
 *
 * without unrolling: 6 vmaddfp + 3 loop insns = 9 cycles/iter -> ~67% eff.
 * with 8x unroll:   48 vmaddfp + 1 bdnz+      = 49 cycles/iter
 *                   -> peak = 384 flops / 49 cycles ~= 25 gflops at 3.2 ghz.
 *
 * disassembly of the inner is clean:
 *  no microcoded ops, no rc=1 forms,
 *  no spills. the kernel is operating as designed!
 *
 * on / lv2 / gameos, single-thread vmx fma on the ppe caps at
 * around 50% of the 25.6 gflops peak documented above. that cap is
 * the platform, not the code
 *
 * 25.6 gflops is the bare-metal peak. ~12 gflops is the practical ps3
 * peak for single-thread ppe-vmx fma. the cell programming model
 * intentionally pushes vectorised work to the spus, where 6 spes hit
 * ~150 gflops combined - more than 12x the ppe-vmx ceiling here.
*/
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

/* 
 * ppe_run_batch
 *
 * raises the calling PPU thread's lv2 priority to a high value for the duration of the timed kernel, then restores the original priority
 */
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

    switch (bench_id) {
    case PPE_BENCH_VMX_FMA: run_vmx_fma(tb_freq); break;
    case PPE_BENCH_L1_BW:   run_l1_bw(tb_freq);   break;
    case PPE_BENCH_L2_BW:   run_l2_bw(tb_freq);   break;
    case PPE_BENCH_L1_LAT:  run_l1_lat(tb_freq);  break;
    case PPE_BENCH_L2_LAT:  run_l2_lat(tb_freq);  break;
    }

    if (old_prio >= 0)
        sys_ppu_thread_set_priority(self, old_prio);
}

const ppe_results_t *ppe_get_results(void)
{
    return &g_results;
}