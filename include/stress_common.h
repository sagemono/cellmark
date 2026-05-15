#ifndef STRESS_COMMON_H
#define STRESS_COMMON_H

#include <stdint.h>

#define MAX_SPES            6

/* compute stress config */
#define BATCH_ITERATIONS    (4 * 1024 * 1024)
#define VERIFY_INTERVAL     64
#define VERIFY_ITERATIONS   1024
#define FMA_CHAINS          12
#define FLOPS_PER_ITER      (FMA_CHAINS * 8)

/* operating modes */
#define MODE_COMPUTE_SP     0
#define MODE_MEMTEST        1
#define MODE_COMPUTE_DP     2
#define MODE_COMPUTE_INT    3
#define MODE_COMPUTE_RECIP  4
#define MODE_COMPUTE_SHUF   5
#define MODE_COMPUTE_DUAL   6
#define MODE_COUNT          7

/* compute benchmark IDs */
#define COMPUTE_SP_FMA      0
#define COMPUTE_DP_FMA      1
#define COMPUTE_INT_MUL     2
#define COMPUTE_RECIP       3
#define COMPUTE_SHUFFLE     4
#define COMPUTE_DUAL_ISSUE  5
#define COMPUTE_COUNT       6

/* memtest sub-tests */
#define MEMTEST_SEQUENTIAL      0
#define MEMTEST_WALKING_ONES    1
#define MEMTEST_CHECKERBOARD    2
#define MEMTEST_RANDOM          3
#define MEMTEST_MOVING_INV      4
#define MEMTEST_BANK_HAMMER     5
#define MEMTEST_RW_TURNAROUND   6
#define MEMTEST_BANDWIDTH       7
#define MEMTEST_OWN_ADDRESS     8
#define MEMTEST_MODULO_N        9
#define MEMTEST_BLOCK_MOVE      10
#define MEMTEST_BIT_FADE        11
#define MEMTEST_BW_PIPELINED    12
#define MEMTEST_COHERENCY       13
#define MEMTEST_LATENCY         14
#define MEMTEST_COUNT           15

/* DMA config */
#define DMA_MAX_SIZE        16384
#define DMA_ALIGN           128

/* XDR geometry */
#define XDR_NUM_BANKS       8
#define XDR_ROW_STRIDE      65536

/* modulo-N test spacing */
#define MODULO_N_STRIDE     20

/* bit fade test delay / decrementer ticks per wait chunk */
#define BIT_FADE_WAIT_SECS  5

/* mailbox */
#define MBOX_CMD_STOP       0x00000000

/* status codes */
#define STATUS_IDLE         0
#define STATUS_RUNNING      1
#define STATUS_ERROR        2
#define STATUS_DONE         3

/* params (PPE -> SPU) */
typedef struct {
    uint64_t ea_results;
    uint64_t ea_test_region;
    uint32_t test_region_size;
    uint32_t spe_index;
    uint32_t mode;
    uint32_t batch_iterations;
    uint32_t verify_interval;
    uint32_t memtest_id;        /* ignored if 0xFF = will autocycle all */
    uint32_t memtest_passes;    /* 0 = infinite */
    uint32_t tb_freq;
    uint32_t num_spes;          /* total SPEs active */
    uint64_t ea_coherency_region; /* shared region base for coherency test (SPE0s slice) */
    uint32_t coherency_region_size;
} __attribute__((aligned(128))) spe_params_t;

