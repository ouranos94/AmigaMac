/* exec.library, native.
 *
 * Milestone 1 runs a single task (the CLI process); the structures are the
 * real ones so a scheduler can be added without changing what programs see.
 * Register conventions follow exec_lib.fd.
 */
#include "os.h"
#include "sched.h"
#include <stdio.h>
#include <string.h>

uint32_t SysBase;

uint32_t this_task(void) { NTask *n = sched_current(); return n ? n->task : 0; }
#define current_task this_task()

/* ---- memory ------------------------------------------------------------- */

uint32_t exec_allocmem(uint32_t size, uint32_t flags) {
    return heap_alloc(size, flags);
}

void exec_freemem(uint32_t addr, uint32_t size) { heap_free(addr, size); }

uint32_t exec_allocvec(uint32_t size, uint32_t flags) {
    uint32_t a = heap_alloc(size + 8, flags);
    if (!a) return 0;
    wr32(a, size + 8);
    return a + 8;                                 /* keep 8-byte alignment */
}

void exec_freevec(uint32_t a) {
    if (!a) return;
    heap_free(a - 8, rd32(a - 8));
}

static uint32_t f_AllocMem(M68K *c) { return exec_allocmem(c->d[0], c->d[1]); }
static uint32_t f_FreeMem(M68K *c) { exec_freemem(c->a[1], c->d[0]); return 0; }
static uint32_t f_AllocVec(M68K *c) { return exec_allocvec(c->d[0], c->d[1]); }
static uint32_t f_FreeVec(M68K *c) { exec_freevec(c->a[1]); return 0; }
static uint32_t f_AvailMem(M68K *c) { return heap_avail(c->d[1]); }
static uint32_t f_TypeOfMem(M68K *c) {
    return c->a[1] < mem_size ? MEMF_PUBLIC | MEMF_FAST : 0;
}

static uint32_t f_AllocEntry(M68K *c) {
    /* MemList: node(14) + ml_NumEntries(2) + entries {addr/reqs(4), length(4)} */
    uint32_t ml = c->a[0];
    int n = rd16(ml + 14);
    uint32_t out = exec_allocmem(16 + n * 8, MEMF_PUBLIC | MEMF_CLEAR);
    wr16(out + 14, n);
    for (int i = 0; i < n; i++) {
        uint32_t reqs = rd32(ml + 16 + i * 8), len = rd32(ml + 20 + i * 8);
        uint32_t a = exec_allocmem(len, reqs);
        if (!a && len) {
            for (int j = 0; j < i; j++) exec_freemem(rd32(out + 16 + j * 8), rd32(out + 20 + j * 8));
            exec_freemem(out, 16 + n * 8);
            return 0x80000000 | reqs;
        }
        wr32(out + 16 + i * 8, a);
        wr32(out + 20 + i * 8, len);
    }
    return out;
}

static uint32_t f_FreeEntry(M68K *c) {
    uint32_t ml = c->a[0];
    int n = rd16(ml + 14);
    for (int i = 0; i < n; i++) exec_freemem(rd32(ml + 16 + i * 8), rd32(ml + 20 + i * 8));
    exec_freemem(ml, 16 + n * 8);
    return 0;
}

static uint32_t f_CopyMem(M68K *c) {
    uint32_t s = c->a[0], d = c->a[1], n = c->d[0];
    if (mem_ok(s, n) && mem_ok(d, n)) memmove(mem + d, mem + s, n);
    else for (uint32_t i = 0; i < n; i++) wr8(d + i, rd8(s + i));
    return 0;
}

/* memory pools: puddles are not needed natively, each allocation is a vec */
static uint32_t f_CreatePool(M68K *c) {
    uint32_t pool = exec_allocmem(LH_SIZE + 8, MEMF_PUBLIC | MEMF_CLEAR);
    list_new(pool);
    wr32(pool + LH_SIZE, c->d[0]);                /* requirements */
    return pool;
}
static uint32_t f_AllocPooled(M68K *c) {
    uint32_t pool = c->a[0], size = c->d[0];
    uint32_t a = exec_allocmem(size + 16, rd32(pool + LH_SIZE));
    if (!a) return 0;
    wr32(a + 8, size + 16);
    list_addtail(pool, a);
    return a + 16;
}
static uint32_t f_FreePooled(M68K *c) {
    uint32_t a = c->a[1] - 16;
    if (!c->a[1]) return 0;
    list_remove(a);
    exec_freemem(a, rd32(a + 8));
    return 0;
}
static uint32_t f_DeletePool(M68K *c) {
    uint32_t pool = c->a[0], n;
    if (!pool) return 0;
    while ((n = list_remhead(pool))) exec_freemem(n, rd32(n + 8));
    exec_freemem(pool, LH_SIZE + 8);
    return 0;
}
static uint32_t f_AllocVecPooled(M68K *c) {
    c->d[0] += 8;
    uint32_t a = f_AllocPooled(c);
    if (!a) return 0;
    wr32(a, c->d[0]);
    return a + 8;
}
static uint32_t f_FreeVecPooled(M68K *c) {
    if (!c->a[1]) return 0;
    c->a[1] -= 8;
    return f_FreePooled(c);
}

