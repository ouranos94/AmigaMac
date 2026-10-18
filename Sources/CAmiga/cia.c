/* The two CIAs' timers and interrupts, for programs that drive them (music
 * players time their notes with them), and ciaa.resource / ciab.resource,
 * written from the 8520 description and the cia.resource autodocs.
 *
 * Registers: CIA-A at odd addresses $BFE001-$BFEF01, CIA-B at even ones
 * $BFD000-$BFDF00, register = (address >> 8) & 15. Timers A and B count down
 * at the E clock (709379 Hz on a PAL machine), continuously or once. When one
 * runs out and its interrupt is enabled, the handler a program set with
 * AddICRVector runs, from a high priority task, as an interrupt would.
 */
#include "os.h"
#include "sched.h"
#include "audio.h"
#include <stdlib.h>
#include <string.h>

#define ECLOCK 709379.0
#define SIGF_CIA (1u << 16)
#define CRF_START 0x01
#define CRF_RUNMODE 0x08                /* one-shot */
#define CRF_LOAD 0x10

typedef struct {
    uint16_t latch[2], count[2];        /* timers A and B */
    int running[2];
    uint64_t start_us[2];               /* when count[] was the value */
    uint8_t cr[2], mask, data, pr[2], ddr[2];
    uint32_t vec[5];                    /* Interrupts from AddICRVector, per ICR bit */
    uint32_t base;                      /* the resource */
} CIA;

static CIA cias[2];
static uint32_t cia_task;

/* exec's interrupt servers and vectors, run from the same task: the vertical
   blank (INTB_VERTB 5) fifty times a second, Paula's audio interrupts (7-10) */
static uint32_t servers[16][8];          /* AddIntServer, by priority order of adding */
static uint32_t vectors[16];             /* SetIntVector */
static uint64_t next_vblank;
#define INTB_VERTB 5
#define INTB_COPER 4

static uint32_t now_count(CIA *k, int t) {
    if (!k->running[t]) return k->count[t];
    double ticks = (double)(now_us() - k->start_us[t]) * ECLOCK / 1e6;
    uint32_t period = k->latch[t] ? k->latch[t] : 65536;
    if (ticks < k->count[t]) return (uint32_t)(k->count[t] - (uint32_t)ticks);
    double after = ticks - k->count[t];
    return period - 1 - (uint32_t)((uint64_t)after % period);
}

static uint64_t due_us(CIA *k, int t) {
    return k->start_us[t] + (uint64_t)((double)(k->count[t] ? k->count[t] : 65536) * 1e6 / ECLOCK);
}

/* wake the interrupt task for the next timer that would interrupt */
static void schedule(void) {
    if (!cia_task) return;
    uint64_t next = 0;
    int vb = servers[INTB_VERTB][0] != 0 || chip_active();
    for (int i = 7; i <= 10 && !vb; i++) vb = vectors[i] != 0;
    if (vb) {                                                   /* a vertical blank every 20 ms */
        if (!next_vblank) next_vblank = now_us() + 20000;
        next = next_vblank;
    }
    for (int i = 0; i < 2; i++)
        for (int t = 0; t < 2; t++)
            if (cias[i].running[t] && (cias[i].mask & (1 << t))) {
                uint64_t d = due_us(&cias[i], t);
                if (!next || d < next) next = d;
            }
    if (next) sched_timer_add(next, TIMER_SIGNAL, cia_task, SIGF_CIA);
}

static void start_timer(CIA *k, int t) {
    k->running[t] = 1;
    k->start_us[t] = now_us();
    schedule();
}

uint32_t cia_read(int which, int reg) {
    CIA *k = &cias[which & 1];
    switch (reg) {
    case 0: case 1: return k->pr[reg] | (uint8_t)~k->ddr[reg];
    case 2: case 3: return k->ddr[reg - 2];
    case 4: return now_count(k, 0) & 0xFF;
    case 5: return now_count(k, 0) >> 8;
    case 6: return now_count(k, 1) & 0xFF;
    case 7: return now_count(k, 1) >> 8;
    case 13: {                                                  /* ICR: reading clears it */
        uint8_t r = (uint8_t)(k->data | ((k->data & k->mask) ? 0x80 : 0));
        k->data = 0;
        return r;
    }
    case 14: return k->cr[0];
    case 15: return k->cr[1];
    }
    return 0xFF;
}