/* results (SPU -> PPE via DMA) */
typedef struct {
    /* compute mode fields */
    uint64_t total_flops;       /* +0x00 */
    uint32_t batches_done;      /* +0x08 */
    uint32_t verify_errors;     /* +0x0C */

    /* shared fields */
    uint32_t status;            /* +0x10 */
    uint32_t current_test;      /* +0x14: MEMTEST_* currently running */

    /* memtest progress */
    uint32_t memtest_pass;      /* +0x18: overall pass number (1-based) */
    uint32_t memtest_errors;    /* +0x1C: total bit errors found */
    uint64_t memtest_bytes;     /* +0x20: total bytes tested */
    uint32_t progress_pct;      /* +0x28: 0-100 progress within current test */
    uint32_t test_pass_mask;    /* +0x2C: bitmask of tests that passed (bit N = test N) */
    uint32_t test_fail_mask;    /* +0x30: bitmask of tests that had errors */
    uint32_t tests_completed;   /* +0x34: count of tests finished this pass */

    /* error details (first error only) */
    uint32_t memtest_err_addr;  /* +0x38 */
    uint32_t memtest_err_exp;   /* +0x3C */
    uint32_t memtest_err_got;   /* +0x40 */
    uint32_t memtest_err_bit;   /* +0x44 */

    /* bandwidth/latency (from MEMTEST_BANDWIDTH) */
    uint32_t bw_read_mbps;      /* +0x48 */
    uint32_t bw_write_mbps;     /* +0x4C */
    uint32_t lat_read_ns;       /* +0x50 */
    uint32_t lat_write_ns;      /* +0x54 */

    /* pipelined bandwidth (from MEMTEST_BW_PIPELINED) */
    uint32_t bw_pipe_read_mbps; /* +0x58 */
    uint32_t bw_pipe_write_mbps;/* +0x5C */

    /* live test info */
    uint32_t current_pattern;   /* +0x60: pattern being written/verified */

    /* per-pass timing (ms) */
    uint32_t last_pass_ms;      /* +0x64: duration of last completed pass */
    uint32_t avg_pass_ms;       /* +0x68: rolling average across passes */
    uint32_t min_pass_ms;       /* +0x6C: fastest pass */
    uint32_t max_pass_ms;       /* +0x70: slowest pass */

    /* precise latency (this is a 128 byte DMA microbenchmark, ns) */
    uint32_t lat_micro_read_ns; /* +0x74: avg read latency (128B DMA) */
    uint32_t lat_micro_write_ns;/* +0x78: avg write latency (128B DMA) */
} __attribute__((aligned(128))) spe_results_t;

/* stock timebase frequency */
#define STOCK_TB_FREQ       79800000ULL
#define STOCK_FREQ_GHZ      3.2f

/* theoretical peak ops/cycle/SPE for each compute mode:
 * SP FMA:     1 FMA/cyc * 4 lanes * 2 ops = 8 FLOPS/cyc (fully pipelined)
 * DP FMA:     1 DFMA/7cyc * 2 lanes * 2 ops = 4/7 FLOPS/cyc (7-cycle throughput)
 * Int mul:    1 mpya/cyc * 4 lanes * 2 ops = 8 IOPS/cyc (fully pipelined)
 * Recip:      1 even-pipe insn/cyc * 4 lanes = 4 ops/cyc
 * Shuffle:    1 odd-pipe insn/cyc * 4 word-lanes = 4 ops/cyc
 * Dual-issue: even FMA + odd shufb = 8 FLOPS + 4 OPS = 12 ops/cyc
 */
#define PEAK_OPS_SP_FMA     8
#define PEAK_OPS_DP_FMA     (4.0f / 7.0f)  /* dfma stalls even pipe 6 extra cycles */
#define PEAK_OPS_INT_MUL    8               /* mpya is fully pipelined, 1/cycle */
#define PEAK_OPS_RECIP      4
#define PEAK_OPS_SHUFFLE    4
#define PEAK_OPS_DUAL       12

/* autocycle sentinel */
#define MEMTEST_AUTO_CYCLE  0xFF

/* page indices */
#define PAGE_CELL           0
#define PAGE_PPE            1
#define PAGE_XDR            2
#define PAGE_DISK           3
#define PAGE_DMA            4
#define PAGE_EIB            5
#define PAGE_COUNT          6

#ifndef __SPU__

/* PPE benchmark indices */
#define PPE_BENCH_VMX_FMA   0
#define PPE_BENCH_L1_BW     1
#define PPE_BENCH_L2_BW     2
#define PPE_BENCH_L1_LAT    3
#define PPE_BENCH_L2_LAT    4
#define PPE_BENCH_COUNT     5

/* DMA benchmark indices (PAGE_DMA selector) */
#define DMA_BENCH_GET       0
#define DMA_BENCH_PUT       1
#define DMA_BENCH_COUNT     2

/* EIB benchmark indices (PAGE_EIB selector) */
#define EIB_BENCH_PAIRS     0   /* 0<->1, 2<->3, 4<->5: 3 disjoint LS-LS pairs */
#define EIB_BENCH_COUNT     1

/* PPE peak: 1 vmaddfp/cyc * 4 lanes * 2 ops = 8 GFLOPS/GHz (single PPE core) */
#define PEAK_PPE_VMX        8.0f

