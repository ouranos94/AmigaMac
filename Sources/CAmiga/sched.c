/* exec's scheduler on macOS threads: see sched.h. */
#include "os.h"
#include "sched.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;   /* the baton */
static pthread_mutex_t evm = PTHREAD_MUTEX_INITIALIZER;   /* external events */
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static NTask *tasks, *current;
volatile int sched_need_resched;
static volatile int ev_pending;

/* external event state (under evm) */
static char *inbuf;
static int in_len, in_cap, brk;
static uint32_t console_owner;
static InputEv *inq;
static int inq_len, inq_cap;

/* timers (under the baton) */
typedef struct { uint64_t when; int kind; uint32_t a, b; } Timer;
static Timer timers[512];
static int ntimers;

#define QUANTUM 20000                       /* instructions between preemption checks */

uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void sched_init(void) {
    tasks = current = 0;
    ntimers = 0;
    in_len = 0;
}

NTask *sched_current(void) { return current; }
NTask *sched_tasks(void) { return tasks; }

NTask *sched_find(uint32_t t) {
    for (NTask *n = tasks; n; n = n->next) if (n->task == t && !n->dead) return n;
    return 0;
}

int sched_forbidden(void) {
    return (int8_t)rd8(SysBase + EB_TDNESTCNT) >= 0 || (int8_t)rd8(SysBase + EB_IDNESTCNT) >= 0;
}

static int8_t pri(uint32_t t) { return (int8_t)rd8(t + LN_PRI); }

static NTask *ready_head(void) {
    uint32_t n = rd32(SysBase + EB_TASKREADY + LH_HEAD);
    while (rd32(n + LN_SUCC)) {
        NTask *t = sched_find(n);
        if (t) return t;
        n = rd32(n + LN_SUCC);                  /* not ours: skip */
    }
    return 0;
}

static void make_ready(uint32_t t) {
    list_remove(t);                             /* from TaskWait */
    wr8(t + TC_STATE, TS_READY);
    list_enqueue(SysBase + EB_TASKREADY, t);
    if (current && pri(t) > pri(current->task)) sched_need_resched = 1;
}

/* ---- switching ----------------------------------------------------------- */

static void exit_thread(void) {
    pthread_mutex_unlock(&big);
    pthread_exit(0);
}

static void switch_to(NTask *me, NTask *next) {
    if (me && !me->dead) {                      /* Forbid/Disable are per task */
        wr8(me->task + TC_TDNESTCNT, rd8(SysBase + EB_TDNESTCNT));
        wr8(me->task + TC_IDNESTCNT, rd8(SysBase + EB_IDNESTCNT));
    }
    list_remove(next->task);                    /* the running task is in no list */
    current = next;
    wr8(next->task + TC_STATE, TS_RUN);
    wr32(SysBase + EB_THISTASK, next->task);
    wr8(SysBase + EB_TDNESTCNT, rd8(next->task + TC_TDNESTCNT));
    wr8(SysBase + EB_IDNESTCNT, rd8(next->task + TC_IDNESTCNT));
    if (next == me) return;
    pthread_cond_signal(&next->cond);
    if (!me) return;
    if (me->dead) exit_thread();
    while (current != me && !me->dead) pthread_cond_wait(&me->cond, &big);
    if (me->dead) exit_thread();
}

static void process_events(void);

static void idle_wait(void) {
    uint64_t deadline = ntimers ? timers[0].when : 0;
    pthread_mutex_unlock(&big);
    pthread_mutex_lock(&evm);
    if (!ev_pending) {
        if (deadline) {
            uint64_t now = now_us();
            if (deadline > now) {
                struct timeval tv;
                gettimeofday(&tv, 0);
                uint64_t abs_us = (uint64_t)tv.tv_sec * 1000000u + tv.tv_usec + (deadline - now);
                struct timespec ts = { (time_t)(abs_us / 1000000u), (long)(abs_us % 1000000u) * 1000 };
                pthread_cond_timedwait(&wake, &evm, &ts);
            }
        } else {
            pthread_cond_wait(&wake, &evm);
        }
    }
    pthread_mutex_unlock(&evm);
    pthread_mutex_lock(&big);
}

/* give the CPU to the best ready task; if none, wait for an event */
static void reschedule(NTask *me) {
    for (;;) {
        process_events();
        NTask *n = ready_head();
        if (n) { sched_need_resched = 0; switch_to(me, n); return; }
        current = 0;
        idle_wait();
        if (me && me->dead && !ready_head()) continue;
    }
}

void sched_block(void) {
    NTask *me = current;
    reschedule(me);
}

void sched_wake(uint32_t t) {
    if (rd8(t + TC_STATE) != TS_WAIT) return;
    if (!(rd32(t + TC_SIGRECVD) & rd32(t + TC_SIGWAIT))) return;
    NTask *n = sched_find(t);
    if (n) n->console_wait = 0;
    make_ready(t);
}

