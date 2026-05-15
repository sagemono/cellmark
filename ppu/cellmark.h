/*
 * cellmark.h - app level state shared across render modules
 */
#ifndef CELLMARK_H
#define CELLMARK_H

#include <stdint.h>

extern int      current_page;       /* PAGE_CELL / PAGE_PPE / PAGE_XDR / PAGE_DISK */
extern int      current_mode;       /* MODE_COMPUTE_* or MODE_MEMTEST */
extern int      current_memtest;    /* MEMTEST_* or MEMTEST_AUTO_CYCLE */
extern int      current_ppe_bench;  /* PPE_BENCH_* */
extern int      current_disk_bench; /* DISK_BENCH_* */
extern int      current_dma_bench;  /* DMA_BENCH_GET / DMA_BENCH_PUT */
extern int      current_eib_bench;  /* EIB_BENCH_PAIRS */
extern int      disk_probe_mode;    /* 0 = bench view, 1 = probe view */
extern int      log_enabled;        /* 0 = off, 1 = on */
extern uint32_t memtest_region_size;/* bytes of XDR allocated for memtest */
extern uint64_t tb_frequency;       /* timebase ticks per second */
extern uint64_t tb_start;           /* timebase value at app launch */

double get_elapsed_seconds(void);

#endif /* CELLMARK_H */