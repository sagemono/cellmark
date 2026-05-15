/*
 * spu_memtest.c - xdr memory test kernel for SPU
 * most of this flew over my head so this was
 * refactored by claude 4.7, pretty good.
 */

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include <string.h>

#include "stress_common.h"

#define DMA_TAG_A       2
#define DMA_TAG_B       3
#define DMA_TAG_RESULTS 4

static uint8_t write_buf[DMA_MAX_SIZE] __attribute__((aligned(128)));
static uint8_t read_buf[DMA_MAX_SIZE]  __attribute__((aligned(128)));

static inline void dma_wait(uint32_t tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

static inline void dma_put_wait(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    mfc_put(ls, ea, size, tag, 0, 0);
    dma_wait(tag);
}

static inline void dma_get_wait(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    mfc_get(ls, ea, size, tag, 0, 0);
    dma_wait(tag);
}

static inline void timer_start(void)
{
    spu_writech(SPU_WrDec, 0xFFFFFFFF);
}

static inline uint32_t timer_elapsed(void)
{
    return 0xFFFFFFFF - spu_readch(SPU_RdDec);
}


static inline int should_stop(void)
{
    if (spu_stat_in_mbox() > 0) {
        uint32_t cmd = spu_read_in_mbox();
        if (cmd == MBOX_CMD_STOP)
            return 1;
    }
    return 0;
}

static spe_results_t *g_res;
static uint64_t       g_ea_res;

static void flush_results(void)
{
    mfc_put(g_res, g_ea_res, 128, DMA_TAG_RESULTS, 0, 0);
    dma_wait(DMA_TAG_RESULTS);
}

static void update_progress(uint32_t done, uint32_t total)
{
    if (total > 0)
        g_res->progress_pct = (done * 100) / total;
    else
        g_res->progress_pct = 0;
    flush_results();
}

static void fill_sequential(uint32_t *buf, uint32_t count, uint32_t base)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        buf[i] = base + (i * 4);
}

static void fill_constant(uint32_t *buf, uint32_t count, uint32_t val)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        buf[i] = val;
}

static void fill_checkerboard(uint32_t *buf, uint32_t count, uint32_t phase)
{
    uint32_t val = phase ? 0xAAAAAAAAu : 0x55555555u;
    uint32_t i;
    for (i = 0; i < count; i++)
        buf[i] = (i & 1) ? ~val : val;
}

static uint32_t prng_state;

static uint32_t prng_next(void)
{
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

static void fill_random(uint32_t *buf, uint32_t count, uint32_t seed)
{
    uint32_t i;
    prng_state = seed;
    for (i = 0; i < count; i++)
        buf[i] = prng_next();
}

static uint32_t compare_buffers(const uint32_t *exp, const uint32_t *got, uint32_t count, uint32_t ea_offset)
{
    uint32_t errors = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (exp[i] != got[i]) {
            errors++;
            if (g_res->memtest_errors == 0) {
                g_res->memtest_err_addr = ea_offset + (i * 4);
                g_res->memtest_err_exp  = exp[i];
                g_res->memtest_err_got  = got[i];
                uint32_t diff = exp[i] ^ got[i];
                uint32_t fbit = 0;
                while (fbit < 32 && !(diff & (1u << fbit))) fbit++;
                g_res->memtest_err_bit = fbit;
            }
            g_res->memtest_errors++;
        }
    }
    return errors;
}

static uint32_t write_verify_block(uint64_t ea, uint32_t size, uint32_t ea_offset)
{
    dma_put_wait(write_buf, ea, size, DMA_TAG_A);
    dma_get_wait(read_buf, ea, size, DMA_TAG_B);
    g_res->memtest_bytes += size;
    return compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, size / 4, ea_offset);
}

static uint32_t test_sequential(uint64_t ea_base, uint32_t region_size)
{
    uint32_t offset, errors = 0;
    uint32_t total_blocks = region_size / DMA_MAX_SIZE;
    uint32_t block = 0;

    g_res->current_pattern = 0x00000000u; // sequential addresses

    for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        fill_sequential((uint32_t *)write_buf, DMA_MAX_SIZE / 4, offset);
        dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 2);
    }

    for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        fill_sequential((uint32_t *)write_buf, DMA_MAX_SIZE / 4, offset);
        dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
        errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
        g_res->memtest_bytes += DMA_MAX_SIZE;
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 2);
    }
    return errors;
}

