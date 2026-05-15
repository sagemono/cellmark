#ifndef EIB_BENCHMARK_H
#define EIB_BENCHMARK_H

#include <stdint.h>

#include "pmu.h"

#define EIB_DIR_GET     0

#define EIB_NUM_SPES        6
#define EIB_WINDOW_SIZE     131072u

typedef struct {
    uint64_t ea_results;
    uint32_t partner_idx;
    uint32_t window_size;
    uint32_t iterations;
    uint32_t direction;
    uint32_t spe_index;
    uint32_t queue_depth;
    uint32_t pad[24];
} __attribute__((aligned(128))) eib_params_t;

typedef struct {
    uint64_t total_bytes;
    uint32_t pad[30];
} __attribute__((aligned(128))) eib_results_t;

typedef struct {
    float    pairs_get_mbps;
    uint32_t pairs_runs;
    float    hotspot_mbps;
    uint32_t hotspot_runs;
} eib_results_summary_t;

#define EIB_SWEEP_COUNT     5
typedef struct {
    float    mbps[EIB_SWEEP_COUNT];
    uint32_t runs[EIB_SWEEP_COUNT];
    int      active_idx;
} eib_sweep_summary_t;

const eib_sweep_summary_t *eib_get_sweep_summary(void);
#define EIB_NXN_SPES        6
typedef struct {
    float    mbps[EIB_NXN_SPES][EIB_NXN_SPES];
    uint32_t runs[EIB_NXN_SPES][EIB_NXN_SPES];
    int      active_src;
    int      active_dst;
} eib_nxn_summary_t;

const eib_nxn_summary_t *eib_get_nxn_summary(void);

void eib_benchmark_init(uint64_t timebase_freq);

float eib_benchmark_run(int mode, int active_readers, int src, int dst);

void eib_run_batch(int bench_id, uint64_t tb_freq);

const eib_results_summary_t *eib_get_results(void);

void                 eib_get_pmu_summary(pmu_summary_t *out);
const pmu_profile_t *eib_get_pmu_profile(void);

#endif /* EIB_BENCHMARK_H */