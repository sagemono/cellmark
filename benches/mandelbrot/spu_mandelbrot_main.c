#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "mandelbrot_benchmark.h"

typedef vector float        vfloat;
typedef vector unsigned int vuint;
typedef vector signed int   vint;

#define TAG_PARAMS  0
#define TAG_ROW     1

#define EARLY_OUT_CHECK_STRIDE 8u

static mandel_params_t  ls_params __attribute__((aligned(128)));
static mandel_result_t  ls_result __attribute__((aligned(128)));

static uint32_t row_buf[2][MANDEL_MAX_W] __attribute__((aligned(128)));

typedef struct {
    float pr, pg, pb;
    float ar, ag, ab;
} palette_def_t;

static const palette_def_t PALETTES[MANDEL_PALETTE_COUNT] = {
    { 0.0f, 1.0f/3.0f, 2.0f/3.0f,   1.0f, 1.0f, 1.0f },
    { 0.0f, 0.08f, 0.16f,           1.0f, 0.65f, 0.20f },
    { 0.55f, 0.62f, 0.70f,          0.30f, 0.70f, 1.0f },
    { 0.0f, 0.0f, 0.0f,             1.0f, 1.0f, 1.0f },
    { 0.0f, 0.5f, 0.0f,             1.0f, 0.0f, 1.0f },
};

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

static inline vfloat frac_v(vfloat x)
{
    return spu_sub(x, spu_convtf(spu_convtu(x, 0), 0));
}

static inline vfloat smooth_tri_v(vfloat x, vfloat one)
{
    vfloat fr = frac_v(x);
    vfloat u  = spu_sub(spu_add(fr, fr), one);
    return spu_sub(one, spu_mul(u, u));
}

static inline vuint colorize_simd(vuint iter, vuint max_iter, vfloat vpr, vfloat vpg, vfloat vpb, vfloat var, vfloat vag, vfloat vab, vfloat vhue)
{
    vuint  in_set = spu_cmpeq(iter, max_iter);
    vuint  alpha  = spu_splats(0xff000000u);
    vfloat one    = spu_splats(1.0f);
    vfloat scale  = spu_splats(255.0f);

    vfloat phase = spu_mul(spu_convtf(iter, 0), spu_splats(1.0f / 32.0f));
    phase = spu_add(phase, vhue);

    vfloat rv = spu_mul(var, smooth_tri_v(spu_add(phase, vpr), one));
    vfloat gv = spu_mul(vag, smooth_tri_v(spu_add(phase, vpg), one));
    vfloat bv = spu_mul(vab, smooth_tri_v(spu_add(phase, vpb), one));

    vuint r = spu_convtu(spu_mul(rv, scale), 0);
    vuint g = spu_convtu(spu_mul(gv, scale), 0);
    vuint b = spu_convtu(spu_mul(bv, scale), 0);

    vuint argb = spu_or(alpha, spu_or(spu_sl(r, spu_splats(16u)), spu_or(spu_sl(g, spu_splats(8u)), b)));

    return spu_sel(argb, alpha, in_set);
}