static uint32_t test_walking_ones(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t total_steps = 64; // 32 bits * 2 phases
    uint32_t step = 0;
    uint32_t bit, offset;
    uint32_t pattern;

    for (bit = 0; bit < 32; bit++) {
        if (should_stop()) return errors;

        // walking 1: write entire region
        pattern = 1u << bit;
        g_res->current_pattern = pattern;
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        }
        // verify entire region
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        step++;
        update_progress(step, total_steps);

        // walking 0: write entire region
        pattern = ~(1u << bit);
        g_res->current_pattern = pattern;
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        }
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        step++;
        update_progress(step, total_steps);
    }
    return errors;
}

static uint32_t test_checkerboard(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t phase, offset;
    uint32_t block = 0;
    uint32_t total_blocks = (region_size / DMA_MAX_SIZE) * 4;

    for (phase = 0; phase < 2; phase++) {
        g_res->current_pattern = phase ? 0xAAAAAAAAu : 0x55555555u;
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            fill_checkerboard((uint32_t *)write_buf, DMA_MAX_SIZE / 4, phase);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
            block++;
            if ((block & 15) == 0) update_progress(block, total_blocks);
        }

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            fill_checkerboard((uint32_t *)write_buf, DMA_MAX_SIZE / 4, phase);
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
            block++;
            if ((block & 15) == 0) update_progress(block, total_blocks);
        }
    }
    return errors;
}

static uint32_t test_random(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t offset;
    uint32_t total_blocks = region_size / DMA_MAX_SIZE;
    uint32_t block = 0;
    int iter;
    // 8 iterations with different base seeds, like memtest 86 test 5
    uint32_t seeds[8] = {
        0xCAFE0000u, 0xDEAD1111u, 0xBEEF2222u, 0x12345678u,
        0x9ABCDEF0u, 0xA5A5A5A5u, 0x0F0F0F0Fu, 0xF00DCAFE,
    };
    uint32_t total_steps = total_blocks * 8 * 2; // 8 iters * (write+verify)

    for (iter = 0; iter < 8; iter++) {
        g_res->current_pattern = seeds[iter];

        // write entire region with this seed
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            uint32_t seed = seeds[iter] + offset;
            fill_random((uint32_t *)write_buf, DMA_MAX_SIZE / 4, seed);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
            block++;
            if ((block & 31) == 0) update_progress(block, total_steps);
        }

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            uint32_t seed = seeds[iter] + offset;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_random((uint32_t *)write_buf, DMA_MAX_SIZE / 4, seed);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
            block++;
            if ((block & 31) == 0) update_progress(block, total_steps);
        }
    }
    return errors;
}

static uint32_t test_moving_inv(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t offset;
    int p;

    static const uint32_t patterns[] = {
        0x00000000u,
        0xFFFFFFFFu,
        0x55555555u,
        0xAAAAAAAAu,
        0x33333333u,
        0xCCCCCCCCu,
        0x0F0F0F0Fu,
        0xF0F0F0F0u,
        0x00FF00FFu,
        0xFF00FF00u,
        0x0000FFFFu,
        0xFFFF0000u,
        0x01234567u,
        0x89ABCDEFu
    };
    uint32_t num_patterns = sizeof(patterns) / sizeof(patterns[0]);
    uint32_t total_phases = num_patterns * 5;  // 5 passes per pattern
    uint32_t phase = 0;

    for (p = 0; p < (int)num_patterns; p++) {
        uint32_t pat  = patterns[p];
        uint32_t ipat = ~pat;
        g_res->current_pattern = pat;

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pat);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        }
        phase++;
        update_progress(phase, total_phases);

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pat);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, ipat);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        phase++;
        update_progress(phase, total_phases);

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, ipat);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        phase++;
        update_progress(phase, total_phases);

        for (offset = region_size; offset >= DMA_MAX_SIZE; offset -= DMA_MAX_SIZE) {
            uint32_t off = offset - DMA_MAX_SIZE;
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + off, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, ipat);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, off);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pat);
            dma_put_wait(write_buf, ea_base + off, DMA_MAX_SIZE, DMA_TAG_A);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        phase++;
        update_progress(phase, total_phases);

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pat);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        phase++;
        update_progress(phase, total_phases);
    }
    return errors;
}

