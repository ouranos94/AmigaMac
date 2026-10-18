/* datatypes.library, written from the datatypes autodocs and the
 * datatypes headers, with two classes:
 *   picture.datatype : IFF ILBM (ByteRun1, mask plane), drawn into windows
 *                      with its colours mapped onto the screen's palette
 *   sound.datatype   : IFF 8SVX (raw or Fibonacci delta), played on the Mac
 * Data type objects are BOOPSI gadgets (datatypesclass is a gadgetclass),
 * so AddDTObject puts them into windows like any gadget. */
#include "intui.h"
#include "ntk.h"
#include "audio.h"
#include "ndk_offsets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint32_t DataTypesBase;

#define TAG_USER_ 0x80000000u
#define DTA(n) (TAG_USER_ + 0x1000 + (n))
#define DTA_TopVert DTA(11)
#define DTA_VisibleVert DTA(12)
#define DTA_TotalVert DTA(13)
#define DTA_TopHoriz DTA(15)
#define DTA_VisibleHoriz DTA(16)
#define DTA_TotalHoriz DTA(17)
#define DTA_NodeName DTA(19)
#define DTA_Title DTA(20)
#define DTA_TriggerMethods DTA(21)
#define DTA_Methods DTA(24)
#define DTA_Busy DTA(28)
#define DTA_Sync DTA(29)
#define DTA_BaseName DTA(30)
#define DTA_GroupID DTA(31)
#define DTA_ErrorNumber DTA(33)
#define DTA_Immediate DTA(37)
#define DTA_Repeat DTA(38)
#define DTA_SourceAddress DTA(39)
#define DTA_SourceSize DTA(40)
#define DTA_Name DTA(100)
#define DTA_SourceType DTA(101)
#define DTA_Handle DTA(102)
#define DTA_DataType DTA(103)
#define DTA_Domain DTA(104)
#define DTA_ObjName DTA(109)
#define DTA_NominalVert DTA(124)
#define DTA_NominalHoriz DTA(125)
#define SDTA(n) DTA(500 + (n))
#define PDTA(n) DTA(200 + (n))
#define DTST_RAM 1
#define DTST_FILE 2
#define DTST_MEMORY 5
#define DTM_FRAMEBOX 0x601
#define DTM_PROCLAYOUT 0x602
#define DTM_ASYNCLAYOUT 0x603
#define DTM_REMOVEDTOBJECT 0x604
#define DTM_TRIGGER 0x631
#define STM_PAUSE 1
#define STM_PLAY 2
#define STM_STOP 14
#define GM_HITTEST 0
#define GM_RENDER 1
#define GM_GOACTIVE 2
#define GM_LAYOUT 6
#define OM_NEW 0x101
#define OM_DISPOSE 0x102
#define OM_SET 0x103
#define OM_GET 0x104
#define OM_UPDATE 0x108
#define DTERROR_UNKNOWN_DATATYPE 2000
#define DTERROR_COULDNT_OPEN 2002
#define ID(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define GID_SOUND ID('s', 'o', 'u', 'n')
#define GID_PICTURE ID('p', 'i', 'c', 't')
#define PAL_CLOCK 3546895u

typedef struct DTO {
    uint32_t obj, group, ident;
    char name[256];
    uint32_t title;
    /* picture */
    uint32_t bmhd, bitmap, cregs, colregs, modeid;
    int w, h, depth, ncolors;
    uint8_t rgb[256][3];
    /* sound */
    uint32_t vhdr, sample, slen, sigtask, sigbit;
    int rate, volume, cycles, voice;
    struct DTO *next;
} DTO;

