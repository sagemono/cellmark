/*
 * disk_benchmark.c - HDD/SSD/SSHD I/O benchmarks
 *
 * test file: 32MB at DISK_TEST_FILE
 * sequential: read/write in 64KB chunks (512 transfers)
 * random 4K:  read/write 4KB at 512 random offsets within the file
 *
 * 64KB chunk size should match the cluster size on /dev_hdd0
 * each cellFsRead issues exactly one cluster read so no fragmentation
 * penalty even on an pwned drive. 1MB chunks span 16 clusters
 * and incur rotational seek overhead on a fragmented file (~6 MB/s
 * vs ~44 MB/s with 64KB).
 *
 * write path: write all chunks, then cellFsFsync before stopping
 * the timer. this measures committed (media) write speed, not
 * write cache speed.
 *
 * timing uses the PPE timebase (79.8 MHz). result accuracy:
 * - seq ~700ms pass: ~56M ticks, gives sub millisecond resolution
 * - random 4K ~100-2000ms: well resolved at all drive speeds
 */

#include <cell/fs/cell_fs_file_api.h>
#include <sys/time_util.h>
#include <sys/ppu_thread.h>
#include <sys/memory.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "disk_benchmark.h"

/* ---------------------------------------------------------------
 * geometry
 * ---------------------------------------------------------------- */
#define SEQ_IO_SIZE         (64 * 1024)         /* 64KB per cellFsRead/Write call  */
#define SEQ_CHUNK_SIZE      (1 * 1024 * 1024)   /* 1MB DMA buffer allocation size  */
#define SEQ_FILE_SIZE       (32 * 1024 * 1024)  /* 32MB test file */

#define RND_BLOCK_SIZE      4096                /* 4K random blocks */
#define RND_OP_COUNT        512                 /* 512 random ops */

/* EMA alpha for result smoothing */
#define EMA_ALPHA           0.15f

/* ----------------------------------------------------------------
 * I/O buffer allocated via sys_memory_allocate (physically pinned
 * large pages, directly DMA-accessible by the hdd controller).
 *
 * a static BSS buffer lives in virtual memory. cellFsRead/Write has
 * to bounce every transfer through a kernel side DMA buffer then
 * memcpy to the user pointer. that cap sits around 5-10 MB/s.
 * sys_memory_allocate with SYS_MEMORY_PAGE_SIZE_1M gives physically
 * contiguous memory that the DMA engine can write to directly.
 * webMAN's FTP implementation uses the same pattern (sys_mem_allocate).
 * ---------------------------------------------------------------- */
static void    *io_buf        = NULL;  /* DMA-accessible, allocated at init */
static uint32_t io_chunk_size = 0;    /* actual allocation size */

/* OS-side I/O buffer memory container
 * cellFsSetIoBuffer / cellFsSetIoBufferFromDefaultContainer requires a
 * valid sys_memory_container_t. Passing SYS_MEMORY_CONTAINER_ID_INVALID
 * without first calling cellFsSetDefaultContainer silently falls through
 * to the kernel default (~16-64KB internal DMA chunks), which causes 16x
 * more per-chunk overhead per 1MB cellFsRead -> ~6 MB/s ceiling.
 * w/ a proper 4MB container and PAGE_SIZE_1MB the OS issues 1MB DMA ops,
 * eliminating that overhead and unlocking the drive's sequential speed */
static sys_memory_container_t g_io_container = SYS_MEMORY_CONTAINER_ID_INVALID;

static disk_bench_state_t g_state;

/* ----------------------------------------------------------------v
 * probe globals  declared here so all translation unit functions can reference them
 * ---------------------------------------------------------------- */
