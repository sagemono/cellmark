#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/time_util.h>
#include <cell/pad/pad_codes.h>

#include "bench.h"
#include "stress_common.h"
#include "gcm.h"
#include "cellmark.h"
#include "mandelbrot_benchmark.h"

#define ERR_IMG_IMPORT   1
#define ERR_GRP_CREATE   2
#define ERR_THR_INIT     3
#define ERR_GRP_START    4
#define ERR_GRP_JOIN     5
#define ERR_DEC_ZERO     6
#define ERR_FRAME_ALLOC  7
#define ERR_GCM_MAP      8

extern const char _binary_spu_mandelbrot_elf_start[];

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;

static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static void     *g_frame_buf       = NULL;
static uint32_t  g_frame_buf_size  = 0;
static uint32_t  g_frame_io_offset = 0;

static uint32_t  g_render_w        = 1280;
static uint32_t  g_render_h        =  720;
static uint32_t  g_render_row_bytes = 1280 * 4;
static uint32_t  g_rows_per_spe    = 120;
static int       g_use_scaled_blit = 0;

static double   g_center_re = -0.75;
static double   g_center_im =  0.0;
static double   g_half_w    =  1.75;
static double   g_half_h    =  1.00;
static uint32_t g_max_iter  =  MANDEL_MAX_ITER_DEF;
static uint32_t g_palette_id = 0;
static float    g_hue_offset = 0.0f;

static double   g_cached_re     = 0.0;
static double   g_cached_im     = 0.0;
static double   g_cached_hw     = 0.0;
static double   g_cached_hh     = 0.0;
static uint32_t g_cached_iter   = 0;
static uint32_t g_cached_palette= 0;
static float    g_cached_hue    = 0.0f;
static int      g_cache_valid   = 0;

static mandel_params_t g_params [MANDEL_NUM_SPES] __attribute__((aligned(128)));
static mandel_result_t g_results[MANDEL_NUM_SPES] __attribute__((aligned(128)));
static mandel_summary_t g_summary;

static const char *palette_name(uint32_t id)
{
    switch (id) {
        case 0: return "rainbow";
        case 1: return "fire";
        case 2: return "ocean";
        case 3: return "grayscale";
        case 4: return "electric";
        default: return "?";
    }
}

static void reset_view(void)
{
    g_center_re = -0.75;
    g_center_im =  0.0;
    g_half_w    =  1.75;

    if (g_render_w > 0)
        g_half_h = g_half_w * ((double)g_render_h / (double)g_render_w);
    else
        g_half_h = 1.0;
    g_max_iter  =  MANDEL_MAX_ITER_DEF;
    g_cache_valid = 0;
}

static void record_failure(int step, int code)
{
    g_summary.last_err_step = step;
    g_summary.last_err_code = code;
    g_summary.disabled = 1;
    printf("[mandelbrot] FAIL step=%d code=0x%08x\n", step, (unsigned)code);
}

static const struct { uint32_t w, h; } MANDEL_RES_TRY[] = {
    { 1920, 1080 },
    { 1600,  900 },
    { 1280,  720 },
    { 1024,  576 },
    {  800,  450 },
    {  640,  360 },
};
#define MANDEL_RES_TRY_N (sizeof(MANDEL_RES_TRY) / sizeof(MANDEL_RES_TRY[0]))

int mandelbrot_init(uint64_t tb_freq)
{
    g_tb_freq = tb_freq;
    memset(&g_summary, 0, sizeof(g_summary));
    g_initialized = 1;
    printf("[mandelbrot] init OK (lazy alloc on first activation)\n");
    return 0;
}