static void compute_row(uint32_t *out, float re_min, float dre, float ci, uint32_t max_iter, const palette_def_t *pal, float hue_offset, uint64_t *iters_accum)
{
    uint32_t x;
    vfloat   vdre   = spu_splats(dre);
    vfloat   v4     = spu_splats(4.0f);
    vfloat   v2     = spu_splats(2.0f);
    vuint    v1u    = spu_splats(1u);
    vuint    vmax   = spu_splats(max_iter);
    vfloat   vci    = spu_splats(ci);
    vfloat   vremin = spu_splats(re_min);
    vfloat   vlane  = (vfloat){0.0f, 1.0f, 2.0f, 3.0f};
    vfloat   vci_sq = spu_mul(vci, vci);
    uint64_t local_iters = 0;

    vfloat vpr = spu_splats(pal->pr);
    vfloat vpg = spu_splats(pal->pg);
    vfloat vpb = spu_splats(pal->pb);
    vfloat var = spu_splats(pal->ar);
    vfloat vag = spu_splats(pal->ag);
    vfloat vab = spu_splats(pal->ab);
    vfloat vhue= spu_splats(hue_offset);

    vfloat vlane4 = spu_splats(4.0f);
    vfloat vquarter = spu_splats(0.25f);
    vfloat vone   = spu_splats(1.0f);
    vfloat vbulbr2 = spu_splats(0.0625f);
    vuint  vallones = spu_splats(0xffffffffu);

    uint32_t W = ls_params.width;
    for (x = 0; x < W; x += 8) {
        vfloat vxA = spu_splats((float)x);
        vfloat vxB = spu_add(vxA, vlane4);
        vfloat vcrA = spu_madd(spu_add(vxA, vlane), vdre, vremin);
        vfloat vcrB = spu_madd(spu_add(vxB, vlane), vdre, vremin);

        vfloat crmqA = spu_sub(vcrA, vquarter);
        vfloat crmqB = spu_sub(vcrB, vquarter);
        vfloat qA = spu_madd(crmqA, crmqA, vci_sq);
        vfloat qB = spu_madd(crmqB, crmqB, vci_sq);
        vfloat cardThr = spu_mul(vci_sq, vquarter);
        vuint  in_cardA = spu_cmpgt(cardThr, spu_mul(qA, spu_add(qA, crmqA)));
        vuint  in_cardB = spu_cmpgt(cardThr, spu_mul(qB, spu_add(qB, crmqB)));
        vfloat cpoA = spu_add(vcrA, vone);
        vfloat cpoB = spu_add(vcrB, vone);
        vfloat bulbA = spu_madd(cpoA, cpoA, vci_sq);
        vfloat bulbB = spu_madd(cpoB, cpoB, vci_sq);
        vuint  in_bulbA = spu_cmpgt(vbulbr2, bulbA);
        vuint  in_bulbB = spu_cmpgt(vbulbr2, bulbB);
        vuint  pre_in_setA = spu_or(in_cardA, in_bulbA);
        vuint  pre_in_setB = spu_or(in_cardB, in_bulbB);

        vfloat zrA = spu_splats(0.0f), ziA = spu_splats(0.0f);
        vfloat zrB = spu_splats(0.0f), ziB = spu_splats(0.0f);
        vuint  iterA = spu_splats(0u), iterB = spu_splats(0u);
        vuint  amA = spu_xor(pre_in_setA, vallones);
        vuint  amB = spu_xor(pre_in_setB, vallones);

        uint32_t any_alive = spu_extract(spu_gather(spu_or(amA, amB)), 0);
        uint32_t n = 0;

        while (n < max_iter && any_alive) {
            uint32_t batch_end = n + EARLY_OUT_CHECK_STRIDE;
            if (batch_end > max_iter) batch_end = max_iter;
            for (; n < batch_end; n++) {
                vfloat zr2A = spu_mul(zrA, zrA);
                vfloat zr2B = spu_mul(zrB, zrB);
                vfloat zi2A = spu_mul(ziA, ziA);
                vfloat zi2B = spu_mul(ziB, ziB);
                vfloat magA = spu_add(zr2A, zi2A);
                vfloat magB = spu_add(zr2B, zi2B);
                vuint  insA = spu_cmpgt(v4, magA);
                vuint  insB = spu_cmpgt(v4, magB);
                amA = spu_and(amA, insA);
                amB = spu_and(amB, insB);

                iterA = spu_add(iterA, spu_and(amA, v1u));
                iterB = spu_add(iterB, spu_and(amB, v1u));

                vfloat nzrA = spu_add(spu_sub(zr2A, zi2A), vcrA);
                vfloat nzrB = spu_add(spu_sub(zr2B, zi2B), vcrB);
                vfloat nziA = spu_madd(v2, spu_mul(zrA, ziA), vci);
                vfloat nziB = spu_madd(v2, spu_mul(zrB, ziB), vci);
                zrA = spu_sel(zrA, nzrA, amA);
                zrB = spu_sel(zrB, nzrB, amB);
                ziA = spu_sel(ziA, nziA, amA);
                ziB = spu_sel(ziB, nziB, amB);
            }
            any_alive = spu_extract(spu_gather(spu_or(amA, amB)), 0);
        }

        iterA = spu_sel(iterA, vmax, pre_in_setA);
        iterB = spu_sel(iterB, vmax, pre_in_setB);

        local_iters += (uint64_t)spu_extract(iterA, 0)
                     + (uint64_t)spu_extract(iterA, 1)
                     + (uint64_t)spu_extract(iterA, 2)
                     + (uint64_t)spu_extract(iterA, 3)
                     + (uint64_t)spu_extract(iterB, 0)
                     + (uint64_t)spu_extract(iterB, 1)
                     + (uint64_t)spu_extract(iterB, 2)
                     + (uint64_t)spu_extract(iterB, 3);

        vuint argbA = colorize_simd(iterA, vmax, vpr, vpg, vpb, var, vag, vab, vhue);
        vuint argbB = colorize_simd(iterB, vmax, vpr, vpg, vpb, var, vag, vab, vhue);
        out[x + 0] = spu_extract(argbA, 0);
        out[x + 1] = spu_extract(argbA, 1);
        out[x + 2] = spu_extract(argbA, 2);
        out[x + 3] = spu_extract(argbA, 3);
        out[x + 4] = spu_extract(argbB, 0);
        out[x + 5] = spu_extract(argbB, 1);
        out[x + 6] = spu_extract(argbB, 2);
        out[x + 7] = spu_extract(argbB, 3);
    }

    *iters_accum += local_iters;
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t i;
    uint32_t dec_start, dec_end;
    uint64_t result_ea;
    uint64_t total_iters = 0;
    uint64_t frame_ea;
    int      buf_sel = 0;
    int      prev_put_pending = 0;
    const palette_def_t *pal;

    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(mandel_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    for (i = 0; i < sizeof(ls_result); i++)
        ((uint8_t *)&ls_result)[i] = 0u;

    pal = &PALETTES[(ls_params.palette_id < MANDEL_PALETTE_COUNT) ? ls_params.palette_id : 0];

    frame_ea = ls_params.ea_frame + (uint64_t)ls_params.row_start * (uint64_t)ls_params.row_bytes;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    dec_start = spu_readch(SPU_RdDec);

    for (i = 0; i < ls_params.row_count; i++) {
        float    ci  = ls_params.im_min + ls_params.dim * (float)(ls_params.row_start + i);
        uint32_t *buf = row_buf[buf_sel];

        if (prev_put_pending && i >= 2) {
            mfc_write_tag_mask(1u << TAG_ROW);
            (void)mfc_read_tag_status_all();
        }

        compute_row(buf, ls_params.re_min, ls_params.dre, ci, ls_params.max_iter, pal, ls_params.hue_offset, &total_iters);

        mfc_put(buf, frame_ea + (uint64_t)i * (uint64_t)ls_params.row_bytes, ls_params.row_bytes, TAG_ROW, 0, 0);
        prev_put_pending = 1;
        buf_sel ^= 1;
    }

    if (prev_put_pending) {
        mfc_write_tag_mask(1u << TAG_ROW);
        (void)mfc_read_tag_status_all();
    }

    dec_end = spu_readch(SPU_RdDec);

    ls_result.dec_ticks   = dec_start - dec_end;
    ls_result.iters_done  = (uint32_t)(total_iters & 0xffffffffull);
    ls_result.pixels_done = ls_params.row_count * ls_params.width;

    result_ea = ls_params.ea_results + (uint64_t)ls_params.spe_index * sizeof(mandel_result_t);
    mfc_put(&ls_result, result_ea, sizeof(mandel_result_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    return 0;
}