static disk_probe_t g_probes[DISK_PROBE_COUNT] = {
    {"R  16KB plain  "},   /*  0 */
    {"R  64KB plain  "},   /*  1 */
    {"R 256KB plain  "},   /*  2 - baseline for contention probes */
    {"R   1MB plain  "},   /*  3 */
    {"R   1MB io64K  "},   /*  4 */
    {"R   1MB io1MB  "},   /*  5 */
    {"R stread 1M/4M "},   /*  6 */
    {"W   1MB +fsync "},   /*  7 */
    {"W   1MB nofsync"},   /*  8 */
    {"W  64KB +fsync "},   /*  9 */
    {"R+aud   256K   "},   /* 10 - 256KB seq + audio stream (4KB seq) */
    {"R+tex   256K   "},   /* 11 - 256KB seq + texture stream (64KB rnd) */
    {"R+both  256K   "},   /* 12 - 256KB seq + audio + texture */
};
static volatile int     g_probe_running = 0;
static volatile int     g_probe_idx     = -1;
static uint64_t         g_probe_tbfreq  = 0;
static sys_ppu_thread_t g_probe_tid     = (sys_ppu_thread_t)0;
static int              g_probe_tid_ok  = 0;

static uint32_t lcg_seed = 0xACE1ACE1u;
static inline uint32_t lcg_rand(void)
{
    lcg_seed = lcg_seed * 1664525u + 1013904223u;
    return lcg_seed;
}

void disk_benchmark_init(void)
{
    int i;
    sys_addr_t addr = 0;

    sys_memory_allocate(SEQ_CHUNK_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &addr);
    if (addr) {
        io_buf        = (void *)(uintptr_t)addr;
        io_chunk_size = SEQ_CHUNK_SIZE;
    } else {
        addr = 0;
        sys_memory_allocate(SEQ_CHUNK_SIZE, SYS_MEMORY_PAGE_SIZE_64K, &addr);
        io_buf        = (void *)(uintptr_t)addr;
        io_chunk_size = addr ? SEQ_CHUNK_SIZE : 0;
    }

    if (sys_memory_container_create(&g_io_container, 16 * 1024 * 1024) == 0)
        cellFsSetDefaultContainer(g_io_container, 16 * 1024 * 1024);

    memset(&g_state, 0, sizeof(g_state));
    for (i = 0; i < DISK_BENCH_COUNT; i++)
        g_state.results[i].status = DISK_STATUS_IDLE;
}

static int prepare_test_file(void)
{
    CellFsErrno ret;

    /* AllocateFileAreaWithoutZeroFill requires the file not to exist so unlink first, ignore error ... the file may not exist yet */
    cellFsUnlink(DISK_TEST_FILE);

    ret = cellFsAllocateFileAreaWithoutZeroFill(DISK_TEST_FILE, SEQ_FILE_SIZE);
    if (ret == CELL_FS_SUCCEEDED) {
        g_state.file_ready = 1;
        return CELL_FS_SUCCEEDED;
    }

    {
        int fd, i;
        uint64_t nwritten;

        memset(io_buf, 0xA5u, SEQ_IO_SIZE);

        ret = cellFsOpen(DISK_TEST_FILE, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &fd, NULL, 0);
        if (ret != CELL_FS_SUCCEEDED) return ret;

        for (i = 0; i < (int)(SEQ_FILE_SIZE / SEQ_IO_SIZE); i++) {
            ret = cellFsWrite(fd, io_buf, SEQ_IO_SIZE, &nwritten);
            if (ret != CELL_FS_SUCCEEDED || nwritten != SEQ_IO_SIZE) {
                cellFsClose(fd);
                return (ret != CELL_FS_SUCCEEDED) ? ret : -1;
            }
        }

        ret = cellFsFsync(fd);
        cellFsClose(fd);
        if (ret != CELL_FS_SUCCEEDED) return ret;
    }

    g_state.file_ready = 1;
    return CELL_FS_SUCCEEDED;
}

static void update_result(int bench_id, float mbps)
{
    disk_result_t *r = &g_state.results[bench_id];
    if (r->status == DISK_STATUS_IDLE || r->mbps == 0.0f)
        r->mbps = mbps;
    else
        r->mbps = (1.0f - EMA_ALPHA) * r->mbps + EMA_ALPHA * mbps;
    r->status = DISK_STATUS_DONE;
    r->last_error = 0;
}

