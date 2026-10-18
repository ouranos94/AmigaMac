/* Core services: 68k memory, heap, traps into native code, lists, strings. */
#include "os.h"
#include "sched.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *mem;
uint32_t mem_size;
AmigaHost host;

void os_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (host.log) host.log(buf);
    else fprintf(stderr, "%s\n", buf);
}

void mem_init(uint32_t size) {
    mem = calloc(1, size);
    mem_size = size;
}

/* the hardware registers polled by programs that wait for a click or the beam */
static int hw_read(uint32_t addr, int size, uint32_t *v) {
    int b = input_buttons();
    uint64_t t = now_us();
    uint32_t line = (uint32_t)(t / 64 % 313);                /* PAL beam: 64 us per line */
    uint32_t hpos = (uint32_t)(t % 64 * 227 / 64);
    uint32_t r;
    switch (addr & ~1u) {
    case 0xBFE000: r = (b & 1) ? 0xBFBF : 0xFFFF; break;    /* CIA-A PRA: bit 6 = left button, low = pressed */
    case 0xDFF016: r = (b & 2) ? 0xFBFF : 0xFFFF; break;    /* POTGOR: bit 10 = right button */
    case 0xDFF004: r = (line >> 8) & 1; break;               /* VPOSR */
    case 0xDFF006: r = (line & 0xFF) << 8 | (hpos & 0xFF); break;   /* VHPOSR */
    case 0xDFF00A: case 0xDFF00C: case 0xDFF002: case 0xDFF01C: case 0xDFF01E: r = 0; break;
    default: return 0;
    }
    if (size == 1) *v = (addr & 1) ? (r & 0xFF) : (r >> 8);
    else if (size == 2) *v = r;
    else { uint32_t lo = 0; hw_read(addr + 2, 2, &lo); *v = r << 16 | lo; }
    return 1;
}

uint32_t mem_fault(uint32_t addr, int write, int size) {
    static uint32_t last = 0xFFFFFFFF;
    static int count;
    uint32_t v;
    if (!write && hw_read(addr, size, &v)) return v;
    if (addr == last && ++count > 3) return 0;
    if (addr != last) count = 0;
    last = addr;
    /* custom chips / CIAs: no hardware here, the OS is native */
    if ((addr >= 0xBFD000 && addr < 0xBFF000) || (addr >= 0xDFF000 && addr < 0xE00000))
        os_log("accès matériel ignoré : %s.%c $%06X (PC=$%06X)", write ? "écriture" : "lecture",
               size == 1 ? 'b' : size == 2 ? 'w' : 'l', addr, sched_current() ? sched_current()->cpu.insn_pc : 0);
    else {
        os_log("accès mémoire hors limites : %s $%08X (PC=$%06X)", write ? "écriture" : "lecture",
               addr, sched_current() ? sched_current()->cpu.insn_pc : 0);
        static int shown;
        if (cpu68k_trace && !shown++ && sched_current()) {
            M68K *c = &sched_current()->cpu;
            char line[600];
            int k = 0;
            for (unsigned i = 0; i < 32; i++) k += snprintf(line + k, sizeof line - (size_t)k, " %06X", c->hist[(c->hidx + i) & 31]);
            os_log("dernières instructions :%s", line);
        }
    }
    return 0;
}

/* ---- heap: first-fit free list kept natively, 8-byte granularity -------- */

typedef struct { uint32_t addr, size; } Block;
static Block *freelist;
static int nfree, capfree;
static uint32_t heap_total;

void heap_init(uint32_t start, uint32_t end) {
    start = (start + 7) & ~7u;
    freelist = malloc(sizeof(Block) * 64);
    capfree = 64;
    /* two pieces, around the hardware range */
    freelist[0].addr = start;
    freelist[0].size = (end < HW_LO ? end : HW_LO) - start;
    nfree = 1;
    heap_total = freelist[0].size;
    if (end > HW_HI) {
        freelist[1].addr = HW_HI;
        freelist[1].size = end - HW_HI;
        nfree = 2;
        heap_total += freelist[1].size;
    }
}

