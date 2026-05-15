/* template/spu_main.c - SPE side of the template benchmark
 *
 * Runs an arbitrary kernel for `iterations` cycles, times itself using the SPU decrementer, DMAs the cycle count back to the PPU.
 *
 * Customise the inner loop below for your bench. Common patterns:
 *   - dense SIMD FMA (see benches/nbody/spu_nbody_kernel.c)
 *   - DMA-bound streaming  (see benches/dma/spu_dma_main.c)
 *   - asm kernel with hbrr branch hint (see benches/branch/)
 */

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#define TAG_PARAMS  0
#define TAG_RESULT  1

typedef struct {
    uint32_t iterations;
    uint32_t pad[31];
} __attribute__((aligned(128))) template_params_t;

typedef struct {
    uint32_t dec_ticks;
    uint32_t pad[31];
} __attribute__((aligned(128))) template_result_t;

static template_params_t ls_params __attribute__((aligned(128)));
static template_result_t ls_result __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

int main(uint64_t arg_params_ea, uint64_t arg_result_ea, uint64_t arg3, uint64_t arg4)
{
    (void)arg3; (void)arg4;

    // pull params from PPU
    mfc_get(&ls_params, arg_params_ea, sizeof(template_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    // time the kernel
    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    uint32_t dec_start = spu_readch(SPU_RdDec);

    /* !!! REPLACE THIS WITH YOUR ACTUAL WORK !!! */
    vector float a = (vector float){1.1f, 1.2f, 1.3f, 1.4f};
    vector float m = (vector float){0.9999f, 0.9999f, 0.9999f, 0.9999f};
    vector float b = (vector float){1.0e-7f, 2.0e-7f, 3.0e-7f, 4.0e-7f};
    for (uint32_t i = 0; i < ls_params.iterations; i++) {
        a = spu_madd(a, m, b);
    }
    __asm__ volatile ("" : "+r"(a));   // this prevents the loop from being DCE'd
    /* !!! END KERNEL !!! */

    uint32_t dec_end = spu_readch(SPU_RdDec);

    // ship result back
    ls_result.dec_ticks = dec_start - dec_end;
    mfc_put(&ls_result, arg_result_ea, sizeof(template_result_t), TAG_RESULT, 0, 0);
    dma_wait(TAG_RESULT);

    return 0;
}