/* ---- libraries ---------------------------------------------------------- */

uint32_t exec_openlibrary(const char *name, uint32_t version) {
    /* exec searches by node name; strip any path like "libs:foo.library" */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : (strchr(name, ':') ? strchr(name, ':') + 1 : name);
    uint32_t lib = list_findname(SysBase + EB_LIBLIST, base);
    if (!lib) {
        os_log("OpenLibrary(\"%s\", %u) : bibliothèque absente", name, version);
        return 0;
    }
    if (rd16(lib + LIB_VERSION) < version) {
        os_log("OpenLibrary(\"%s\", %u) : version %u seulement", name, version, rd16(lib + LIB_VERSION));
        return 0;
    }
    wr16(lib + LIB_OPENCNT, rd16(lib + LIB_OPENCNT) + 1);
    return lib;
}

static uint32_t f_OpenLibrary(M68K *c) {
    char name[256];
    str_get(c->a[1], name, sizeof name);
    return exec_openlibrary(name, c->d[0]);
}
static uint32_t f_OldOpenLibrary(M68K *c) {
    char name[256];
    str_get(c->a[1], name, sizeof name);
    return exec_openlibrary(name, 0);
}
static uint32_t f_CloseLibrary(M68K *c) {
    uint32_t lib = c->a[1];
    if (lib && rd16(lib + LIB_OPENCNT)) wr16(lib + LIB_OPENCNT, rd16(lib + LIB_OPENCNT) - 1);
    return 0;
}
static uint32_t f_TaggedOpenLibrary(M68K *c) {
    static const char *names[] = { 0, "graphics.library", "layers.library", "intuition.library",
                                   "dos.library", "icon.library", "expansion.library",
                                   "utility.library", "keymap.library", "gadtools.library",
                                   "workbench.library" };
    int tag = (int)c->d[0];
    if (tag < 1 || tag > 10) return 0;
    return exec_openlibrary(names[tag], 0);
}
static uint32_t f_AddLibrary(M68K *c) { lib_add(c->a[1]); return 0; }
static uint32_t f_RemLibrary(M68K *c) { list_remove(c->a[1]); return 0; }
static uint32_t f_SetFunction(M68K *c) {
    /* A1 = library, A0 = LVO (negative), D0 = new function */
    uint32_t slot = c->a[1] + (int16_t)c->a[0];
    uint32_t old;
    if (rd16(slot) == 0x4EF9) old = rd32(slot + 2);
    else old = slot;                              /* a trap slot: calling it still works */
    if (old == slot) {
        /* preserve the original trap so the returned pointer stays callable */
        uint32_t copy = exec_allocmem(6, MEMF_PUBLIC);
        for (int i = 0; i < 6; i++) wr8(copy + i, rd8(slot + i));
        old = copy;
    }
    wr16(slot, 0x4EF9);
    wr32(slot + 2, c->d[0]);
    wr16(c->a[1] + LIB_FLAGS, rd8(c->a[1] + LIB_FLAGS) | 0x02);
    return old;
}
static uint32_t f_SumLibrary(M68K *c) { (void)c; return 0; }
static uint32_t f_MakeFunctions(M68K *c) {
    uint32_t target = c->a[0], arr = c->a[1], disp = c->a[2], n = 0;
    uint32_t slot = target - 6;
    if (disp) {
        for (;; n++) {
            int16_t w = (int16_t)rd16(arr + n * 2);
            if (w == -1) break;
            wr16(slot, 0x4EF9); wr32(slot + 2, disp + w); slot -= 6;
        }
    } else {
        for (;; n++) {
            uint32_t f = rd32(arr + n * 4);
            if (f == 0xFFFFFFFF) break;
            wr16(slot, 0x4EF9); wr32(slot + 2, f); slot -= 6;
        }
    }
    return n * 6;
}

/* ---- tasks, signals ----------------------------------------------------- */

static uint32_t f_FindTask(M68K *c) {
    if (!c->a[1]) return current_task;
    char name[128], tn[128];
    str_get(c->a[1], name, sizeof name);
    str_get(rd32(current_task + LN_NAME), tn, sizeof tn);
    if (!strcmp(name, tn)) return current_task;
    uint32_t t = list_findname(SysBase + EB_TASKREADY, name);
    if (!t) t = list_findname(SysBase + EB_TASKWAIT, name);
    return t;
}

/* AddTask(task A1, initialPC A2, finalPC A3) */
static uint32_t f_AddTask(M68K *c) {
    uint32_t t = c->a[1];
    if (!rd32(t + TC_SIGALLOC)) wr32(t + TC_SIGALLOC, 0x0000FFFF);
    if (!rd16(t + TC_TRAPALLOC)) wr16(t + TC_TRAPALLOC, 0x8000);
    if (!rd32(t + TC_MEMENTRY + LH_HEAD)) list_new(t + TC_MEMENTRY);
    wr8(t + LN_TYPE, rd8(t + LN_TYPE) ? rd8(t + LN_TYPE) : NT_TASK);
    sched_add(t, 0, 0, c->a[2], c->a[3]);
    sched_preempt_point();
    return t;
}

