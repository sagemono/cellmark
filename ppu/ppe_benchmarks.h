/*
 * ppe_benchmarks.h - PPE (PowerPC) core benchmark declarations
 */
#ifndef PPE_BENCHMARKS_H
#define PPE_BENCHMARKS_H

#include <stdint.h>

typedef struct {
    float vmx_gflops;       /* VMX FMA throughput */
    float l1_gbps;          /* L1 cache read bandwidth */
    float l2_gbps;          /* L2 cache read bandwidth */
    float l1_lat_ns;        /* L1 pointer-chase latency */
    float l2_lat_ns;        /* L2 pointer-chase latency */
} ppe_results_t;

void ppe_benchmarks_init(void);
void ppe_run_batch(int bench_id, uint64_t tb_freq);
const ppe_results_t *ppe_get_results(void);

#endif