static uint32_t test_bank_hammer(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t max_hammers = 500000;
    uint32_t h;

    if (region_size < XDR_ROW_STRIDE + DMA_MAX_SIZE)
        return 0;

    g_res->current_pattern = 0xA5A5A5A5u;

    fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, 0xA5A5A5A5u);
    dma_put_wait(write_buf, ea_base, DMA_MAX_SIZE, DMA_TAG_A);
    fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, 0x5A5A5A5Au);
    dma_put_wait(write_buf, ea_base + XDR_ROW_STRIDE, DMA_MAX_SIZE, DMA_TAG_A);

    for (h = 0; h < max_hammers; h++) {
        if ((h & 1023) == 0 && should_stop()) return errors;
        mfc_get(read_buf, ea_base, DMA_ALIGN, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);
        mfc_get(read_buf, ea_base + XDR_ROW_STRIDE, DMA_ALIGN, DMA_TAG_B, 0, 0);
        dma_wait(DMA_TAG_B);
        g_res->memtest_bytes += DMA_ALIGN * 2;
        if ((h & 255) == 0) update_progress(h, max_hammers);
    }

    fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, 0xA5A5A5A5u);
    dma_get_wait(read_buf, ea_base, DMA_MAX_SIZE, DMA_TAG_B);
    errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, 0);
    fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, 0x5A5A5A5Au);
    dma_get_wait(read_buf, ea_base + XDR_ROW_STRIDE, DMA_MAX_SIZE, DMA_TAG_B);
    errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, XDR_ROW_STRIDE);
    return errors;
}

static uint32_t test_rw_turnaround(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t max_turns = 100000;
    uint32_t t;

    g_res->current_pattern = 0xDEADBEEFu;
    fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, 0xDEADBEEFu);
    dma_put_wait(write_buf, ea_base, DMA_MAX_SIZE, DMA_TAG_A);

    for (t = 0; t < max_turns; t++) {
        if ((t & 1023) == 0 && should_stop()) return errors;
        uint32_t offset = (t * DMA_ALIGN) % region_size;
        offset &= ~(DMA_ALIGN - 1);
        uint64_t ea = ea_base + offset;

        mfc_get(read_buf, ea, DMA_ALIGN, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);

        fill_constant((uint32_t *)write_buf, DMA_ALIGN / 4, 0xBEEF0000u + t);
        mfc_put(write_buf, ea, DMA_ALIGN, DMA_TAG_B, 0, 0);
        dma_wait(DMA_TAG_B);

        mfc_get(read_buf, ea, DMA_ALIGN, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);

        errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_ALIGN / 4, offset);
        g_res->memtest_bytes += DMA_ALIGN * 3;
        if ((t & 255) == 0) update_progress(t, max_turns);
    }
    return errors;
}

static uint32_t test_bandwidth(uint64_t ea_base, uint32_t region_size, uint32_t tb_freq)
{
    uint32_t offset;
    uint32_t ticks_read, ticks_write;
    uint64_t bytes = 0;
    uint32_t num_transfers = region_size / DMA_MAX_SIZE;

    fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, 0x12345678u);

    update_progress(0, 3);

    timer_start();
    for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
        mfc_put(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);
        bytes += DMA_MAX_SIZE;
    }
    ticks_write = timer_elapsed();

    update_progress(1, 3);

    bytes = 0;
    timer_start();
    for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
        mfc_get(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B, 0, 0);
        dma_wait(DMA_TAG_B);
        bytes += DMA_MAX_SIZE;
    }
    ticks_read = timer_elapsed();

    update_progress(2, 3);

    if (ticks_write > 0 && tb_freq > 0) {
        g_res->bw_write_mbps = (uint32_t)((uint64_t)region_size * (uint64_t)tb_freq / ((uint64_t)ticks_write * 1000000ULL));
    }
    if (ticks_read > 0 && tb_freq > 0) {
        g_res->bw_read_mbps = (uint32_t)((uint64_t)region_size * (uint64_t)tb_freq / ((uint64_t)ticks_read * 1000000ULL));
    }
    if (num_transfers > 0 && tb_freq > 0) {
        uint64_t ns_per_tick = 1000000000ULL / (uint64_t)tb_freq;
        g_res->lat_write_ns = (ticks_write / num_transfers) * (uint32_t)ns_per_tick;
        g_res->lat_read_ns  = (ticks_read / num_transfers) * (uint32_t)ns_per_tick;
    }

    g_res->memtest_bytes += region_size * 2;
    return 0;
}

