/* iffparse.library, written from the iffparse autodocs and libraries/iffparse.h.
 *
 * The parser state lives natively per IFFHandle; the ContextNodes, stored
 * properties and collection items that programs read are real structures in
 * 68k memory. Streams are DOS file handles (InitIFFasDOS: dos.library Read,
 * Write, Seek) or the program's own hook (InitIFF), called with IFFStreamCmd. */
#include "os.h"
#include "gfx.h"
#include "ntk.h"
#include <stdlib.h>
#include <string.h>

uint32_t IFFParseBase;
extern uint32_t DOSBase;

#define IFFF_WRITE 1
#define IFFF_RSEEK 4
#define IFFERR_EOF (-1)
#define IFFERR_EOC (-2)
#define IFFERR_NOSCOPE (-3)
#define IFFERR_NOMEM (-4)
#define IFFERR_READ (-5)
#define IFFERR_WRITE (-6)
#define IFFERR_SEEK (-7)
#define IFFERR_MANGLED (-8)
#define IFFERR_SYNTAX (-9)
#define IFFERR_NOTIFF (-10)
#define IFFERR_NOHOOK (-11)
#define IFF_RETURN2CLIENT (-12)
#define IFFCMD_READ 2
#define IFFCMD_WRITE 3
#define IFFCMD_SEEK 4
#define IFFCMD_ENTRY 5
#define IFFCMD_EXIT 6
#define IFFPARSE_SCAN 0
#define IFFPARSE_STEP 1
#define IFFSIZE_UNKNOWN (-1)
#define ID(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define ID_FORM ID('F', 'O', 'R', 'M')
#define ID_LIST ID('L', 'I', 'S', 'T')
#define ID_CAT ID('C', 'A', 'T', ' ')
#define ID_PROP ID('P', 'R', 'O', 'P')
#define ID_NULL 0

/* IFFHandle: iff_Stream, iff_Flags, iff_Depth; ContextNode: MinNode, cn_ID, cn_Type, cn_Size, cn_Scan */
#define IFF_STREAM 0
#define IFF_FLAGS 4
#define IFF_DEPTH 8
#define CN_ID 8
#define CN_TYPE 12
#define CN_SIZE 16
#define CN_SCAN 20
#define CN_SIZEOF 24

enum { H_STOP, H_PROP, H_COLL, H_EXIT_STOP, H_ENTRY, H_EXIT };
typedef struct { int kind; uint32_t type, id; uint32_t hook, obj, pos; } Handler;
typedef struct Item { uint32_t type, id, ident; uint32_t mem, size; int depth; uint32_t next68; struct Item *next; } Item;   /* props, collections, local items */

typedef struct Ctx { uint32_t node; uint32_t id, type; int32_t size, scan; int container, unknown; uint32_t sizepos; } Ctx;

typedef struct IFF {
    uint32_t iff;
    int dos;                         /* InitIFFasDOS */
    uint32_t hook;                   /* stream hook (InitIFF) */
    int open, write, need_pop, entered, pending_eoc;
    Ctx st[64];
    int depth;
    uint32_t root;                   /* ContextNode of the stream itself */
    Handler h[256];
    int nh;
    Item *items;
    uint32_t pos;                    /* stream position */
    struct IFF *next;
} IFF;

static IFF *iffs;
static IFF *iff_of(uint32_t a) { for (IFF *f = iffs; f; f = f->next) if (f->iff == a) return f; return 0; }

/* ---- stream ------------------------------------------------------------------------ */

