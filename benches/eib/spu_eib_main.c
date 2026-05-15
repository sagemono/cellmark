#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include <sys/spu_thread.h>

#include "eib_benchmark.h"

extern uint64_t spu_dma_get_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations, uint32_t queue_depth, uint32_t chunk_size);

#define EIB_CHUNK_SIZE 16384u

#define TAG_PARAMS    0
#define TAG_RESULTS   1

static eib_params_t  ls_params  __attribute__((aligned(128)));
static eib_results_t ls_results __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint64_t bytes;
    uint64_t partner_ls_ea;
    (void)arg2;
    (void)arg3;
    (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(eib_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    partner_ls_ea = (uint64_t)SYS_SPU_THREAD_BASE_LOW + (uint64_t)ls_params.partner_idx * (uint64_t)SYS_SPU_THREAD_OFFSET;

    bytes = spu_dma_get_kernel(partner_ls_ea, ls_params.window_size, ls_params.iterations, ls_params.queue_depth, EIB_CHUNK_SIZE);

    ls_results.total_bytes = bytes;
    mfc_put(&ls_results, ls_params.ea_results, sizeof(eib_results_t), TAG_RESULTS, 0, 0);
    dma_wait(TAG_RESULTS);

    return 0;
}