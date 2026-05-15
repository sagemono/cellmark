#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#define DMA_CHUNK_SIZE  16384u

#define TAG_A   16u
#define TAG_B   17u

static uint8_t __attribute__((aligned(128))) dma_ls_buf_a[DMA_CHUNK_SIZE];
static uint8_t __attribute__((aligned(128))) dma_ls_buf_b[DMA_CHUNK_SIZE];

uint64_t spu_dma_get_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations)
{
    uint32_t i;
    uint32_t window_mask = window_size - 1u; 
    uint32_t off_a = 0u;
    uint32_t off_b = DMA_CHUNK_SIZE;

    if (iterations < 2u) return 0;

    mfc_get(dma_ls_buf_a, ea_base + off_a, DMA_CHUNK_SIZE, TAG_A, 0, 0);
    mfc_get(dma_ls_buf_b, ea_base + off_b, DMA_CHUNK_SIZE, TAG_B, 0, 0);

    for (i = 2u; i + 1u < iterations; i += 2u) {
        off_a = (off_a + 2u * DMA_CHUNK_SIZE) & window_mask;
        mfc_write_tag_mask(1u << TAG_A);
        (void)mfc_read_tag_status_all();
        mfc_get(dma_ls_buf_a, ea_base + off_a, DMA_CHUNK_SIZE, TAG_A, 0, 0);

        off_b = (off_b + 2u * DMA_CHUNK_SIZE) & window_mask;
        mfc_write_tag_mask(1u << TAG_B);
        (void)mfc_read_tag_status_all();
        mfc_get(dma_ls_buf_b, ea_base + off_b, DMA_CHUNK_SIZE, TAG_B, 0, 0);
    }

    mfc_write_tag_mask((1u << TAG_A) | (1u << TAG_B));
    (void)mfc_read_tag_status_all();

    return (uint64_t)iterations * (uint64_t)DMA_CHUNK_SIZE;
}

uint64_t spu_dma_put_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations)
{
    uint32_t i;
    uint32_t window_mask = window_size - 1u;
    uint32_t off_a = 0u;
    uint32_t off_b = DMA_CHUNK_SIZE;

    if (iterations < 2u) return 0;

    mfc_put(dma_ls_buf_a, ea_base + off_a, DMA_CHUNK_SIZE, TAG_A, 0, 0);
    mfc_put(dma_ls_buf_b, ea_base + off_b, DMA_CHUNK_SIZE, TAG_B, 0, 0);

    for (i = 2u; i + 1u < iterations; i += 2u) {
        off_a = (off_a + 2u * DMA_CHUNK_SIZE) & window_mask;
        mfc_write_tag_mask(1u << TAG_A);
        (void)mfc_read_tag_status_all();
        mfc_put(dma_ls_buf_a, ea_base + off_a, DMA_CHUNK_SIZE, TAG_A, 0, 0);

        off_b = (off_b + 2u * DMA_CHUNK_SIZE) & window_mask;
        mfc_write_tag_mask(1u << TAG_B);
        (void)mfc_read_tag_status_all();
        mfc_put(dma_ls_buf_b, ea_base + off_b, DMA_CHUNK_SIZE, TAG_B, 0, 0);
    }

    mfc_write_tag_mask((1u << TAG_A) | (1u << TAG_B));
    (void)mfc_read_tag_status_all();

    return (uint64_t)iterations * (uint64_t)DMA_CHUNK_SIZE;
}