static int32_t s_cmd(M68K *c, IFF *f, int cmd, uint32_t buf, int32_t n) {
    uint32_t stream = rd32(f->iff + IFF_STREAM);
    if (f->dos) {
        if (cmd == IFFCMD_READ) return (int32_t)lib_call(c, DOSBase, "dos", "Read", stream, buf, (uint32_t)n);
        if (cmd == IFFCMD_WRITE) return (int32_t)lib_call(c, DOSBase, "dos", "Write", stream, buf, (uint32_t)n);
        if (cmd == IFFCMD_SEEK) return (int32_t)lib_call(c, DOSBase, "dos", "Seek", stream, (uint32_t)n, 0 /* OFFSET_CURRENT */) == -1 ? -1 : 0;
        return 0;
    }
    if (!f->hook) return -1;
    uint32_t msg = exec_allocmem(12, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(msg, (uint32_t)cmd);
    wr32(msg + 4, buf);
    wr32(msg + 8, (uint32_t)n);
    int32_t r = (int32_t)call_hook(c, f->hook, f->iff, msg);
    exec_freemem(msg, 12);
    /* hooks return 0 for success */
    if (cmd == IFFCMD_READ || cmd == IFFCMD_WRITE) return r == 0 ? n : -1;
    return r;
}

static int s_read(M68K *c, IFF *f, uint32_t buf, int32_t n) {
    if (n <= 0) return 0;
    int32_t r = s_cmd(c, f, IFFCMD_READ, buf, n);
    if (r > 0) f->pos += (uint32_t)r;
    return r;
}

static int s_read_native(M68K *c, IFF *f, uint8_t *dst, int n) {
    uint32_t b = exec_allocmem((uint32_t)n + 4, MEMF_PUBLIC);
    int r = s_read(c, f, b, n);
    for (int i = 0; i < r; i++) dst[i] = (uint8_t)rd8(b + (uint32_t)i);
    exec_freemem(b, (uint32_t)n + 4);
    return r;
}

static int s_write(M68K *c, IFF *f, uint32_t buf, int32_t n) {
    if (n <= 0) return 0;
    int32_t r = s_cmd(c, f, IFFCMD_WRITE, buf, n);
    if (r > 0) f->pos += (uint32_t)r;
    return r;
}

static int s_write_native(M68K *c, IFF *f, const uint8_t *src, int n) {
    uint32_t b = exec_allocmem((uint32_t)n + 4, MEMF_PUBLIC);
    for (int i = 0; i < n; i++) wr8(b + (uint32_t)i, src[i]);
    int r = s_write(c, f, b, n);
    exec_freemem(b, (uint32_t)n + 4);
    return r;
}

/* skip forward: seek when the stream can, else read and drop */
static int s_skip(M68K *c, IFF *f, int32_t n) {
    if (n <= 0) return 0;
    if (f->dos || (rd32(f->iff + IFF_FLAGS) & 6)) {
        if (s_cmd(c, f, IFFCMD_SEEK, 0, n) == 0) { f->pos += (uint32_t)n; return 0; }
    }
    uint8_t tmp[512];
    while (n > 0) {
        int k = n > 512 ? 512 : n, r = s_read_native(c, f, tmp, k);
        if (r <= 0) return IFFERR_READ;
        n -= r;
    }
    return 0;
}

/* ---- contexts ------------------------------------------------------------------------ */

static void sync_node(Ctx *x) {
    wr32(x->node + CN_ID, x->id);
    wr32(x->node + CN_TYPE, x->type);
    wr32(x->node + CN_SIZE, (uint32_t)x->size);
    wr32(x->node + CN_SCAN, (uint32_t)x->scan);
}

static Ctx *push(IFF *f, uint32_t id, uint32_t type, int32_t size) {
    if (f->depth >= 64) return 0;
    Ctx *x = &f->st[f->depth++];
    memset(x, 0, sizeof *x);
    x->node = exec_allocmem(CN_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    x->id = id;
    x->type = type;
    x->size = size;
    x->container = id == ID_FORM || id == ID_LIST || id == ID_CAT || id == ID_PROP;
    sync_node(x);
    wr32(f->iff + IFF_DEPTH, (uint32_t)f->depth);
    return x;
}

static void purge_items(IFF *f, int depth) {
    for (Item **pp = &f->items; *pp;) {
        Item *it = *pp;
        if (it->depth >= depth) { *pp = it->next; if (it->mem) exec_freemem(it->mem, it->size); free(it); }
        else pp = &it->next;
    }
}

static void pop(IFF *f) {
    if (!f->depth) return;
    Ctx *x = &f->st[--f->depth];
    purge_items(f, f->depth + 1);
    exec_freemem(x->node, CN_SIZEOF);
    wr32(f->iff + IFF_DEPTH, (uint32_t)f->depth);
}

static Handler *handler(IFF *f, int kind, uint32_t type, uint32_t id) {
    for (int i = 0; i < f->nh; i++)
        if (f->h[i].kind == kind && f->h[i].type == type && f->h[i].id == id) return &f->h[i];
    return 0;
}

static int32_t add_handler(IFF *f, int kind, uint32_t type, uint32_t id, uint32_t hook, uint32_t obj) {
    if (f->nh >= 256) return IFFERR_NOMEM;
    Handler *h = &f->h[f->nh++];
    h->kind = kind; h->type = type; h->id = id; h->hook = hook; h->obj = obj;
    return 0;
}

/* the context that receives properties: the innermost FORM, LIST or PROP */
static int prop_scope(IFF *f) {
    for (int i = f->depth - 1; i >= 0; i--)
        if (f->st[i].id == ID_FORM || f->st[i].id == ID_LIST || f->st[i].id == ID_PROP) return i + 1;
    return 0;
}

/* read the current chunk's data into a stored property or collection item */
static int32_t store_chunk(M68K *c, IFF *f, int coll) {
    Ctx *x = &f->st[f->depth - 1];
    int32_t n = x->size - x->scan;
    uint32_t mem = exec_allocmem((uint32_t)(coll ? 12 : 8) + (uint32_t)n + 2, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t data = mem + (coll ? 12 : 8);
    if (s_read(c, f, data, n) != n) { exec_freemem(mem, (uint32_t)(coll ? 12 : 8) + (uint32_t)n + 2); return IFFERR_READ; }
    x->scan += n;
    Item *it = calloc(1, sizeof *it);
    it->type = x->type;
    it->id = x->id;
    it->ident = coll ? ID('c', 'o', 'l', 'l') : ID('p', 'r', 'o', 'p');
    it->mem = mem;
    it->size = (uint32_t)(coll ? 12 : 8) + (uint32_t)n + 2;
    it->depth = prop_scope(f);
    if (coll) {
        /* CollectionItem: ci_Next, ci_Size, ci_Data; newest first */
        uint32_t prev = 0;
        for (Item *o = f->items; o; o = o->next) if (o->ident == it->ident && o->type == it->type && o->id == it->id) { prev = o->mem; break; }
        wr32(mem, prev);
        wr32(mem + 4, (uint32_t)n);
        wr32(mem + 8, data);
    } else {
        /* StoredProperty: sp_Size, sp_Data; a newer one replaces it in its scope */
        for (Item **pp = &f->items; *pp; pp = &(*pp)->next) {
            Item *o = *pp;
            if (o->ident == it->ident && o->type == it->type && o->id == it->id && o->depth == it->depth) {
                *pp = o->next; exec_freemem(o->mem, o->size); free(o); break;
            }
        }
        wr32(mem, (uint32_t)n);
        wr32(mem + 4, data);
    }
    it->next = f->items;
    f->items = it;
    return 0;
}

static int32_t call_handler(M68K *c, IFF *f, Handler *h, int cmd) {
    uint32_t m = exec_allocmem(4, MEMF_PUBLIC);
    wr32(m, (uint32_t)cmd);
    int32_t r = (int32_t)call_hook(c, h->hook, h->obj, m);
    exec_freemem(m, 4);
    return r;
}

/* leave the top context: skip what was not read, the pad byte, account in the parent */
static int32_t leave(M68K *c, IFF *f) {
    Ctx *x = &f->st[f->depth - 1];
    int32_t rest = x->size - x->scan + (x->size & 1);
    if (rest > 0) { int32_t e = s_skip(c, f, rest); if (e) return e; }
    int32_t used = x->size + (x->size & 1) + 8;
    pop(f);
    if (f->depth) { Ctx *p = &f->st[f->depth - 1]; p->scan += used; sync_node(p); }
    return 0;
}

static int32_t parse(M68K *c, IFF *f, int mode) {
    for (;;) {
        if (f->need_pop) {                         /* the chunk the client stopped on, or a finished container */
            f->need_pop = 0;
            Ctx *x = &f->st[f->depth - 1];
            Handler *eh = handler(f, H_EXIT, x->type, x->id);
            if (eh) { int32_t r = call_handler(c, f, eh, IFFCMD_EXIT); if (r && r != IFF_RETURN2CLIENT) return r; }
            int32_t e = leave(c, f);
            if (e) return e;
        }
        Ctx *top = &f->st[f->depth - 1];
        if (top->scan >= top->size) {              /* a container ran out */
            if (f->depth == 1) return IFFERR_EOF;
            f->need_pop = 1;
            if (mode != IFFPARSE_SCAN) return IFFERR_EOC;
            if (handler(f, H_EXIT_STOP, top->type, top->id)) return IFFERR_EOC;
            continue;
        }
        /* the next chunk header */
        uint8_t hdr[12];
        int r = s_read_native(c, f, hdr, 8);
        if (r == 0 && f->depth == 1) return IFFERR_EOF;
        if (r != 8) return f->depth == 1 ? IFFERR_EOF : IFFERR_READ;
        uint32_t id = (uint32_t)hdr[0] << 24 | (uint32_t)hdr[1] << 16 | (uint32_t)hdr[2] << 8 | hdr[3];
        int32_t size = (int32_t)((uint32_t)hdr[4] << 24 | (uint32_t)hdr[5] << 16 | (uint32_t)hdr[6] << 8 | hdr[7]);
        if (size < 0) return IFFERR_MANGLED;
        uint32_t type = top->id == ID_FORM || top->id == ID_PROP ? top->type : 0;
        int container = id == ID_FORM || id == ID_LIST || id == ID_CAT || id == ID_PROP;
        if (f->depth == 1 && !container) return IFFERR_NOTIFF;
        if (container) {
            if (s_read_native(c, f, hdr + 8, 4) != 4) return IFFERR_READ;
            type = (uint32_t)hdr[8] << 24 | (uint32_t)hdr[9] << 16 | (uint32_t)hdr[10] << 8 | hdr[11];
        }
        Ctx *x = push(f, id, type, size);
        if (!x) return IFFERR_NOMEM;
        if (container) { x->scan = 4; sync_node(x); }
        Handler *en = handler(f, H_ENTRY, type, id);
        if (en) {
            int32_t rr = call_handler(c, f, en, IFFCMD_ENTRY);
            if (rr == IFF_RETURN2CLIENT) return 0;
            if (rr) return rr;
        }
        if (mode != IFFPARSE_SCAN) {
            if (!container) f->need_pop = 0;
            /* STEP: the client sees each entry; the leaf is left on the next call */
            if (!container) f->need_pop = 1;
            if (mode == IFFPARSE_STEP && !container) {
                Handler *p = handler(f, H_PROP, type, id), *cl = handler(f, H_COLL, type, id);
                if (p || cl) { int32_t e = store_chunk(c, f, !p); if (e) return e; }
            }
            return 0;
        }
        if (container) continue;
        if (handler(f, H_STOP, type, id)) { f->need_pop = 1; return 0; }
        Handler *p = handler(f, H_PROP, type, id), *cl = handler(f, H_COLL, type, id);
        if (p || cl) { int32_t e = store_chunk(c, f, !p); if (e) return e; }
        f->need_pop = 1;
    }
}

/* ---- library vectors -------------------------------------------------------------------- */

static uint32_t i_AllocIFF(M68K *c) {
    (void)c;
    IFF *f = calloc(1, sizeof *f);
    f->iff = exec_allocmem(32, MEMF_PUBLIC | MEMF_CLEAR);
    f->next = iffs;
    iffs = f;
    return f->iff;
}
static uint32_t i_FreeIFF(M68K *c) {
    IFF **pp = &iffs;
    while (*pp && (*pp)->iff != c->a[0]) pp = &(*pp)->next;
    if (!*pp) return 0;
    IFF *f = *pp;
    *pp = f->next;
    while (f->depth) pop(f);
    purge_items(f, 0);
    exec_freemem(f->iff, 32);
    free(f);
    return 0;
}
static uint32_t i_InitIFF(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return 0;
    f->dos = 0;
    f->hook = c->a[1];
    wr32(f->iff + IFF_FLAGS, (rd32(f->iff + IFF_FLAGS) & ~6u) | (c->d[0] & 6));
    return 0;
}
static uint32_t i_InitIFFasDOS(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return 0;
    f->dos = 1;
    f->hook = 0;
    wr32(f->iff + IFF_FLAGS, rd32(f->iff + IFF_FLAGS) | IFFF_RSEEK);
    return 0;
}
static uint32_t i_InitIFFasClip(M68K *c) { (void)c; return 0; }

static uint32_t i_OpenIFF(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return (uint32_t)IFFERR_NOMEM;
    if (!f->dos && !f->hook) return (uint32_t)IFFERR_NOHOOK;
    f->write = (c->d[0] & IFFF_WRITE) != 0;
    wr32(f->iff + IFF_FLAGS, (rd32(f->iff + IFF_FLAGS) & ~1u) | (uint32_t)f->write);
    while (f->depth) pop(f);
    f->pos = 0;
    f->need_pop = 0;
    push(f, 0, 0, 0x7FFFFFFF);                     /* the stream */
    f->open = 1;
    return 0;
}
static uint32_t i_CloseIFF(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return 0;
    while (f->depth) pop(f);
    purge_items(f, 0);
    f->open = 0;
    return 0;
}

static uint32_t i_ParseIFF(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f || !f->open || f->write) return (uint32_t)IFFERR_SYNTAX;
    return (uint32_t)parse(c, f, (int)c->d[0]);
}

static uint32_t i_ReadChunkBytes(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f || f->depth < 2) return (uint32_t)IFFERR_EOC;
    Ctx *x = &f->st[f->depth - 1];
    int32_t n = (int32_t)c->d[0], left = x->size - x->scan;
    if (n > left) n = left;
    if (n <= 0) return 0;
    int r = s_read(c, f, c->a[1], n);
    if (r < 0) return (uint32_t)IFFERR_READ;
    x->scan += r;
    sync_node(x);
    return (uint32_t)r;
}
static uint32_t i_ReadChunkRecords(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f || f->depth < 2) return (uint32_t)IFFERR_EOC;
    Ctx *x = &f->st[f->depth - 1];
    int32_t rec = (int32_t)c->d[0], n = (int32_t)c->d[1];
    if (rec <= 0) return 0;
    int32_t can = (x->size - x->scan) / rec;
    if (n > can) n = can;
    if (n <= 0) return 0;
    int r = s_read(c, f, c->a[1], n * rec);
    if (r < 0) return (uint32_t)IFFERR_READ;
    x->scan += r;
    sync_node(x);
    return (uint32_t)(r / rec);
}

