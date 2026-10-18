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
#include <sys/stat.h>

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


/* ---- libraries and devices on disk (what ramlib does) ------------------------------ */

static void init_struct(uint32_t table, uint32_t mem, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) wr8(mem + i, 0);
    uint32_t p = table, dst = mem;
    for (int guard = 0; guard < 10000; guard++) {
        uint32_t cmd = rd8(p);
        if (!cmd) break;
        int type = (int)(cmd >> 6), sz = (int)((cmd >> 4) & 3), cnt = (int)(cmd & 15) + 1;
        if (type == 3) { dst = mem + (rd32(p) & 0xFFFFFF); p += 4; }
        else if (type == 2) { dst = mem + rd8(p + 1); p += 2; }
        else p++;
        int esz = sz == 0 ? 4 : sz == 1 ? 2 : 1;
        if (sz == 3) break;
        if (esz > 1) p = (p + 1) & ~1u;
        for (int i = 0; i < cnt; i++) {
            uint32_t src = type == 1 ? p : p + (uint32_t)(i * esz);
            if (esz == 4) wr32(dst, rd32(src)); else if (esz == 2) wr16(dst, rd16(src)); else wr8(dst, rd8(src));
            dst += (uint32_t)esz;
        }
        p += type == 1 ? (uint32_t)esz : (uint32_t)(esz * cnt);
        p = (p + 1) & ~1u;                                    /* commands start on a word */
    }
}
static uint32_t f_InitStruct(M68K *c) { init_struct(c->a[1], c->a[2], c->d[0]); return 0; }

static uint32_t make_library(M68K *c, uint32_t funcs, uint32_t data, uint32_t init, uint32_t size, uint32_t seglist) {
    uint32_t n = 0, disp = 0;
    if (rd16(funcs) == 0xFFFF) { disp = funcs; funcs += 2; while ((int16_t)rd16(funcs + 2 * n) != -1) n++; }
    else while (rd32(funcs + 4 * n) != 0xFFFFFFFF) n++;
    uint32_t neg = (n * 6 + 3) & ~3u;
    uint32_t blk = exec_allocmem(neg + size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!blk) return 0;
    uint32_t lib = blk + neg;
    uint32_t slot = lib - 6;
    for (uint32_t i = 0; i < n; i++, slot -= 6) {
        wr16(slot, 0x4EF9);
        wr32(slot + 2, disp ? disp + (uint32_t)(int16_t)rd16(funcs + 2 * i) : rd32(funcs + 4 * i));
    }
    if (data) init_struct(data, lib, 0);
    wr16(lib + LIB_NEGSIZE, neg);
    wr16(lib + LIB_POSSIZE, size);
    if (init) {
        uint32_t d[8], a[8];
        memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
        c->d[0] = lib; c->a[0] = seglist; c->a[6] = SysBase;
        cpu_call(c, init);
        lib = c->d[0];
        memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
    }
    return lib;
}
static uint32_t f_MakeLibrary(M68K *c) { return make_library(c, c->a[0], c->a[1], c->a[2], c->d[0], c->d[1]); }

/* InitResident(resident, segList) */
static uint32_t init_resident(M68K *c, uint32_t rt, uint32_t seglist) {
    uint32_t flags = rd8(rt + 10), init = rd32(rt + 22), type = rd8(rt + 12);
    if (!init) return 0;
    if (flags & 0x80) {                                           /* RTF_AUTOINIT */
        uint32_t lib = make_library(c, rd32(init + 4), rd32(init + 8), rd32(init + 12), rd32(init), seglist);
        if (!lib) return 0;
        if (type == NT_DEVICE) list_enqueue(SysBase + EB_DEVICELIST, lib);
        else if (type == NT_LIBRARY) lib_add(lib);
        return lib;
    }
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
    c->d[0] = 0; c->a[0] = seglist; c->a[6] = SysBase;
    cpu_call(c, init);
    uint32_t r = c->d[0];
    memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
    return r;
}
static uint32_t f_InitResident(M68K *c) { return init_resident(c, c->a[1], c->d[1]); }

/* the RomTag of a loaded file: RTC_MATCHWORD followed by a pointer to itself */
static uint32_t find_romtag(uint32_t seglist) {
    for (uint32_t seg = seglist; seg; seg = rd32(BADDR(seg))) {
        uint32_t start = BADDR(seg) + 4, size = rd32(BADDR(seg) - 4);
        if (size < 8) continue;
        for (uint32_t a = start; a + 26 <= start + size - 8; a += 2)
            if (rd16(a) == 0x4AFC && rd32(a + 2) == a) return a;
    }
    return 0;
}

