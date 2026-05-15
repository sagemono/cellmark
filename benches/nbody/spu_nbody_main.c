#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "nbody_benchmark.h"

extern void nbody_compute_one_iter(uint32_t i_start);

#define TAG_PARAMS  0
#define TAG_RESULT  1

static nbody_params_t ls_params __attribute__((aligned(128)));
static nbody_result_t ls_result __attribute__((aligned(128)));

float nbody_xj[NBODY_N] __attribute__((aligned(128)));
float nbody_yj[NBODY_N] __attribute__((aligned(128)));
float nbody_zj[NBODY_N] __attribute__((aligned(128)));
float nbody_mj[NBODY_N] __attribute__((aligned(128)));

float nbody_ax[NBODY_BODIES_PER_SPE] __attribute__((aligned(128)));
float nbody_ay[NBODY_BODIES_PER_SPE] __attribute__((aligned(128)));
float nbody_az[NBODY_BODIES_PER_SPE] __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

static uint32_t lcg_state;
static inline float lcg_unit(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (float)(lcg_state & 0x00ffffffu) / (float)0x01000000u;
}

static void init_bodies(void)
{
    uint32_t i;
    lcg_state = 0xdeadbeefu;
    for (i = 0; i < NBODY_N; i++) {
        nbody_xj[i] = lcg_unit() * 2.0f - 1.0f;
        nbody_yj[i] = lcg_unit() * 2.0f - 1.0f;
        nbody_zj[i] = lcg_unit() * 2.0f - 1.0f;
        nbody_mj[i] = 0.5f + lcg_unit();
    }
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t i, iter;
    uint32_t dec_start, dec_end;
    uint32_t i_start;
    uint64_t result_ea;
    float    checksum = 0.0f;
    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(nbody_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    init_bodies();

    for (i = 0; i < sizeof(ls_result); i++) {
        ((uint8_t *)&ls_result)[i] = 0u;
    }

    i_start = ls_params.spe_index * NBODY_BODIES_PER_SPE;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    dec_start = spu_readch(SPU_RdDec);

    for (iter = 0; iter < ls_params.iterations; iter++) {
        nbody_compute_one_iter(i_start);
    }

    dec_end = spu_readch(SPU_RdDec);

    for (i = 0; i < NBODY_BODIES_PER_SPE; i++) {
        checksum += nbody_ax[i] * nbody_ax[i] + nbody_ay[i] * nbody_ay[i] + nbody_az[i] * nbody_az[i];
    }

    ls_result.i_bodies_done = NBODY_BODIES_PER_SPE;
    ls_result.dec_ticks     = dec_start - dec_end;
    ls_result.checksum      = checksum;

    result_ea = ls_params.ea_results + (uint64_t)ls_params.spe_index * sizeof(nbody_result_t);
    mfc_put(&ls_result, result_ea, sizeof(nbody_result_t), TAG_RESULT, 0, 0);
    dma_wait(TAG_RESULT);

    return 0;
}