static uint32_t test_own_address(uint64_t ea_base, uint32_t region_size)
{
    uint32_t offset, errors = 0;
    uint32_t total_blocks = region_size / DMA_MAX_SIZE;
    uint32_t block = 0;
    uint32_t i;

    g_res->current_pattern = (uint32_t)ea_base;

    for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        uint32_t *wb = (uint32_t *)write_buf;
        uint32_t ea_lo = (uint32_t)(ea_base + offset);
        for (i = 0; i < DMA_MAX_SIZE / 4; i++)
            wb[i] = ea_lo + (i * 4);
        dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 2);
    }

    for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
        uint32_t *wb = (uint32_t *)write_buf;
        uint32_t ea_lo = (uint32_t)(ea_base + offset);
        for (i = 0; i < DMA_MAX_SIZE / 4; i++)
            wb[i] = ea_lo + (i * 4);
        errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
        g_res->memtest_bytes += DMA_MAX_SIZE;
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 2);
    }
    return errors;
}

static uint32_t test_modulo_n(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t offset;
    uint32_t n_off;
    uint32_t total_phases = MODULO_N_STRIDE * 2;
    uint32_t phase = 0;
    uint32_t pattern1 = 0xA5A5A5A5u;
    uint32_t pattern2 = 0x5A5A5A5Au;
    g_res->current_pattern = pattern1;

    for (n_off = 0; n_off < MODULO_N_STRIDE; n_off++) {
        if (should_stop()) return errors;

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            uint32_t *wb = (uint32_t *)write_buf;
            uint32_t i;
            uint32_t base_word = offset / 4;
            for (i = 0; i < DMA_MAX_SIZE / 4; i++) {
                if (((base_word + i) % MODULO_N_STRIDE) == n_off)
                    wb[i] = pattern1;
                else
                    wb[i] = pattern2;
            }
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        }
        phase++;
        update_progress(phase, total_phases);

        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            uint32_t *wb = (uint32_t *)write_buf;
            uint32_t i;
            uint32_t base_word = offset / 4;
            for (i = 0; i < DMA_MAX_SIZE / 4; i++) {
                if (((base_word + i) % MODULO_N_STRIDE) == n_off)
                    wb[i] = pattern1;
                else
                    wb[i] = pattern2;
            }
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }
        phase++;
        update_progress(phase, total_phases);
    }
    return errors;
}

static uint32_t test_block_move(uint64_t ea_base, uint32_t region_size)
{
    uint32_t errors = 0;
    uint32_t half = region_size / 2;
    uint32_t offset;
    uint32_t total_blocks = half / DMA_MAX_SIZE;
    uint32_t block = 0;
    uint32_t i;

    if (half < DMA_MAX_SIZE)
        return 0;

    half &= ~(DMA_MAX_SIZE - 1);

    for (offset = 0; offset < half; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        uint32_t *wb = (uint32_t *)write_buf;
        uint32_t seed = 0xDEAD0000 + offset;
        for (i = 0; i < DMA_MAX_SIZE / 4; i += 4) {
            wb[i]   = seed;
            wb[i+1] = seed;
            wb[i+2] = ~seed;
            wb[i+3] = ~seed;
            seed = (seed << 1) | (seed >> 31);
        }
        dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 4);
    }

    for (offset = 0; offset < half; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        dma_put_wait(read_buf, ea_base + half + offset, DMA_MAX_SIZE, DMA_TAG_B);
        g_res->memtest_bytes += DMA_MAX_SIZE * 2;
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 4);
    }

    for (offset = half; offset >= DMA_MAX_SIZE; offset -= DMA_MAX_SIZE) {
        uint32_t src_off = offset - DMA_MAX_SIZE;
        if (should_stop()) return errors;
        dma_get_wait(read_buf, ea_base + half + src_off, DMA_MAX_SIZE, DMA_TAG_A);
        dma_put_wait(read_buf, ea_base + src_off, DMA_MAX_SIZE, DMA_TAG_B);
        g_res->memtest_bytes += DMA_MAX_SIZE * 2;
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 4);
    }

    for (offset = 0; offset < half; offset += DMA_MAX_SIZE) {
        if (should_stop()) return errors;
        uint32_t *wb = (uint32_t *)write_buf;
        uint32_t seed = 0xDEAD0000 + offset;
        for (i = 0; i < DMA_MAX_SIZE / 4; i += 4) {
            wb[i]   = seed;
            wb[i+1] = seed;
            wb[i+2] = ~seed;
            wb[i+3] = ~seed;
            seed = (seed << 1) | (seed >> 31);
        }
        dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
        errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
        g_res->memtest_bytes += DMA_MAX_SIZE;
        block++;
        if ((block & 15) == 0) update_progress(block, total_blocks * 4);
    }
    return errors;
}

