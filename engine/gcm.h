#ifndef GCM_H
#define GCM_H

#include <stdint.h>
#include <cell/gcm.h>
#include <cell/dbgfont.h>

extern uint32_t screen_width;
extern uint32_t screen_height;

extern void *local_heap_ptr;

extern CellDbgFontConsoleId dbg_console;

int init_display(void);

int init_dbgfont(void *local_addr, uint32_t *local_used);

void set_render_target(void);

void flip_frame(void);

uint32_t gcm_map_main_buffer(void *addr, uint32_t size);

int gcm_unmap_main_buffer(uint32_t io_offset);

void gcm_blit_main_to_screen(uint32_t src_io_offset, uint32_t src_pitch, uint32_t src_w, uint32_t src_h);

void gcm_blit_main_to_screen_scaled(uint32_t src_io_offset, uint32_t src_pitch, uint32_t src_w, uint32_t src_h);

#endif /* GCM_H */