static DTO *dtos;
static uint32_t dt_class, pic_class, snd_class;
static DTO *dto_of(uint32_t o) { for (DTO *d = dtos; d; d = d->next) if (d->obj == o) return d; return 0; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* ---- IFF chunks ---------------------------------------------------------------- */

static const uint8_t *chunk(const uint8_t *b, size_t len, const char *id, uint32_t *size) {
    for (size_t pos = 12; pos + 8 <= len;) {
        uint32_t sz = be32(b + pos + 4);
        if (!memcmp(b + pos, id, 4) && pos + 8 + sz <= len) { *size = sz; return b + pos + 8; }
        pos += 8 + ((sz + 1) & ~1u);
    }
    return 0;
}

static int load_ilbm(DTO *d, const uint8_t *b, size_t len) {
    uint32_t sz;
    const uint8_t *h = chunk(b, len, "BMHD", &sz);
    if (!h || sz < 20) return 0;
    d->w = (int)be16(h);
    d->h = (int)be16(h + 2);
    d->depth = h[8];
    int masking = h[9], comp = h[10];
    if (d->w <= 0 || d->h <= 0 || d->depth < 1 || d->depth > 8) { os_log("datatypes : ILBM %dx%dx%d non géré", d->w, d->h, d->depth); return 0; }
    d->bmhd = exec_allocmem(BitMapHeader_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    for (int i = 0; i < 20; i++) wr8(d->bmhd + (uint32_t)i, h[i]);
    const uint8_t *cm = chunk(b, len, "CMAP", &sz);
    d->ncolors = cm ? (int)(sz / 3) : 0;
    if (d->ncolors > 256) d->ncolors = 256;
    for (int i = 0; i < d->ncolors; i++) memcpy(d->rgb[i], cm + 3 * i, 3);
    if (!d->ncolors) {                                     /* no palette: a grey ramp */
        d->ncolors = 1 << d->depth;
        for (int i = 0; i < d->ncolors; i++) d->rgb[i][0] = d->rgb[i][1] = d->rgb[i][2] = (uint8_t)(i * 255 / (d->ncolors - 1 ? d->ncolors - 1 : 1));
    }
    d->colregs = exec_allocmem(3 * (uint32_t)d->ncolors + 2, MEMF_PUBLIC);
    d->cregs = exec_allocmem(12 * (uint32_t)d->ncolors, MEMF_PUBLIC);
    for (int i = 0; i < d->ncolors; i++)
        for (int k = 0; k < 3; k++) {
            wr8(d->colregs + 3 * (uint32_t)i + (uint32_t)k, d->rgb[i][k]);
            wr32(d->cregs + 12 * (uint32_t)i + 4 * (uint32_t)k, d->rgb[i][k] * 0x01010101u);
        }
    const uint8_t *camg = chunk(b, len, "CAMG", &sz);
    d->modeid = camg && sz >= 4 ? be32(camg) : 0;
    const uint8_t *body = chunk(b, len, "BODY", &sz);
    if (!body) return 0;
    d->bitmap = gfx_alloc_bitmap(d->w, d->h, d->depth, 1);
    BM bm;
    bm_load(d->bitmap, &bm);
    int rowbytes = ((d->w + 15) / 16) * 2, planes = d->depth + (masking == 1);
    uint8_t row[2048];
    const uint8_t *p = body, *end = body + sz;
    for (int y = 0; y < d->h; y++)
        for (int pl = 0; pl < planes; pl++) {
            if (comp == 1) {                                /* ByteRun1 */
                int n = 0;
                while (n < rowbytes && p < end) {
                    int8_t ctl = (int8_t)*p++;
                    if (ctl >= 0) { for (int k = 0; k <= ctl && p < end && n < rowbytes; k++) row[n++] = *p++; }
                    else if (ctl != -128) { uint8_t v = p < end ? *p++ : 0; for (int k = 0; k <= -ctl && n < rowbytes; k++) row[n++] = v; }
                }
            } else {
                for (int n = 0; n < rowbytes; n++) row[n] = p < end ? *p++ : 0;
            }
            if (pl < d->depth && rowbytes <= bm.bpr)
                for (int n = 0; n < rowbytes; n++) wr8(bm.planes[pl] + (uint32_t)(y * bm.bpr + n), row[n]);
        }
    return 1;
}

static int load_8svx(DTO *d, const uint8_t *b, size_t len) {
    uint32_t sz;
    const uint8_t *vh = chunk(b, len, "VHDR", &sz);
    const uint8_t *body = chunk(b, len, "BODY", &sz);
    if (!vh || !body) return 0;
    d->vhdr = exec_allocmem(VoiceHeader_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    for (int i = 0; i < VoiceHeader_SIZEOF; i++) wr8(d->vhdr + (uint32_t)i, vh[i]);
    d->rate = (int)be16(vh + VoiceHeader_vh_SamplesPerSec);
    if (d->rate <= 0) d->rate = 8363;
    uint32_t vol = be32(vh + VoiceHeader_vh_Volume);
    d->volume = vol >= 0x10000 ? 64 : (int)(vol * 64 / 0x10000);
    if (!d->volume) d->volume = 64;
    int comp = vh[VoiceHeader_vh_Compression];
    if (comp == 1) {                                        /* Fibonacci delta */
        static const int8_t fib[16] = { -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21 };
        uint32_t n = sz > 2 ? (sz - 2) * 2 : 0;
        d->sample = exec_allocmem(n + 2, MEMF_PUBLIC);
        int8_t x = (int8_t)body[1];
        for (uint32_t i = 0; i < n; i++) {
            uint8_t byte = body[2 + i / 2];
            x = (int8_t)(x + fib[(i & 1) ? byte & 15 : byte >> 4]);
            wr8(d->sample + i, (uint8_t)x);
        }
        d->slen = n;
    } else {
        d->sample = exec_allocmem(sz + 2, MEMF_PUBLIC);
        for (uint32_t i = 0; i < sz; i++) wr8(d->sample + i, body[i]);
        d->slen = sz;
    }
    d->cycles = 1;
    return 1;
}

/* the data type of some bytes: group and file type */
static uint32_t identify(const uint8_t *b, size_t len, uint32_t *ident) {
    if (len >= 12 && !memcmp(b, "FORM", 4)) {
        if (!memcmp(b + 8, "ILBM", 4)) { *ident = ID('i', 'l', 'b', 'm'); return GID_PICTURE; }
        if (!memcmp(b + 8, "8SVX", 4)) { *ident = ID('8', 's', 'v', 'x'); return GID_SOUND; }
    }
    *ident = 0;
    return 0;
}

static uint8_t *read_file(const char *name, size_t *len) {
    char hp[2048];
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64 << 20)) { fclose(f); return 0; }
    uint8_t *b = malloc((size_t)n);
    *len = fread(b, 1, (size_t)n, f);
    fclose(f);
    return b;
}

/* ---- the classes ---------------------------------------------------------------- */

static void play(M68K *c, DTO *d) {
    (void)c;
    if (!d->sample || !d->slen) return;
    const int8_t *p = (const int8_t *)mem_ptr(d->sample, d->slen);
    if (!p) return;
    if (d->voice) host_audio_stop(d->voice);
    uint64_t now = now_us();
    d->voice = host_audio_play(p, (int)d->slen, d->rate, d->volume, d->cycles, 0, now);
    if (d->sigtask && d->cycles) {
        uint64_t end = now + (uint64_t)d->slen * (uint64_t)d->cycles * 1000000u / (uint64_t)d->rate;
        sched_timer_add(end, TIMER_SIGNAL, d->sigtask, d->sigbit);
    }
}

static void set_attrs(DTO *d, uint32_t tags) {
    uint32_t st = tags, ti;
    while ((ti = tag_next(&st))) {
        uint32_t v = rd32(ti + 4);
        switch (rd32(ti)) {
        case SDTA(2): if (v) d->sample = v; break;                        /* SDTA_Sample */
        case SDTA(3): d->slen = v; break;                                 /* SDTA_SampleLength */
        case SDTA(4): if (v) d->rate = (int)(PAL_CLOCK / v); break;       /* SDTA_Period */
        case SDTA(5): d->volume = (int)v; break;                          /* SDTA_Volume */
        case SDTA(6): d->cycles = (int)v; break;                          /* SDTA_Cycles */
        case SDTA(7): d->sigtask = v; break;                              /* SDTA_SignalTask */
        case SDTA(8): d->sigbit = v; break;                               /* SDTA_SignalBit (mask) */
        case SDTA(10): d->sigbit = 1u << v; break;                        /* SDTA_SignalBitNumber */
        case SDTA(11): if (v) d->rate = (int)v; break;                    /* SDTA_SamplesPerSec */
        case DTA_Title: case DTA_ObjName: d->title = v; break;
        }
    }
}

static int get_attr(DTO *d, uint32_t id, uint32_t *v) {
    switch (id) {
    case DTA_GroupID: *v = d->group; return 1;
    case DTA_Name: case DTA_Title: case DTA_ObjName: case DTA_NodeName:
        if (!d->title) d->title = str_alloc(d->name);
        *v = d->title;
        return 1;
    case DTA_TotalVert: case DTA_NominalVert: case DTA_VisibleVert: *v = (uint32_t)d->h; return 1;
    case DTA_TotalHoriz: case DTA_NominalHoriz: case DTA_VisibleHoriz: *v = (uint32_t)d->w; return 1;
    case DTA_TopVert: case DTA_TopHoriz: case DTA_Busy: *v = 0; return 1;
    case PDTA(1): *v = d->bmhd; return 1;                                /* PDTA_BitMapHeader */
    case PDTA(2): case PDTA(15): case PDTA(16): *v = d->bitmap; return 1; /* BitMap, DestBitMap, ClassBitMap */
    case PDTA(3): *v = d->colregs; return 1;                             /* PDTA_ColorRegisters */
    case PDTA(4): *v = d->cregs; return 1;                               /* PDTA_CRegs */
    case PDTA(9): *v = (uint32_t)d->ncolors; return 1;                   /* PDTA_NumColors */
    case PDTA(10): *v = (uint32_t)d->ncolors; return 1;                  /* PDTA_NumAlloc */
    case PDTA(0): *v = d->modeid; return 1;                              /* PDTA_ModeID */
    case SDTA(1): *v = d->vhdr; return 1;                                /* SDTA_VoiceHeader */
    case SDTA(2): *v = d->sample; return 1;
    case SDTA(3): *v = d->slen; return 1;
    case SDTA(4): *v = d->rate ? PAL_CLOCK / (uint32_t)d->rate : 0; return 1;
    case SDTA(5): *v = (uint32_t)d->volume; return 1;
    case SDTA(6): *v = (uint32_t)d->cycles; return 1;
    case SDTA(11): *v = (uint32_t)d->rate; return 1;
    case SDTA(18): *v = 8; return 1;                                     /* SDTA_BitsPerSample */
    }
    return 0;
}

static void render(M68K *c, DTO *d, uint32_t ginfo, uint32_t rp) {
    (void)c;
    if (!d->bitmap || !ginfo) return;
    uint32_t win = rd32(ginfo + GadgetInfo_gi_Window);
    NWindow *w = win ? nwin_of(win) : 0;
    if (!w) return;
    int gx, gy, gw, gh;
    gadget_box(w, d->obj, &gx, &gy, &gw, &gh);
    NScreen *s = w->s;
    /* each picture colour to the nearest screen colour */
    uint8_t map[256];
    int n = 1 << s->depth;
    for (int i = 0; i < d->ncolors; i++) {
        int best = 0;
        long bd = 1L << 40;
        for (int k = 0; k < n; k++) {
            int r = (int)(s->pal[k] >> 16 & 255) - d->rgb[i][0], g = (int)(s->pal[k] >> 8 & 255) - d->rgb[i][1], b = (int)(s->pal[k] & 255) - d->rgb[i][2];
            long dd = (long)r * r * 3 + (long)g * g * 4 + (long)b * b * 2;
            if (dd < bd) { bd = dd; best = k; }
        }
        map[i] = (uint8_t)best;
    }
    BM src, dst;
    bm_load(d->bitmap, &src);
    if (!bm_load(rd32(rp + RastPort_BitMap), &dst)) return;
    int ww = d->w < gw ? d->w : gw, hh = d->h < gh ? d->h : gh;
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < ww; x++) {
            int pen = bm_read(&src, x, y);
            bm_pixel(&dst, gx + x, gy + y, map[pen < d->ncolors ? pen : 0], JAM1, 0xFF);
        }
    gfx_touch();
}