static uint32_t i_PushChunk(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f || !f->open || !f->write) return (uint32_t)IFFERR_SYNTAX;
    uint32_t type = c->d[0], id = c->d[1];
    int32_t size = (int32_t)c->d[2];
    uint8_t h[12];
    int container = id == ID_FORM || id == ID_LIST || id == ID_CAT || id == ID_PROP;
    for (int i = 0; i < 4; i++) { h[i] = (uint8_t)(id >> (24 - 8 * i)); h[4 + i] = (uint8_t)((uint32_t)(size == IFFSIZE_UNKNOWN ? 0 : size + (container ? 4 : 0)) >> (24 - 8 * i)); h[8 + i] = (uint8_t)(type >> (24 - 8 * i)); }
    uint32_t sizepos = f->pos + 4;
    if (s_write_native(c, f, h, container ? 12 : 8) < 0) return (uint32_t)IFFERR_WRITE;
    if (!container && f->depth) type = f->st[f->depth - 1].type;
    Ctx *x = push(f, id, type, size == IFFSIZE_UNKNOWN ? 0 : size);
    if (!x) return (uint32_t)IFFERR_NOMEM;
    x->unknown = size == IFFSIZE_UNKNOWN;
    x->sizepos = sizepos;
    if (container) { x->scan = 4; if (!x->unknown) x->size += 4; }
    sync_node(x);
    return 0;
}

