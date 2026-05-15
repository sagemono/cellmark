#ifndef CELL_PMU_H
#define CELL_PMU_H

#include <stdint.h>
#include "pmu.h"

void cell_pmu_tick(int active, int current_kernel_id);

void                 cell_pmu_get_summary(pmu_summary_t *out);
const pmu_profile_t *cell_pmu_get_profile(void);

int  cell_pmu_get_kernel_id(void);

#endif /* CELL_PMU_H */