static uint32_t dt_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    DTO *d = dto_of(obj);
    switch (id) {
    case OM_GET: {
        uint32_t v;
        if (d && get_attr(d, rd32(msg + 4), &v)) { wr32(rd32(msg + 8), v); return 1; }
        break;
    }
    case OM_SET: case OM_UPDATE:
        if (d) set_attrs(d, rd32(msg + 4));
        break;
    case OM_DISPOSE:
        if (d) {
            if (d->voice) host_audio_stop(d->voice);
            DTO **pp = &dtos;
            while (*pp && *pp != d) pp = &(*pp)->next;
            if (*pp) *pp = d->next;
            if (d->bitmap) gfx_free_bitmap(d->bitmap);
            free(d);
        }
        break;
    case DTM_TRIGGER:
        if (d) {
            uint32_t fn = rd32(msg + dtTrigger_dtt_Function);
            if (fn == STM_PLAY) play(c, d);
            else if ((fn == STM_STOP || fn == STM_PAUSE) && d->voice) { host_audio_stop(d->voice); d->voice = 0; }
        }
        return 0;
    case DTM_PROCLAYOUT: case DTM_ASYNCLAYOUT: case GM_LAYOUT: return 1;
    case DTM_FRAMEBOX: case DTM_REMOVEDTOBJECT: return 0;
    case GM_RENDER:
        if (d && d->group == GID_PICTURE) render(c, d, rd32(msg + 4), rd32(msg + 8));
        return 0;
    case GM_HITTEST: return 4;                                            /* GMR_GADGETHIT */
    case GM_GOACTIVE:
        if (d && d->group == GID_SOUND) play(c, d);
        return 2;                                                         /* GMR_NOREUSE */
    }
    return bo_super(c, cl, obj, msg);
}