static int spin_wait_seconds(uint32_t secs, uint32_t tb_freq, uint32_t phase, uint32_t total_phases)
{
    uint32_t s, chunk;
    uint32_t chunks_per_sec = 10;
    uint32_t ticks_per_chunk = tb_freq / chunks_per_sec;

    for (s = 0; s < secs; s++) {
        g_res->progress_pct = ((phase * secs + s) * 100) / (total_phases * secs);
        flush_results();
        for (chunk = 0; chunk < chunks_per_sec; chunk++) {
            if (should_stop()) return 1;
            timer_start();
            while (timer_elapsed() < ticks_per_chunk);
        }
    }
    return 0;
}

static uint32_t test_bit_fade(uint64_t ea_base, uint32_t region_size, uint32_t tb_freq)
{
    uint32_t errors = 0;
    uint32_t offset;
    uint32_t total_blocks = region_size / DMA_MAX_SIZE;
    uint32_t block;
    uint32_t pattern;
    int phase;

    for (phase = 0; phase < 2; phase++) {
        pattern = (phase == 0) ? 0x00000000u : 0xFFFFFFFFu;
        g_res->current_pattern = pattern;

        block = 0;
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            dma_put_wait(write_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A);
            block++;
            if ((block & 15) == 0)
                update_progress(phase * 3, 6);
        }

        if (spin_wait_seconds(BIT_FADE_WAIT_SECS, tb_freq, phase * 3 + 1, 6))
            return errors;
        if (should_stop()) return errors;

        block = 0;
        for (offset = 0; offset < region_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
            block++;
            if ((block & 15) == 0)
                update_progress(phase * 3 + 2, 6);
        }
    }
    return errors;
}

static uint8_t pipe_buf_a[DMA_MAX_SIZE] __attribute__((aligned(128)));
static uint8_t pipe_buf_b[DMA_MAX_SIZE] __attribute__((aligned(128)));