uint32_t heap_alloc(uint32_t size, uint32_t flags) {
    if (!size) return 0;
    size = (size + 7) & ~7u;
    int best = -1;
    for (int i = 0; i < nfree; i++) {
        if (freelist[i].size < size) continue;
        if (flags & MEMF_REVERSE) best = i;          /* highest fitting block */
        else { best = i; break; }
    }
    if (best < 0) return 0;
    uint32_t a;
    if (flags & MEMF_REVERSE) {
        a = freelist[best].addr + freelist[best].size - size;
        freelist[best].size -= size;
    } else {
        a = freelist[best].addr;
        freelist[best].addr += size;
        freelist[best].size -= size;
    }
    if (!freelist[best].size) {
        memmove(&freelist[best], &freelist[best + 1], sizeof(Block) * (nfree - best - 1));
        nfree--;
    }
    if (flags & MEMF_CLEAR) memset(mem + a, 0, size);
    return a;
}

void heap_free(uint32_t addr, uint32_t size) {
    if (!addr || !size) return;
    size = (size + 7) & ~7u;
    int i = 0;
    while (i < nfree && freelist[i].addr < addr) i++;
    /* merge with neighbours */
    if (i > 0 && freelist[i - 1].addr + freelist[i - 1].size == addr) {
        freelist[i - 1].size += size;
        if (i < nfree && addr + size == freelist[i].addr) {
            freelist[i - 1].size += freelist[i].size;
            memmove(&freelist[i], &freelist[i + 1], sizeof(Block) * (nfree - i - 1));
            nfree--;
        }
        return;
    }
    if (i < nfree && addr + size == freelist[i].addr) {
        freelist[i].addr = addr;
        freelist[i].size += size;
        return;
    }
    if (nfree == capfree) {
        capfree *= 2;
        freelist = realloc(freelist, sizeof(Block) * capfree);
    }
    memmove(&freelist[i + 1], &freelist[i], sizeof(Block) * (nfree - i));
    freelist[i].addr = addr;
    freelist[i].size = size;
    nfree++;
}

uint32_t heap_avail(uint32_t flags) {
    uint32_t total = 0, largest = 0;
    for (int i = 0; i < nfree; i++) {
        total += freelist[i].size;
        if (freelist[i].size > largest) largest = freelist[i].size;
    }
    if (flags & MEMF_TOTAL) return heap_total;
    return (flags & MEMF_LARGEST) ? largest : total;
}

/* ---- traps -------------------------------------------------------------- */

typedef struct { const char *name; NativeFn fn; } Trap;
static Trap traps[4096];
static int ntraps;
uint32_t stub_exit, stub_return, stub_taskexit;
static int call_depth;

uint32_t trap_new(const char *name, NativeFn fn) {
    if (ntraps >= 4096) { os_log("plus de pièges disponibles"); return 0; }
    traps[ntraps].name = name;
    traps[ntraps].fn = fn;
    return ntraps++;
}

static uint32_t t_exit(M68K *c) { c->stop = 2; return c->d[0]; }
static uint32_t t_return(M68K *c) { c->stop = 3; return c->d[0]; }
static uint32_t t_taskexit(M68K *c) { c->stop = 2; return c->d[0]; }

void trap_init(void) {
    ntraps = 0;
    trap_new("<exit>", t_exit);
    trap_new("<return>", t_return);
    /* stubs in low memory, after the exception vectors */
    stub_exit = 0x400;
    wr16(stub_exit, 0xA000);
    stub_return = 0x404;
    wr16(stub_return, 0xA001);
    trap_new("<taskexit>", t_taskexit);
    stub_taskexit = 0x408;
    wr16(stub_taskexit, 0xA002);
}

void aline_handler(M68K *c, uint16_t op) {
    int id = op & 0xFFF;
    if (id >= ntraps) {
        c->pc = c->insn_pc;
        cpu68k_exception(c, VEC_LINE_A);
        return;
    }
    Trap *t = &traps[id];
    if (!t->fn) {
        os_log("fonction non implémentée : %s (appelée depuis $%06X)", t->name, rd32(c->a[7]));
        c->d[0] = 0;
        return;
    }
    static int trace = -1;
    if (trace < 0) { trace = getenv("AMIGAMAC_TRACE") != 0; cpu68k_trace = trace; }
    if (trace) os_log("appel %s depuis $%06X (d0=%08X d1=%08X a0=%08X a1=%08X)", t->name, rd32(c->a[7]), c->d[0], c->d[1], c->a[0], c->a[1]);
    c->d[0] = t->fn(c);
    if (trace) os_log("  -> %08X", c->d[0]);
    /* a Signal/PutMsg in that call may have woken a higher-priority task */
    if (sched_need_resched && !sched_forbidden()) sched_preempt_point();
}