static int try_allocate_frame_buffer(void)
{
    extern uint32_t screen_width, screen_height;
    unsigned t;

    for (t = 0; t < MANDEL_RES_TRY_N; t++) {
        uint32_t tw = MANDEL_RES_TRY[t].w;
        uint32_t th = MANDEL_RES_TRY[t].h;
        uint32_t row_bytes, frame_bytes, size;
        void *buf;
        uint32_t io;

        if (tw > screen_width || th > screen_height) continue;

        tw &= ~7u;
        th -= (th % MANDEL_NUM_SPES);
        if (tw == 0 || th == 0) continue;

        row_bytes   = tw * MANDEL_PIXEL_BYTES;
        frame_bytes = row_bytes * th;
        size = (frame_bytes + (1u << 20) - 1u) & ~((1u << 20) - 1u);

        buf = memalign(1024 * 1024, size);
        if (!buf) {
            printf("[mandelbrot] %u x %u (%u MB) alloc failed, trying smaller\n", (unsigned)tw, (unsigned)th, (unsigned)(size / (1024 * 1024)));
            continue;
        }
        memset(buf, 0, frame_bytes);

        io = gcm_map_main_buffer(buf, size);
        if (io == 0) {
            free(buf);
            printf("[mandelbrot] %u x %u gcm map failed, trying smaller\n", (unsigned)tw, (unsigned)th);
            continue;
        }

        g_render_w         = tw;
        g_render_h         = th;
        g_render_row_bytes = row_bytes;
        g_rows_per_spe     = th / MANDEL_NUM_SPES;
        g_frame_buf        = buf;
        g_frame_buf_size   = size;
        g_frame_io_offset  = io;
        g_use_scaled_blit  = (tw != screen_width || th != screen_height);

        reset_view();
        printf("[mandelbrot] %u x %u%s, %u rows/SPE, %u MB buf, io=0x%08x\n", (unsigned)tw, (unsigned)th, g_use_scaled_blit ? " scaled-blit" : " native", (unsigned)g_rows_per_spe, (unsigned)(size / (1024 * 1024)), (unsigned)io);
        return 0;
    }

    printf("[mandelbrot] all resolutions failed to allocate\n");
    return -1;
}

static void release_frame_buffer(void)
{
    if (g_frame_io_offset != 0) {
        gcm_unmap_main_buffer(g_frame_io_offset);
        g_frame_io_offset = 0;
    }
    if (g_frame_buf) {
        free(g_frame_buf);
        g_frame_buf = NULL;
    }
    g_frame_buf_size = 0;
    g_cache_valid    = 0;
}

static void mandelbrot_start(void)
{
    if (g_frame_buf) return;
    if (try_allocate_frame_buffer() != 0) {
        record_failure(ERR_FRAME_ALLOC, 0);
        return;
    }
    g_summary.disabled = 0;
}

static void mandelbrot_stop(void)
{
    release_frame_buffer();
}

void mandelbrot_cleanup(void)
{
    release_frame_buffer();
}

static double analog_axis(uint8_t raw)
{
    int v = (int)raw - 128;
    if (v >  -24 && v <  24) return 0.0;
    if (v < 0) v += 24; else v -= 24;
    return (double)v / 103.0;
}

static void apply_held_pad(void)
{
    double aspect   = g_half_h / g_half_w;
    double pan_step = 0.02 * g_half_w;

    {
        double lx = analog_axis(pad_lx);
        double ly = analog_axis(pad_ly);
        g_center_re += pan_step * lx;
        g_center_im += pan_step * ly * aspect;
    }

    if (pad_held2 & CELL_PAD_CTRL_CROSS) {
        g_half_w *= 0.95; g_half_h *= 0.95;
    }
    if (pad_held2 & CELL_PAD_CTRL_CIRCLE) {
        g_half_w *= 1.0 / 0.95; g_half_h *= 1.0 / 0.95;
    }

    {
        double ry = analog_axis(pad_ry);
        if (ry != 0.0) {
            double factor = 1.0 + ry * 0.06;
            g_half_w *= factor;
            g_half_h *= factor;
        }
    }

    if (pad_held1 & CELL_PAD_CTRL_UP)   g_hue_offset += 0.008f;
    if (pad_held1 & CELL_PAD_CTRL_DOWN) g_hue_offset -= 0.008f;
    if (g_hue_offset >= 1.0f) g_hue_offset -= 1.0f;
    if (g_hue_offset <  0.0f) g_hue_offset += 1.0f;

    if (g_half_w > 4.0) { g_half_w = 4.0; g_half_h = 4.0 * aspect; }
}

