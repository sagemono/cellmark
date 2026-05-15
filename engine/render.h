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

void render_workload_stats(double elapsed);

void render_burn_stats(double elapsed);

#define LOG_PATH "/dev_hdd0/game/CELLMARK0/USRDIR/cellmark.log"
void write_log(const char *reason);

#endif /* RENDER_H */