static void run_seq_read(uint64_t tb_freq)
{
    disk_result_t *r = &g_state.results[DISK_BENCH_SEQ_READ];
    int fd;
    int32_t ret;
    uint64_t nread;
    uint64_t t0, t1;
    int i;

    r->status = DISK_STATUS_RUNNING;

    if (!g_state.file_ready) {
        r->status = DISK_STATUS_PREPARE;
        ret = prepare_test_file();
        if (ret != CELL_FS_SUCCEEDED) {
            r->status = DISK_STATUS_ERROR;
            r->last_error = ret;
            return;
        }
    }

    ret = cellFsOpen(DISK_TEST_FILE, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) {
        r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
    }

    /* warmup pass: read the file once untimed to prime the drive's
     * internal read ahead cache.  w/o this, the first timed pass
     * catches the drive seeking cold and will be ~11 MB/s instead of
     * the sustained ~44 MB/s seen on subsequent passes */
    for (i = 0; i < (int)(SEQ_FILE_SIZE / SEQ_IO_SIZE); i++)
        cellFsRead(fd, io_buf, SEQ_IO_SIZE, &nread);

    /* seek back to start for the timed pass */
    {
        uint64_t pos;
        cellFsLseek(fd, 0, CELL_FS_SEEK_SET, &pos);
    }

    SYS_TIMEBASE_GET(t0);
    for (i = 0; i < (int)(SEQ_FILE_SIZE / SEQ_IO_SIZE); i++) {
        ret = cellFsRead(fd, io_buf, SEQ_IO_SIZE, &nread);
        if (ret != CELL_FS_SUCCEEDED || nread != SEQ_IO_SIZE) {
            cellFsClose(fd);
            r->status = DISK_STATUS_ERROR;
            r->last_error = (ret != CELL_FS_SUCCEEDED) ? ret : -1;
            return;
        }
    }
    SYS_TIMEBASE_GET(t1);

    cellFsClose(fd);

    {
        double secs = (double)(t1 - t0) / (double)tb_freq;
        float mbps = (float)((double)SEQ_FILE_SIZE / (secs * 1.0e6));
        update_result(DISK_BENCH_SEQ_READ, mbps);
    }
}

static void run_seq_write(uint64_t tb_freq)
{
    disk_result_t *r = &g_state.results[DISK_BENCH_SEQ_WRITE];
    int fd;
    int32_t ret;
    uint64_t nwritten;
    uint64_t t0, t1;
    int i;

    r->status = DISK_STATUS_RUNNING;

    /* fill buffer with incompressible pattern (SEQ_IO_SIZE bytes used per call) */
    {
        uint8_t *p = (uint8_t *)io_buf;
        for (i = 0; i < (int)SEQ_IO_SIZE; i++)
            p[i] = (uint8_t)(i ^ (i >> 8) ^ 0x5Au);
    }

    cellFsUnlink(DISK_TEST_FILE);
    if (cellFsAllocateFileAreaWithoutZeroFill(DISK_TEST_FILE, SEQ_FILE_SIZE)
        != CELL_FS_SUCCEEDED){  }

    ret = cellFsOpen(DISK_TEST_FILE, CELL_FS_O_WRONLY | CELL_FS_O_CREAT, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) {
        r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
    }

    SYS_TIMEBASE_GET(t0);
    for (i = 0; i < (int)(SEQ_FILE_SIZE / SEQ_IO_SIZE); i++) {
        ret = cellFsWrite(fd, io_buf, SEQ_IO_SIZE, &nwritten);
        if (ret != CELL_FS_SUCCEEDED || nwritten != SEQ_IO_SIZE) {
            cellFsClose(fd);
            r->status = DISK_STATUS_ERROR;
            r->last_error = (ret != CELL_FS_SUCCEEDED) ? ret : -1;
            return;
        }
    }
    /* fsync before stopping timer measures committed write speed */
    ret = cellFsFsync(fd);
    SYS_TIMEBASE_GET(t1);
    cellFsClose(fd);

    if (ret != CELL_FS_SUCCEEDED) {
        r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
    }

    g_state.file_ready = 1;

    {
        double secs = (double)(t1 - t0) / (double)tb_freq;
        float mbps = (float)((double)SEQ_FILE_SIZE / (secs * 1.0e6));
        update_result(DISK_BENCH_SEQ_WRITE, mbps);
    }
}