void sched_signal(uint32_t t, uint32_t sigs) {
    if (!t) return;
    wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) | sigs);
    sched_wake(t);
}

static void preempt(int quantum) {
    NTask *me = current;
    if (!me) return;
    if (ev_pending || (ntimers && timers[0].when <= now_us())) process_events();
    if (sched_forbidden()) return;
    NTask *n = ready_head();
    if (!n) { sched_need_resched = 0; return; }
    if (pri(n->task) > pri(me->task) || (quantum && pri(n->task) == pri(me->task))) {
        wr8(me->task + TC_STATE, TS_READY);
        list_enqueue(SysBase + EB_TASKREADY, me->task);     /* behind its equals: round robin */
        sched_need_resched = 0;
        switch_to(me, n);
    }
}

void sched_preempt_point(void) { preempt(0); }

void sched_run_cpu(M68K *c) {
    for (;;) {
        cpu68k_run(c, QUANTUM);
        if (c->stop) break;
        preempt(1);
    }
}

/* ---- task lifecycle ------------------------------------------------------- */

static uint32_t t_taskexit_id;

static int task_exception(M68K *c, int vec) {
    char dump[300];
    cpu68k_dump(c, dump, sizeof dump);
    char name[64];
    str_get(rd32(current->task + LN_NAME), name, sizeof name);
    os_log("Software Failure dans la tâche « %s » : exception %d à $%06X\n%s", name, vec, c->insn_pc, dump);
    c->stop = 2;
    return 1;
}

static void *thread_main(void *arg) {
    NTask *me = arg;
    pthread_mutex_lock(&big);
    while (current != me && !me->dead) pthread_cond_wait(&me->cond, &big);
    if (me->dead) exit_thread();
    if (me->native) {
        /* native bodies get a 68k stack too: they may call 68k hooks/dispatchers */
        if (rd32(me->task + TC_SPUPPER)) me->cpu.a[7] = rd32(me->task + TC_SPUPPER);
        me->native(me->arg);
    } else {
        M68K *c = &me->cpu;
        c->a[7] = rd32(me->task + TC_SPREG);
        cpu68k_push32(c, me->finalpc ? me->finalpc : stub_taskexit);
        c->pc = me->initpc;
        sched_run_cpu(c);                        /* stops on exit or guru */
    }
    exec_remove_task(me->task);
    sched_exit();
}

NTask *sched_add(uint32_t task, void (*native)(void *), void *arg, uint32_t initpc, uint32_t finalpc) {
    NTask *n = calloc(1, sizeof *n);
    n->task = task;
    n->native = native;
    n->arg = arg;
    n->initpc = initpc;
    n->finalpc = finalpc;
    cpu68k_init(&n->cpu, CPU_68020);
    n->cpu.aline = aline_handler;
    n->cpu.exception = task_exception;
    pthread_cond_init(&n->cond, 0);
    n->next = tasks;
    tasks = n;
    wr8(task + TC_IDNESTCNT, 0xFF);
    wr8(task + TC_TDNESTCNT, 0xFF);
    wr8(task + TC_STATE, TS_READY);
    list_enqueue(SysBase + EB_TASKREADY, task);
    if (current && pri(task) > pri(current->task)) sched_need_resched = 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&n->thread, &at, thread_main, n);
    pthread_attr_destroy(&at);
    (void)t_taskexit_id;
    return n;
}

void sched_start(void) {
    pthread_mutex_lock(&big);
    NTask *n = ready_head();
    if (n) switch_to(0, n);
    pthread_mutex_unlock(&big);
}

void sched_exit(void) {
    NTask *me = current;
    me->dead = 1;
    for (int i = 0; i < ntimers; i++)            /* its pending wake-ups go with it */
        if (timers[i].kind == TIMER_SIGNAL && timers[i].a == me->task) {
            memmove(&timers[i], &timers[i + 1], sizeof(Timer) * (ntimers - i - 1));
            ntimers--; i--;
        }
    reschedule(me);
    exit_thread();
    for (;;) ;
}

void sched_kill(uint32_t t) {
    NTask *n = sched_find(t);
    if (!n) return;
    if (n == current) sched_exit();
    list_remove(t);
    wr8(t + TC_STATE, TS_REMOVED);
    n->dead = 1;
    pthread_cond_signal(&n->cond);
}

/* ---- timers and external events ------------------------------------------ */

void sched_timer_add(uint64_t when, int kind, uint32_t a, uint32_t b) {
    if (ntimers == 512) { os_log("trop de minuteries en attente"); return; }
    int i = ntimers;
    while (i > 0 && timers[i - 1].when > when) { timers[i] = timers[i - 1]; i--; }
    timers[i].when = when; timers[i].kind = kind; timers[i].a = a; timers[i].b = b;
    ntimers++;
}

