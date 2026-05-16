#include <stdio.h>

#include <cell/dbgfont.h>

#include "cellmark.h"
#include "gcm.h"
#include "mandelbrot_benchmark.h"

extern uint32_t    mandel_get_io_offset(void);
extern uint32_t    mandel_get_render_w(void);
extern uint32_t    mandel_get_render_h(void);
extern uint32_t    mandel_get_row_bytes(void);
extern int         mandel_use_scaled_blit(void);
extern const char *mandel_palette_name(uint32_t id);

void mandelbrot_render(double elapsed)
{
    const mandel_summary_t *s = mandelbrot_get_summary();
    (void)elapsed;

    cellDbgFontConsoleClear(dbg_console);

    if (s->disabled) {
        cellDbgFontConsolePrintf(dbg_console, "mandelbrot: DISABLED\n");
        cellDbgFontConsolePrintf(dbg_console, "  err step %d code 0x%08x\n", s->last_err_step, (unsigned)s->last_err_code);
        return;
    }

    {
        uint32_t io = mandel_get_io_offset();
        if (io != 0) {
            uint32_t pitch = mandel_get_row_bytes();
            uint32_t w = mandel_get_render_w();
            uint32_t h = mandel_get_render_h();
            if (mandel_use_scaled_blit())
                gcm_blit_main_to_screen_scaled(io, pitch, w, h);
            else
                gcm_blit_main_to_screen(io, pitch, w, h);
        }
    }

    if (s->frames_rendered == 0u) {
        cellDbgFontConsolePrintf(dbg_console, "mandelbrot (SPE compute + RSX blit)\n\n");
        cellDbgFontConsolePrintf(dbg_console, "  (warming up...)\n");
        return;
    }

    cellDbgFontConsolePrintf(dbg_console, "Mandelbrot:  %.1f fps   %.2f ms/frame   %.1f Miter/sec   %.2f Mpix/sec%s\n", s->fps, s->ms_per_frame, s->miters_per_sec, s->mpix_per_sec, s->static_view ? "   [STATIC]" : "");
    cellDbgFontConsolePrintf(dbg_console, "  center  ( %+.6f , %+.6f )   zoom %.3e   max_iter %u   frames %u\n", s->center_re, s->center_im, s->zoom, (unsigned)s->max_iter, (unsigned)s->frames_rendered);
    cellDbgFontConsolePrintf(dbg_console, "  palette: %s  hue: %.2f\n", mandel_palette_name(s->palette_id), s->hue_offset);
    cellDbgFontConsolePrintf(dbg_console, "  L-stick pan   R-stick Y zoom   X/O zoom   D-LR palette   D-UD hue   Square iter   L1 reset\n");
}