/* ----------------------------------------------------------------
 * run_rnd4k_read
 * ---------------------------------------------------------------- */
static void run_rnd4k_read(uint64_t tb_freq)
{
    disk_result_t *r = &g_state.results[DISK_BENCH_RND4K_READ];
    int fd;
    int32_t ret;
    uint64_t nread;
    uint64_t t0, t1;
    int i;

    r->status = DISK_STATUS_RUNNING;

    if (!g_state.file_ready) {
        r->status = DISK_STATUS_PREPARE;
        ret = prepare_test_file();
        if (ret != CELL_FS_SUCCEEDED) {
            r->status = DISK_STATUS_ERROR;
            r->last_error = ret;
            return;
        }
    }

    ret = cellFsOpen(DISK_TEST_FILE, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) {
        r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
    }

    const uint32_t max_block = (SEQ_FILE_SIZE / RND_BLOCK_SIZE) - 1;

    lcg_seed = 0xDEADC0DEu;  /* deterministic sequence for reproducibility */

    SYS_TIMEBASE_GET(t0);
    for (i = 0; i < RND_OP_COUNT; i++) {
        uint64_t offset = (uint64_t)(lcg_rand() % max_block) * RND_BLOCK_SIZE;
        ret = cellFsReadWithOffset(fd, offset, io_buf, RND_BLOCK_SIZE, &nread);
        if (ret != CELL_FS_SUCCEEDED) {
            cellFsClose(fd);
            r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
        }
    }
    SYS_TIMEBASE_GET(t1);

    cellFsClose(fd);

    {
        double secs = (double)(t1 - t0) / (double)tb_freq;
        /* report in MB/s: total bytes / time */
        float mbps = (float)((double)RND_OP_COUNT * RND_BLOCK_SIZE
                             / (secs * 1.0e6));
        update_result(DISK_BENCH_RND4K_READ, mbps);
    }
}

/* ----------------------------------------------------------------
 * run_rnd4k_write
 * ---------------------------------------------------------------- */
static void run_rnd4k_write(uint64_t tb_freq)
{
    disk_result_t *r = &g_state.results[DISK_BENCH_RND4K_WRITE];
    int fd;
    int32_t ret;
    uint64_t nwritten;
    uint64_t t0, t1;
    int i;

    r->status = DISK_STATUS_RUNNING;

    if (!g_state.file_ready) {
        r->status = DISK_STATUS_PREPARE;
        ret = prepare_test_file();
        if (ret != CELL_FS_SUCCEEDED) {
            r->status = DISK_STATUS_ERROR;
            r->last_error = ret;
            return;
        }
    }

    ret = cellFsOpen(DISK_TEST_FILE, CELL_FS_O_WRONLY, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) {
        r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
    }

    /* fill block with incompressible data */
    {
        uint8_t *p = (uint8_t *)io_buf;
        for (i = 0; i < RND_BLOCK_SIZE; i++)
            p[i] = (uint8_t)(i ^ 0xA5u ^ (i >> 4));
    }

    const uint32_t max_block = (SEQ_FILE_SIZE / RND_BLOCK_SIZE) - 1;
    lcg_seed = 0xDEADC0DEu;

    SYS_TIMEBASE_GET(t0);
    for (i = 0; i < RND_OP_COUNT; i++) {
        uint64_t offset = (uint64_t)(lcg_rand() % max_block) * RND_BLOCK_SIZE;
        ret = cellFsWriteWithOffset(fd, offset, io_buf, RND_BLOCK_SIZE, &nwritten);
        if (ret != CELL_FS_SUCCEEDED) {
            cellFsClose(fd);
            r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
        }
    }
    /* fsync: committed write speed */
    ret = cellFsFsync(fd);
    SYS_TIMEBASE_GET(t1);
    cellFsClose(fd);

    if (ret != CELL_FS_SUCCEEDED) {
        r->status = DISK_STATUS_ERROR; r->last_error = ret; return;
    }

    {
        double secs = (double)(t1 - t0) / (double)tb_freq;
        float mbps = (float)((double)RND_OP_COUNT * RND_BLOCK_SIZE / (secs * 1.0e6));
        update_result(DISK_BENCH_RND4K_WRITE, mbps);
    }
}