/* ---- library vectors -------------------------------------------------------------- */

static uint32_t new_object(M68K *c, const char *name, uint32_t tags) {
    uint32_t want = tag_get(tags, DTA_GroupID, 0), stype = tag_get(tags, DTA_SourceType, DTST_FILE);
    size_t len = 0;
    uint8_t *b = 0;
    if (stype == DTST_FILE && name) b = read_file(name, &len);
    else if (stype == DTST_MEMORY) {
        uint32_t a = tag_get(tags, DTA_SourceAddress, 0), n = tag_get(tags, DTA_SourceSize, 0);
        if (a && n && n < (64u << 20)) { b = malloc(n); for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rd8(a + i); len = n; }
    }
    uint32_t ident = 0, group = b ? identify(b, len, &ident) : (stype == DTST_RAM ? want : 0);
    if (!group || (want && group != want)) {
        free(b);
        dos_set_ioerr(b || stype == DTST_RAM ? DTERROR_UNKNOWN_DATATYPE : DTERROR_COULDNT_OPEN);
        return 0;
    }
    uint32_t obj = bo_new(c, group == GID_PICTURE ? pic_class : snd_class, tags);
    if (!obj) { free(b); return 0; }
    DTO *d = calloc(1, sizeof *d);
    d->obj = obj;
    d->group = group;
    d->ident = ident;
    d->volume = 64;
    d->cycles = 1;
    d->rate = 8363;
    snprintf(d->name, sizeof d->name, "%s", name ? name : "");
    d->next = dtos;
    dtos = d;
    int ok = 1;
    if (b) ok = group == GID_PICTURE ? load_ilbm(d, b, len) : load_8svx(d, b, len);
    free(b);
    if (!ok) {
        uint32_t m[1] = { OM_DISPOSE };
        bo_domethod(c, obj, 1, m);
        dos_set_ioerr(DTERROR_UNKNOWN_DATATYPE);
        return 0;
    }
    set_attrs(d, tags);
    if (group == GID_PICTURE) {
        /* the gadget's size, unless the program gave one */
        if (!rd16(obj + Gadget_Width)) wr16(obj + Gadget_Width, (uint32_t)d->w);
        if (!rd16(obj + Gadget_Width + 2)) wr16(obj + Gadget_Width + 2, (uint32_t)d->h);
    }
    return obj;
}

