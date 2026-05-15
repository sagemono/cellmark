#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>

#define DMA_CHUNK_MAX   16384u

/* deep ring of in-flight DMAs. 8 * 16 KB = 128 KB LS for buffers,
 * leaving the other 128 KB for code + stack + params. MFC cmd queue
 * holds 16 entries per SPE so 8 in flight is well under capacity. */
#define DMA_MAX_QUEUE   8u

/* tag range [16..23] - keeps tags 0..15 free for caller params/results */
#define TAG_BASE        16u

static uint8_t __attribute__((aligned(128))) dma_ls_ring[DMA_MAX_QUEUE][DMA_CHUNK_MAX];

static inline uint32_t clamp_depth(uint32_t d)
{
    if (d == 0u)             return 1u;
    if (d > DMA_MAX_QUEUE)   return DMA_MAX_QUEUE;
    return d;
}

static inline uint32_t clamp_chunk(uint32_t c)
{
    if (c == 0u || c > DMA_CHUNK_MAX) return DMA_CHUNK_MAX;
    return c;   /* caller is responsible for 16-byte alignment */
}

static inline uint32_t drain_mask_for(uint32_t depth)
{
    uint32_t m = 0u;
    uint32_t i;
    for (i = 0u; i < depth; i++) m |= (1u << (TAG_BASE + i));
    return m;
}

uint64_t spu_dma_get_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations, uint32_t queue_depth, uint32_t chunk_size)
{
    uint32_t window_mask = window_size - 1u;
    uint32_t depth = clamp_depth(queue_depth);
    uint32_t csize = clamp_chunk(chunk_size);
    uint32_t off = 0u;
    uint32_t slot;
    uint32_t i;

    if (iterations < depth) return 0;

    for (slot = 0u; slot < depth; slot++) {
        mfc_get(dma_ls_ring[slot], ea_base + off, csize, TAG_BASE + slot, 0, 0);
        off = (off + csize) & window_mask;
    }

    slot = 0u;
    for (i = depth; i < iterations; i++) {
        mfc_write_tag_mask(1u << (TAG_BASE + slot));
        (void)mfc_read_tag_status_all();
        mfc_get(dma_ls_ring[slot], ea_base + off, csize, TAG_BASE + slot, 0, 0);
        off  = (off + csize) & window_mask;
        slot = (slot + 1u);
        if (slot == depth) slot = 0u;
    }

    mfc_write_tag_mask(drain_mask_for(depth));
    (void)mfc_read_tag_status_all();

    return (uint64_t)iterations * (uint64_t)csize;
}

uint64_t spu_dma_put_kernel(uint64_t ea_base, uint32_t window_size, uint32_t iterations, uint32_t queue_depth, uint32_t chunk_size)
{
    uint32_t window_mask = window_size - 1u;
    uint32_t depth = clamp_depth(queue_depth);
    uint32_t csize = clamp_chunk(chunk_size);
    uint32_t off = 0u;
    uint32_t slot;
    uint32_t i;

    if (iterations < depth) return 0;

    for (slot = 0u; slot < depth; slot++) {
        mfc_put(dma_ls_ring[slot], ea_base + off, csize, TAG_BASE + slot, 0, 0);
        off = (off + csize) & window_mask;
    }

    slot = 0u;
    for (i = depth; i < iterations; i++) {
        mfc_write_tag_mask(1u << (TAG_BASE + slot));
        (void)mfc_read_tag_status_all();
        mfc_put(dma_ls_ring[slot], ea_base + off, csize, TAG_BASE + slot, 0, 0);
        off  = (off + csize) & window_mask;
        slot = (slot + 1u);
        if (slot == depth) slot = 0u;
    }

    mfc_write_tag_mask(drain_mask_for(depth));
    (void)mfc_read_tag_status_all();

    return (uint64_t)iterations * (uint64_t)csize;
}
