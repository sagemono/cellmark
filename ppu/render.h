/*
 * render.h - per page display rendering declarations
 *
 * each page has its own .c file:
 *   render_cell.c  - cell compute / XDR memtest pages
 *   render_ppe.c   - ppe core benchmark page
 *   render_disk.c  - HDD/SSD/SSHD io benchmark page
 *   render.c       - top tevel dispatch (render_stats) n' file logging
 */
#ifndef RENDER_H
#define RENDER_H

void        render_compute_stats(double elapsed);
void        render_memtest_stats(double elapsed);
float       get_peak_ops_stock(void);
const char *ops_unit_name(void);

void render_ppe_stats(double elapsed);

void render_disk_stats(double elapsed);
void render_disk_probe_view(double elapsed);

void render_dma_stats(double elapsed);

void render_eib_stats(double elapsed);

void render_stats(void);

#define LOG_PATH "/dev_hdd0/game/CELLMARK0/USRDIR/cellmark.log"
void write_log(const char *reason);

#endif /* RENDER_H */