static uint32_t d_NewDTObjectA(M68K *c) {
    char name[512];
    if (c->d[0]) str_get(c->d[0], name, sizeof name);
    return new_object(c, c->d[0] ? name : 0, c->a[0]);
}
static uint32_t d_DisposeDTObject(M68K *c) {
    if (!c->a[0]) return 0;
    uint32_t m[1] = { OM_DISPOSE };
    bo_domethod(c, c->a[0], 1, m);
    return 0;
}
static uint32_t d_GetDTAttrsA(M68K *c) {
    DTO *d = dto_of(c->a[0]);
    if (!d) return 0;
    uint32_t st = c->a[2], ti, n = 0;
    while ((ti = tag_next(&st))) {
        uint32_t v;
        if (get_attr(d, rd32(ti), &v)) { if (rd32(ti + 4)) wr32(rd32(ti + 4), v); n++; }
    }
    return n;
}
static uint32_t d_SetDTAttrsA(M68K *c) {
    DTO *d = dto_of(c->a[0]);
    if (!d) return 0;
    set_attrs(d, c->a[3]);
    return lib_call(c, IntuitionBase, "intuition", "SetGadgetAttrsA", c->a[0], c->a[1], c->a[2], c->a[3]);
}
static uint32_t d_DoDTMethodA(M68K *c) {
    uint32_t o = c->a[0], win = c->a[1], msg = c->a[3];
    if (!o || !msg) return 0;
    if (rd32(msg) == DTM_TRIGGER && win) {
        NWindow *w = nwin_of(win);
        uint32_t gi = w ? bo_ginfo(w) : 0;
        if (gi && !rd32(msg + dtTrigger_dtt_GInfo)) wr32(msg + dtTrigger_dtt_GInfo, gi);
        uint32_t r = bo_dispatch(c, bo_class_of(o), o, msg);
        if (gi) { wr32(msg + dtTrigger_dtt_GInfo, 0); bo_ginfo_free(gi); }
        return r;
    }
    if (win) return lib_call(c, IntuitionBase, "intuition", "DoGadgetMethodA", o, win, c->a[2], msg);
    return bo_dispatch(c, bo_class_of(o), o, msg);
}
static uint32_t d_AddDTObject(M68K *c) {
    return lib_call(c, IntuitionBase, "intuition", "AddGList", c->a[0], c->a[2], c->d[0], 1, c->a[1]);
}
static uint32_t d_RemoveDTObject(M68K *c) {
    return lib_call(c, IntuitionBase, "intuition", "RemoveGList", c->a[0], c->a[1], 1);
}
static uint32_t d_RefreshDTObjectA(M68K *c) {
    lib_call(c, IntuitionBase, "intuition", "RefreshGList", c->a[0], c->a[1], c->a[2], 1);
    return 0;
}
static uint32_t d_DoAsyncLayout(M68K *c) { (void)c; return DOSTRUE; }

