/*
 * ppe_benchmarks.h - PPE (PowerPC) core benchmark declarations
 */
#ifndef PPE_BENCHMARKS_H
#define PPE_BENCHMARKS_H

#include <stdint.h>

#include "pmu.h"

typedef struct {
    float vmx_gflops;       /* VMX FMA throughput */
    float l1_gbps;          /* L1 cache read bandwidth */
    float l2_gbps;          /* L2 cache read bandwidth */
    float l1_lat_ns;        /* L1 pointer-chase latency */
    float l2_lat_ns;        /* L2 pointer-chase latency */
    float fp_scalar_gflops; /* scalar double FMA throughput (FPU) */
    float fxu_gops;         /* FXU integer add throughput */

    float smt_same_aggr_gflops;
    float smt_same_scaling;
    float smt_cross_fp_gflops;
    float smt_cross_fxu_gops;
    float smt_cross_scaling;
    uint32_t smt_runs;
} ppe_results_t;

void ppe_benchmarks_init(void);
void ppe_run_batch(int bench_id, uint64_t tb_freq);
const ppe_results_t *ppe_get_results(void);

const pmu_result_t  *ppe_get_pmu_result(void);
const pmu_profile_t *ppe_get_pmu_profile(void);

void ppe_get_pmu_summary(pmu_summary_t *out);

#endif