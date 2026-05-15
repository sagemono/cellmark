#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "dma_benchmark.h"

extern uint64_t spu_dma_get_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations);
extern uint64_t spu_dma_put_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations);

#define TAG_PARAMS    0
#define TAG_RESULTS   1

static dma_params_t  ls_params  __attribute__((aligned(128)));
static dma_results_t ls_results __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint64_t bytes;
    (void)arg2;
    (void)arg3;
    (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(dma_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    if (ls_params.direction == DMA_DIR_PUT) {
        bytes = spu_dma_put_kernel(ls_params.ea_test_region, ls_params.window_size, ls_params.iterations);
    } else {
        bytes = spu_dma_get_kernel(ls_params.ea_test_region, ls_params.window_size, ls_params.iterations);
    }

    ls_results.total_bytes = bytes;
    ls_results.status      = DMA_ST_DONE;
    mfc_put(&ls_results, ls_params.ea_results, sizeof(dma_results_t), TAG_RESULTS, 0, 0);
    dma_wait(TAG_RESULTS);

    return 0;
}