static volatile int     disk_thread_running = 0;
static sys_ppu_thread_t disk_thread_id      = (sys_ppu_thread_t)0;
static int              disk_thread_started = 0;  /* whether thread_id is valid */
static int              disk_pending_bench;
static uint64_t         disk_pending_tb_freq;

static void disk_bench_thread(uint64_t arg)
{
    (void)arg;

    switch (disk_pending_bench) {
    case DISK_BENCH_SEQ_READ:    run_seq_read(disk_pending_tb_freq);   break;
    case DISK_BENCH_SEQ_WRITE:   run_seq_write(disk_pending_tb_freq);  break;
    case DISK_BENCH_RND4K_READ:  run_rnd4k_read(disk_pending_tb_freq); break;
    case DISK_BENCH_RND4K_WRITE: run_rnd4k_write(disk_pending_tb_freq);break;
    }

    /* barrier: result writes must be globally visible before we clear the flag */
    __sync_synchronize();
    disk_thread_running = 0;
    sys_ppu_thread_exit(0);
}

void disk_trigger_bench(int bench_id, uint64_t tb_freq)
{
    if (disk_thread_running) return;  /* bench already in progress */

    /* join the previous thread to free its resources.
     * it has already exited (disk_thread_running == 0), so this returns
     * immediately with no blocking. */
    if (disk_thread_started) {
        uint64_t exit_val;
        sys_ppu_thread_join(disk_thread_id, &exit_val);
        disk_thread_started = 0;
    }

    disk_pending_bench   = bench_id;
    disk_pending_tb_freq = tb_freq;

    /* barrier: pending_* writes visible to new thread before it starts */
    __sync_synchronize();
    disk_thread_running = 1;

    sys_ppu_thread_create(&disk_thread_id, disk_bench_thread, 0, 1100, 64 * 1024, SYS_PPU_THREAD_CREATE_JOINABLE, "disk_bench");
    disk_thread_started = 1;
}

int disk_is_running(void)
{
    return disk_thread_running;
}

const disk_bench_state_t *disk_get_state(void)
{
    return &g_state;
}

#define PROBE_SZ  DISK_PROBE_FILE_SZ    /* 32MB */

static int probe_make_file(void)
{
    CellFsErrno ret;
    int fd;
    uint64_t nw;
    uint32_t done = 0;

    cellFsUnlink(DISK_PROBE_FILE);
    ret = cellFsAllocateFileAreaWithoutZeroFill(DISK_PROBE_FILE, PROBE_SZ);
    if (ret == CELL_FS_SUCCEEDED) return 0;

    /* fallback: write loop */
    ret = cellFsOpen(DISK_PROBE_FILE, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) return -1;
    while (done < PROBE_SZ) {
        uint32_t want = io_chunk_size < (PROBE_SZ - done) ? io_chunk_size : (PROBE_SZ - done);
        ret = cellFsWrite(fd, io_buf, want, &nw);
        if (ret != CELL_FS_SUCCEEDED || nw == 0) { cellFsClose(fd); return -1; }
        done += (uint32_t)nw;
    }
    cellFsFsync(fd);
    cellFsClose(fd);
    return 0;
}