/* a library or device not in memory: from LIBS: or DEVS: */
static uint32_t load_from_disk(M68K *c, const char *name, int device) {
    char path[300], hp[1024];
    if (strchr(name, ':') || strchr(name, '/')) snprintf(path, sizeof path, "%s", name);
    else snprintf(path, sizeof path, "%s:%s", device ? "DEVS" : "LIBS", name);
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    struct stat st;
    if (stat(hp, &st) || S_ISDIR(st.st_mode)) {
        if (strchr(name, ':') || strchr(name, '/')) return 0;
        /* also next to the program, as many do */
        snprintf(path, sizeof path, "PROGDIR:%s%s", device ? "Devs/" : "Libs/", name);
        if (!dos_resolve(path, hp, sizeof hp) || stat(hp, &st)) return 0;
    }
    uint32_t seg = dos_loadseg(hp);
    if (!seg) return 0;
    uint32_t rt = find_romtag(seg);
    if (!rt) { os_log("%s : pas de RomTag", path); dos_unloadseg(seg); return 0; }
    uint32_t base = init_resident(c, rt, seg);
    if (!base) { dos_unloadseg(seg); return 0; }
    os_log("%s chargé depuis %s", name, path);
    return base;
}

/* a 68k library's own Open: through its vector (native ones answer directly) */
static uint32_t lib_open_vector(M68K *c, uint32_t lib, uint32_t version) {
    if (rd16(lib - 6) != 0x4EF9) return lib;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
    c->d[0] = version; c->a[6] = lib;
    cpu_call(c, lib - 6);
    uint32_t r = c->d[0];
    memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
    return r;
}