static uint32_t i_WriteChunkBytes(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f || f->depth < 2) return (uint32_t)IFFERR_EOC;
    Ctx *x = &f->st[f->depth - 1];
    int32_t n = (int32_t)c->d[0];
    if (!x->unknown && x->scan + n > x->size) n = x->size - x->scan;
    int r = s_write(c, f, c->a[1], n);
    if (r < 0) return (uint32_t)IFFERR_WRITE;
    x->scan += r;
    if (x->unknown && x->scan > x->size) x->size = x->scan;
    sync_node(x);
    return (uint32_t)r;
}
static uint32_t i_WriteChunkRecords(M68K *c) {
    uint32_t d0 = c->d[0], d1 = c->d[1];
    c->d[0] = d0 * d1;
    int32_t r = (int32_t)i_WriteChunkBytes(c);
    c->d[0] = d0; c->d[1] = d1;
    return r < 0 ? (uint32_t)r : (uint32_t)(d0 ? (uint32_t)r / d0 : 0);
}

static uint32_t i_PopChunk(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f || f->depth < 2) return (uint32_t)IFFERR_EOC;
    Ctx *x = &f->st[f->depth - 1];
    if (f->write) {
        int32_t size = x->unknown ? x->scan : x->size;
        if (x->unknown) {
            /* go back and write the size */
            int32_t back = (int32_t)(f->pos - x->sizepos);
            uint8_t b[4] = { (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size };
            if (s_cmd(c, f, IFFCMD_SEEK, 0, -back) < 0) return (uint32_t)IFFERR_SEEK;
            f->pos -= (uint32_t)back;
            s_write_native(c, f, b, 4);
            if (s_cmd(c, f, IFFCMD_SEEK, 0, back - 4) < 0) return (uint32_t)IFFERR_SEEK;
            f->pos += (uint32_t)(back - 4);
        }
        if (size & 1) { uint8_t z = 0; s_write_native(c, f, &z, 1); }
        pop(f);
        if (f->depth) {
            Ctx *p = &f->st[f->depth - 1];
            p->scan += size + (size & 1) + 8;
            if (p->unknown && p->scan > p->size) p->size = p->scan;
            sync_node(p);
        }
        return 0;
    }
    return (uint32_t)leave(c, f);
}