static float probe_read(uint32_t chunk_sz, int io_mode, uint64_t tb_freq)
{
    int fd;
    int32_t ret;
    uint64_t nread;
    uint64_t t0, t1;
    uint32_t done = 0;

    ret = cellFsOpen(DISK_PROBE_FILE, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) return -1.0f;

    if (io_mode == 1 && g_io_container != SYS_MEMORY_CONTAINER_ID_INVALID)
        cellFsSetIoBufferFromDefaultContainer(fd, 4*1024*1024, CELL_FS_IO_BUFFER_PAGE_SIZE_64KB);
    else if (io_mode == 2 && g_io_container != SYS_MEMORY_CONTAINER_ID_INVALID)
        cellFsSetIoBufferFromDefaultContainer(fd, 4*1024*1024, CELL_FS_IO_BUFFER_PAGE_SIZE_1MB);

    SYS_TIMEBASE_GET(t0);
    while (done < PROBE_SZ) {
        uint32_t want = chunk_sz < (PROBE_SZ - done) ? chunk_sz : (PROBE_SZ - done);
        ret = cellFsRead(fd, io_buf, want, &nread);
        if (ret != CELL_FS_SUCCEEDED || nread == 0) break;
        done += (uint32_t)nread;
    }
    SYS_TIMEBASE_GET(t1);
    cellFsClose(fd);

    if (done < PROBE_SZ) return -1.0f;
    return (float)((double)PROBE_SZ / ((double)(t1 - t0) / (double)tb_freq * 1.0e6));
}

static float probe_stread(uint64_t tb_freq)
{
    int fd;
    CellFsRingBuffer rb;
    uint64_t t0, t1, rsize;
    int32_t ret;
    uint32_t done = 0;

    ret = cellFsOpen(DISK_PROBE_FILE, CELL_FS_O_RDONLY, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) return -1.0f;

    rb.ringbuf_size  = 4 * 1024 * 1024;   /* 4MB ring                  */
    rb.block_size    = 1 * 1024 * 1024;   /* 1MB per async DMA request */
    rb.transfer_rate = 0;                  /* unlimited                  */
    rb.copy          = CELL_FS_ST_COPY;   /* copy into our io_buf       */

    if (cellFsStReadInit(fd, &rb) != CELL_FS_SUCCEEDED) {
        cellFsClose(fd); return -1.0f;
    }
    if (cellFsStReadStart(fd, 0, PROBE_SZ) != CELL_FS_SUCCEEDED) {
        cellFsStReadFinish(fd); cellFsClose(fd); return -1.0f;
    }

    SYS_TIMEBASE_GET(t0);
    while (done < PROBE_SZ) {
        rsize = 0;
        ret = cellFsStRead(fd, (char *)io_buf, io_chunk_size, &rsize);
        if (ret != CELL_FS_SUCCEEDED || rsize == 0) break;
        done += (uint32_t)rsize;
    }
    SYS_TIMEBASE_GET(t1);

    cellFsStReadStop(fd);
    cellFsStReadFinish(fd);
    cellFsClose(fd);

    if (done < PROBE_SZ) return -1.0f;
    return (float)((double)PROBE_SZ / ((double)(t1 - t0) / (double)tb_freq * 1.0e6));
}