/* free tc_MemEntry (MemList nodes allocated with AllocEntry) */
void exec_remove_task(uint32_t t) {
    uint32_t ml;
    while ((ml = list_remhead(t + TC_MEMENTRY))) {
        int n = rd16(ml + 14);
        for (int i = 0; i < n; i++) exec_freemem(rd32(ml + 16 + i * 8), rd32(ml + 20 + i * 8));
        exec_freemem(ml, 16 + n * 8);
    }
}

static uint32_t f_RemTask(M68K *c) {
    uint32_t t = c->a[1] ? c->a[1] : current_task;
    exec_remove_task(t);
    sched_kill(t);                                 /* never returns for the current task */
    return 0;
}

static uint32_t f_Forbid(M68K *c) { (void)c; wr8(SysBase + EB_TDNESTCNT, rd8(SysBase + EB_TDNESTCNT) + 1); return 0; }
static uint32_t f_Permit(M68K *c) {
    (void)c;
    wr8(SysBase + EB_TDNESTCNT, rd8(SysBase + EB_TDNESTCNT) - 1);
    if (sched_need_resched) sched_preempt_point();
    return 0;
}
static uint32_t f_Disable(M68K *c) { (void)c; wr8(SysBase + EB_IDNESTCNT, rd8(SysBase + EB_IDNESTCNT) + 1); return 0; }
static uint32_t f_Enable(M68K *c) {
    (void)c;
    wr8(SysBase + EB_IDNESTCNT, rd8(SysBase + EB_IDNESTCNT) - 1);
    if (sched_need_resched) sched_preempt_point();
    return 0;
}

static uint32_t f_SetSignal(M68K *c) {
    uint32_t t = current_task, old = rd32(t + TC_SIGRECVD);
    wr32(t + TC_SIGRECVD, (old & ~c->d[1]) | (c->d[0] & c->d[1]));
    return old;
}

static uint32_t f_Signal(M68K *c) {
    sched_signal(c->a[1], c->d[0]);
    if (sched_need_resched) sched_preempt_point();
    return 0;
}

uint32_t exec_wait(uint32_t mask) {
    uint32_t t = current_task;
    for (;;) {
        uint32_t got = rd32(t + TC_SIGRECVD) & mask;
        if (got) {
            wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) & ~got);
            return got;
        }
        if (!mask) return 0;                       /* Wait(0) would sleep forever */
        wr32(t + TC_SIGWAIT, mask);
        wr8(t + TC_STATE, TS_WAIT);
        list_addtail(SysBase + EB_TASKWAIT, t);
        sched_block();
    }
}

static uint32_t f_Wait(M68K *c) { return exec_wait(c->d[0]); }

static uint32_t f_AllocSignal(M68K *c) {
    uint32_t t = current_task, alloc = rd32(t + TC_SIGALLOC);
    int8_t want = (int8_t)c->d[0];
    if (want >= 0) {
        if (alloc & (1u << want)) return 0xFFFFFFFF;
    } else {
        for (want = 31; want >= 16 && (alloc & (1u << want)); want--) ;
        if (want < 16) return 0xFFFFFFFF;
    }
    wr32(t + TC_SIGALLOC, alloc | (1u << want));
    wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) & ~(1u << want));
    return (uint32_t)want;
}

static uint32_t f_FreeSignal(M68K *c) {
    int8_t s = (int8_t)c->d[0];
    if (s >= 0) wr32(current_task + TC_SIGALLOC, rd32(current_task + TC_SIGALLOC) & ~(1u << s));
    return 0;
}

static uint32_t f_SetTaskPri(M68K *c) {
    uint32_t t = c->a[1], old = (uint32_t)(int8_t)rd8(t + LN_PRI);
    wr8(t + LN_PRI, c->d[0]);
    if (rd8(t + TC_STATE) == TS_READY) {           /* keep TaskReady ordered */
        list_remove(t);
        list_enqueue(SysBase + EB_TASKREADY, t);
    }
    sched_need_resched = 1;
    sched_preempt_point();
    return old;
}

static uint32_t f_SetExcept(M68K *c) {
    uint32_t t = current_task, old = rd32(t + TC_SIGEXCEPT);
    wr32(t + TC_SIGEXCEPT, (old & ~c->d[1]) | (c->d[0] & c->d[1]));
    return old;
}

/* ---- message ports ------------------------------------------------------ */

