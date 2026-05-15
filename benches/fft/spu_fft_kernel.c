#include <spu_intrinsics.h>
#include <stdint.h>
#include <math.h>

#include "fft_benchmark.h"

typedef vector float vfloat;

static uint16_t bit_rev_table[FFT_N];
static float    cos_table[FFT_N / 2];
static float    sin_table[FFT_N / 2];
static int      tables_initialized = 0;

void fft_init(void)
{
    int      i;
    uint32_t bit, n, j;
    double   two_pi, ang;

    if (tables_initialized) return;

    for (i = 0; i < (int)FFT_N; i++) {
        j = 0u;
        n = (uint32_t)i;
        for (bit = 0u; bit < FFT_LOG2_N; bit++) {
            j  = (j << 1) | (n & 1u);
            n >>= 1;
        }
        bit_rev_table[i] = (uint16_t)j;
    }

    two_pi = 6.283185307179586;
    for (i = 0; i < (int)(FFT_N / 2); i++) {
        ang = -two_pi * (double)i / (double)FFT_N;
        cos_table[i] = (float)cos(ang);
        sin_table[i] = (float)sin(ang);
    }
    tables_initialized = 1;
}

void fft_run_batch(vfloat * __restrict__ re, vfloat * __restrict__ im)
{
    int    i, j, k, s;
    int    m, m_half, twiddle_step;
    vfloat va_re, va_im, vb_re, vb_im;
    vfloat vw_re, vw_im;
    vfloat vp_imim, vp_imre;
    vfloat vtmp_re, vtmp_im;

    for (i = 0; i < (int)FFT_N; i++) {
        int idx = (int)bit_rev_table[i];
        if (idx > i) {
            vfloat tr = re[i]; re[i] = re[idx]; re[idx] = tr;
            vfloat ti = im[i]; im[i] = im[idx]; im[idx] = ti;
        }
    }

    for (s = 1; s <= (int)FFT_LOG2_N; s++) {
        m            = 1 << s;
        m_half       = m >> 1;
        twiddle_step = (int)FFT_N / m;

        for (k = 0; k < (int)FFT_N; k += m) {
            for (j = 0; j < m_half; j++) {
                int idx_i  = k + j;
                int idx_j  = k + j + m_half;
                int tw_idx = j * twiddle_step;

                vw_re = spu_splats(cos_table[tw_idx]);
                vw_im = spu_splats(sin_table[tw_idx]);

                va_re = re[idx_i];
                va_im = im[idx_i];
                vb_re = re[idx_j];
                vb_im = im[idx_j];

                vp_imim = spu_mul(vw_im, vb_im);
                vp_imre = spu_mul(vw_im, vb_re);
                vtmp_re = spu_msub(vw_re, vb_re, vp_imim);
                vtmp_im = spu_madd(vw_re, vb_im, vp_imre);

                re[idx_i] = spu_add(va_re, vtmp_re);
                im[idx_i] = spu_add(va_im, vtmp_im);
                re[idx_j] = spu_sub(va_re, vtmp_re);
                im[idx_j] = spu_sub(va_im, vtmp_im);
            }
        }
    }
}