#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "branch_benchmark.h"

#define TAG_PARAMS 0

extern uint32_t spu_stress_branch_hinted_kernel(uint32_t iterations);
extern uint32_t spu_stress_branch_unhinted_kernel(uint32_t iterations);

static branch_params_t ls_params __attribute__((aligned(128)));

static inline void dma_wait_tag(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t iterations;
    uint32_t dec_start, dec_end;
    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(branch_params_t), TAG_PARAMS, 0, 0);
    dma_wait_tag(TAG_PARAMS);

    iterations = ls_params.iterations;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    dec_start = spu_readch(SPU_RdDec);
    (void)spu_stress_branch_hinted_kernel(iterations);
    dec_end   = spu_readch(SPU_RdDec);
    ls_params.hinted_dec_ticks = dec_start - dec_end;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    dec_start = spu_readch(SPU_RdDec);
    (void)spu_stress_branch_unhinted_kernel(iterations);
    dec_end   = spu_readch(SPU_RdDec);
    ls_params.unhinted_dec_ticks = dec_start - dec_end;

    mfc_put(&ls_params, arg_ea, sizeof(branch_params_t), TAG_PARAMS, 0, 0);
    dma_wait_tag(TAG_PARAMS);

    return 0;
}