static uint32_t i_PropChunk(M68K *c) { IFF *f = iff_of(c->a[0]); return f ? (uint32_t)add_handler(f, H_PROP, c->d[0], c->d[1], 0, 0) : 0; }
static uint32_t i_StopChunk(M68K *c) { IFF *f = iff_of(c->a[0]); return f ? (uint32_t)add_handler(f, H_STOP, c->d[0], c->d[1], 0, 0) : 0; }
static uint32_t i_CollectionChunk(M68K *c) { IFF *f = iff_of(c->a[0]); return f ? (uint32_t)add_handler(f, H_COLL, c->d[0], c->d[1], 0, 0) : 0; }
static uint32_t i_StopOnExit(M68K *c) { IFF *f = iff_of(c->a[0]); return f ? (uint32_t)add_handler(f, H_EXIT_STOP, c->d[0], c->d[1], 0, 0) : 0; }
static uint32_t chunks(M68K *c, int kind) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return 0;
    for (uint32_t i = 0; i < c->d[0]; i++) {
        int32_t r = add_handler(f, kind, rd32(c->a[1] + 8 * i), rd32(c->a[1] + 8 * i + 4), 0, 0);
        if (r) return (uint32_t)r;
    }
    return 0;
}
static uint32_t i_PropChunks(M68K *c) { return chunks(c, H_PROP); }
static uint32_t i_StopChunks(M68K *c) { return chunks(c, H_STOP); }
static uint32_t i_CollectionChunks(M68K *c) { return chunks(c, H_COLL); }
static uint32_t i_EntryHandler(M68K *c) { IFF *f = iff_of(c->a[0]); return f ? (uint32_t)add_handler(f, H_ENTRY, c->d[0], c->d[1], c->a[1], c->a[2]) : 0; }
static uint32_t i_ExitHandler(M68K *c) { IFF *f = iff_of(c->a[0]); return f ? (uint32_t)add_handler(f, H_EXIT, c->d[0], c->d[1], c->a[1], c->a[2]) : 0; }