void cpu_call(M68K *c, uint32_t addr) {
    uint32_t pc = c->pc, ipc = c->insn_pc;
    uint16_t ir = c->ir;
    cpu68k_push32(c, stub_return);
    c->pc = addr;
    call_depth++;
    sched_run_cpu(c);                        /* stops on <return> (3) or an exit (2) */
    call_depth--;
    if (c->stop == 3) c->stop = 0;
    c->pc = pc;
    c->insn_pc = ipc;
    c->ir = ir;
}

/* ---- libraries ---------------------------------------------------------- */

static const FdLib *fd_find(const char *name) {
    for (int i = 0; i < fd_lib_count; i++)
        if (!strcmp(fd_libs[i].lib, name)) return &fd_libs[i];
    return 0;
}

int lvo_of(const char *lib, const char *fn) {
    const FdLib *fd = fd_find(lib);
    if (fd) for (int i = 0; i < fd->count; i++) if (!strcmp(fd->funcs[i].name, fn)) return fd->funcs[i].lvo;
    os_log("LVO introuvable : %s.%s", lib, fn);
    return 0;
}

/* call a library function from native code, arguments in .fd order:
   lib_call(c, IntuitionBase, "intuition", "OpenWindowTagList", nw, tags).
   Goes through the jump table, so SetFunction patches are honoured; all
   registers but D0 are preserved. */
uint32_t lib_call(M68K *c, uint32_t base, const char *lib, const char *fn, ...) {
    const FdLib *fd = fd_find(lib);
    const FdFunc *f = 0;
    if (fd) for (int i = 0; i < fd->count; i++) if (!strcmp(fd->funcs[i].name, fn)) { f = &fd->funcs[i]; break; }
    if (!f) { os_log("lib_call : %s.%s inconnue", lib, fn); return 0; }
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    va_list ap;
    va_start(ap, fn);
    for (const char *r = f->regs; r[0] && r[1]; ) {
        uint32_t v = va_arg(ap, uint32_t);
        int n = r[1] - '0';
        if (r[0] == 'd') c->d[n] = v; else c->a[n] = v;
        r += 2;
        if (*r == '/' || *r == ',') r++;
    }
    va_end(ap);
    c->a[6] = base;
    cpu_call(c, base - f->lvo);
    uint32_t r = c->d[0];
    memcpy(c->d, d, sizeof d);
    a[7] = c->a[7];
    memcpy(c->a, a, sizeof a);
    return r;
}

uint32_t lib_create(const char *name, const char *fdname, uint32_t possize,
                    const NativeEntry *natives, int version, int revision, const char *idstring) {
    const FdLib *fd = fdname ? fd_find(fdname) : 0;
    int maxlvo = 36;                                 /* Open/Close/Expunge/Reserved/(BeginIO/AbortIO) */
    if (fd) for (int i = 0; i < fd->count; i++) if (fd->funcs[i].lvo > maxlvo) maxlvo = fd->funcs[i].lvo;
    uint32_t negsize = (maxlvo + 6 + 3) & ~3u;
    possize = (possize + 3) & ~3u;
    uint32_t mem_ = heap_alloc(negsize + possize, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t base = mem_ + negsize;

    /* every slot first gets a trap that names itself */
    for (int lvo = 6; lvo <= maxlvo; lvo += 6) {
        const char *fname = 0;
        static const char *std[] = { 0, "Open", "Close", "Expunge", "Reserved", "BeginIO", "AbortIO" };
        if (fd) for (int i = 0; i < fd->count; i++) if (fd->funcs[i].lvo == lvo) fname = fd->funcs[i].name;
        if (!fname && lvo <= 36) fname = std[lvo / 6];
        char full[96];
        snprintf(full, sizeof full, "%s.%s", name, fname ? fname : "?");
        NativeFn fn = 0;
        for (const NativeEntry *n = natives; n && n->name; n++)
            if (fname && !strcmp(n->name, fname)) { fn = n->fn; break; }
        uint32_t id = trap_new(strdup(full), fn);
        uint32_t slot = base - lvo;
        wr16(slot, 0xA000 | id);
        wr16(slot + 2, 0x4E75);                      /* RTS */
        wr16(slot + 4, 0x4E71);
    }
    wr8(base + LN_TYPE, NT_LIBRARY);
    wr32(base + LN_NAME, str_alloc(name));
    wr8(base + LIB_FLAGS, 0x06);                     /* LIBF_SUMUSED | LIBF_CHANGED */
    wr16(base + LIB_NEGSIZE, negsize);
    wr16(base + LIB_POSSIZE, possize);
    wr16(base + LIB_VERSION, version);
    wr16(base + LIB_REVISION, revision);
    wr32(base + LIB_IDSTRING, str_alloc(idstring ? idstring : name));
    return base;
}

void lib_add(uint32_t base) {
    list_enqueue(SysBase + EB_LIBLIST, base);
}

/* ---- strings ------------------------------------------------------------ */

uint32_t str_alloc(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = heap_alloc(n, MEMF_PUBLIC);
    if (a) memcpy(mem + a, s, n);
    return a;
}

int str_get(uint32_t addr, char *buf, int max) {
    int i = 0;
    if (!addr) { buf[0] = 0; return 0; }
    while (i < max - 1) {
        uint32_t ch = rd8(addr + i);
        if (!ch) break;
        buf[i++] = (char)ch;
    }
    buf[i] = 0;
    return i;
}

int bstr_get(uint32_t bptr, char *buf, int max) {
    uint32_t a = BADDR(bptr);
    if (!a) { buf[0] = 0; return 0; }
    int n = rd8(a);
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; i++) buf[i] = (char)rd8(a + 1 + i);
    buf[n] = 0;
    return n;
}

