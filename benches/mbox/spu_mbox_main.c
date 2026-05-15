#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>
#include <stdint.h>

#define MBOX_SPU_PORT  58u

int main(uint64_t iterations, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint32_t i;
    uint32_t n = (uint32_t)iterations;
    (void)arg2; (void)arg3; (void)arg4;

    for (i = 0; i < n; i++) {
        uint32_t v = spu_read_in_mbox();
        sys_spu_thread_send_event((uint8_t)MBOX_SPU_PORT, (v + 1u) & 0x00FFFFFFu, 0u);
    }
    return 0;
}