static uint32_t methods, trig_sound;
static uint32_t d_GetDTMethods(M68K *c) { (void)c; return methods; }
static uint32_t d_GetDTTriggerMethods(M68K *c) { DTO *d = dto_of(c->a[0]); return d && d->group == GID_SOUND ? trig_sound : 0; }

static uint32_t d_ObtainDataTypeA(M68K *c) {
    /* type D0 (DTST_FILE: handle is a lock, DTST_RAM: a buffer), handle A0 */
    uint8_t buf[64];
    size_t len = 0;
    if (c->d[0] == DTST_FILE) {
        const char *hp = dos_lock_host_path(c->a[0]);
        FILE *f = hp ? fopen(hp, "rb") : 0;
        if (f) { len = fread(buf, 1, sizeof buf, f); fclose(f); }
    } else if (c->a[0]) {
        for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)rd8(c->a[0] + (uint32_t)i);
        len = sizeof buf;
    }
    uint32_t ident, group = identify(buf, len, &ident);
    if (!group) { dos_set_ioerr(DTERROR_UNKNOWN_DATATYPE); return 0; }
    uint32_t dt = exec_allocvec(DataType_SIZEOF + DataTypeHeader_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t h = dt + DataType_SIZEOF;
    int pic = group == GID_PICTURE;
    wr32(dt + DataType_dtn_Header, h);
    wr32(h + DataTypeHeader_dth_Name, str_alloc(pic ? "ILBM" : "8SVX"));
    wr32(h + DataTypeHeader_dth_BaseName, str_alloc(pic ? "ilbm" : "8svx"));
    wr32(h + DataTypeHeader_dth_Pattern, str_alloc("#?"));
    wr32(h + DataTypeHeader_dth_GroupID, group);
    wr32(h + DataTypeHeader_dth_ID, ident);
    wr16(h + DataTypeHeader_dth_Flags, 2);                               /* DTF_IFF */
    wr32(dt + DataType_dtn_Length, DataType_SIZEOF);
    return dt;
}
static uint32_t d_ReleaseDataType(M68K *c) { if (c->a[0]) exec_freevec(c->a[0]); return 0; }

