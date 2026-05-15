#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "fft_benchmark.h"

typedef vector float vfloat;

extern void fft_init(void);
extern void fft_run_batch(vfloat *re, vfloat *im);

#define TAG_PARAMS  0
#define TAG_RESULT  1

static fft_params_t ls_params __attribute__((aligned(128)));
static fft_result_t ls_result __attribute__((aligned(128)));

static vfloat re_buf[FFT_N] __attribute__((aligned(128)));
static vfloat im_buf[FFT_N] __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

static inline float scalar_abs(float x) { return (x < 0.0f) ? -x : x; }

static void load_impulse(void)
{
    int    i;
    vfloat vzero = spu_splats(0.0f);
    vfloat vone  = spu_splats(1.0f);
    for (i = 0; i < (int)FFT_N; i++) {
        re_buf[i] = vzero;
        im_buf[i] = vzero;
    }
    re_buf[0] = vone;
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t i;
    uint32_t dec_start, dec_end;
    uint32_t simd_batches;
    uint64_t result_ea;
    float    max_err = 0.0f;
    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(fft_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    fft_init();

    for (i = 0; i < sizeof(ls_result); i++) {
        ((uint8_t *)&ls_result)[i] = 0u;
    }

    load_impulse();
    fft_run_batch(re_buf, im_buf);
    for (i = 0; i < FFT_N; i++) {
        float er = scalar_abs(spu_extract(re_buf[i], 0) - 1.0f);
        float ei = scalar_abs(spu_extract(im_buf[i], 0));
        if (er > max_err) max_err = er;
        if (ei > max_err) max_err = ei;
    }
    ls_result.first_fft_max_err = max_err;

    load_impulse();
    simd_batches = ls_params.ffts_to_run / FFT_BATCH_LANES;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    dec_start = spu_readch(SPU_RdDec);

    for (i = 0; i < simd_batches; i++) {
        fft_run_batch(re_buf, im_buf);
    }

    dec_end = spu_readch(SPU_RdDec);

    ls_result.ffts_done = simd_batches * FFT_BATCH_LANES;
    ls_result.dec_ticks = dec_start - dec_end;

    result_ea = ls_params.ea_results + (uint64_t)ls_params.spe_index * sizeof(fft_result_t);
    mfc_put(&ls_result, result_ea, sizeof(fft_result_t), TAG_RESULT, 0, 0);
    dma_wait(TAG_RESULT);

    return 0;
}