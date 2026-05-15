#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <sys/syscall.h>
#include <sys/time_util.h>

#include "cellmark.h"
#include "sysmon.h"

extern int sys_game_get_temperature(uint8_t tzone, uint32_t *out);

#define SYS_SM_GET_FAN_POLICY     409 // works on DECR? use raw syscall number and try

#define TZONE_CELL  0x00
#define TZONE_RSX   0x01
#define TZONE_SB    0x14

static int sm_get_temperature(uint8_t tzone, uint32_t *out_raw)
{
    uint32_t local = 0;
    int ret = sys_game_get_temperature(tzone, &local);
    if (ret != 0) return ret;
    *out_raw = local;
    return 0;
}

static int sm_get_fan_policy(uint8_t id, uint8_t *st, uint8_t *mode, uint8_t *speed, uint8_t *duty)
{
    system_call_5(SYS_SM_GET_FAN_POLICY, (uint64_t)id, (uint64_t)(uintptr_t)st, (uint64_t)(uintptr_t)mode, (uint64_t)(uintptr_t)speed, (uint64_t)(uintptr_t)duty);
    return (int)p1;
}

const char *sysmon_get_status_string(void)
{
    static char     buf[96];
    static uint64_t last_tb = 0;
    static int      have_sb = 0;     // SB sensor present? sticky once seen. some boards dont have em
    static int      probe_logged = 0;
    uint64_t        now_tb;
    double          since_s;

    SYS_TIMEBASE_GET(now_tb);
    since_s = (last_tb == 0) ? 999.0 : (double)(now_tb - last_tb) / (double)tb_frequency;

    if (since_s >= 1.0) {
        uint32_t cell_raw = 0, rsx_raw = 0, sb_raw = 0;
        int      cell_ok = (sm_get_temperature(TZONE_CELL, &cell_raw) == 0);
        int      rsx_ok  = (sm_get_temperature(TZONE_RSX,  &rsx_raw)  == 0);
        int      sb_ok   = (sm_get_temperature(TZONE_SB,   &sb_raw)   == 0);
        int      cell_c = 0, rsx_c = 0, sb_c = 0;
        uint8_t  st = 0, mode = 0, speed = 0, duty = 0;
        int      fan_ok = (sm_get_fan_policy(0, &st, &mode, &speed, &duty) == 0);
        int      fan_pct = 0;
        int      pos = 0;

        if (sb_ok)   have_sb = 1;
        if (cell_ok) cell_c  = (int)((cell_raw >> 24) & 0xFF);
        if (rsx_ok)  rsx_c   = (int)((rsx_raw  >> 24) & 0xFF);
        if (sb_ok)   sb_c    = (int)((sb_raw   >> 24) & 0xFF);
        if (fan_ok)  fan_pct = (int)(((uint32_t)speed * 100u + 127u) / 255u);

        if (!probe_logged) {
            probe_logged = 1;
            if (!cell_ok && !rsx_ok && !sb_ok && !fan_ok) {
                printf("[sysmon] thermal/fan syscalls unavailable to this process " "(DECR detected!); header will omit the field\n");
            }
        }

        buf[0] = '\0';
        if (cell_ok)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "CELL: %dC", cell_c);
        if (rsx_ok)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%sRSX: %dC", pos ? " " : "", rsx_c);
        if (have_sb)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%sSB: %dC", pos ? " " : "", sb_c);
        if (fan_ok)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%sFan: %d%%", pos ? " " : "", fan_pct);

        last_tb = now_tb;
    }

    return buf;
}