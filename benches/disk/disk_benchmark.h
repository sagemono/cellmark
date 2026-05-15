/*
 * disk_benchmark.h - HDD/SSD/SSHD sequential and random I/O benchmarks
 *
 * yoinked from CrystalDiskMark:
 *   Sequential Read/Write - 64KB-chunk transfers, 32MB file
 *   Random 4K Read/Write - 4K blocks at random offsets, 512 ops each
 *
 * All results in MB/s. ema smoothed across passes for stable display.
 * write results are post fsync (true media latency, not write-cache).
 *
 * Benchmarks run on a background PPU thread (disk_trigger_bench) so
 * the main render loop stays responsive during I/O
 */
#ifndef DISK_BENCHMARK_H
#define DISK_BENCHMARK_H

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>

#ifdef CELLMARK_DECR
#define DISK_TEST_FILE  "/dev_hdd0/game/CELLMARKD/USRDIR/disktest.bin"
#define DISK_PROBE_FILE "/dev_hdd0/game/CELLMARKD/USRDIR/dskprobe.bin"
#else
#define DISK_TEST_FILE  "/dev_hdd0/game/CELLMARK0/USRDIR/disktest.bin"
#define DISK_PROBE_FILE "/dev_hdd0/game/CELLMARK0/USRDIR/dskprobe.bin"
#endif

/* benchmark IDs */
#define DISK_BENCH_SEQ_READ     0
#define DISK_BENCH_SEQ_WRITE    1
#define DISK_BENCH_RND4K_READ   2
#define DISK_BENCH_RND4K_WRITE  3
#define DISK_BENCH_COUNT        4

/* status codes */
#define DISK_STATUS_IDLE        0   /* no test run yet */
#define DISK_STATUS_PREPARE     1   /* creating/filling test file */
#define DISK_STATUS_RUNNING     2   /* benchmark in progress */
#define DISK_STATUS_DONE        3   /* result valid */
#define DISK_STATUS_ERROR       4   /* cellFs error */

typedef struct {
    volatile float    mbps;      /* current result (EMA) */
    volatile uint32_t status;    /* DISK_STATUS_* */
    volatile int32_t  last_error;/* last cellFs return code, 0 = ok */
} disk_result_t;

typedef struct {
    disk_result_t results[DISK_BENCH_COUNT];
    uint32_t      file_ready;   /* 1 if test file has been prepared */
} disk_bench_state_t;

void disk_benchmark_init(void);

void disk_trigger_bench(int bench_id, uint64_t tb_freq);

/* 1 while the background thread is active, 0 when done */
int disk_is_running(void);

const disk_bench_state_t *disk_get_state(void);

#define DISK_PROBE_FILE_SZ (32 * 1024 * 1024)   /* 32 MB: fast iteration */
#define DISK_PROBE_COUNT   13

#define PROBE_ST_PENDING   0
#define PROBE_ST_RUNNING   1
#define PROBE_ST_DONE      2
#define PROBE_ST_ERROR     3

typedef struct {
    char  label[18];   /* short display label          */
    float mbps;        /* result, 0 if not done yet    */
    int   state;       /* PROBE_ST_*                   */
} disk_probe_t;

/* Start all probes on a background thread (ignored if already running) */
void disk_run_probes(uint64_t tb_freq);

/* 1 while the probe thread is active */
int disk_probes_running(void);

/* index of currently executing probe, -1 if idle */
int disk_probe_current(void);

/* pointer to DISK_PROBE_COUNT-element results array */
const disk_probe_t *disk_get_probes(void);

#endif
