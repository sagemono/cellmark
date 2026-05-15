#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "pi_benchmark.h"

#define TAG_PARAMS  0
#define TAG_RESULT  1

extern uint8_t pi_hex_digit(uint32_t n);

static pi_params_t ls_params __attribute__((aligned(128)));
static pi_result_t ls_result __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t i;
    uint32_t start, count;
    uint32_t dec_start, dec_end;
    uint64_t result_ea;
    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(pi_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    start = ls_params.digit_start + ls_params.spe_index * ls_params.digit_count;
    count = ls_params.digit_count;
    if (count > PI_MAX_DIGITS_PER_SPE) count = PI_MAX_DIGITS_PER_SPE;

    for (i = 0; i < sizeof(ls_result); i++) {
        ((uint8_t *)&ls_result)[i] = 0u;
    }
    ls_result.digit_start = start;
    ls_result.digit_count = count;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    dec_start = spu_readch(SPU_RdDec);

    for (i = 0; i < count; i++) {
        ls_result.digits[i] = pi_hex_digit(start + i);
    }

    dec_end = spu_readch(SPU_RdDec);
    ls_result.dec_ticks = dec_start - dec_end;

    result_ea = ls_params.ea_results + (uint64_t)ls_params.spe_index * sizeof(pi_result_t);
    mfc_put(&ls_result, result_ea, sizeof(pi_result_t), TAG_RESULT, 0, 0);
    dma_wait(TAG_RESULT);

    return 0;
}