static Item *find_item(IFF *f, uint32_t ident, uint32_t type, uint32_t id) {
    Item *best = 0;
    for (Item *it = f->items; it; it = it->next)
        if (it->ident == ident && it->type == type && it->id == id && (!best || it->depth > best->depth)) best = it;
    return best;
}
static uint32_t i_FindProp(M68K *c) { IFF *f = iff_of(c->a[0]); Item *it = f ? find_item(f, ID('p', 'r', 'o', 'p'), c->d[0], c->d[1]) : 0; return it ? it->mem : 0; }
static uint32_t i_FindCollection(M68K *c) { IFF *f = iff_of(c->a[0]); Item *it = f ? find_item(f, ID('c', 'o', 'l', 'l'), c->d[0], c->d[1]) : 0; return it ? it->mem : 0; }
static uint32_t i_FindPropContext(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return 0;
    int d = prop_scope(f);
    return d ? f->st[d - 1].node : 0;
}
static uint32_t i_CurrentChunk(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    return f && f->depth > 1 ? f->st[f->depth - 1].node : 0;
}
static uint32_t i_ParentChunk(M68K *c) {
    for (IFF *f = iffs; f; f = f->next)
        for (int i = f->depth - 1; i > 1; i--) if (f->st[i].node == c->a[0]) return f->st[i - 1].node;
    return 0;
}

