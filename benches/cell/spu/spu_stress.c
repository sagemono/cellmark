#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include "stress_common.h"

#define DMA_TAG_PARAMS  0
#define DMA_TAG_RESULTS 1

static spe_params_t  ls_params  __attribute__((aligned(128)));
static spe_results_t ls_results __attribute__((aligned(128)));

static vec_float4 verify_ref[FMA_CHAINS] __attribute__((aligned(16)));

static const float STRESS_MUL = 0.99999988079071044921875f;
static const float STRESS_ADD = 1.1920928955078125e-7f;

static inline vec_float4 chain_init(int chain_index)
{
    return spu_splats((float)(chain_index + 1) * 0.1f);
}

static void compute_verify_reference(void)
{
    vec_float4 acc[FMA_CHAINS];
    vec_float4 mul = spu_splats(STRESS_MUL);
    vec_float4 add = spu_splats(STRESS_ADD);
    int i, j;

    for (j = 0; j < FMA_CHAINS; j++)
        acc[j] = chain_init(j);

    for (i = 0; i < VERIFY_ITERATIONS; i++) {
        acc[0]  = spu_madd(acc[0],  mul, add);
        acc[1]  = spu_madd(acc[1],  mul, add);
        acc[2]  = spu_madd(acc[2],  mul, add);
        acc[3]  = spu_madd(acc[3],  mul, add);
        acc[4]  = spu_madd(acc[4],  mul, add);
        acc[5]  = spu_madd(acc[5],  mul, add);
        acc[6]  = spu_madd(acc[6],  mul, add);
        acc[7]  = spu_madd(acc[7],  mul, add);
        acc[8]  = spu_madd(acc[8],  mul, add);
        acc[9]  = spu_madd(acc[9],  mul, add);
        acc[10] = spu_madd(acc[10], mul, add);
        acc[11] = spu_madd(acc[11], mul, add);
    }

    for (j = 0; j < FMA_CHAINS; j++)
        verify_ref[j] = acc[j];
}

static uint32_t run_verification(void)
{
    vec_float4 acc[FMA_CHAINS];
    vec_float4 mul = spu_splats(STRESS_MUL);
    vec_float4 add = spu_splats(STRESS_ADD);
    uint32_t errors = 0;
    int i, j;

    for (j = 0; j < FMA_CHAINS; j++)
        acc[j] = chain_init(j);

    for (i = 0; i < VERIFY_ITERATIONS; i++) {
        acc[0]  = spu_madd(acc[0],  mul, add);
        acc[1]  = spu_madd(acc[1],  mul, add);
        acc[2]  = spu_madd(acc[2],  mul, add);
        acc[3]  = spu_madd(acc[3],  mul, add);
        acc[4]  = spu_madd(acc[4],  mul, add);
        acc[5]  = spu_madd(acc[5],  mul, add);
        acc[6]  = spu_madd(acc[6],  mul, add);
        acc[7]  = spu_madd(acc[7],  mul, add);
        acc[8]  = spu_madd(acc[8],  mul, add);
        acc[9]  = spu_madd(acc[9],  mul, add);
        acc[10] = spu_madd(acc[10], mul, add);
        acc[11] = spu_madd(acc[11], mul, add);
    }

    for (j = 0; j < FMA_CHAINS; j++) {
        vec_uint4 cmp = spu_cmpeq(acc[j], verify_ref[j]);
        if (spu_extract(cmp, 0) != 0xFFFFFFFF ||
            spu_extract(cmp, 1) != 0xFFFFFFFF ||
            spu_extract(cmp, 2) != 0xFFFFFFFF ||
            spu_extract(cmp, 3) != 0xFFFFFFFF)
            errors++;
    }

    return errors;
}

extern uint32_t spu_stress_fma_kernel(uint32_t iterations);
extern uint32_t spu_stress_dp_fma_kernel(uint32_t iterations);
extern uint32_t spu_stress_int_kernel(uint32_t iterations);
extern uint32_t spu_stress_recip_kernel(uint32_t iterations);
extern uint32_t spu_stress_shuf_kernel(uint32_t iterations);
extern uint32_t spu_stress_dual_kernel(uint32_t iterations);

#define ASM_FLOPS_PER_ITER      48  /* SP FMA: iterations * 48 */
#define ASM_DP_FLOPS_PER_ITER   52  /* DP FMA: iterations * 52 */
#define ASM_INT_OPS_PER_ITER    56  /* Int MUL: iterations * 56 */
#define ASM_RECIP_OPS_PER_ITER  264 /* Recip: 6ch*(4+7)insns*4lanes, 8x unroll = iterations * 264 */
#define ASM_SHUF_OPS_PER_ITER   36  /* Shuffle: 9insns*4lanes, 64x unroll = iterations * 36 */
#define ASM_DUAL_OPS_PER_ITER   72  /* Dual: iterations * 72 */