uint32_t exec_openlibrary(const char *name, uint32_t version) {
    /* exec searches by node name; strip any path like "libs:foo.library" */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : (strchr(name, ':') ? strchr(name, ':') + 1 : name);
    uint32_t lib = list_findname(SysBase + EB_LIBLIST, base);
    NTask *me = sched_current();
    if (!lib && me) lib = load_from_disk(&me->cpu, name, 0);
    if (!lib) {
        os_log("OpenLibrary(\"%s\", %u) : bibliothèque absente", name, version);
        return 0;
    }
    if (rd16(lib + LIB_VERSION) < version) {
        os_log("OpenLibrary(\"%s\", %u) : version %u seulement", name, version, rd16(lib + LIB_VERSION));
        return 0;
    }
    if (rd16(lib - 6) == 0x4EF9 && me) return lib_open_vector(&me->cpu, lib, version);
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
    if (!lib) return 0;
    if (rd16(lib - 12) == 0x4EF9) {                            /* a 68k library: its Close */
        uint32_t d[8], a[8];
        memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
        c->a[6] = lib;
        cpu_call(c, lib - 12);
        memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
        return 0;                                               /* (expunged seglists stay loaded) */
    }
    if (rd16(lib + LIB_OPENCNT)) wr16(lib + LIB_OPENCNT, rd16(lib + LIB_OPENCNT) - 1);
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

/* Forbid/Permit/Disable/Enable touch no register on the Amiga: D0 comes back as it was */
static uint32_t f_Forbid(M68K *c) { wr8(SysBase + EB_TDNESTCNT, rd8(SysBase + EB_TDNESTCNT) + 1); return c->d[0]; }
static uint32_t f_Permit(M68K *c) {
    (void)c;
    wr8(SysBase + EB_TDNESTCNT, rd8(SysBase + EB_TDNESTCNT) - 1);
    if (sched_need_resched) sched_preempt_point();
    return c->d[0];
}
static uint32_t f_Disable(M68K *c) { wr8(SysBase + EB_IDNESTCNT, rd8(SysBase + EB_IDNESTCNT) + 1); return c->d[0]; }
static uint32_t f_Enable(M68K *c) {
    (void)c;
    wr8(SysBase + EB_IDNESTCNT, rd8(SysBase + EB_IDNESTCNT) - 1);
    if (sched_need_resched) sched_preempt_point();
    return c->d[0];
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

/* Wait(0) from a program sleeps for good: subtasks end that way and wait for
   their parent's RemTask() */
static uint32_t f_Wait(M68K *c) {
    if (c->d[0]) return exec_wait(c->d[0]);
    uint32_t t = current_task;
    for (;;) {
        wr32(t + TC_SIGWAIT, 0);
        wr8(t + TC_STATE, TS_WAIT);
        list_addtail(SysBase + EB_TASKWAIT, t);
        sched_block();
    }
}

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
    c->a[0] = p;                 /* the ROM leaves the port in A0 too, and some programs count on it */
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
    if (port && port == dos_handler_port) {
        /* a DOS packet for a file or lock: done now, then replied like a handler would */
        uint32_t dp = rd32(msg + LN_NAME), rp = rd32(dp + 4);
        dos_packet(dp);
        wr32(dp + 4, port);
        if (rp) { put_msg(rp, msg); wr8(msg + LN_TYPE, NT_REPLYMSG); }
        return;
    }
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

void exec_put_msg(uint32_t port, uint32_t msg) { put_msg(port, msg); }

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
    if (!dev) dev = load_from_disk(c, name, 1);
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
/* StackSwap(newStack A0): struct StackSwapStruct { stk_Lower, stk_Upper, stk_Pointer }.
   The JSR's return address moves to the new stack, so the RTS of the
   jump table lands back in the caller with the new SP. */
static uint32_t f_StackSwap(M68K *c) {
    uint32_t sss = c->a[0], t = this_task();
    uint32_t ret = rd32(c->a[7]), old_sp = c->a[7] + 4;
    uint32_t nlo = rd32(sss), nhi = rd32(sss + 4), nsp = rd32(sss + 8);
    if (t) {
        wr32(sss, rd32(t + TC_SPLOWER));
        wr32(sss + 4, rd32(t + TC_SPUPPER));
        wr32(t + TC_SPLOWER, nlo);
        wr32(t + TC_SPUPPER, nhi);
    }
    wr32(sss + 8, old_sp);
    c->a[7] = nsp - 4;
    wr32(c->a[7], ret);
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

/* Allocate(memHeader, size) / Deallocate(memHeader, block, size): a program's own
   memory region, MemHeader {node, mh_Attributes 14, mh_First 16, mh_Lower 20,
   mh_Upper 24, mh_Free 28}, free MemChunks {mc_Next, mc_Bytes} in address order */
static uint32_t f_Allocate(M68K *c) {
    uint32_t mh = c->a[0], size = (c->d[0] + 7) & ~7u;
    if (!size || size > rd32(mh + 28)) return 0;
    for (uint32_t prev = mh + 16, mc = rd32(prev); mc; prev = mc, mc = rd32(mc)) {
        uint32_t bytes = rd32(mc + 4);
        if (bytes < size) continue;
        if (bytes == size) wr32(prev, rd32(mc));
        else {
            uint32_t rest = mc + size;
            wr32(rest, rd32(mc));
            wr32(rest + 4, bytes - size);
            wr32(prev, rest);
        }
        wr32(mh + 28, rd32(mh + 28) - size);
        return mc;
    }
    return 0;
}
static uint32_t f_Deallocate(M68K *c) {
    uint32_t mh = c->a[0], blk = c->a[1] & ~7u, size = (c->d[0] + (c->a[1] & 7) + 7) & ~7u;
    if (!blk || !size) return 0;
    uint32_t prev = mh + 16, mc = rd32(prev);
    while (mc && mc < blk) { prev = mc; mc = rd32(mc); }
    wr32(blk, mc);
    wr32(blk + 4, size);
    if (mc && blk + size == mc) { wr32(blk, rd32(mc)); wr32(blk + 4, size + rd32(mc + 4)); }   /* join the next */
    if (prev != mh + 16 && prev + rd32(prev + 4) == blk) { wr32(prev, rd32(blk)); wr32(prev + 4, rd32(prev + 4) + rd32(blk + 4)); }
    else wr32(prev, blk);
    wr32(mh + 28, rd32(mh + 28) + size);
    return 0;
}

extern uint32_t exec_add_int_server(uint32_t num, uint32_t is);
extern uint32_t exec_rem_int_server(uint32_t num, uint32_t is);
extern uint32_t exec_set_int_vector(uint32_t num, uint32_t is);
/* Cause(interrupt): a software interrupt, run now: A1 = is_Data, A5 = is_Code */
static uint32_t f_Cause(M68K *c) {
    uint32_t is = c->a[1];
    if (!is || !rd32(is + 18)) return 0;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
    c->a[1] = rd32(is + 14);
    c->a[5] = rd32(is + 18);
    c->a[6] = SysBase;
    cpu_call(c, rd32(is + 18));
    memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
    return 0;
}
static uint32_t f_AddIntServer(M68K *c) { return exec_add_int_server(c->d[0], c->a[1]); }
static uint32_t f_RemIntServer(M68K *c) { return exec_rem_int_server(c->d[0], c->a[1]); }
static uint32_t f_SetIntVector(M68K *c) { return exec_set_int_vector(c->d[0], c->a[1]); }

static const NativeEntry exec_natives[] = {
    { "AllocMem", f_AllocMem }, { "FreeMem", f_FreeMem }, { "AllocVec", f_AllocVec },
    { "Allocate", f_Allocate }, { "Deallocate", f_Deallocate },
    { "Cause", f_Cause }, { "AddIntServer", f_AddIntServer }, { "RemIntServer", f_RemIntServer }, { "SetIntVector", f_SetIntVector },
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
    { "MakeFunctions", f_MakeFunctions }, { "MakeLibrary", f_MakeLibrary },
    { "InitStruct", f_InitStruct }, { "InitResident", f_InitResident },
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
    { "Alert", f_Alert }, { "StackSwap", f_StackSwap }, { "Debug", f_Debug }, { "SetSR", f_SetSR }, { "GetCC", f_GetCC },
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
    wr16(SysBase + EB_ATTNFLAGS, AFF_68010 | AFF_68020 | AFF_68881 | AFF_68882);
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