void cia_write(int which, int reg, uint8_t v) {
    CIA *k = &cias[which & 1];
    int t = (reg == 6 || reg == 7 || reg == 15) ? 1 : 0;
    switch (reg) {
    case 0: case 1: k->pr[reg] = v; break;
    case 2: case 3: k->ddr[reg - 2] = v; break;
    case 4: case 6: k->latch[t] = (uint16_t)((k->latch[t] & 0xFF00) | v); break;
    case 5: case 7:
        k->latch[t] = (uint16_t)((k->latch[t] & 0x00FF) | v << 8);
        if (!k->running[t]) k->count[t] = k->latch[t];
        if (k->cr[t] & CRF_RUNMODE) { k->count[t] = k->latch[t]; k->cr[t] |= CRF_START; start_timer(k, t); }   /* one-shot: starts */
        break;
    case 13:                                                    /* ICR: bit 7 sets the mask bits, else clears */
        if (v & 0x80) k->mask |= v & 0x1F; else k->mask &= (uint8_t)~v;
        schedule();
        break;
    case 14: case 15: {
        int was = k->running[t];
        if (was) { k->count[t] = (uint16_t)now_count(k, t); k->running[t] = 0; }
        k->cr[t] = v & (uint8_t)~CRF_LOAD;
        if (v & CRF_LOAD) k->count[t] = k->latch[t];
        if (v & CRF_START) start_timer(k, t);
        break;
    }
    }
}

/* a handler, as exec calls it: A0 = the custom chips ($DFF000), A1 = is_Data,
   A5 = is_Code, A6 = SysBase, D1 = INTENAR & INTREQR */
static void call_vector(M68K *c, uint32_t is) {
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
    c->a[0] = 0xDFF000;
    c->d[1] = paula_intena() & paula_intreq();
    c->a[1] = rd32(is + 14);
    c->a[5] = rd32(is + 18);
    c->a[6] = SysBase;
    cpu_call(c, rd32(is + 18));
    memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
}

static void deliver(M68K *c, CIA *k) {
    for (int b = 0; b < 5; b++)
        if ((k->data & k->mask & (1 << b)) && k->vec[b]) {
            k->data &= (uint8_t)~(1 << b);
            call_vector(c, k->vec[b]);
        }
}

/* the interrupt task: timers that ran out, their handlers */
static void cia_main(void *arg) {
    (void)arg;
    NTask *me = sched_current();
    M68K *c = &me->cpu;
    if (!c->a[7]) c->a[7] = rd32(me->task + TC_SPUPPER);
    for (;;) {
        exec_wait(SIGF_CIA);
        uint64_t now = now_us();
        if (next_vblank && now + 500 >= next_vblank) {
            next_vblank = now + 20000;
            paula_raise(0x20);                                  /* VERTB */
            chip_frame();
            for (int k = 0; k < 8 && servers[INTB_VERTB][k]; k++) call_vector(c, servers[INTB_VERTB][k]);
            /* the copper's interrupt (COPER), if its list raised it */
            if ((paula_intreq() & 0x10) && (paula_intena() & 0x4010) == 0x4010 && servers[INTB_COPER][0]) {
                for (int k = 0; k < 8 && servers[INTB_COPER][k]; k++) call_vector(c, servers[INTB_COPER][k]);
                paula_write(0x9C, 0x0010);
            }
        }
        /* audio interrupts Paula raised, if enabled (INTEN and the channel's bit) */
        uint32_t req = paula_intreq(), ena = paula_intena();
        for (int i = 7; i <= 10; i++)
            if (vectors[i] && (ena & 0x4000) && (ena & req & (1u << i))) {
                call_vector(c, vectors[i]);
            }
        for (int i = 0; i < 2; i++) {
            CIA *k = &cias[i];
            for (int t = 0; t < 2; t++) {
                if (!k->running[t]) continue;
                uint64_t due = due_us(k, t);
                if (due > now + 500) continue;                 /* not yet */
                k->data |= (uint8_t)(1 << t);
                if (k->cr[t] & CRF_RUNMODE) {                   /* one-shot: stops, reloaded */
                    k->running[t] = 0;
                    k->cr[t] &= (uint8_t)~CRF_START;
                    k->count[t] = k->latch[t];
                } else {                                        /* continuous: again from the latch */
                    k->count[t] = k->latch[t];
                    k->start_us[t] = due + 40000 < now ? now : due;   /* far behind: catch up once */
                }
            }
            deliver(c, k);
        }
        schedule();
    }
}

void cia_kick(void) { schedule(); }
void cia_wake(void) { if (cia_task) sched_timer_add(now_us(), TIMER_SIGNAL, cia_task, SIGF_CIA); }