static uint32_t f_CreateMsgPort(M68K *c) {
    uint32_t p = exec_allocmem(MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    c->d[0] = 0xFFFFFFFF;
    uint32_t sig = f_AllocSignal(c);
    if (sig == 0xFFFFFFFF) { exec_freemem(p, MP_SIZE); return 0; }
    wr8(p + LN_TYPE, NT_MSGPORT);
    wr8(p + MP_FLAGS, PA_SIGNAL);
    wr8(p + MP_SIGBIT, sig);
    wr32(p + MP_SIGTASK, current_task);
    list_new(p + MP_MSGLIST);
    return p;
}

static uint32_t f_DeleteMsgPort(M68K *c) {
    uint32_t p = c->a[0];
    if (!p) return 0;
    c->d[0] = rd8(p + MP_SIGBIT);
    f_FreeSignal(c);
    exec_freemem(p, MP_SIZE);
    return 0;
}

static void put_msg(uint32_t port, uint32_t msg) {
    wr8(msg + LN_TYPE, NT_MESSAGE);
    list_addtail(port + MP_MSGLIST, msg);
    switch (rd8(port + MP_FLAGS) & 3) {
    case PA_SIGNAL:
        sched_signal(rd32(port + MP_SIGTASK), 1u << rd8(port + MP_SIGBIT));
        break;
    case PA_SOFTINT:
        os_log("PutMsg : port PA_SOFTINT pas encore géré");
        break;
    }
}

static uint32_t f_PutMsg(M68K *c) {
    put_msg(c->a[0], c->a[1]);
    if (sched_need_resched) sched_preempt_point();
    return 0;
}
static uint32_t f_GetMsg(M68K *c) { return list_remhead(c->a[0] + MP_MSGLIST); }
void exec_reply_msg(uint32_t msg) {
    uint32_t rp = rd32(msg + MN_REPLYPORT);
    if (!rp) { wr8(msg + LN_TYPE, NT_FREEMSG); return; }
    put_msg(rp, msg);
    wr8(msg + LN_TYPE, NT_REPLYMSG);
}
static uint32_t f_ReplyMsg(M68K *c) {
    exec_reply_msg(c->a[1]);
    if (sched_need_resched) sched_preempt_point();
    return 0;
}
uint32_t exec_waitport(uint32_t p) {
    for (;;) {
        uint32_t head = rd32(p + MP_MSGLIST + LH_HEAD);
        if (rd32(head + LN_SUCC)) return head;
        exec_wait(1u << rd8(p + MP_SIGBIT));
    }
}
static uint32_t f_WaitPort(M68K *c) { return exec_waitport(c->a[0]); }
static uint32_t f_AddPort(M68K *c) {
    uint32_t p = c->a[1];
    list_new(p + MP_MSGLIST);
    list_enqueue(SysBase + EB_PORTLIST, p);
    return 0;
}
static uint32_t f_RemPort(M68K *c) { list_remove(c->a[1]); return 0; }
static uint32_t f_FindPort(M68K *c) {
    char name[128];
    str_get(c->a[1], name, sizeof name);
    return list_findname(SysBase + EB_PORTLIST, name);
}

/* ---- semaphores ---------------------------------------------------------- */

#define SS_NESTCOUNT 14
#define SS_WAITQUEUE 16
#define SS_OWNER 40
#define SS_QUEUECOUNT 44
/* SemaphoreRequest: MinNode(8), sr_Waiter(4) */

static uint32_t f_InitSemaphore(M68K *c) {
    uint32_t s = c->a[0];
    for (int i = 0; i < SS_SIZE; i++) wr8(s + i, 0);
    wr8(s + LN_TYPE, NT_SIGNALSEM);
    list_new(s + SS_WAITQUEUE);
    wr16(s + SS_QUEUECOUNT, 0xFFFF);
    return 0;
}

static void sem_obtain(uint32_t s) {
    uint32_t me = current_task;
    wr16(s + SS_QUEUECOUNT, rd16(s + SS_QUEUECOUNT) + 1);
    uint32_t owner = rd32(s + SS_OWNER);
    if (!owner || owner == me) {
        wr32(s + SS_OWNER, me);
        wr16(s + SS_NESTCOUNT, rd16(s + SS_NESTCOUNT) + 1);
        return;
    }
    /* queue a request on our stack-like scratch and sleep until handed over */
    uint32_t req = exec_allocmem(12, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(req + 8, me);
    list_addtail(s + SS_WAITQUEUE, req);
    wr32(me + TC_SIGRECVD, rd32(me + TC_SIGRECVD) & ~SIGF_SINGLE);
    while (rd32(s + SS_OWNER) != me) exec_wait(SIGF_SINGLE);
    exec_freemem(req, 12);
}

static int sem_attempt(uint32_t s) {
    uint32_t owner = rd32(s + SS_OWNER);
    if (owner && owner != current_task) return 0;
    sem_obtain(s);
    return 1;
}

static void sem_release(uint32_t s) {
    uint32_t n = rd16(s + SS_NESTCOUNT);
    wr16(s + SS_QUEUECOUNT, rd16(s + SS_QUEUECOUNT) - 1);
    if (n > 1) { wr16(s + SS_NESTCOUNT, n - 1); return; }
    wr16(s + SS_NESTCOUNT, 0);
    wr32(s + SS_OWNER, 0);
    uint32_t req = list_remhead(s + SS_WAITQUEUE);
    if (req) {                                     /* hand over to the first waiter */
        uint32_t w = rd32(req + 8);
        wr32(s + SS_OWNER, w);
        wr16(s + SS_NESTCOUNT, 1);
        sched_signal(w, SIGF_SINGLE);
        if (sched_need_resched) sched_preempt_point();
    }
}

static uint32_t f_ObtainSemaphore(M68K *c) { sem_obtain(c->a[0]); return 0; }
static uint32_t f_ReleaseSemaphore(M68K *c) { sem_release(c->a[0]); return 0; }
static uint32_t f_AttemptSemaphore(M68K *c) { return sem_attempt(c->a[0]); }
static uint32_t f_ObtainSemaphoreShared(M68K *c) { sem_obtain(c->a[0]); return 0; }
static uint32_t f_AttemptSemaphoreShared(M68K *c) { return sem_attempt(c->a[0]); }
static uint32_t f_AddSemaphore(M68K *c) {
    c->a[0] = c->a[1];
    f_InitSemaphore(c);
    list_enqueue(SysBase + EB_SEMAPHORELIST, c->a[1]);
    return 0;
}
static uint32_t f_RemSemaphore(M68K *c) { list_remove(c->a[1]); return 0; }
static uint32_t f_FindSemaphore(M68K *c) {
    char name[128];
    str_get(c->a[1], name, sizeof name);
    return list_findname(SysBase + EB_SEMAPHORELIST, name);
}

/* ---- devices and I/O -------------------------------------------------------- */

#define IO_DEVICE 20
#define IO_UNIT 24
#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define IOF_QUICK 1
#define IOERR_OPENFAIL (-1)

/* call a library vector with registers set up, preserving the caller's */
static void call_vector(M68K *c, uint32_t base, int lvo, uint32_t a1, uint32_t d0, uint32_t d1) {
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    c->a[1] = a1; c->a[6] = base; c->d[0] = d0; c->d[1] = d1;
    cpu_call(c, base - lvo);
    uint32_t r = c->d[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->d[0] = r;
}

static uint32_t f_OpenDevice(M68K *c) {
    char name[128];
    str_get(c->a[0], name, sizeof name);
    uint32_t ior = c->a[1], unit = c->d[0], flags = c->d[1];
    uint32_t dev = list_findname(SysBase + EB_DEVICELIST, name);
    if (!dev) {
        os_log("OpenDevice(\"%s\") : périphérique absent", name);
        wr8(ior + IO_ERROR, (uint8_t)IOERR_OPENFAIL);
        return (uint32_t)IOERR_OPENFAIL;
    }
    wr32(ior + IO_DEVICE, dev);
    wr8(ior + IO_ERROR, 0);
    /* device Open: A1 = ior, D0 = unit, D1 = flags, A6 = device */
    call_vector(c, dev, 6, ior, unit, flags);
    int8_t err = (int8_t)rd8(ior + IO_ERROR);
    if (err) { wr32(ior + IO_DEVICE, 0); return (uint32_t)(int32_t)err; }
    wr16(dev + LIB_OPENCNT, rd16(dev + LIB_OPENCNT) + 1);
    return 0;
}

static uint32_t f_CloseDevice(M68K *c) {
    uint32_t ior = c->a[1], dev = rd32(ior + IO_DEVICE);
    if (!dev) return 0;
    call_vector(c, dev, 12, ior, 0, 0);
    if (rd16(dev + LIB_OPENCNT)) wr16(dev + LIB_OPENCNT, rd16(dev + LIB_OPENCNT) - 1);
    wr32(ior + IO_DEVICE, 0);
    return 0;
}

static uint32_t f_AddDevice(M68K *c) { list_enqueue(SysBase + EB_DEVICELIST, c->a[1]); return 0; }
static uint32_t f_RemDevice(M68K *c) { list_remove(c->a[1]); return 0; }

static void begin_io(M68K *c, uint32_t ior) {
    wr8(ior + LN_TYPE, NT_MESSAGE);
    call_vector(c, rd32(ior + IO_DEVICE), 30, ior, 0, 0);   /* BeginIO */
}

static uint32_t wait_io(M68K *c, uint32_t ior) {
    (void)c;
    if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) {
        uint32_t port = rd32(ior + MN_REPLYPORT);
        while (rd8(ior + LN_TYPE) != NT_REPLYMSG) exec_wait(1u << rd8(port + MP_SIGBIT));
        list_remove(ior);           /* take it off the reply port */
    }
    return (uint32_t)(int32_t)(int8_t)rd8(ior + IO_ERROR);
}

static uint32_t f_DoIO(M68K *c) {
    uint32_t ior = c->a[1];
    wr8(ior + IO_FLAGS, IOF_QUICK);
    begin_io(c, ior);
    return wait_io(c, ior);
}
static uint32_t f_SendIO(M68K *c) {
    wr8(c->a[1] + IO_FLAGS, 0);
    begin_io(c, c->a[1]);
    return 0;
}
static uint32_t f_WaitIO(M68K *c) { return wait_io(c, c->a[1]); }
static uint32_t f_CheckIO(M68K *c) {
    uint32_t ior = c->a[1];
    if (rd8(ior + IO_FLAGS) & IOF_QUICK) return ior;
    return rd8(ior + LN_TYPE) == NT_REPLYMSG ? ior : 0;
}
static uint32_t f_AbortIO(M68K *c) {
    uint32_t ior = c->a[1];
    call_vector(c, rd32(ior + IO_DEVICE), 36, ior, 0, 0);
    return 0;
}

static uint32_t f_CreateIORequest(M68K *c) {
    uint32_t port = c->a[0], size = c->d[0];
    if (!port) return 0;
    uint32_t ior = exec_allocmem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ior) return 0;
    wr8(ior + LN_TYPE, NT_REPLYMSG);
    wr32(ior + MN_REPLYPORT, port);
    wr16(ior + MN_LENGTH, size);
    return ior;
}
static uint32_t f_DeleteIORequest(M68K *c) {
    uint32_t ior = c->a[0];
    if (ior) exec_freemem(ior, rd16(ior + MN_LENGTH));
    return 0;
}

/* ---- lists -------------------------------------------------------------- */

static uint32_t f_Insert(M68K *c) { list_insert(c->a[0], c->a[1], c->a[2]); return 0; }
static uint32_t f_AddHead(M68K *c) { list_addhead(c->a[0], c->a[1]); return 0; }
static uint32_t f_AddTail(M68K *c) { list_addtail(c->a[0], c->a[1]); return 0; }
static uint32_t f_Remove(M68K *c) { list_remove(c->a[1]); return 0; }
static uint32_t f_RemHead(M68K *c) { return list_remhead(c->a[0]); }
static uint32_t f_RemTail(M68K *c) { return list_remtail(c->a[0]); }
static uint32_t f_Enqueue(M68K *c) { list_enqueue(c->a[0], c->a[1]); return 0; }
static uint32_t f_FindName(M68K *c) {
    char name[256];
    str_get(c->a[1], name, sizeof name);
    return list_findname(c->a[0], name);
}
static uint32_t f_NewMinList(M68K *c) { list_new(c->a[0]); return 0; }

/* ---- RawDoFmt ----------------------------------------------------------- */

/* calls the putch procedure for each character (A3 = PutChData) */
static void putch(M68K *c, uint32_t proc, uint32_t ch) {
    /* fast path for the ubiquitous "move.b d0,(a3)+ ; rts" */
    if (rd16(proc) == 0x16C0 && rd16(proc + 2) == 0x4E75) { wr8(c->a[3], ch); c->a[3]++; return; }
    c->d[0] = ch;
    cpu_call(c, proc);
}

static uint32_t f_RawDoFmt(M68K *c) {
    uint32_t fmt = c->a[0], args = c->a[1], proc = c->a[2];
    uint32_t sa2 = c->a[2], sd0 = c->d[0], sd1 = c->d[1], sa0 = c->a[0], sa1 = c->a[1];
    char out[300];
    for (;;) {
        uint32_t ch = rd8(fmt++);
        if (!ch) { putch(c, proc, 0); break; }
        if (ch != '%') { putch(c, proc, ch); continue; }
        int left = 0, zero = 0, width = 0, limit = -1, lng = 0;
        ch = rd8(fmt++);
        if (ch == '-') { left = 1; ch = rd8(fmt++); }
        if (ch == '0') { zero = 1; ch = rd8(fmt++); }
        while (ch >= '0' && ch <= '9') { width = width * 10 + (int)(ch - '0'); ch = rd8(fmt++); }
        if (ch == '.') {
            limit = 0;
            ch = rd8(fmt++);
            while (ch >= '0' && ch <= '9') { limit = limit * 10 + (int)(ch - '0'); ch = rd8(fmt++); }
        }
        if (ch == 'l') { lng = 1; ch = rd8(fmt++); }
        int n = 0;
        switch (ch) {
        case 'd': case 'D': case 'u': case 'U': case 'x': case 'X': {
            uint32_t v;
            if (lng) { v = rd32(args); args += 4; } else { v = rd16(args); args += 2; }
            if (ch == 'd' || ch == 'D') {
                int32_t sv = lng ? (int32_t)v : (int16_t)v;
                n = snprintf(out, sizeof out, "%d", sv);
            } else if (ch == 'u' || ch == 'U') n = snprintf(out, sizeof out, "%u", v);
            else n = snprintf(out, sizeof out, ch == 'x' ? "%x" : "%X", v);
            break;
        }
        case 's': {
            uint32_t p = rd32(args);
            args += 4;
            n = str_get(p, out, sizeof out);
            break;
        }
        case 'b': {
            uint32_t p = rd32(args);
            args += 4;
            n = bstr_get(p, out, sizeof out);
            break;
        }
        case 'c': {
            uint32_t v;
            if (lng) { v = rd32(args); args += 4; } else { v = rd16(args); args += 2; }
            out[0] = (char)v; out[1] = 0; n = 1;
            break;
        }
        case '%': out[0] = '%'; out[1] = 0; n = 1; break;
        default: out[0] = (char)ch; out[1] = 0; n = 1; break;
        }
        if (limit >= 0 && n > limit) n = limit;
        int pad = width > n ? width - n : 0;
        if (!left) for (int i = 0; i < pad; i++) putch(c, proc, zero ? '0' : ' ');
        for (int i = 0; i < n; i++) putch(c, proc, (uint8_t)out[i]);
        if (left) for (int i = 0; i < pad; i++) putch(c, proc, ' ');
    }
    c->a[0] = sa0; c->a[1] = sa1; c->a[2] = sa2; c->d[1] = sd1; (void)sd0;
    return args;
}

/* ---- misc --------------------------------------------------------------- */

static uint32_t f_CacheClearU(M68K *c) { (void)c; return 0; }
static uint32_t f_CacheClearE(M68K *c) { (void)c; return 0; }
static uint32_t f_CacheControl(M68K *c) { (void)c; return 0; }
static uint32_t f_SuperState(M68K *c) { uint32_t sp = c->a[7]; cpu68k_set_sr(c, c->sr | SR_S); c->a[7] = sp; return sp; }
static uint32_t f_UserState(M68K *c) { cpu68k_set_sr(c, c->sr & ~SR_S); c->a[7] = c->d[0]; return 0; }
static uint32_t f_Supervisor(M68K *c) {
    /* run the routine (A5) in supervisor mode; it returns with RTE, which
       pops the frame built here and lands on the return stub */
    uint32_t old = c->sr, pc = c->pc;
    cpu68k_set_sr(c, c->sr | SR_S);
    if (c->model >= CPU_68020) { c->a[7] -= 2; wr16(c->a[7], 0); }   /* format 0 */
    cpu68k_push32(c, stub_return);
    c->a[7] -= 2; wr16(c->a[7], old);
    c->pc = c->a[5];
    for (;;) { cpu68k_run(c, 1u << 30); if (c->stop) break; }
    if (c->stop == 3) c->stop = 0;
    c->pc = pc;
    return c->d[0];
}
static uint32_t f_Alert(M68K *c) {
    os_log("Alert($%08X) depuis $%06X", c->d[7], rd32(c->a[7]));
    return 0;
}
static uint32_t f_Debug(M68K *c) { (void)c; return 0; }
static uint32_t f_SetSR(M68K *c) {
    uint32_t old = c->sr & 0xFFFF;
    cpu68k_set_sr(c, (c->sr & ~c->d[1]) | (c->d[0] & c->d[1]));
    return old;
}
static uint32_t f_GetCC(M68K *c) { return c->sr & 0x1F; }
static uint32_t f_OpenResource(M68K *c) {
    char name[128];
    str_get(c->a[1], name, sizeof name);
    return list_findname(SysBase + EB_RESOURCELIST, name);
}
static uint32_t f_ColdReboot(M68K *c) { os_log("ColdReboot()"); c->stop = 2; return 0; }

static const NativeEntry exec_natives[] = {
    { "AllocMem", f_AllocMem }, { "FreeMem", f_FreeMem }, { "AllocVec", f_AllocVec },
    { "FreeVec", f_FreeVec }, { "AvailMem", f_AvailMem }, { "TypeOfMem", f_TypeOfMem },
    { "AllocEntry", f_AllocEntry }, { "FreeEntry", f_FreeEntry },
    { "CopyMem", f_CopyMem }, { "CopyMemQuick", f_CopyMem },
    { "CreatePool", f_CreatePool }, { "DeletePool", f_DeletePool },
    { "AllocPooled", f_AllocPooled }, { "FreePooled", f_FreePooled },
    { "AllocVecPooled", f_AllocVecPooled }, { "FreeVecPooled", f_FreeVecPooled },
    { "OpenLibrary", f_OpenLibrary }, { "OldOpenLibrary", f_OldOpenLibrary },
    { "CloseLibrary", f_CloseLibrary }, { "TaggedOpenLibrary", f_TaggedOpenLibrary },
    { "AddLibrary", f_AddLibrary }, { "RemLibrary", f_RemLibrary },
    { "SetFunction", f_SetFunction }, { "SumLibrary", f_SumLibrary },
    { "MakeFunctions", f_MakeFunctions },
    { "FindTask", f_FindTask }, { "Forbid", f_Forbid }, { "Permit", f_Permit },
    { "Disable", f_Disable }, { "Enable", f_Enable },
    { "SetSignal", f_SetSignal }, { "Signal", f_Signal }, { "Wait", f_Wait },
    { "AllocSignal", f_AllocSignal }, { "FreeSignal", f_FreeSignal },
    { "SetTaskPri", f_SetTaskPri }, { "SetExcept", f_SetExcept },
    { "CreateMsgPort", f_CreateMsgPort }, { "DeleteMsgPort", f_DeleteMsgPort },
    { "PutMsg", f_PutMsg }, { "GetMsg", f_GetMsg }, { "ReplyMsg", f_ReplyMsg },
    { "WaitPort", f_WaitPort }, { "AddPort", f_AddPort }, { "RemPort", f_RemPort },
    { "FindPort", f_FindPort },
    { "InitSemaphore", f_InitSemaphore }, { "ObtainSemaphore", f_ObtainSemaphore },
    { "ReleaseSemaphore", f_ReleaseSemaphore }, { "AttemptSemaphore", f_AttemptSemaphore },
    { "ObtainSemaphoreShared", f_ObtainSemaphoreShared },
    { "AttemptSemaphoreShared", f_AttemptSemaphoreShared },
    { "AddSemaphore", f_AddSemaphore }, { "FindSemaphore", f_FindSemaphore },
    { "RemSemaphore", f_RemSemaphore }, { "AddTask", f_AddTask }, { "RemTask", f_RemTask },
    { "AddDevice", f_AddDevice }, { "RemDevice", f_RemDevice },
    { "DoIO", f_DoIO }, { "SendIO", f_SendIO }, { "WaitIO", f_WaitIO },
    { "CheckIO", f_CheckIO }, { "AbortIO", f_AbortIO },
    { "CreateIORequest", f_CreateIORequest }, { "DeleteIORequest", f_DeleteIORequest },
    { "Insert", f_Insert }, { "AddHead", f_AddHead }, { "AddTail", f_AddTail },
    { "Remove", f_Remove }, { "RemHead", f_RemHead }, { "RemTail", f_RemTail },
    { "Enqueue", f_Enqueue }, { "FindName", f_FindName }, { "NewMinList", f_NewMinList },
    { "RawDoFmt", f_RawDoFmt },
    { "CacheClearU", f_CacheClearU }, { "CacheClearE", f_CacheClearE },
    { "CacheControl", f_CacheControl }, { "SuperState", f_SuperState },
    { "UserState", f_UserState }, { "Supervisor", f_Supervisor },
    { "Alert", f_Alert }, { "Debug", f_Debug }, { "SetSR", f_SetSR }, { "GetCC", f_GetCC },
    { "OpenResource", f_OpenResource }, { "OpenDevice", f_OpenDevice },
    { "CloseDevice", f_CloseDevice }, { "ColdReboot", f_ColdReboot },
    { 0, 0 }
};

/* ---- boot --------------------------------------------------------------- */

void exec_init(void) {
    SysBase = lib_create("exec.library", "exec", EB_SIZE, exec_natives, 40, 1,
                         "exec 40.1 (AmigaMac)");
    wr32(4, SysBase);
    wr16(SysBase + EB_SOFTVER, 1);
    wr16(SysBase + EB_ATTNFLAGS, AFF_68010 | AFF_68020);
    wr8(SysBase + EB_VBLANKFREQ, 50);
    wr8(SysBase + EB_POWERFREQ, 50);
    wr32(SysBase + EB_ECLOCKFREQ, 709379);
    wr8(SysBase + EB_IDNESTCNT, 0xFF);               /* -1: enabled */
    wr8(SysBase + EB_TDNESTCNT, 0xFF);
    static const int lists[] = { EB_MEMLIST, EB_RESOURCELIST, EB_DEVICELIST, EB_INTRLIST,
                                 EB_LIBLIST, EB_PORTLIST, EB_TASKREADY, EB_TASKWAIT,
                                 EB_SEMAPHORELIST };
    for (unsigned i = 0; i < sizeof lists / sizeof *lists; i++) list_new(SysBase + lists[i]);
    lib_add(SysBase);
}

/* the CLI process every command runs in */
uint32_t exec_create_process(const char *name, uint32_t stacksize) {
    uint32_t p = exec_allocmem(PR_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(p + LN_TYPE, NT_PROCESS);
    wr32(p + LN_NAME, str_alloc(name));
    wr8(p + TC_STATE, TS_RUN);
    wr32(p + TC_SIGALLOC, 0x0000FFFF);               /* system signals reserved */
    wr16(p + TC_TRAPALLOC, 0x8000);
    list_new(p + TC_MEMENTRY);
    uint32_t stack = exec_allocmem(stacksize, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(p + TC_SPLOWER, stack);
    wr32(p + TC_SPUPPER, stack + stacksize);
    /* its message port */
    wr8(p + PR_MSGPORT + LN_TYPE, NT_MSGPORT);
    wr8(p + PR_MSGPORT + MP_SIGBIT, 8);              /* SIGB_DOS */
    wr32(p + PR_MSGPORT + MP_SIGTASK, p);
    list_new(p + PR_MSGPORT + MP_MSGLIST);
    wr32(p + PR_STACKSIZE, stacksize);
    wr32(p + PR_STACKBASE, MKBADDR(stack + stacksize));
    list_new(p + PR_LOCALVARS);
    wr32(p + TC_SPREG, stack + stacksize);
    return p;
}