static float probe_write(uint32_t chunk_sz, int do_fsync, uint64_t tb_freq)
{
    int fd;
    int32_t ret;
    uint64_t nw;
    uint64_t t0, t1;
    uint32_t done = 0, i;
    uint8_t *p = (uint8_t *)io_buf;

    /* incompressible fill up to chunk_sz (stays within 1MB io_buf) */
    for (i = 0; i < chunk_sz && i < io_chunk_size; i++)
        p[i] = (uint8_t)(i ^ (i >> 8) ^ 0x5Au);

    cellFsUnlink(DISK_PROBE_FILE);
    cellFsAllocateFileAreaWithoutZeroFill(DISK_PROBE_FILE, PROBE_SZ);

    ret = cellFsOpen(DISK_PROBE_FILE,
                     CELL_FS_O_WRONLY | CELL_FS_O_CREAT, &fd, NULL, 0);
    if (ret != CELL_FS_SUCCEEDED) return -1.0f;

    SYS_TIMEBASE_GET(t0);
    while (done < PROBE_SZ) {
        uint32_t want = chunk_sz < (PROBE_SZ - done) ? chunk_sz : (PROBE_SZ - done);
        ret = cellFsWrite(fd, io_buf, want, &nw);
        if (ret != CELL_FS_SUCCEEDED || nw == 0) break;
        done += (uint32_t)nw;
    }
    if (do_fsync) cellFsFsync(fd);
    SYS_TIMEBASE_GET(t1);
    cellFsClose(fd);

    if (done < PROBE_SZ) return -1.0f;
    return (float)((double)PROBE_SZ / ((double)(t1 - t0) / (double)tb_freq * 1.0e6));
}

static volatile int     g_contend_stop;
static sys_ppu_thread_t g_contend_tids[2];
static uint8_t          g_audio_buf[4  * 1024];   /* BSS, audio sim  */
static uint8_t          g_tex_buf  [64 * 1024];   /* BSS, texture sim */