static uint64_t stress_compute_sp(uint32_t iterations)
{
    iterations = (iterations + 63) & ~63u;
    (void)spu_stress_fma_kernel(iterations);
    return (uint64_t)iterations * ASM_FLOPS_PER_ITER;
}

static uint64_t stress_compute_dp(uint32_t iterations)
{
    iterations = (iterations + 15) & ~15u;
    (void)spu_stress_dp_fma_kernel(iterations);
    return (uint64_t)iterations * ASM_DP_FLOPS_PER_ITER;
}

static uint64_t stress_compute_int(uint32_t iterations)
{
    iterations = (iterations + 31) & ~31u;
    (void)spu_stress_int_kernel(iterations);
    return (uint64_t)iterations * ASM_INT_OPS_PER_ITER;
}

static uint64_t stress_compute_recip(uint32_t iterations)
{
    iterations = (iterations + 7) & ~7u;
    (void)spu_stress_recip_kernel(iterations);
    return (uint64_t)iterations * ASM_RECIP_OPS_PER_ITER;
}

static uint64_t stress_compute_shuf(uint32_t iterations)
{
    iterations = (iterations + 63) & ~63u;
    (void)spu_stress_shuf_kernel(iterations);
    return (uint64_t)iterations * ASM_SHUF_OPS_PER_ITER;
}

static uint64_t stress_compute_dual(uint32_t iterations)
{
    iterations = (iterations + 63) & ~63u;
    (void)spu_stress_dual_kernel(iterations);
    return (uint64_t)iterations * ASM_DUAL_OPS_PER_ITER;
}

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

static inline void put_results(uint64_t ea)
{
    mfc_put(&ls_results, ea, sizeof(spe_results_t), DMA_TAG_RESULTS, 0, 0);
}

static inline int check_stop(void)
{
    if (spu_stat_in_mbox() > 0) {
        uint32_t cmd = spu_read_in_mbox();
        if (cmd == MBOX_CMD_STOP)
            return 1;
    }
    return 0;
}

extern void memtest_main(spe_params_t *params, spe_results_t *results, uint64_t ea_results, uint32_t tb_freq);


int main(uint64_t ea_params, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t batch_iters;
    uint32_t verify_iv;
    uint64_t ea_results;

    (void)arg2; (void)arg3; (void)arg4;
    mfc_get(&ls_params, ea_params, sizeof(spe_params_t),
            DMA_TAG_PARAMS, 0, 0);
    dma_wait(DMA_TAG_PARAMS);

    ea_results = ls_params.ea_results;

    if (ls_params.mode == MODE_MEMTEST) {
        memtest_main(&ls_params, &ls_results, ea_results, ls_params.tb_freq);
        return 0;
    }
    {
    uint32_t mode = ls_params.mode;
    batch_iters = ls_params.batch_iterations;
    verify_iv   = ls_params.verify_interval;

    if (batch_iters == 0) batch_iters = BATCH_ITERATIONS;
    if (verify_iv == 0)   verify_iv   = VERIFY_INTERVAL;

    batch_iters = (batch_iters + 1) & ~1u;

    ls_results.total_flops   = 0;
    ls_results.batches_done  = 0;
    ls_results.verify_errors = 0;
    ls_results.status        = STATUS_RUNNING;

    if (mode == MODE_COMPUTE_SP)
        compute_verify_reference();

    put_results(ea_results);
    dma_wait(DMA_TAG_RESULTS);

    while (1) {
        uint64_t ops;

        switch (mode) {
        case MODE_COMPUTE_DP:    ops = stress_compute_dp(batch_iters);    break;
        case MODE_COMPUTE_INT:   ops = stress_compute_int(batch_iters);   break;
        case MODE_COMPUTE_RECIP: ops = stress_compute_recip(batch_iters); break;
        case MODE_COMPUTE_SHUF:  ops = stress_compute_shuf(batch_iters);  break;
        case MODE_COMPUTE_DUAL:  ops = stress_compute_dual(batch_iters);  break;
        default:                 ops = stress_compute_sp(batch_iters);     break;
        }

        ls_results.total_flops += ops;
        ls_results.batches_done++;

        if (mode == MODE_COMPUTE_SP && (ls_results.batches_done % verify_iv) == 0) {
            uint32_t errs = run_verification();
            ls_results.verify_errors += errs;
            if (errs > 0)
                ls_results.status = STATUS_ERROR;
        }

        put_results(ea_results);

        if (check_stop())
            break;
    }

    ls_results.status = STATUS_DONE;
    put_results(ea_results);
    dma_wait(DMA_TAG_RESULTS);

    return 0;
    }
}