/*
 * spu.c - SPU thread group lifecycle management
 */

#include <stdio.h>
#include <string.h>

#include <sys/ppu_thread.h>
#include <sys/spu_initialize.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/timer.h>

#include "stress_common.h"
#include "spu.h"

extern const char _binary_spu_stress_elf_start[];

spe_params_t  spe_params [MAX_SPES] __attribute__((aligned(128)));
spe_results_t spe_results[MAX_SPES] __attribute__((aligned(128)));
int           num_spes_active = 0;

static sys_spu_thread_group_t spu_group;
static sys_spu_thread_t       spu_threads[MAX_SPES];
static sys_spu_image_t        spu_img;
static int                    spu_initialized = 0;

int init_spu(void)
{
    int ret;

    ret = sys_spu_initialize(MAX_SPES, 0);
    if (ret != CELL_OK) {
        printf("sys_spu_initialize failed: 0x%x\n", ret);
        return ret;
    }

    ret = sys_spu_image_import(&spu_img, (const void *)_binary_spu_stress_elf_start, SYS_SPU_IMAGE_PROTECT);
    if (ret != CELL_OK) {
        ret = sys_spu_image_import(&spu_img, (const void *)_binary_spu_stress_elf_start, SYS_SPU_IMAGE_DIRECT);
    }
    if (ret != CELL_OK) {
        printf("spu: image import failed: 0x%x\n", ret);
        return ret;
    }

    spu_initialized = 1;
    printf("spu: image imported ok\n");
    return 0;
}

int start_spu_stress(int mode, int memtest_id, void *memtest_region, uint32_t region_size, uint32_t tb_freq)
{
    sys_spu_thread_group_attribute_t group_attr;
    sys_spu_thread_attribute_t       thread_attr;
    sys_spu_thread_argument_t        thread_args;
    int ret, i;

    if (!spu_initialized)
        return -1;

    sys_spu_thread_group_attribute_initialize(group_attr);
    sys_spu_thread_group_attribute_name(group_attr, "cellmark");

    ret = sys_spu_thread_group_create(&spu_group, MAX_SPES, 100, &group_attr);
    if (ret != CELL_OK) {
        printf("spu: thread_group_create failed: 0x%x\n", ret);
        return ret;
    }

    memset(spe_params,  0, sizeof(spe_params));
    memset(spe_results, 0, sizeof(spe_results));

    for (i = 0; i < MAX_SPES; i++) {
        spe_params[i].ea_results       = (uint64_t)(uintptr_t)&spe_results[i];
        spe_params[i].spe_index        = i;
        spe_params[i].mode             = mode;
        spe_params[i].batch_iterations = BATCH_ITERATIONS;
        spe_params[i].verify_interval  = VERIFY_INTERVAL;
        spe_params[i].tb_freq          = tb_freq;

        if (mode == MODE_MEMTEST && memtest_region) {
            uint32_t slice_size = region_size / MAX_SPES;
            slice_size &= ~(DMA_MAX_SIZE - 1);
            spe_params[i].ea_test_region     = (uint64_t)(uintptr_t)memtest_region + (uint64_t)i * slice_size;
            spe_params[i].test_region_size   = slice_size;
            spe_params[i].memtest_id         = memtest_id;
            spe_params[i].memtest_passes     = 0;
            spe_params[i].num_spes           = MAX_SPES;
            spe_params[i].ea_coherency_region = (uint64_t)(uintptr_t)memtest_region;
            spe_params[i].coherency_region_size = slice_size;
        }
    }

    num_spes_active = 0;
    for (i = 0; i < MAX_SPES; i++) {
        sys_spu_thread_attribute_initialize(thread_attr);
        sys_spu_thread_attribute_name(thread_attr, "stress_spe");

        memset(&thread_args, 0, sizeof(thread_args));
        thread_args.arg1 = (uint64_t)(uintptr_t)&spe_params[i];

        ret = sys_spu_thread_initialize(&spu_threads[i], spu_group, i, &spu_img, &thread_attr, &thread_args);
        if (ret != CELL_OK) {
            printf("spu: thread_init[%d] failed: 0x%x\n", i, ret);
            break;
        }
        num_spes_active++;
    }

    if (num_spes_active == 0) {
        printf("no SPU threads could be created\n");
        return -1;
    }

    ret = sys_spu_thread_group_start(spu_group);
    if (ret != CELL_OK) {
        printf("sys_spu_thread_group_start failed: 0x%x\n", ret);
        return ret;
    }

    printf("spu: %d SPEs started, mode=%s\n", num_spes_active, mode_name(mode));
    return 0;
}

uint32_t spu_get_thread(int idx)
{
    if (idx < 0 || idx >= num_spes_active) return 0;
    return (uint32_t)spu_threads[idx];
}

void stop_spu_stress(void)
{
    int i, ret, cause, status;

    for (i = 0; i < num_spes_active; i++)
        sys_spu_thread_write_spu_mb(spu_threads[i], MBOX_CMD_STOP);

    sys_timer_usleep(100000); // 100ms

    sys_spu_thread_group_terminate(spu_group, 0);

    ret = sys_spu_thread_group_join(spu_group, &cause, &status);
    if (ret != CELL_OK)
        printf("spu_thread_group_join: 0x%x (cause=%d status=%d)\n", ret, cause, status);

    sys_spu_thread_group_destroy(spu_group);
    num_spes_active = 0;
}