static int view_changed(void)
{
    if (!g_cache_valid) return 1;
    return g_center_re  != g_cached_re ||
           g_center_im  != g_cached_im ||
           g_half_w     != g_cached_hw ||
           g_half_h     != g_cached_hh ||
           g_max_iter   != g_cached_iter ||
           g_palette_id != g_cached_palette ||
           g_hue_offset != g_cached_hue;
}

static void commit_cache(void)
{
    g_cached_re      = g_center_re;
    g_cached_im      = g_center_im;
    g_cached_hw      = g_half_w;
    g_cached_hh      = g_half_h;
    g_cached_iter    = g_max_iter;
    g_cached_palette = g_palette_id;
    g_cached_hue     = g_hue_offset;
    g_cache_valid    = 1;
}

void mandelbrot_tick(uint64_t tb_freq)
{
    sys_spu_thread_group_t              spu_group;
    sys_spu_thread_t                    spu_threads[MANDEL_NUM_SPES];
    sys_spu_thread_group_attribute_t    group_attr;
    sys_spu_thread_attribute_t          thread_attr;
    sys_spu_thread_argument_t           thread_args;
    int      ret, cause, status;
    int      i;
    uint32_t max_dec_ticks = 0;
    uint64_t total_iters   = 0;
    float    re_min, im_min, dre, dim;

    (void)tb_freq;

    if (g_summary.disabled || !g_initialized) return;

    apply_held_pad();

    if (!view_changed()) {
        g_summary.static_view = 1;
        return;
    }
    g_summary.static_view = 0;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img,
            (const void *)_binary_spu_mandelbrot_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img,
                (const void *)_binary_spu_mandelbrot_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { record_failure(ERR_IMG_IMPORT, ret); return; }
        }
        g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "mandelbrot");
    ret = sys_spu_thread_group_create(&spu_group, MANDEL_NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) { record_failure(ERR_GRP_CREATE, ret); return; }

    re_min = (float)(g_center_re - g_half_w);
    im_min = (float)(g_center_im - g_half_h);
    dre    = (float)((2.0 * g_half_w) / (double)g_render_w);
    dim    = (float)((2.0 * g_half_h) / (double)g_render_h);

    memset(g_params,  0, sizeof(g_params));
    memset(g_results, 0, sizeof(g_results));
    for (i = 0; i < (int)MANDEL_NUM_SPES; i++) {
        g_params[i].spe_index   = (uint32_t)i;
        g_params[i].total_spes  = MANDEL_NUM_SPES;
        g_params[i].row_start   = (uint32_t)i * g_rows_per_spe;
        g_params[i].row_count   = g_rows_per_spe;
        g_params[i].width       = g_render_w;
        g_params[i].row_bytes   = g_render_row_bytes;
        g_params[i].max_iter    = g_max_iter;
        g_params[i].palette_id  = g_palette_id;
        g_params[i].ea_frame    = (uint64_t)(uintptr_t)g_frame_buf;
        g_params[i].ea_results  = (uint64_t)(uintptr_t)g_results;
        g_params[i].re_min      = re_min;
        g_params[i].im_min      = im_min;
        g_params[i].dre         = dre;
        g_params[i].dim         = dim;
        g_params[i].hue_offset  = g_hue_offset;
    }

    for (i = 0; i < (int)MANDEL_NUM_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "mandel_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&g_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &g_spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) { record_failure(ERR_THR_INIT, ret); goto fail_group; }
    }

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) { record_failure(ERR_GRP_START, ret); goto fail_group; }

    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK) { record_failure(ERR_GRP_JOIN, ret); goto fail_group; }

    sys_spu_thread_group_destroy(spu_group);

    for (i = 0; i < (int)MANDEL_NUM_SPES; i++) {
        if (g_results[i].dec_ticks > max_dec_ticks)
            max_dec_ticks = g_results[i].dec_ticks;
        total_iters += (uint64_t)g_results[i].iters_done;
    }
    if (max_dec_ticks == 0u) { record_failure(ERR_DEC_ZERO, 0); return; }

    {
        double secs   = (double)max_dec_ticks / (double)g_tb_freq;
        float  miters = (float)((double)total_iters / secs / 1.0e6);
        float  mpix   = (float)((double)(g_render_w * g_render_h) / secs / 1.0e6);
        float  ms     = (float)(secs * 1.0e3);
        float  fps    = (float)(1.0 / secs);
        if (g_summary.frames_rendered == 0u) {
            g_summary.miters_per_sec = miters;
            g_summary.mpix_per_sec   = mpix;
            g_summary.ms_per_frame   = ms;
            g_summary.fps            = fps;
        } else {
            g_summary.miters_per_sec = 0.7f * g_summary.miters_per_sec + 0.3f * miters;
            g_summary.mpix_per_sec   = 0.7f * g_summary.mpix_per_sec   + 0.3f * mpix;
            g_summary.ms_per_frame   = 0.7f * g_summary.ms_per_frame   + 0.3f * ms;
            g_summary.fps            = 0.7f * g_summary.fps            + 0.3f * fps;
        }
    }
    g_summary.max_iter        = g_max_iter;
    g_summary.center_re       = g_center_re;
    g_summary.center_im       = g_center_im;
    g_summary.zoom            = 2.0 * g_half_w;
    g_summary.palette_id      = g_palette_id;
    g_summary.hue_offset      = g_hue_offset;
    g_summary.frames_rendered++;
    commit_cache();
    return;

