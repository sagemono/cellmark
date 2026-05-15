#include <spu_intrinsics.h>
#include <stdint.h>

#include "nbody_benchmark.h"

typedef vector float vfloat;

extern float nbody_xj[NBODY_N];
extern float nbody_yj[NBODY_N];
extern float nbody_zj[NBODY_N];
extern float nbody_mj[NBODY_N];

extern float nbody_ax[NBODY_BODIES_PER_SPE];
extern float nbody_ay[NBODY_BODIES_PER_SPE];
extern float nbody_az[NBODY_BODIES_PER_SPE];

#define SOFTENING_SQ  (0.01f * 0.01f)

void nbody_compute_one_iter(uint32_t i_start)
{
    uint32_t i, jv;
    const vfloat *vxj = (const vfloat *)nbody_xj;
    const vfloat *vyj = (const vfloat *)nbody_yj;
    const vfloat *vzj = (const vfloat *)nbody_zj;
    const vfloat *vmj = (const vfloat *)nbody_mj;
    const uint32_t nj_vec = NBODY_N / 4u;
    const vfloat vsoft  = spu_splats(SOFTENING_SQ);

    for (i = 0; i < NBODY_BODIES_PER_SPE; i++) {
        vfloat vxi = spu_splats(nbody_xj[i_start + i]);
        vfloat vyi = spu_splats(nbody_yj[i_start + i]);
        vfloat vzi = spu_splats(nbody_zj[i_start + i]);
        vfloat vax = spu_splats(0.0f);
        vfloat vay = spu_splats(0.0f);
        vfloat vaz = spu_splats(0.0f);

        for (jv = 0; jv < nj_vec; jv++) {
            vfloat vdx = spu_sub(vxj[jv], vxi);
            vfloat vdy = spu_sub(vyj[jv], vyi);
            vfloat vdz = spu_sub(vzj[jv], vzi);

            vfloat vr2 = vsoft;
            vr2 = spu_madd(vdx, vdx, vr2);
            vr2 = spu_madd(vdy, vdy, vr2);
            vr2 = spu_madd(vdz, vdz, vr2);

            vfloat vr_inv  = spu_rsqrte(vr2);
            vfloat vr_inv2 = spu_mul(vr_inv, vr_inv);
            vfloat vmr3    = spu_mul(spu_mul(vmj[jv], vr_inv2), vr_inv);

            vax = spu_madd(vmr3, vdx, vax);
            vay = spu_madd(vmr3, vdy, vay);
            vaz = spu_madd(vmr3, vdz, vaz);
        }

        vax = spu_add(vax, (vfloat)spu_rlqwbyte((vector unsigned int)vax, 8));
        vax = spu_add(vax, (vfloat)spu_rlqwbyte((vector unsigned int)vax, 4));
        vay = spu_add(vay, (vfloat)spu_rlqwbyte((vector unsigned int)vay, 8));
        vay = spu_add(vay, (vfloat)spu_rlqwbyte((vector unsigned int)vay, 4));
        vaz = spu_add(vaz, (vfloat)spu_rlqwbyte((vector unsigned int)vaz, 8));
        vaz = spu_add(vaz, (vfloat)spu_rlqwbyte((vector unsigned int)vaz, 4));

        nbody_ax[i] = spu_extract(vax, 0);
        nbody_ay[i] = spu_extract(vay, 0);
        nbody_az[i] = spu_extract(vaz, 0);
    }
}