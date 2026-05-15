#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "atomic_benchmark.h"

#define TAG_PARAMS 0

static atomic_params_t  ls_params   __attribute__((aligned(128)));
static uint8_t          atomic_buf[128] __attribute__((aligned(128)));

static inline void dma_wait_tag(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t target;
    uint32_t my_parity;
    uint32_t successful;
    uint32_t spin_giveup_count;
    uint64_t ea_atomic;
    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(atomic_params_t), TAG_PARAMS, 0, 0);
    dma_wait_tag(TAG_PARAMS);

    ea_atomic   = ls_params.ea_atomic;
    target      = ls_params.iterations;
    my_parity   = ls_params.spe_index & 1u;
    successful  = 0u;
    spin_giveup_count = 0u;

    while (successful < target) {
        uint32_t val;
        uint32_t status;

        // read line w/ reservation
        mfc_getllar(atomic_buf, ea_atomic, 0, 0);
        (void)mfc_read_atomic_status();

        val = *(volatile uint32_t *)atomic_buf;

        // if not our turn, spin (re-reserve next iter)
        // the reservation is implicitly broken when the other SPE writes, which signals to reread
        if ((val & 1u) != my_parity) {
            spin_giveup_count++;
            // every 64 unsuccessful polls, bail out check. this prevents a hang if the partner SPE failed to launch. 
            // once it hits 1M idle spins it almost certainly means partner is gone
            if (spin_giveup_count >= (1u << 20)) break;
            continue;
        }

        spin_giveup_count = 0u;

        // our turn: increment and write conditionally
        *(volatile uint32_t *)atomic_buf = val + 1u;

        mfc_putllc(atomic_buf, ea_atomic, 0, 0);
        status = mfc_read_atomic_status();
        if (status & MFC_PUTLLC_STATUS) {
            // reservation lost, extremely rare under strict alternation
            // tho, just retry
            continue;
        }
        successful++;
    }

    ls_params.iterations = successful;
    mfc_put(&ls_params, arg_ea, sizeof(atomic_params_t), TAG_PARAMS, 0, 0);
    dma_wait_tag(TAG_PARAMS);

    return 0;
}