fail_group:
    sys_spu_thread_group_destroy(spu_group);
}

void mandelbrot_input(uint16_t btns1, uint16_t btns2)
{
    if (btns1 & CELL_PAD_CTRL_LEFT) {
        g_palette_id = (g_palette_id + MANDEL_PALETTE_COUNT - 1u) % MANDEL_PALETTE_COUNT;
    }
    if (btns1 & CELL_PAD_CTRL_RIGHT) {
        g_palette_id = (g_palette_id + 1u) % MANDEL_PALETTE_COUNT;
    }

    if (btns2 & CELL_PAD_CTRL_SQUARE) {
        if      (g_max_iter <=  64u) g_max_iter =  128u;
        else if (g_max_iter <= 128u) g_max_iter =  256u;
        else if (g_max_iter <= 256u) g_max_iter =  512u;
        else if (g_max_iter <= 512u) g_max_iter = 1024u;
        else if (g_max_iter <=1024u) g_max_iter = 2048u;
        else if (g_max_iter <=2048u) g_max_iter = 4096u;
        else                         g_max_iter =   64u;
    }
    if (btns2 & CELL_PAD_CTRL_L1) {
        reset_view();
    }
}

const mandel_summary_t *mandelbrot_get_summary(void) { return &g_summary; }

uint32_t mandel_get_io_offset(void) { return g_frame_io_offset; }

uint32_t mandel_get_render_w(void) { return g_render_w; }
uint32_t mandel_get_render_h(void) { return g_render_h; }
uint32_t mandel_get_row_bytes(void) { return g_render_row_bytes; }
int      mandel_use_scaled_blit(void) { return g_use_scaled_blit; }

const char *mandel_palette_name(uint32_t id) { return palette_name(id); }

const bench_module_t bench_mandelbrot = {
    .id           = "mandelbrot",
    .display_name = "Mandelbrot (SPE compute + RSX blit)",
    .category     = "render",
    .kind         = BENCH_KIND_BATCH,
    .init         = mandelbrot_init,
    .start        = mandelbrot_start,
    .stop         = mandelbrot_stop,
    .tick         = mandelbrot_tick,
    .render       = mandelbrot_render,
    .input        = mandelbrot_input,
    .cleanup      = mandelbrot_cleanup,
};