/* AddIntServer(intNum, interrupt) / RemIntServer / SetIntVector(intNum, interrupt): old one */
uint32_t exec_add_int_server(uint32_t num, uint32_t is) {
    if (num > 15) return 0;
    for (int k = 0; k < 8; k++) if (!servers[num][k]) { servers[num][k] = is; break; }
    schedule();
    return 0;
}
uint32_t exec_rem_int_server(uint32_t num, uint32_t is) {
    if (num > 15) return 0;
    for (int k = 0; k < 8; k++)
        if (servers[num][k] == is) { memmove(&servers[num][k], &servers[num][k + 1], sizeof(uint32_t) * (size_t)(7 - k)); servers[num][7] = 0; break; }
    if (!servers[INTB_VERTB][0] && !chip_active()) next_vblank = 0;
    return 0;
}
uint32_t exec_set_int_vector(uint32_t num, uint32_t is) {
    if (num > 15) return 0;
    uint32_t old = vectors[num];
    vectors[num] = is;
    schedule();
    return old;
}

/* ---- ciaa.resource, ciab.resource ----------------------------------------------------- */

static CIA *cia_of(M68K *c) { return c->a[6] == cias[1].base ? &cias[1] : &cias[0]; }

/* AddICRVector(resource, bit, interrupt): 0, or the interrupt already there */
static uint32_t r_AddICRVector(M68K *c) {
    CIA *k = cia_of(c);
    int b = (int)(c->d[0] & 7);
    if (b > 4) return c->a[1];
    if (k->vec[b]) return k->vec[b];
    k->vec[b] = c->a[1];
    k->mask |= (uint8_t)(1 << b);                              /* and its interrupt is on */
    schedule();
    return 0;
}
static uint32_t r_RemICRVector(M68K *c) {
    CIA *k = cia_of(c);
    int b = (int)(c->d[0] & 7);
    if (b <= 4 && k->vec[b] == c->a[1]) { k->vec[b] = 0; k->mask &= (uint8_t)~(1 << b); }
    return 0;
}
/* AbleICR(resource, mask): bit 7 enables the bits given, else disables; the old mask */
static uint32_t r_AbleICR(M68K *c) {
    CIA *k = cia_of(c);
    uint8_t old = k->mask, m = (uint8_t)c->d[0];
    if (m & 0x80) k->mask |= m & 0x1F; else k->mask &= (uint8_t)~(m & 0x1F);
    schedule();
    return old;
}
/* SetICR(resource, mask): sets or clears interrupt flags; the old ones. Setting one
   that is enabled causes the interrupt. */
static uint32_t r_SetICR(M68K *c) {
    CIA *k = cia_of(c);
    uint8_t old = k->data, m = (uint8_t)c->d[0];
    if (m & 0x80) k->data |= m & 0x1F; else k->data &= (uint8_t)~(m & 0x1F);
    if (k->data & k->mask) sched_signal(cia_task, SIGF_CIA);
    return old;
}

static const NativeEntry cia_natives[] = {
    { "AddICRVector", r_AddICRVector }, { "RemICRVector", r_RemICRVector },
    { "AbleICR", r_AbleICR }, { "SetICR", r_SetICR },
    { 0, 0 }
};

void cia_init(void) {
    static const char *names[] = { "ciaa.resource", "ciab.resource" };
    for (int i = 0; i < 2; i++) {
        cias[i].base = lib_create(names[i], "cia", LIB_SIZE, cia_natives, 39, 1, i ? "ciab 39.1 (AmigaMac)" : "ciaa 39.1 (AmigaMac)");
        wr8(cias[i].base + LN_TYPE, 8);                         /* NT_RESOURCE */
        list_enqueue(SysBase + EB_RESOURCELIST, cias[i].base);
        cias[i].latch[0] = cias[i].latch[1] = 0xFFFF;
        cias[i].count[0] = cias[i].count[1] = 0xFFFF;
    }
    /* the task the interrupts run on: above everything else */
    uint32_t t = exec_allocmem(TC_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(t + LN_TYPE, NT_TASK);
    wr8(t + LN_PRI, 100);
    wr32(t + LN_NAME, str_alloc("CIA interrupts"));
    wr32(t + TC_SIGALLOC, 0xFFFF | SIGF_CIA);
    list_new(t + TC_MEMENTRY);
    uint32_t stack = exec_allocmem(16384, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(t + TC_SPLOWER, stack);
    wr32(t + TC_SPUPPER, stack + 16384);
    cia_task = t;
    sched_add(t, cia_main, 0, 0, 0);
}
