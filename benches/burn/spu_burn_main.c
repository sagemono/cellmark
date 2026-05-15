#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#include "burn_benchmark.h"

#define TAG_PARAMS  0
#define TAG_STAT    1
#define TAG_DMA_A   2
#define TAG_DMA_B   3
#define TAG_STOP    4

extern uint32_t spu_stress_dual_kernel(uint32_t iterations);

static burn_params_t   ls_params   __attribute__((aligned(128)));
static burn_progress_t ls_progress __attribute__((aligned(128)));
static uint32_t        ls_stop[32] __attribute__((aligned(128)));

static uint8_t dma_buf_a[BURN_DMA_CHUNK_BYTES] __attribute__((aligned(128)));
static uint8_t dma_buf_b[BURN_DMA_CHUNK_BYTES] __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
}

static inline int stop_requested(void)
{
    mfc_get(ls_stop, ls_params.ea_stop_flag, 128, TAG_STOP, 0, 0);
    dma_wait(TAG_STOP);
    return ls_stop[0] != 0u;
}

static inline void publish_progress(void)
{
    mfc_put(&ls_progress, ls_params.ea_progress,
            sizeof(ls_progress), TAG_STAT, 0, 0);
    dma_wait(TAG_STAT);
}

#define BURN_DUAL_ITERS_PER_BATCH  (16u * 1024u * 1024u)
#define BURN_DUAL_FLOPS_PER_BATCH  ((uint64_t)BURN_DUAL_ITERS_PER_BATCH * 48ULL)

static void run_compute_role(void)
{
    uint64_t total_flops = 0;

    while (!stop_requested()) {
        (void)spu_stress_dual_kernel(BURN_DUAL_ITERS_PER_BATCH);
        total_flops += BURN_DUAL_FLOPS_PER_BATCH;
        ls_progress.counter = total_flops;
        publish_progress();
    }
}

static void run_dma_role(void)
{
    const uint64_t STATS_INTERVAL_CHUNKS = 256ULL;
    uint64_t total_bytes = 0;
    uint32_t offset      = 0;
    uint32_t i;
    int      cur_tag = TAG_DMA_A;
    void    *cur_buf = dma_buf_a;

    mfc_get(cur_buf, ls_params.ea_dma_buffer + offset,
            BURN_DMA_CHUNK_BYTES, cur_tag, 0, 0);
    offset += BURN_DMA_CHUNK_BYTES;

    while (!stop_requested()) {
        for (i = 0; i < STATS_INTERVAL_CHUNKS; i++) {
            int   next_tag = (cur_tag == TAG_DMA_A) ? TAG_DMA_B : TAG_DMA_A;
            void *next_buf = (cur_buf == dma_buf_a)  ? dma_buf_b  : dma_buf_a;

            if (offset >= BURN_DMA_BUFFER_BYTES) offset = 0u;

            mfc_get(next_buf, ls_params.ea_dma_buffer + offset,
                    BURN_DMA_CHUNK_BYTES, next_tag, 0, 0);
            offset += BURN_DMA_CHUNK_BYTES;

            dma_wait(cur_tag);
            total_bytes += BURN_DMA_CHUNK_BYTES;

            cur_tag = next_tag;
            cur_buf = next_buf;
        }
        ls_progress.counter = total_bytes;
        publish_progress();
    }

    dma_wait(cur_tag);
}

int main(uint64_t arg_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg2; (void)arg3; (void)arg4;

    mfc_get(&ls_params, arg_ea, sizeof(burn_params_t), TAG_PARAMS, 0, 0);
    dma_wait(TAG_PARAMS);

    ls_progress.counter = 0u;
    publish_progress();

    if (ls_params.role == BURN_ROLE_COMPUTE) {
        run_compute_role();
    } else {
        run_dma_role();
    }

    publish_progress();
    return 0;
}