#ifndef MANDELBROT_BENCHMARK_H
#define MANDELBROT_BENCHMARK_H

#include <stdint.h>

#define MANDEL_MAX_W       1920u
#define MANDEL_MAX_H       1080u
#define MANDEL_NUM_SPES       6u
#define MANDEL_PIXEL_BYTES    4u
#define MANDEL_MAX_ROW_BYTES   (MANDEL_MAX_W * MANDEL_PIXEL_BYTES)
#define MANDEL_MAX_FRAME_BYTES (MANDEL_MAX_W * MANDEL_MAX_H * MANDEL_PIXEL_BYTES)

#define MANDEL_MAX_ITER_MIN   64u
#define MANDEL_MAX_ITER_DEF  256u
#define MANDEL_MAX_ITER_MAX 4096u

#define MANDEL_PALETTE_COUNT  5u

typedef struct {
    uint32_t spe_index;
    uint32_t total_spes;
    uint32_t row_start;
    uint32_t row_count;
    uint32_t width;
    uint32_t row_bytes;
    uint32_t max_iter;
    uint32_t palette_id;
    uint64_t ea_frame;
    uint64_t ea_results;
    float    re_min;
    float    im_min;
    float    dre;
    float    dim;
    float    hue_offset;
    uint32_t pad1[19];
} __attribute__((aligned(128))) mandel_params_t;

typedef struct {
    uint32_t dec_ticks;
    uint32_t iters_done;
    uint32_t pixels_done;
    uint32_t pad[29];
} __attribute__((aligned(128))) mandel_result_t;

typedef struct {
    float    miters_per_sec;
    float    mpix_per_sec;
    float    ms_per_frame;
    float    fps;
    uint32_t frames_rendered;
    uint32_t max_iter;
    double   center_re;
    double   center_im;
    double   zoom;
    uint32_t palette_id;
    float    hue_offset;
    uint32_t static_view;
    int      last_err_step;
    int      last_err_code;
    int      disabled;
} mandel_summary_t;

#ifndef __SPU__
int  mandelbrot_init(uint64_t tb_freq);
void mandelbrot_tick(uint64_t tb_freq);
void mandelbrot_render(double elapsed);
void mandelbrot_input(uint16_t btns1, uint16_t btns2);
void mandelbrot_cleanup(void);
const mandel_summary_t *mandelbrot_get_summary(void);
#endif

#endif /* MANDELBROT_BENCHMARK_H */