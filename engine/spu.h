/*
 * spu.h - SPU thread group lifecycle
 */
#ifndef SPU_H
#define SPU_H

#include <stdint.h>
#include "stress_common.h"

extern spe_params_t  spe_params[];
extern spe_results_t spe_results[];
extern int           num_spes_active;

int init_spu(void);

/*
 * start_spu_stress create the SPU thread group and start all SPEs
 *
 * mode            : MODE_COMPUTE_* or MODE_MEMTEST
 * memtest_id      : MEMTEST_* or MEMTEST_AUTO_CYCLE (ignored for compute)
 * memtest_region  : base address of the XDR test region (NULL for compute)
 * region_size     : total bytes in the test region
 * tb_freq         : timebase frequency in Hz (passed to SPEs)
*/
int start_spu_stress(int mode, int memtest_id, void *memtest_region, uint32_t region_size, uint32_t tb_freq);

void stop_spu_stress(void);

/* spu_get_thread - return the lv2 thread id of stress-group SPE n.
 * returns 0 if idx is out of range or the group isn't running.
 * used by the cell-page PMU sample-window state machine */
uint32_t spu_get_thread(int idx);

#endif /* SPU_H */