static uint32_t d_GetDTString(M68K *c) {
    static uint32_t s[16];
    int id = (int)c->d[0];
    const char *t = id == DTERROR_UNKNOWN_DATATYPE ? "Type de donn\xe9" "es inconnu" : id == DTERROR_COULDNT_OPEN ? "Impossible d'ouvrir" : "Erreur de type de donn\xe9" "es";
    int k = id - 2000;
    if (k < 0 || k > 15) k = 15;
    if (!s[k]) s[k] = str_alloc(t);
    return s[k];
}
static uint32_t d_FindMethod(M68K *c) {
    for (uint32_t p = c->a[0]; p && rd32(p) != 0xFFFFFFFFu; p += 4) if (rd32(p) == c->a[1]) return p;
    return 0;
}
static uint32_t d_FindTriggerMethod(M68K *c) {
    for (uint32_t p = c->a[0]; p && rd32(p + DTMethod_dtm_Label); p += DTMethod_SIZEOF)
        if (rd32(p + DTMethod_dtm_Method) == c->d[0]) return p;
    return 0;
}
static uint32_t d_Zero(M68K *c) { (void)c; return 0; }

static const NativeEntry dt_natives[] = {
    { "ObtainDataTypeA", d_ObtainDataTypeA }, { "ReleaseDataType", d_ReleaseDataType },
    { "NewDTObjectA", d_NewDTObjectA }, { "DisposeDTObject", d_DisposeDTObject },
    { "SetDTAttrsA", d_SetDTAttrsA }, { "GetDTAttrsA", d_GetDTAttrsA }, { "AddDTObject", d_AddDTObject },
    { "RefreshDTObjectA", d_RefreshDTObjectA }, { "DoAsyncLayout", d_DoAsyncLayout }, { "DoDTMethodA", d_DoDTMethodA },
    { "RemoveDTObject", d_RemoveDTObject }, { "GetDTMethods", d_GetDTMethods },
    { "GetDTTriggerMethods", d_GetDTTriggerMethods }, { "PrintDTObjectA", d_Zero }, { "GetDTString", d_GetDTString },
    { "FindMethod", d_FindMethod }, { "FindTriggerMethod", d_FindTriggerMethod }, { "CopyDTMethods", d_Zero },
    { "CopyDTTriggerMethods", d_Zero }, { "FreeDTMethods", d_Zero }, { "GetDTTriggerMethodDataFlags", d_Zero },
    { "SaveDTObjectA", d_Zero }, { "StartDragSelect", d_Zero },
    { 0, 0 }
};

void datatypes_init(void) {
    dt_class = bo_native_class("datatypesclass", "gadgetclass", 0, dt_dispatch);
    pic_class = bo_native_class("picture.datatype", "datatypesclass", 0, dt_dispatch);
    snd_class = bo_native_class("sound.datatype", "datatypesclass", 0, dt_dispatch);
    static const uint32_t m[] = { OM_NEW, OM_GET, OM_SET, OM_UPDATE, OM_DISPOSE, GM_LAYOUT, GM_RENDER, GM_HITTEST,
                                  DTM_PROCLAYOUT, DTM_FRAMEBOX, DTM_TRIGGER, 0xFFFFFFFFu };
    methods = exec_allocmem(sizeof m, MEMF_PUBLIC);
    for (unsigned i = 0; i < sizeof m / 4; i++) wr32(methods + 4 * i, m[i]);
    trig_sound = exec_allocmem(3 * DTMethod_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(trig_sound + DTMethod_dtm_Label, str_alloc("Play"));
    wr32(trig_sound + DTMethod_dtm_Command, str_alloc("PLAY"));
    wr32(trig_sound + DTMethod_dtm_Method, STM_PLAY);
    wr32(trig_sound + DTMethod_SIZEOF + DTMethod_dtm_Label, str_alloc("Stop"));
    wr32(trig_sound + DTMethod_SIZEOF + DTMethod_dtm_Command, str_alloc("STOP"));
    wr32(trig_sound + DTMethod_SIZEOF + DTMethod_dtm_Method, STM_STOP);
    DataTypesBase = lib_create("datatypes.library", "datatypes", 64, dt_natives, 45, 1, "datatypes 45.1 (AmigaMac)");
    lib_add(DataTypesBase);
}