static uint32_t test_bw_pipelined(uint64_t ea_base, uint32_t region_size, uint32_t tb_freq)
{
    uint32_t offset;
    uint32_t ticks_read, ticks_write;
    uint32_t num_transfers = region_size / DMA_MAX_SIZE;

    fill_constant((uint32_t *)pipe_buf_a, DMA_MAX_SIZE / 4, 0x12345678u);
    fill_constant((uint32_t *)pipe_buf_b, DMA_MAX_SIZE / 4, 0x12345678u);

    update_progress(0, 3);

    timer_start();
    if (num_transfers >= 2) {
        mfc_put(pipe_buf_a, ea_base, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
        for (offset = DMA_MAX_SIZE; offset < region_size; offset += DMA_MAX_SIZE) {
            if ((offset / DMA_MAX_SIZE) & 1) {
                mfc_put(pipe_buf_b, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B, 0, 0);
                dma_wait(DMA_TAG_A);
            } else {
                mfc_put(pipe_buf_a, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
                dma_wait(DMA_TAG_B);
            }
        }
        dma_wait(DMA_TAG_A);
        dma_wait(DMA_TAG_B);
    } else if (num_transfers == 1) {
        mfc_put(pipe_buf_a, ea_base, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);
    }
    ticks_write = timer_elapsed();

    update_progress(1, 3);

    timer_start();
    if (num_transfers >= 2) {
        mfc_get(pipe_buf_a, ea_base, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
        for (offset = DMA_MAX_SIZE; offset < region_size; offset += DMA_MAX_SIZE) {
            if ((offset / DMA_MAX_SIZE) & 1) {
                mfc_get(pipe_buf_b, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_B, 0, 0);
                dma_wait(DMA_TAG_A);
            } else {
                mfc_get(pipe_buf_a, ea_base + offset, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
                dma_wait(DMA_TAG_B);
            }
        }
        dma_wait(DMA_TAG_A);
        dma_wait(DMA_TAG_B);
    } else if (num_transfers == 1) {
        mfc_get(pipe_buf_a, ea_base, DMA_MAX_SIZE, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);
    }
    ticks_read = timer_elapsed();

    update_progress(2, 3);

    if (ticks_write > 0 && tb_freq > 0) {
        g_res->bw_pipe_write_mbps = (uint32_t)((uint64_t)region_size * (uint64_t)tb_freq / ((uint64_t)ticks_write * 1000000ULL));
    }
    if (ticks_read > 0 && tb_freq > 0) {
        g_res->bw_pipe_read_mbps = (uint32_t)((uint64_t)region_size * (uint64_t)tb_freq / ((uint64_t)ticks_read * 1000000ULL));
    }

    g_res->memtest_bytes += region_size * 2;
    return 0;
}

#define COHERENCY_FLAG_READY    0xC0DE0001u
#define COHERENCY_FLAG_DONE     0xC0DE0002u
#define COHERENCY_POLL_LIMIT    50000000u

static uint32_t test_coherency(uint64_t ea_base, uint32_t region_size, uint32_t spe_index, uint32_t num_spes)
{
    uint32_t errors = 0;
    uint32_t data_offset = DMA_ALIGN;
    uint32_t data_size = region_size - DMA_ALIGN;
    uint32_t offset;
    uint32_t pattern = 0xC04E4E4Cu;
    uint32_t poll;

    if (region_size < DMA_ALIGN * 2 || num_spes < 2)
        return 0;

    data_size &= ~(DMA_MAX_SIZE - 1);
    if (data_size == 0)
        return 0;

    if (spe_index == 0) {
        update_progress(0, 3);
        for (offset = 0; offset < data_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            dma_put_wait(write_buf, ea_base + data_offset + offset, DMA_MAX_SIZE, DMA_TAG_A);
        }

        update_progress(1, 3);
        fill_constant((uint32_t *)write_buf, DMA_ALIGN / 4, COHERENCY_FLAG_READY);
        mfc_putb(write_buf, ea_base, DMA_ALIGN, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);

        for (poll = 0; poll < COHERENCY_POLL_LIMIT; poll++) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base, DMA_ALIGN, DMA_TAG_B);
            uint32_t flag = ((uint32_t *)read_buf)[0];
            if (flag == COHERENCY_FLAG_DONE)
                break;
        }
        update_progress(3, 3);
        g_res->memtest_bytes += data_size;
    } else {
        update_progress(0, 3);
        for (poll = 0; poll < COHERENCY_POLL_LIMIT; poll++) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base, DMA_ALIGN, DMA_TAG_B);
            uint32_t flag = ((uint32_t *)read_buf)[0];
            if (flag == COHERENCY_FLAG_READY)
                break;
        }

        if (poll >= COHERENCY_POLL_LIMIT) {
            update_progress(3, 3);
            return 0;
        }

        update_progress(1, 3);
        for (offset = 0; offset < data_size; offset += DMA_MAX_SIZE) {
            if (should_stop()) return errors;
            dma_get_wait(read_buf, ea_base + data_offset + offset, DMA_MAX_SIZE, DMA_TAG_B);
            fill_constant((uint32_t *)write_buf, DMA_MAX_SIZE / 4, pattern);
            errors += compare_buffers((uint32_t *)write_buf, (uint32_t *)read_buf, DMA_MAX_SIZE / 4, data_offset + offset);
            g_res->memtest_bytes += DMA_MAX_SIZE;
        }

        update_progress(2, 3);
        if (spe_index == num_spes - 1) {
            fill_constant((uint32_t *)write_buf, DMA_ALIGN / 4, COHERENCY_FLAG_DONE);
            dma_put_wait(write_buf, ea_base, DMA_ALIGN, DMA_TAG_A);
        }
        update_progress(3, 3);
    }
    return errors;
}

