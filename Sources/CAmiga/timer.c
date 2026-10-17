/* timer.device, native, on the host's monotonic clock.
 * Pending TR_ADDREQUESTs are scheduler timers that reply the request.
 */
#include "os.h"
#include "sched.h"
#include <string.h>
#include <sys/time.h>

#define IO_DEVICE 20
#define IO_UNIT 24
#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define TR_SECS 32
#define TR_MICRO 36
#define IOF_QUICK 1

enum { UNIT_MICROHZ, UNIT_VBLANK, UNIT_ECLOCK, UNIT_WAITUNTIL, UNIT_WAITECLOCK };
enum { TR_ADDREQUEST = 9, TR_GETSYSTIME = 10, TR_SETSYSTIME = 11 };
#define IOERR_NOCMD (-3)
#define IOERR_ABORTED (-2)
#define ECLOCK_HZ 709379u                     /* PAL E-clock, as programs expect */

static uint32_t TimerBase;
static uint32_t units[5];
static int64_t systime_offset_us;             /* TR_SETSYSTIME */
static uint64_t boot_us;

/* system time: seconds since 1 Jan 1978, local time like AmigaOS */
static uint64_t systime_us(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t t = tv.tv_sec;
    struct tm lt;
    localtime_r(&t, &lt);
    int64_t secs = (int64_t)tv.tv_sec + lt.tm_gmtoff - 252460800;
    return (uint64_t)(secs * 1000000 + tv.tv_usec + systime_offset_us);
}

uint32_t timer_systime_secs(void) { return (uint32_t)(systime_us() / 1000000u); }

static int unit_of(uint32_t ior) {
    uint32_t u = rd32(ior + IO_UNIT);
    for (int i = 0; i < 5; i++) if (units[i] == u) return i;
    return UNIT_MICROHZ;
}

static void complete(uint32_t ior) {
    if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) exec_reply_msg(ior);
}

/* ---- device vectors ------------------------------------------------------ */

static uint32_t t_Open(M68K *c) {
    uint32_t ior = c->a[1], unit = c->d[0];
    if (unit > UNIT_WAITECLOCK) { wr8(ior + IO_ERROR, (uint8_t)-1); return 0; }
    wr32(ior + IO_UNIT, units[unit]);
    wr8(ior + IO_ERROR, 0);
    return 0;
}

static uint32_t t_Close(M68K *c) { (void)c; return 0; }
static uint32_t t_Expunge(M68K *c) { (void)c; return 0; }

static uint32_t t_BeginIO(M68K *c) {
    uint32_t ior = c->a[1];
    wr8(ior + IO_ERROR, 0);
    switch (rd16(ior + IO_COMMAND)) {
    case TR_ADDREQUEST: {
        uint64_t secs = rd32(ior + TR_SECS), micro = rd32(ior + TR_MICRO);
        int unit = unit_of(ior);
        uint64_t now = now_us(), when;
        if (unit == UNIT_WAITUNTIL) {
            uint64_t target = secs * 1000000u + micro, sys = systime_us();
            when = target > sys ? now + (target - sys) : now;
        } else if (unit == UNIT_WAITECLOCK) {
            uint64_t ticks = secs << 32 | micro;           /* EClockVal: ev_hi, ev_lo */
            uint64_t target = ticks * 1000000u / ECLOCK_HZ, cur = now - boot_us;
            when = target > cur ? now + (target - cur) : now;
        } else {
            when = now + secs * 1000000u + micro;
        }
        wr8(ior + IO_FLAGS, rd8(ior + IO_FLAGS) & ~IOF_QUICK);   /* replied later */
        sched_timer_add(when, TIMER_REPLY, ior, 0);
        return 0;
    }
    case TR_GETSYSTIME: {
        uint64_t t = systime_us();
        wr32(ior + TR_SECS, (uint32_t)(t / 1000000u));
        wr32(ior + TR_MICRO, (uint32_t)(t % 1000000u));
        complete(ior);
        return 0;
    }
    case TR_SETSYSTIME: {
        int64_t want = (int64_t)rd32(ior + TR_SECS) * 1000000 + rd32(ior + TR_MICRO);
        systime_offset_us += want - (int64_t)systime_us();
        complete(ior);
        return 0;
    }
    default:
        wr8(ior + IO_ERROR, (uint8_t)IOERR_NOCMD);
        complete(ior);
        return 0;
    }
}

static uint32_t t_AbortIO(M68K *c) {
    uint32_t ior = c->a[1];
    if (sched_timer_remove(TIMER_REPLY, ior)) {
        wr8(ior + IO_ERROR, (uint8_t)IOERR_ABORTED);
        exec_reply_msg(ior);
    }
    return 0;
}

/* ---- library functions (timer_lib.fd) ------------------------------------ */

static void tv_norm(uint32_t tv, int64_t us) {
    if (us < 0) us = 0;
    wr32(tv, (uint32_t)(us / 1000000));
    wr32(tv + 4, (uint32_t)(us % 1000000));
}
static int64_t tv_get(uint32_t tv) { return (int64_t)rd32(tv) * 1000000 + rd32(tv + 4); }

static uint32_t t_AddTime(M68K *c) { tv_norm(c->a[0], tv_get(c->a[0]) + tv_get(c->a[1])); return 0; }
static uint32_t t_SubTime(M68K *c) { tv_norm(c->a[0], tv_get(c->a[0]) - tv_get(c->a[1])); return 0; }
static uint32_t t_CmpTime(M68K *c) {
    int64_t d = tv_get(c->a[0]), s = tv_get(c->a[1]);
    return d == s ? 0 : d > s ? 0xFFFFFFFF : 1;      /* -1: dest has more time */
}
static uint32_t t_GetSysTime(M68K *c) {
    uint64_t t = systime_us();
    wr32(c->a[0], (uint32_t)(t / 1000000u));
    wr32(c->a[0] + 4, (uint32_t)(t % 1000000u));
    return 0;
}
static uint32_t t_ReadEClock(M68K *c) {
    uint64_t ticks = (now_us() - boot_us) * ECLOCK_HZ / 1000000u;
    wr32(c->a[0], (uint32_t)(ticks >> 32));
    wr32(c->a[0] + 4, (uint32_t)ticks);
    return ECLOCK_HZ;
}

static const NativeEntry timer_natives[] = {
    { "Open", t_Open }, { "Close", t_Close }, { "Expunge", t_Expunge },
    { "BeginIO", t_BeginIO }, { "AbortIO", t_AbortIO },
    { "AddTime", t_AddTime }, { "SubTime", t_SubTime }, { "CmpTime", t_CmpTime },
    { "GetSysTime", t_GetSysTime }, { "ReadEClock", t_ReadEClock },
    { 0, 0 }
};

void timer_init(void) {
    boot_us = now_us();
    TimerBase = lib_create("timer.device", "timer", LIB_SIZE, timer_natives, 40, 1, "timer 40.1 (AmigaMac)");
    wr8(TimerBase + LN_TYPE, NT_DEVICE);
    for (int i = 0; i < 5; i++) units[i] = exec_allocmem(38, MEMF_PUBLIC | MEMF_CLEAR);   /* struct Unit */
    list_enqueue(SysBase + EB_DEVICELIST, TimerBase);
}