/* local context items: an 8-byte header (type, id) is kept natively, the data in 68k memory */
static uint32_t i_AllocLocalItem(M68K *c) {
    uint32_t size = c->d[3];
    uint32_t lci = exec_allocmem(24 + size, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(lci + 8, c->d[1]);   /* id */
    wr32(lci + 12, c->d[0]);  /* type */
    wr32(lci + 16, c->d[2]);  /* ident */
    wr32(lci + 20, size);
    return lci;
}
static uint32_t i_LocalItemData(M68K *c) { return c->a[0] ? c->a[0] + 24 : 0; }
static uint32_t i_FreeLocalItem(M68K *c) { if (c->a[0]) exec_freemem(c->a[0], 24 + rd32(c->a[0] + 20)); return 0; }
static uint32_t i_SetLocalItemPurge(M68K *c) { (void)c; return 0; }
static uint32_t i_StoreLocalItem(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return (uint32_t)IFFERR_NOSCOPE;
    uint32_t lci = c->a[1];
    Item *it = calloc(1, sizeof *it);
    it->type = rd32(lci + 12);
    it->id = rd32(lci + 8);
    it->ident = rd32(lci + 16);
    it->depth = c->d[0] == 1 ? 1 : c->d[0] == 2 ? f->depth : prop_scope(f);
    it->mem = 0;                                     /* owned by the client: FreeLocalItem */
    it->next68 = lci;
    it->next = f->items;
    f->items = it;
    return 0;
}
static uint32_t i_StoreItemInContext(M68K *c) { (void)c; return 0; }
static uint32_t i_FindLocalItem(M68K *c) {
    IFF *f = iff_of(c->a[0]);
    if (!f) return 0;
    Item *it = find_item(f, c->d[2], c->d[0], c->d[1]);
    return it ? (it->next68 ? it->next68 : it->mem) : 0;
}

static uint32_t i_OpenClipboard(M68K *c) { (void)c; return 0; }
static uint32_t i_CloseClipboard(M68K *c) { (void)c; return 0; }
static int good_chars(uint32_t id) {
    for (int i = 0; i < 4; i++) { int ch = (id >> (24 - 8 * i)) & 0xFF; if (ch < 0x20 || ch > 0x7E) return 0; }
    return 1;
}
static uint32_t i_GoodID(M68K *c) { uint32_t id = c->d[0]; return good_chars(id) && (id >> 24) != ' ' ? DOSTRUE : 0; }
static uint32_t i_GoodType(M68K *c) {
    uint32_t id = c->d[0];
    if (!good_chars(id) || (id >> 24) == ' ') return 0;
    for (int i = 0; i < 4; i++) { int ch = (id >> (24 - 8 * i)) & 0xFF; if (ch >= 'a' && ch <= 'z') return 0; }
    return DOSTRUE;
}
static uint32_t i_IDtoStr(M68K *c) {
    for (int i = 0; i < 4; i++) wr8(c->a[0] + (uint32_t)i, (c->d[0] >> (24 - 8 * i)) & 0xFF);
    wr8(c->a[0] + 4, 0);
    return c->a[0];
}

static const NativeEntry iff_natives[] = {
    { "AllocIFF", i_AllocIFF }, { "OpenIFF", i_OpenIFF }, { "ParseIFF", i_ParseIFF }, { "CloseIFF", i_CloseIFF },
    { "FreeIFF", i_FreeIFF }, { "ReadChunkBytes", i_ReadChunkBytes }, { "WriteChunkBytes", i_WriteChunkBytes },
    { "ReadChunkRecords", i_ReadChunkRecords }, { "WriteChunkRecords", i_WriteChunkRecords },
    { "PushChunk", i_PushChunk }, { "PopChunk", i_PopChunk }, { "EntryHandler", i_EntryHandler },
    { "ExitHandler", i_ExitHandler }, { "PropChunk", i_PropChunk }, { "PropChunks", i_PropChunks },
    { "StopChunk", i_StopChunk }, { "StopChunks", i_StopChunks }, { "CollectionChunk", i_CollectionChunk },
    { "CollectionChunks", i_CollectionChunks }, { "StopOnExit", i_StopOnExit }, { "FindProp", i_FindProp },
    { "FindCollection", i_FindCollection }, { "FindPropContext", i_FindPropContext },
    { "CurrentChunk", i_CurrentChunk }, { "ParentChunk", i_ParentChunk }, { "AllocLocalItem", i_AllocLocalItem },
    { "LocalItemData", i_LocalItemData }, { "SetLocalItemPurge", i_SetLocalItemPurge },
    { "FreeLocalItem", i_FreeLocalItem }, { "FindLocalItem", i_FindLocalItem }, { "StoreLocalItem", i_StoreLocalItem },
    { "StoreItemInContext", i_StoreItemInContext }, { "InitIFF", i_InitIFF }, { "InitIFFasDOS", i_InitIFFasDOS },
    { "InitIFFasClip", i_InitIFFasClip }, { "OpenClipboard", i_OpenClipboard }, { "CloseClipboard", i_CloseClipboard },
    { "GoodID", i_GoodID }, { "GoodType", i_GoodType }, { "IDtoStr", i_IDtoStr },
    { 0, 0 }
};

void iffparse_init(void) {
    IFFParseBase = lib_create("iffparse.library", "iffparse", 64, iff_natives, 40, 1, "iffparse 40.1 (AmigaMac)");
    lib_add(IFFParseBase);
}