static uint32_t g_spe_index;
static uint32_t g_num_spes;
static uint64_t g_ea_coherency;
static uint32_t g_coherency_size;

static uint32_t test_latency(uint64_t ea_base, uint32_t region_size, uint32_t tb_freq);

static uint32_t run_test(uint32_t test_id, uint64_t ea_base, uint32_t region_size, uint32_t tb_freq)
{
    g_res->current_test = test_id;
    g_res->progress_pct = 0;
    flush_results();

    switch (test_id) {
    case MEMTEST_SEQUENTIAL:    return test_sequential(ea_base, region_size);
    case MEMTEST_WALKING_ONES:  return test_walking_ones(ea_base, region_size);
    case MEMTEST_CHECKERBOARD:  return test_checkerboard(ea_base, region_size);
    case MEMTEST_RANDOM:        return test_random(ea_base, region_size);
    case MEMTEST_MOVING_INV:    return test_moving_inv(ea_base, region_size);
    case MEMTEST_BANK_HAMMER:   return test_bank_hammer(ea_base, region_size);
    case MEMTEST_RW_TURNAROUND: return test_rw_turnaround(ea_base, region_size);
    case MEMTEST_BANDWIDTH:     return test_bandwidth(ea_base, region_size, tb_freq);
    case MEMTEST_OWN_ADDRESS:   return test_own_address(ea_base, region_size);
    case MEMTEST_MODULO_N:      return test_modulo_n(ea_base, region_size);
    case MEMTEST_BLOCK_MOVE:    return test_block_move(ea_base, region_size);
    case MEMTEST_BIT_FADE:      return test_bit_fade(ea_base, region_size, tb_freq);
    case MEMTEST_BW_PIPELINED:  return test_bw_pipelined(ea_base, region_size, tb_freq);
    case MEMTEST_COHERENCY:     return test_coherency(g_ea_coherency, g_coherency_size, g_spe_index, g_num_spes);
    case MEMTEST_LATENCY:       return test_latency(ea_base, region_size, tb_freq);
    default:                    return 0;
    }
}

#define LAT_PROBES      4096
#define LAT_DMA_SIZE    128 // min aligned dma

static uint8_t lat_buf[LAT_DMA_SIZE] __attribute__((aligned(128)));

static uint64_t lat_accum_read_ticks;
static uint64_t lat_accum_write_ticks;
static uint64_t lat_accum_probes;
static uint32_t lat_min_read_ticks;
static uint32_t lat_max_read_ticks;
static uint32_t lat_min_write_ticks;
static uint32_t lat_max_write_ticks;
static int      lat_initialized;