/* ---- exec lists (same semantics as exec's own) -------------------------- */

void list_new(uint32_t l) {
    wr32(l + LH_HEAD, l + LH_TAIL);
    wr32(l + LH_TAIL, 0);
    wr32(l + LH_TAILPRED, l + LH_HEAD);
}

void list_addhead(uint32_t l, uint32_t n) {
    uint32_t first = rd32(l + LH_HEAD);
    wr32(n + LN_SUCC, first);
    wr32(n + LN_PRED, l + LH_HEAD);
    wr32(first + LN_PRED, n);
    wr32(l + LH_HEAD, n);
}

void list_addtail(uint32_t l, uint32_t n) {
    uint32_t last = rd32(l + LH_TAILPRED);
    wr32(n + LN_SUCC, l + LH_TAIL);
    wr32(n + LN_PRED, last);
    wr32(last + LN_SUCC, n);
    wr32(l + LH_TAILPRED, n);
}

void list_remove(uint32_t n) {
    uint32_t succ = rd32(n + LN_SUCC), pred = rd32(n + LN_PRED);
    wr32(pred + LN_SUCC, succ);
    wr32(succ + LN_PRED, pred);
}

uint32_t list_remhead(uint32_t l) {
    uint32_t n = rd32(l + LH_HEAD);
    if (!rd32(n + LN_SUCC)) return 0;
    list_remove(n);
    return n;
}

uint32_t list_remtail(uint32_t l) {
    uint32_t n = rd32(l + LH_TAILPRED);
    if (!rd32(n + LN_PRED)) return 0;
    list_remove(n);
    return n;
}

void list_insert(uint32_t l, uint32_t n, uint32_t pred) {
    if (!pred) { list_addhead(l, n); return; }
    uint32_t succ = rd32(pred + LN_SUCC);
    if (!succ) { list_addtail(l, n); return; }          /* pred is the tail node */
    wr32(n + LN_SUCC, succ);
    wr32(n + LN_PRED, pred);
    wr32(succ + LN_PRED, n);
    wr32(pred + LN_SUCC, n);
}

void list_enqueue(uint32_t l, uint32_t n) {
    int8_t pri = (int8_t)rd8(n + LN_PRI);
    uint32_t cur = rd32(l + LH_HEAD);
    while (rd32(cur + LN_SUCC)) {
        if ((int8_t)rd8(cur + LN_PRI) < pri) break;
        cur = rd32(cur + LN_SUCC);
    }
    list_insert(l, n, rd32(cur + LN_PRED));
}

uint32_t list_findname(uint32_t start, const char *name) {
    /* start is a list or a node: search from its successor, like exec */
    uint32_t cur = rd32(start + LN_SUCC);
    char buf[256];
    while (cur && rd32(cur + LN_SUCC)) {
        str_get(rd32(cur + LN_NAME), buf, sizeof buf);
        if (!strcmp(buf, name)) return cur;
        cur = rd32(cur + LN_SUCC);
    }
    return 0;
}
