#include <stdio.h>
#include <string.h>

#include "cellmark_engine.h"
#include "bench.h"

#define MAX_CATEGORIES 16

typedef struct {
    const char *category;
    int         first_idx;
    int         count;
} category_info_t;

static category_info_t g_categories[MAX_CATEGORIES];
static int             g_num_categories  = 0;
static int             g_current_cat     = 0;
static int             g_current_in_cat  = 0;
static uint64_t        g_tb_freq         = 0;

static void    *g_shared_xdr_buffer = NULL;
static uint32_t g_shared_xdr_size   = 0;

void *cellmark_engine_shared_xdr_buffer(uint32_t *size_out)
{
    if (size_out) *size_out = g_shared_xdr_size;
    return g_shared_xdr_buffer;
}

static int registry_size(void)
{
    int n = 0;
    while (cellmark_bench_registry[n] != 0) n++;
    return n;
}

static const bench_module_t *current_module(void)
{
    if (g_num_categories == 0) return 0;
    int idx = g_categories[g_current_cat].first_idx + g_current_in_cat;
    return cellmark_bench_registry[idx];
}

static void build_category_index(void)
{
    int  i, n = registry_size();
    const char *cur_cat = 0;

    g_num_categories = 0;
    for (i = 0; i < n; i++) {
        const char *cat = cellmark_bench_registry[i]->category;
        if (cat == 0) continue;

        if (cur_cat == 0 || strcmp(cat, cur_cat) != 0) {
            if (g_num_categories >= MAX_CATEGORIES) {
                printf("[engine] WARNING: more than %d categories, truncating\n", MAX_CATEGORIES);
                break;
            }
            g_categories[g_num_categories].category  = cat;
            g_categories[g_num_categories].first_idx = i;
            g_categories[g_num_categories].count     = 0;
            g_num_categories++;
            cur_cat = cat;
        }
        g_categories[g_num_categories - 1].count++;
    }

    printf("[engine] registry: %d modules in %d categories\n", n, g_num_categories);
    for (i = 0; i < g_num_categories; i++) {
        printf("[engine]   [%d] '%s' (%d module%s)\n", i, g_categories[i].category, g_categories[i].count, g_categories[i].count == 1 ? "" : "s");
    }
}

int cellmark_engine_init(uint64_t tb_freq, void *shared_xdr_buffer, uint32_t shared_xdr_size)
{
    int  i, n;

    g_tb_freq = tb_freq;
    g_shared_xdr_buffer = shared_xdr_buffer;
    g_shared_xdr_size = shared_xdr_size;

    build_category_index();
    n = registry_size();

    for (i = 0; i < n; i++) {
        const bench_module_t *m = cellmark_bench_registry[i];
        if (m->init) {
            int r = m->init(tb_freq);
            if (r != 0) {
                printf("[engine] bench '%s' init returned %d (still listed)\n", m->id, r);
            }
        }
    }

    {
        const bench_module_t *m = current_module();
        if (m && m->start) m->start();
    }
    return 0;
}

static void switch_to(int new_cat, int new_in_cat)
{
    const bench_module_t *old = current_module();
    if (old && old->stop) old->stop();

    g_current_cat    = new_cat;
    g_current_in_cat = new_in_cat;

    const bench_module_t *m = current_module();
    if (m) {
        printf("[engine] switched to '%s' (category '%s')\n", m->id, m->category);
        if (m->start) m->start();
    }
}

void cellmark_engine_handle_input(uint16_t pressed1, uint16_t pressed2)
{
    int  cat_dir = 0;
    int  in_dir  = 0;

    if (pressed2 & 0x01) cat_dir = -1;
    if (pressed2 & 0x02) cat_dir = +1;
    if (pressed2 & 0x04) in_dir  = -1;
    if (pressed2 & 0x08) in_dir  = +1;
    if (pressed1 & 0x10) in_dir  = -1;
    if (pressed1 & 0x40) in_dir  = +1;

    if (cat_dir != 0 && g_num_categories > 0) {
        int nc = (g_current_cat + cat_dir + g_num_categories) % g_num_categories;
        if (nc != g_current_cat) switch_to(nc, 0);
        return;
    }

    if (in_dir != 0 && g_num_categories > 0) {
        int count = g_categories[g_current_cat].count;
        if (count > 1) {
            int ni = (g_current_in_cat + in_dir + count) % count;
            switch_to(g_current_cat, ni);
        }
        else {
            const bench_module_t *m = current_module();
            if (m && m->input) m->input(pressed1, pressed2);
        }
        return;
    }

    const bench_module_t *m = current_module();
    if (m && m->input) m->input(pressed1, pressed2);
}

void cellmark_engine_tick(uint64_t tb_freq)
{
    const bench_module_t *m = current_module();
    if (m && m->tick) m->tick(tb_freq);
}

void cellmark_engine_render(double elapsed_sec)
{
    const bench_module_t *m = current_module();
    if (m && m->render) m->render(elapsed_sec);
}

void cellmark_engine_shutdown(void)
{
    int i, n = registry_size();

    {
        const bench_module_t *m = current_module();
        if (m && m->stop) m->stop();
    }

    for (i = 0; i < n; i++) {
        const bench_module_t *m = cellmark_bench_registry[i];
        if (m->cleanup) m->cleanup();
    }
}

const bench_module_t *cellmark_engine_current_module(void)
{
    return current_module();
}

const char *cellmark_engine_current_category(void)
{
    if (g_num_categories == 0) return "";
    return g_categories[g_current_cat].category;
}