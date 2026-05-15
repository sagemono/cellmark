#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/event.h>
#include <sys/timer.h>
#include <sys/time_util.h>

#include "stress_common.h"
#include "mbox_benchmark.h"
#define NUM_SPES   1
#define MBOX_SPU_PORT       58
#define MBOX_RECV_TIMEOUT_US    100000ULL
#define ERR_IMAGE_IMPORT    1
#define ERR_GROUP_CREATE    2
#define ERR_THREAD_INIT     3
#define ERR_QUEUE_CREATE    4
#define ERR_CONNECT_EVENT   5
#define ERR_GROUP_START     6
#define ERR_WRITE_MB        7
#define ERR_RECV_EVENT      8

extern const char _binary_spu_mbox_elf_start[];

static uint64_t g_tb_freq     = 0;
static int      g_initialized = 0;
static int      g_disabled    = 0;

static sys_spu_image_t g_spu_img;
static int             g_image_loaded = 0;

static mbox_results_summary_t g_summary;

void mbox_benchmark_init(uint64_t timebase_freq)
{
    g_tb_freq     = timebase_freq;
    g_initialized = 1;
    g_disabled    = 0;
    memset(&g_summary, 0, sizeof(g_summary));
}

static void record_failure(int step, int code)
{
    g_summary.last_err_step = step;
    g_summary.last_err_code = code;
    g_disabled = 1;
    printf("[mbox] FAIL step=%d code=0x%08x\n", step, (unsigned)code);
}

float mbox_benchmark_run(uint32_t iterations)
{
    sys_spu_thread_group_t              spu_group = 0;
    sys_spu_thread_t                    spu_thread = 0;
    sys_spu_thread_group_attribute_t    group_attr;
    sys_spu_thread_attribute_t          thread_attr;
    sys_spu_thread_argument_t           thread_args;
    sys_event_queue_attribute_t         eq_attr;
    sys_event_queue_t                   eq = 0;
    sys_event_t                         event;
    uint64_t t0 = 0, t1 = 0;
    uint32_t val;
    uint32_t i;
    uint32_t completed = 0;
    int      ret, cause, status;
    int      event_connected = 0;
    int      queue_created   = 0;
    int      group_created   = 0;
    int      group_started   = 0;
    int      ok              = 0;

    if (g_disabled)                       return 0.0f;
    if (!g_initialized || g_tb_freq == 0) return 0.0f;
    if (iterations == 0)                  return 0.0f;

    if (!g_image_loaded) {
        ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_mbox_elf_start, SYS_SPU_IMAGE_PROTECT);
        if (ret != CELL_OK) {
            ret = sys_spu_image_import(&g_spu_img, (const void *)_binary_spu_mbox_elf_start, SYS_SPU_IMAGE_DIRECT);
            if (ret != CELL_OK) { record_failure(ERR_IMAGE_IMPORT, ret); return 0.0f; }
        }       g_image_loaded = 1;
    }

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "mbox_bw");
    ret = sys_spu_thread_group_create(&spu_group, NUM_SPES, 100, &group_attr);
    if (ret != CELL_OK) { record_failure(ERR_GROUP_CREATE, ret); return 0.0f; }
    group_created = 1;

    sys_spu_thread_attribute_initialize(thread_attr);
    sys_spu_thread_attribute_name(thread_attr, "mbox_spe");

    memset(&thread_args, 0, sizeof(thread_args));
    thread_args.arg1 = (uint64_t)iterations;

    ret = sys_spu_thread_initialize(&spu_thread, spu_group, 0, &g_spu_img, &thread_attr, &thread_args);
    if (ret != CELL_OK) { record_failure(ERR_THREAD_INIT, ret); goto cleanup; }

    sys_event_queue_attribute_initialize(eq_attr);
    ret = sys_event_queue_create(&eq, &eq_attr, SYS_EVENT_QUEUE_LOCAL, 8);
    if (ret != CELL_OK) { record_failure(ERR_QUEUE_CREATE, ret); goto cleanup; }
    queue_created = 1;

    ret = sys_spu_thread_connect_event(spu_thread, eq, SYS_SPU_THREAD_EVENT_USER, MBOX_SPU_PORT);
    if (ret != CELL_OK) { record_failure(ERR_CONNECT_EVENT, ret); goto cleanup; }
    event_connected = 1;

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) { record_failure(ERR_GROUP_START, ret); goto cleanup; }
    group_started = 1;

    val = 0u;
    SYS_TIMEBASE_GET(t0);
    for (i = 0; i < iterations; i++) {
        ret = sys_spu_thread_write_spu_mb(spu_thread, val);
        if (ret != CELL_OK) { record_failure(ERR_WRITE_MB, ret); goto cleanup; }
        ret = sys_event_queue_receive(eq, &event, MBOX_RECV_TIMEOUT_US);
        if (ret != CELL_OK) { record_failure(ERR_RECV_EVENT, ret); goto cleanup; }
        val = (uint32_t)(event.data2 & 0x00FFFFFFu);
        completed++;
    }
    SYS_TIMEBASE_GET(t1);
    ok = 1;

cleanup:
    g_summary.completed_iters = completed;
    if (event_connected)
        sys_spu_thread_disconnect_event(spu_thread, SYS_SPU_THREAD_EVENT_USER, MBOX_SPU_PORT);
    if (queue_created)
        sys_event_queue_destroy(eq, 0);
    if (group_started) {
        sys_spu_thread_group_terminate(spu_group, 0);
        sys_spu_thread_group_join(spu_group, &cause, &status);
    }
    if (group_created)
        sys_spu_thread_group_destroy(spu_group);

    if (!ok || completed == 0) return 0.0f;

    {
        double secs = (double)(t1 - t0) / (double)g_tb_freq;
        if (secs <= 0.0) return 0.0f;
        return (float)((secs * 1.0e9) / (double)completed);
    }
}

#define MBOX_ITERATIONS_PER_BATCH   4096u

void mbox_run_batch(uint64_t tb_freq)
{
    float ns;

    (void)tb_freq;

    ns = mbox_benchmark_run(MBOX_ITERATIONS_PER_BATCH);
    if (ns <= 0.0f) return;

    if (g_summary.runs == 0) g_summary.ns_per_roundtrip = ns;
    else g_summary.ns_per_roundtrip = 0.7f * g_summary.ns_per_roundtrip + 0.3f * ns;
    g_summary.ns_one_way       = g_summary.ns_per_roundtrip * 0.5f;
    g_summary.rounds_per_sec_M = 1000.0f / g_summary.ns_per_roundtrip;
    g_summary.runs++;
}

const mbox_results_summary_t *mbox_get_results(void)
{
    return &g_summary;
}