#ifndef CELLMARK_ENGINE_H
#define CELLMARK_ENGINE_H

#include <stdint.h>
#include "bench.h"

int cellmark_engine_init(uint64_t tb_freq, void *shared_xdr_buffer, uint32_t shared_xdr_size);

void cellmark_engine_handle_input(uint16_t pressed1, uint16_t pressed2);

void cellmark_engine_tick(uint64_t tb_freq);

void cellmark_engine_render(double elapsed_sec);

void cellmark_engine_shutdown(void);

const bench_module_t *cellmark_engine_current_module(void);
const char           *cellmark_engine_current_category(void);

#endif /* CELLMARK_ENGINE_H */