static void audio_contend_fn(uint64_t arg)
{
    int fd;
    uint64_t nread;
    uint64_t offset = 0;
    (void)arg;

    if (cellFsOpen(DISK_PROBE_FILE, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        sys_ppu_thread_exit(0);

    while (!g_contend_stop) {
        if (offset + sizeof(g_audio_buf) > PROBE_SZ) offset = 0;
        cellFsReadWithOffset(fd, offset, g_audio_buf, sizeof(g_audio_buf), &nread);
        offset += sizeof(g_audio_buf);
    }

    cellFsClose(fd);
    sys_ppu_thread_exit(0);
}

static void texture_contend_fn(uint64_t arg)
{
    int fd;
    uint64_t nread;
    uint32_t seed = 0xFEEDF00Du;
    uint32_t max_blk = (PROBE_SZ / sizeof(g_tex_buf)) - 1;
    (void)arg;

    if (cellFsOpen(DISK_PROBE_FILE, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
        sys_ppu_thread_exit(0);

    while (!g_contend_stop) {
        uint64_t offset;
        seed = seed * 1664525u + 1013904223u;
        offset = (uint64_t)(seed % max_blk) * sizeof(g_tex_buf);
        cellFsReadWithOffset(fd, offset, g_tex_buf, sizeof(g_tex_buf), &nread);
    }

    cellFsClose(fd);
    sys_ppu_thread_exit(0);
}

static float probe_contend(int mode, uint64_t tb_freq)
{
    int n = 0;
    float mbps;

    g_contend_stop = 0;
    __sync_synchronize();

    if (mode & 1)
        sys_ppu_thread_create(&g_contend_tids[n++], audio_contend_fn, 0, 1150, 64 * 1024, SYS_PPU_THREAD_CREATE_JOINABLE, "c_aud");
    if (mode & 2)
        sys_ppu_thread_create(&g_contend_tids[n++], texture_contend_fn, 0, 1150, 64 * 1024, SYS_PPU_THREAD_CREATE_JOINABLE, "c_tex");

    /* time the sequential read while contention is running */
    mbps = probe_read(256 * 1024, 0, tb_freq);

    /* signal threads to stop and wait for clean exit */
    g_contend_stop = 1;
    __sync_synchronize();
    while (n > 0) {
        uint64_t ev;
        sys_ppu_thread_join(g_contend_tids[--n], &ev);
    }

    return mbps;
}

#define RUN_PROBE(idx, expr) do {                               \
    float _r;                                                   \
    g_probe_idx = (idx);                                        \
    g_probes[idx].state = PROBE_ST_RUNNING;                     \
    __sync_synchronize();                                       \
    _r = (expr);                                                \
    g_probes[idx].mbps  = _r;                                   \
    g_probes[idx].state = (_r >= 0.0f) ? PROBE_ST_DONE          \
                                       : PROBE_ST_ERROR;        \
    __sync_synchronize();                                       \
} while(0)

static void disk_probe_thread_fn(uint64_t arg)
{
    uint64_t tb = g_probe_tbfreq;
    int i;
    (void)arg;

    for (i = 0; i < DISK_PROBE_COUNT; i++) {
        g_probes[i].mbps  = 0.0f;
        g_probes[i].state = PROBE_ST_PENDING;
    }
    __sync_synchronize();

    /* create probe file... write it fresh so read probes see a warm file */
    g_probe_idx = 0;
    g_probes[0].state = PROBE_ST_RUNNING;
    if (probe_make_file() != 0) {
        for (i = 0; i < DISK_PROBE_COUNT; i++) {
            g_probes[i].state = PROBE_ST_ERROR;
            g_probes[i].mbps  = -1.0f;
        }
        goto done;
    }
    g_probes[0].state = PROBE_ST_PENDING;  /* reset! will be re-set by RUN_PROBE */

    RUN_PROBE(0, probe_read(  16 * 1024, 0, tb));   /* R  16KB plain  */
    RUN_PROBE(1, probe_read(  64 * 1024, 0, tb));   /* R  64KB plain  */
    RUN_PROBE(2, probe_read( 256 * 1024, 0, tb));   /* R 256KB plain  */
    RUN_PROBE(3, probe_read(1024 * 1024, 0, tb));   /* R   1MB plain  */
    RUN_PROBE(4, probe_read(1024 * 1024, 1, tb));   /* R   1MB io64K  */
    RUN_PROBE(5, probe_read(1024 * 1024, 2, tb));   /* R   1MB io1MB  */
    RUN_PROBE(6, probe_stread(tb));                  /* R stread 1M/4M */

    RUN_PROBE(7, probe_write(1024 * 1024, 1, tb));  /* W   1MB +fsync */
    RUN_PROBE(8, probe_write(1024 * 1024, 0, tb));  /* W   1MB nofsync*/
    RUN_PROBE(9, probe_write(  64 * 1024, 1, tb));  /* W  64KB +fsync */

    probe_make_file();
    RUN_PROBE(10, probe_contend(1, tb));  /* R+aud   256K: seq + audio */
    RUN_PROBE(11, probe_contend(2, tb));  /* R+tex   256K: seq + texture */
    RUN_PROBE(12, probe_contend(3, tb));  /* R+both  256K: seq + audio + texture */

done:
    __sync_synchronize();
    g_probe_running = 0;
    g_probe_idx     = -1;
    sys_ppu_thread_exit(0);
}

void disk_run_probes(uint64_t tb_freq)
{
    int i;
    if (g_probe_running || disk_thread_running) return;

    if (g_probe_tid_ok) {
        uint64_t ev;
        sys_ppu_thread_join(g_probe_tid, &ev);
        g_probe_tid_ok = 0;
    }

    for (i = 0; i < DISK_PROBE_COUNT; i++) {
        g_probes[i].mbps  = 0.0f;
        g_probes[i].state = PROBE_ST_PENDING;
    }

    g_probe_tbfreq = tb_freq;
    __sync_synchronize();
    g_probe_running = 1;

    sys_ppu_thread_create(&g_probe_tid, disk_probe_thread_fn, 0, 1100, 64 * 1024, SYS_PPU_THREAD_CREATE_JOINABLE, "disk_probe");
    g_probe_tid_ok = 1;
}

int disk_probes_running(void)         { return g_probe_running; }
int disk_probe_current(void)          { return g_probe_idx;     }
const disk_probe_t *disk_get_probes(void) { return g_probes;    }