int sched_timer_remove(int kind, uint32_t a) {
    for (int i = 0; i < ntimers; i++)
        if (timers[i].kind == kind && timers[i].a == a) {
            memmove(&timers[i], &timers[i + 1], sizeof(Timer) * (ntimers - i - 1));
            ntimers--;
            return 1;
        }
    return 0;
}

static void process_events(void) {
    uint64_t t = now_us();
    while (ntimers && timers[0].when <= t) {
        Timer tm = timers[0];
        memmove(&timers[0], &timers[1], sizeof(Timer) * (ntimers - 1));
        ntimers--;
        if (tm.kind == TIMER_REPLY) exec_reply_msg(tm.a);
        else sched_signal(tm.a, tm.b);
    }
    if (!ev_pending) return;
    pthread_mutex_lock(&evm);
    int avail = in_len > 0, b = brk;
    brk = 0;
    ev_pending = 0;
    uint32_t owner = console_owner;
    pthread_mutex_unlock(&evm);
    if (b && owner) sched_signal(owner, SIGBREAKF_CTRL_C);
    pthread_mutex_lock(&evm);
    int ev = inq_len > 0;
    pthread_mutex_unlock(&evm);
    if (ev)
        for (NTask *n = tasks; n; n = n->next)
            if (!n->dead && n->input_wait && rd8(n->task + TC_STATE) == TS_WAIT) {
                n->input_wait = 0;
                make_ready(n->task);
            }
    if (avail)
        for (NTask *n = tasks; n; n = n->next)
            if (!n->dead && n->console_wait && rd8(n->task + TC_STATE) == TS_WAIT) {
                n->console_wait = 0;
                make_ready(n->task);
            }
}

void amiga_console_input(const char *s, int len) {
    pthread_mutex_lock(&evm);
    if (in_len + len > in_cap) {
        in_cap = (in_len + len) * 2 + 256;
        inbuf = realloc(inbuf, in_cap);
    }
    memcpy(inbuf + in_len, s, len);
    in_len += len;
    ev_pending = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&evm);
}

void amiga_console_break(void) {
    pthread_mutex_lock(&evm);
    brk = 1;
    ev_pending = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&evm);
}

void sched_set_console_owner(uint32_t t) {
    pthread_mutex_lock(&evm);
    console_owner = t;
    pthread_mutex_unlock(&evm);
}

int sched_console_ready(void) {
    pthread_mutex_lock(&evm);
    int r = in_len > 0;
    pthread_mutex_unlock(&evm);
    return r;
}

int sched_console_read(char *buf, int max) {
    NTask *me = current;
    for (;;) {
        pthread_mutex_lock(&evm);
        if (in_len) {
            /* one line at most, like a console handler */
            int n = 0;
            while (n < in_len && n < max) { if (inbuf[n++] == '\n') break; }
            memcpy(buf, inbuf, n);
            memmove(inbuf, inbuf + n, in_len - n);
            in_len -= n;
            pthread_mutex_unlock(&evm);
            return n;
        }
        pthread_mutex_unlock(&evm);
        me->console_wait = 1;
        wr8(me->task + TC_STATE, TS_WAIT);
        list_addtail(SysBase + EB_TASKWAIT, me->task);
        sched_block();
    }
}

/* ---- input events ----------------------------------------------------------- */

void amiga_input_event(const InputEv *e) {
    pthread_mutex_lock(&evm);
    /* coalesce consecutive mouse moves */
    if (e->type == IEV_MOUSE && inq_len && inq[inq_len - 1].type == IEV_MOUSE && inq[inq_len - 1].screen == e->screen)
        inq[inq_len - 1] = *e;
    else if (e->type == IEV_TICK && inq_len && inq[inq_len - 1].type == IEV_TICK)
        ;
    else {
        if (inq_len == inq_cap) { inq_cap = inq_cap * 2 + 64; inq = realloc(inq, sizeof *inq * (size_t)inq_cap); }
        inq[inq_len++] = *e;
    }
    ev_pending = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&evm);
}

int sched_input_read(InputEv *e) {
    NTask *me = current;
    for (;;) {
        pthread_mutex_lock(&evm);
        if (inq_len) {
            *e = inq[0];
            memmove(inq, inq + 1, sizeof *inq * (size_t)(inq_len - 1));
            inq_len--;
            pthread_mutex_unlock(&evm);
            return 1;
        }
        pthread_mutex_unlock(&evm);
        me->input_wait = 1;
        wr8(me->task + TC_STATE, TS_WAIT);
        list_addtail(SysBase + EB_TASKWAIT, me->task);
        sched_block();
    }
}