static inline const char *memtest_name(uint32_t id)
{
    switch (id) {
    case MEMTEST_SEQUENTIAL:    return "Sequential";
    case MEMTEST_WALKING_ONES:  return "Walking 1/0";
    case MEMTEST_CHECKERBOARD:  return "Checkerboard";
    case MEMTEST_RANDOM:        return "Random";
    case MEMTEST_MOVING_INV:    return "Moving Inv";
    case MEMTEST_BANK_HAMMER:   return "Bank Hammer";
    case MEMTEST_RW_TURNAROUND: return "R/W Turnaround";
    case MEMTEST_BANDWIDTH:     return "Bandwidth";
    case MEMTEST_OWN_ADDRESS:   return "Own Address";
    case MEMTEST_MODULO_N:      return "Modulo-N";
    case MEMTEST_BLOCK_MOVE:    return "Block Move";
    case MEMTEST_BIT_FADE:      return "Bit Fade";
    case MEMTEST_BW_PIPELINED:  return "BW Pipelined";
    case MEMTEST_COHERENCY:     return "Coherency";
    case MEMTEST_LATENCY:       return "Latency";
    default:                    return "Unknown";
    }
}

static inline const char *memtest_short_name(uint32_t id)
{
    switch (id) {
    case MEMTEST_SEQUENTIAL:    return "Seq";
    case MEMTEST_WALKING_ONES:  return "Wlk";
    case MEMTEST_CHECKERBOARD:  return "Chk";
    case MEMTEST_RANDOM:        return "Rnd";
    case MEMTEST_MOVING_INV:    return "Inv";
    case MEMTEST_BANK_HAMMER:   return "Bnk";
    case MEMTEST_RW_TURNAROUND: return "R/W";
    case MEMTEST_BANDWIDTH:     return "BW ";
    case MEMTEST_OWN_ADDRESS:   return "Own";
    case MEMTEST_MODULO_N:      return "Mod";
    case MEMTEST_BLOCK_MOVE:    return "Blk";
    case MEMTEST_BIT_FADE:      return "Fad";
    case MEMTEST_BW_PIPELINED:  return "Pip";
    case MEMTEST_COHERENCY:     return "Coh";
    case MEMTEST_LATENCY:       return "Lat";
    default:                    return "???";
    }
}

static inline const char *mode_name(uint32_t m)
{
    switch (m) {
    case MODE_COMPUTE_SP:    return "SP FMA Stress";
    case MODE_MEMTEST:       return "XDR Memtest";
    case MODE_COMPUTE_DP:    return "DP FMA Stress";
    case MODE_COMPUTE_INT:   return "Int Multiply";
    case MODE_COMPUTE_RECIP: return "Recip/Rsqrt";
    case MODE_COMPUTE_SHUF:  return "Shuffle Storm";
    case MODE_COMPUTE_DUAL:  return "Dual-Issue Max";
    default:                 return "Unknown";
    }
}

/* map compute benchmark index to mode */
static inline uint32_t compute_index_to_mode(uint32_t idx)
{
    switch (idx) {
    case COMPUTE_SP_FMA:     return MODE_COMPUTE_SP;
    case COMPUTE_DP_FMA:     return MODE_COMPUTE_DP;
    case COMPUTE_INT_MUL:    return MODE_COMPUTE_INT;
    case COMPUTE_RECIP:      return MODE_COMPUTE_RECIP;
    case COMPUTE_SHUFFLE:    return MODE_COMPUTE_SHUF;
    case COMPUTE_DUAL_ISSUE: return MODE_COMPUTE_DUAL;
    default:                 return MODE_COMPUTE_SP;
    }
}

static inline uint32_t mode_to_compute_index(uint32_t m)
{
    switch (m) {
    case MODE_COMPUTE_SP:    return COMPUTE_SP_FMA;
    case MODE_COMPUTE_DP:    return COMPUTE_DP_FMA;
    case MODE_COMPUTE_INT:   return COMPUTE_INT_MUL;
    case MODE_COMPUTE_RECIP: return COMPUTE_RECIP;
    case MODE_COMPUTE_SHUF:  return COMPUTE_SHUFFLE;
    case MODE_COMPUTE_DUAL:  return COMPUTE_DUAL_ISSUE;
    default:                 return COMPUTE_SP_FMA;
    }
}

static inline int mode_is_compute(uint32_t m)
{
    return m != MODE_MEMTEST;
}
#endif

#endif