static uint32_t test_latency(uint64_t ea_base, uint32_t region_size, uint32_t tb_freq)
{
    uint32_t i, ticks;
    uint32_t stride = XDR_ROW_STRIDE;
    uint32_t num_addrs = region_size / stride;
    uint64_t ns_per_tick_x1000;

    if (num_addrs < 2 || tb_freq == 0)
        return 0;

    if (!lat_initialized) {
        lat_accum_read_ticks = 0;
        lat_accum_write_ticks = 0;
        lat_accum_probes = 0;
        lat_min_read_ticks = 0xFFFFFFFF;
        lat_max_read_ticks = 0;
        lat_min_write_ticks = 0xFFFFFFFF;
        lat_max_write_ticks = 0;
        lat_initialized = 1;
    }

    g_res->bw_read_mbps = 0;
    g_res->bw_write_mbps = 0;
    g_res->bw_pipe_read_mbps = 0;
    g_res->bw_pipe_write_mbps = 0;

    ns_per_tick_x1000 = 1000000000000ULL / (uint64_t)tb_freq;

    for (i = 0; i < LAT_PROBES; i++) {
        if ((i & 255) == 0 && should_stop()) return 0;

        uint32_t idx = i % num_addrs;
        uint64_t ea = ea_base + (uint64_t)idx * stride;

        timer_start();
        mfc_get(lat_buf, ea, LAT_DMA_SIZE, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);
        ticks = timer_elapsed();

        lat_accum_read_ticks += ticks;
        if (ticks < lat_min_read_ticks) lat_min_read_ticks = ticks;
        if (ticks > lat_max_read_ticks) lat_max_read_ticks = ticks;

        if ((i & 511) == 0)
            update_progress(i, LAT_PROBES * 2);
    }

    for (i = 0; i < LAT_PROBES; i++) {
        if ((i & 255) == 0 && should_stop()) return 0;

        uint32_t idx = i % num_addrs;
        uint64_t ea = ea_base + (uint64_t)idx * stride;

        mfc_get(lat_buf, ea, LAT_DMA_SIZE, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);

        timer_start();
        mfc_put(lat_buf, ea, LAT_DMA_SIZE, DMA_TAG_A, 0, 0);
        dma_wait(DMA_TAG_A);
        ticks = timer_elapsed();

        lat_accum_write_ticks += ticks;
        if (ticks < lat_min_write_ticks) lat_min_write_ticks = ticks;
        if (ticks > lat_max_write_ticks) lat_max_write_ticks = ticks;

        if ((i & 511) == 0)
            update_progress(LAT_PROBES + i, LAT_PROBES * 2);
    }

    lat_accum_probes += LAT_PROBES;

    g_res->lat_micro_read_ns  = (uint32_t)((lat_accum_read_ticks * ns_per_tick_x1000) / (lat_accum_probes * 1000));
    g_res->lat_micro_write_ns = (uint32_t)((lat_accum_write_ticks * ns_per_tick_x1000) / (lat_accum_probes * 1000));

    g_res->lat_read_ns   = (uint32_t)(((uint64_t)lat_min_read_ticks * ns_per_tick_x1000) / 1000);
    g_res->lat_write_ns  = (uint32_t)(((uint64_t)lat_min_write_ticks * ns_per_tick_x1000) / 1000);
    g_res->bw_read_mbps  = (uint32_t)(((uint64_t)lat_max_read_ticks * ns_per_tick_x1000) / 1000);
    g_res->bw_write_mbps = (uint32_t)(((uint64_t)lat_max_write_ticks * ns_per_tick_x1000) / 1000);

    g_res->current_pattern = (uint32_t)(lat_accum_probes / 1000);

    g_res->memtest_bytes += (uint64_t)LAT_PROBES * LAT_DMA_SIZE * 2;
    return 0;
}

void memtest_main(spe_params_t *params, spe_results_t *results, uint64_t ea_results, uint32_t tb_freq)
{
    uint64_t ea_base = params->ea_test_region;
    uint32_t region_size = params->test_region_size;
    uint32_t max_passes = params->memtest_passes;
    uint32_t pass, t;

    g_res = results;
    g_ea_res = ea_results;
    g_spe_index = params->spe_index;
    g_num_spes = params->num_spes;
    g_ea_coherency = params->ea_coherency_region;
    g_coherency_size = params->coherency_region_size;

    memset(results, 0, sizeof(spe_results_t));
    results->status = STATUS_RUNNING;
    flush_results();

    for (pass = 0; max_passes == 0 || pass < max_passes; pass++) {
        results->memtest_pass = pass + 1;
        results->test_pass_mask = 0;
        results->test_fail_mask = 0;
        results->tests_completed = 0;

        if (params->memtest_id != MEMTEST_AUTO_CYCLE) {
            t = params->memtest_id;
            if (should_stop()) goto done;

            uint32_t errs = run_test(t, ea_base, region_size, tb_freq);

            if (errs > 0)
                results->test_fail_mask |= (1u << t);
            else
                results->test_pass_mask |= (1u << t);

            results->tests_completed = 1;
            results->progress_pct = 100;
            flush_results();
        } else {
            for (t = 0; t < MEMTEST_COUNT; t++) {
                if (should_stop()) goto done;

                uint32_t errs = run_test(t, ea_base, region_size, tb_freq);

                if (errs > 0)
                    results->test_fail_mask |= (1u << t);
                else
                    results->test_pass_mask |= (1u << t);

                results->tests_completed = t + 1;
                results->progress_pct = 100;
                flush_results();
            }
        }

        flush_results();
    }

done:
    results->status = STATUS_DONE;
    results->progress_pct = 100;
    flush_results();
}