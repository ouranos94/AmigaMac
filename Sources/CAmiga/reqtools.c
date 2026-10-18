/* reqtools.library (v38), written from its public description (the reqtools
 * autodocs and libraries/reqtools.h). Many Aminet programs ask for it.
 *
 * The requesters are AmigaMac's own: files, fonts and screen modes go
 * through asl.library, messages through EasyRequestArgs(), and the string
 * and number requesters are small GadTools windows. All of them run
 * synchronously in the caller's task. A program that asks for a request
 * handler (RT_ReqHandler) gets one that already holds the answer.
 *
 * ReqToolsBase carries, as the real one does, the bases of the libraries it
 * uses: programs read them from there instead of opening them. */
#include "wb.h"
#include "ntk.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

extern uint32_t UtilityBase;

uint32_t ReqToolsBase;

#define RT_TB 0x80000000u
#define RT_Window (RT_TB + 1)
#define RT_ReqPos (RT_TB + 3)
#define RT_Screen (RT_TB + 7)
#define RT_ReqHandler (RT_TB + 8)
#define RT_Underscore (RT_TB + 11)
#define RTEZ_ReqTitle (RT_TB + 20)
#define RTGL_Min (RT_TB + 30)
#define RTGL_Max (RT_TB + 31)
#define RTGL_Width (RT_TB + 32)
#define RTGL_GadFmt (RT_TB + 34)
#define RTGL_TextFmt (RT_TB + 38)
#define RTGL_TextFmtArgs (RT_TB + 39)
#define RTFI_Flags (RT_TB + 40)
#define RTFI_OkText (RT_TB + 42)
#define RTFI_VolumeRequest (RT_TB + 43)
#define RTFI_Dir (RT_TB + 50)
#define RTFI_MatchPat (RT_TB + 51)
#define RTFO_FontName (RT_TB + 63)
#define RTFO_FontHeight (RT_TB + 64)
#define RTFO_FontStyle (RT_TB + 65)
#define RTGS_AllowEmpty (RT_TB + 80)
#define RTSC_DisplayID (RT_TB + 81)

#define RT_FILEREQ 0
#define RT_REQINFO 1
#define RT_FONTREQ 2
#define RT_SCREENMODEREQ 3

#define FREQF_MULTISELECT 0x1
#define FREQF_SAVE 0x2
#define FREQF_NOFILES 0x8
#define FREQF_PATGAD 0x10
#define FREQF_SELECTDIRS 0x1000

#define CALL_HANDLER 0x80000000u

/* rtFileRequester */
#define FR_FLAGS 8
#define FR_DIR 16
#define FR_MATCHPAT 20
/* rtFontRequester */
#define FO_ATTR 16
/* rtScreenModeRequester */
#define SM_ID 16
#define SM_WIDTH 20
#define SM_HEIGHT 22
#define SM_DEPTH 24
#define SM_OVERSCAN 26
#define SM_AUTOSCROLL 28
/* rtReqInfo */
#define RI_TITLE 12

/* private buffers after the public part */
#define REQ_SIZE 512
#define B_DIR 64
#define B_PAT 320
#define B_FONT 448

/* ReqToolsBase */
#define RTB_INTUITION 42
#define RTB_GFX 46
#define RTB_DOS 50
#define RTB_GADTOOLS 54
#define RTB_UTILITY 58
#define RTB_PREFS 82
#define RTB_SIZE 256

#define ASL_TB 0x80080000u
#define ASL_(n) (ASL_TB + (n))

typedef struct RtReq { uint32_t req; int type; struct RtReq *next; } RtReq;
static RtReq *reqs;
static RtReq *rt_of(uint32_t r) { for (RtReq *q = reqs; q; q = q->next) if (q->req == r) return q; return 0; }

/* what the common tags say */
typedef struct {
    uint32_t window, screen, handler, title, gadfmt, textfmt, textargs, okt;
    int underscore, allow_empty, has_min, has_max;
    int32_t min, max;
    uint32_t flags;
    int fflags;
} Opts;

static void read_opts(uint32_t tags, Opts *o) {
    uint32_t st = tags, ti;
    while ((ti = tag_next(&st))) {
        uint32_t t = rd32(ti), d = rd32(ti + 4);
        switch (t) {
        case RT_Window: o->window = d; break;
        case RT_Screen: o->screen = d; break;
        case RT_ReqHandler: o->handler = d; break;
        case RT_Underscore: o->underscore = (int)(d & 0xFF); break;
        case RTEZ_ReqTitle: o->title = d; break;
        case RTGL_Min: o->min = (int32_t)d; o->has_min = 1; break;
        case RTGL_Max: o->max = (int32_t)d; o->has_max = 1; break;
        case RTGL_GadFmt: o->gadfmt = d; break;
        case RTGL_TextFmt: o->textfmt = d; break;
        case RTGL_TextFmtArgs: o->textargs = d; break;
        case RTFI_Flags: o->flags = d; o->fflags = 1; break;
        case RTFI_OkText: o->okt = d; break;
        case RTGS_AllowEmpty: o->allow_empty = d != 0; break;
        }
    }
}

/* the answer, now or through a request handler that already holds it */
static uint32_t answer(Opts *o, uint32_t result) {
    if (!o->handler) return result;
    uint32_t hi = exec_allocvec(32, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(hi + 4, 0);            /* WaitMask */
    wr32(hi + 8, DOSTRUE);      /* DoNotWait */
    wr32(hi + 12, result);
    wr32(o->handler, hi);
    return CALL_HANDLER;
}

static void strip(char *s, int u) {
    if (!u) return;
    char *d = s;
    for (; *s; s++) if (*s != u) *d++ = *s;
    *d = 0;
}

static NScreen *pick_screen(Opts *o) {
    NScreen *s = 0;
    if (o->window) s = nscr_of(rd32(o->window + Window_WScreen));
    if (!s && o->screen) s = nscr_of(o->screen);
    if (!s) s = workbench_screen(1);
    return s;
}

/* ---- requests through asl.library ------------------------------------------------ */

static void apply_attrs(RtReq *q, uint32_t tags) {
    uint32_t st = tags, ti, r = q->req;
    char b[256];
    while ((ti = tag_next(&st))) {
        uint32_t t = rd32(ti), d = rd32(ti + 4);
        if (q->type == RT_FILEREQ && t == RTFI_Dir && d) { str_get(d, b, sizeof b); put_cstr(r + B_DIR, b, 256); }
        else if (q->type == RT_FILEREQ && t == RTFI_MatchPat && d) { str_get(d, b, sizeof b); put_cstr(r + B_PAT, b, 128); }
        else if (q->type == RT_FILEREQ && t == RTFI_Flags) wr32(r + FR_FLAGS, d);
        else if (q->type == RT_FONTREQ && t == RTFO_FontName && d) { str_get(d, b, sizeof b); put_cstr(r + B_FONT, b, 64); }
        else if (q->type == RT_FONTREQ && t == RTFO_FontHeight) wr16(r + FO_ATTR + 4, d);
        else if (q->type == RT_FONTREQ && t == RTFO_FontStyle) wr8(r + FO_ATTR + 6, d);
        else if (q->type == RT_SCREENMODEREQ && t == RTSC_DisplayID) wr32(r + SM_ID, d);
    }
}

static uint32_t rt_AllocRequestA(M68K *c) {
    int type = (int)c->d[0];
    if (type < RT_FILEREQ || type > RT_SCREENMODEREQ) return 0;
    uint32_t r = exec_allocvec(REQ_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!r) return 0;
    RtReq *q = calloc(1, sizeof *q);
    q->req = r;
    q->type = type;
    q->next = reqs;
    reqs = q;
    if (type == RT_FILEREQ) {
        wr32(r + FR_DIR, r + B_DIR);
        wr32(r + FR_MATCHPAT, r + B_PAT);
    } else if (type == RT_FONTREQ) {
        put_cstr(r + B_FONT, "topaz.font", 64);
        wr32(r + FO_ATTR, r + B_FONT);
        wr16(r + FO_ATTR + 4, 8);
    } else if (type == RT_SCREENMODEREQ) {
        wr32(r + SM_ID, 0x00029000);
        wr16(r + SM_WIDTH, 640);
        wr16(r + SM_HEIGHT, 256);
        wr16(r + SM_DEPTH, 2);
    }
    apply_attrs(q, c->a[0]);
    return r;
}

static uint32_t rt_FreeRequest(M68K *c) {
    RtReq **pp = &reqs;
    while (*pp && (*pp)->req != c->a[1]) pp = &(*pp)->next;
    if (!*pp) return 0;
    RtReq *q = *pp;
    *pp = q->next;
    exec_freevec(q->req);
    free(q);
    return 0;
}

static uint32_t rt_ChangeReqAttrA(M68K *c) {
    RtReq *q = rt_of(c->a[1]);
    if (q) apply_attrs(q, c->a[0]);
    return 0;
}

static uint32_t rt_FileRequestA(M68K *c) {
    uint32_t r = c->a[1], file = c->a[2], title = c->a[3], tags = c->a[0];
    RtReq *q = rt_of(r);
    if (!q || q->type != RT_FILEREQ) return 0;
    apply_attrs(q, tags);
    Opts o = { 0 };
    read_opts(tags, &o);
    uint32_t flags = o.fflags ? o.flags : rd32(r + FR_FLAGS);
    int multi = (flags & FREQF_MULTISELECT) != 0, nofiles = (flags & FREQF_NOFILES) != 0;
    Arena A = { 0 };
    char name[256] = "", ok[64] = "";
    if (file && !nofiles) str_get(file, name, sizeof name);
    if (o.okt) { str_get(o.okt, ok, sizeof ok); strip(ok, o.underscore ? o.underscore : '_'); }
    uint32_t at = ar_tags(&A, ASL_(1), title ? title : ar_str(&A, "Choisissez un fichier"),
                          ASL_(2), o.window, ASL_(40), o.screen,
                          ASL_(8), ar_str(&A, name), ASL_(9), r + B_DIR, ASL_(10), r + B_PAT,
                          ASL_(44), (flags & FREQF_SAVE) != 0, ASL_(46), (flags & FREQF_PATGAD) != 0,
                          ASL_(47), nofiles, ASL_(18), ok[0] ? ar_str(&A, ok) : 0, 0);
    uint32_t ar = lib_call(c, AslBase, "asl", "AllocAslRequest", 0, at);
    uint32_t result = 0;
    if (ar && lib_call(c, AslBase, "asl", "AslRequest", ar, 0)) {
        char dir[256] = "", got[256] = "";
        str_get(rd32(ar + FileRequester_fr_Drawer), dir, sizeof dir);
        str_get(rd32(ar + FileRequester_fr_File), got, sizeof got);
        put_cstr(r + B_DIR, dir, 256);
        if (!got[0] && !nofiles && !(flags & FREQF_SELECTDIRS)) result = 0;
        else if (multi && !nofiles) {
            uint32_t n = (uint32_t)strlen(got);
            uint32_t fl = exec_allocvec(12 + n + 1, MEMF_PUBLIC | MEMF_CLEAR);
            wr32(fl, 0);
            wr32(fl + 4, n);
            wr32(fl + 8, fl + 12);
            put_cstr(fl + 12, got, (int)n + 1);
            result = fl;
        } else {
            if (file && !nofiles) put_cstr(file, got, 108);
            result = DOSTRUE;
        }
    }
    if (ar) lib_call(c, AslBase, "asl", "FreeAslRequest", ar);
    ar_free(&A);
    return answer(&o, result);
}

static uint32_t rt_FreeFileList(M68K *c) {
    for (uint32_t fl = c->a[0]; fl;) { uint32_t n = rd32(fl); exec_freevec(fl); fl = n; }
    return 0;
}

static uint32_t rt_FontRequestA(M68K *c) {
    uint32_t r = c->a[1], title = c->a[3], tags = c->a[0];
    RtReq *q = rt_of(r);
    if (!q || q->type != RT_FONTREQ) return 0;
    apply_attrs(q, tags);
    Opts o = { 0 };
    read_opts(tags, &o);
    Arena A = { 0 };
    uint32_t at = ar_tags(&A, ASL_(1), title ? title : ar_str(&A, "Choisissez une police"), ASL_(2), o.window,
                          ASL_(40), o.screen, ASL_(10), r + B_FONT, ASL_(11), rd16(r + FO_ATTR + 4), 0);
    uint32_t ar = lib_call(c, AslBase, "asl", "AllocAslRequest", 1, at), result = 0;
    if (ar && lib_call(c, AslBase, "asl", "AslRequest", ar, 0)) {
        uint32_t ta = ar + 8;                                    /* fo_Attr */
        char fn[64];
        str_get(rd32(ta), fn, sizeof fn);
        put_cstr(r + B_FONT, fn, 64);
        wr16(r + FO_ATTR + 4, rd16(ta + 4));
        wr8(r + FO_ATTR + 6, rd8(ta + 6));
        wr8(r + FO_ATTR + 7, rd8(ta + 7));
        result = DOSTRUE;
    }
    if (ar) lib_call(c, AslBase, "asl", "FreeAslRequest", ar);
    ar_free(&A);
    return answer(&o, result);
}

static uint32_t rt_ScreenModeRequestA(M68K *c) {
    uint32_t r = c->a[1], title = c->a[3], tags = c->a[0];
    RtReq *q = rt_of(r);
    if (!q || q->type != RT_SCREENMODEREQ) return 0;
    apply_attrs(q, tags);
    Opts o = { 0 };
    read_opts(tags, &o);
    Arena A = { 0 };
    uint32_t at = ar_tags(&A, ASL_(1), title ? title : ar_str(&A, "Choisissez un mode d'\xe9" "cran"), ASL_(2), o.window,
                          ASL_(40), o.screen, ASL_(100), rd32(r + SM_ID), 0);
    uint32_t ar = lib_call(c, AslBase, "asl", "AllocAslRequest", 2, at), result = 0;
    if (ar && lib_call(c, AslBase, "asl", "AslRequest", ar, 0)) {
        wr32(r + SM_ID, rd32(ar));
        wr16(r + SM_WIDTH, rd32(ar + 4));
        wr16(r + SM_HEIGHT, rd32(ar + 8));
        wr16(r + SM_DEPTH, rd16(ar + 12));
        wr16(r + SM_OVERSCAN, rd16(ar + 14));
        wr32(r + SM_AUTOSCROLL, rd16(ar + 16));
        result = DOSTRUE;
    }
    if (ar) lib_call(c, AslBase, "asl", "FreeAslRequest", ar);
    ar_free(&A);
    return answer(&o, result);
}

/* ---- messages -------------------------------------------------------------------- */

static uint32_t req_title(uint32_t reqinfo, Opts *o) {
    if (o->title) return o->title;
    if (reqinfo && rd32(reqinfo + RI_TITLE)) return rd32(reqinfo + RI_TITLE);
    return 0;
}

static uint32_t rt_EZRequestA(M68K *c) {
    uint32_t body = c->a[1], gadfmt = c->a[2], reqinfo = c->a[3], args = c->a[4], tags = c->a[0];
    Opts o = { 0 };
    read_opts(tags, &o);
    Arena A = { 0 };
    char gads[256] = "Ok";
    if (gadfmt) str_get(gadfmt, gads, sizeof gads);
    strip(gads, o.underscore);
    uint32_t title = req_title(reqinfo, &o);
    uint32_t es = ar_alloc(&A, 20);
    wr32(es, 20);
    wr32(es + 4, 0);
    wr32(es + 8, title ? title : ar_str(&A, "Requ\xeate"));
    wr32(es + 12, body ? body : ar_str(&A, ""));
    wr32(es + 16, ar_str(&A, gads));
    uint32_t r = lib_call(c, IntuitionBase, "intuition", "EasyRequestArgs", o.window, es, 0, args);
    ar_free(&A);
    return answer(&o, r);
}

/* a string to edit, in a small window: text lines, the field, OK and Cancel */
static int run_string(M68K *c, Opts *o, uint32_t title, const char *text, char *buf, int max, int numeric) {
    NScreen *s = pick_screen(o);
    if (!s) return 0;
    Arena A = { 0 };
    char lines[16][128];
    int nl = 0, tw = 0;
    for (const char *p = text; p && *p && nl < 16;) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if (n > 127) n = 127;
        memcpy(lines[nl], p, (size_t)n);
        lines[nl][n] = 0;
        if (n * 8 > tw) tw = n * 8;
        nl++;
        p = e ? e + 1 : p + n;
    }
    int W = tw + 40;
    if (W < 300) W = 300;
    if (W > s->w) W = s->w;
    int bt = rd8(s->scr + 30) + 1, H = bt + 8 + nl * 10 + 4 + 14 + 8 + 14 + 8;
    char ttl[128] = "Entrez une valeur";
    if (title) str_get(title, ttl, sizeof ttl);
    uint32_t idcmp = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW;
    uint32_t tags = ar_tags(&A, WA_Left_, (s->w - W) / 2, WA_Top_, (s->h - H) / 2, WA_Width_, W, WA_Height_, H,
                            WA_Title_, ar_str(&A, ttl), WA_CustomScreen_, s->scr,
                            WA_DragBar_, 1, WA_DepthGadget_, 1, WA_CloseGadget_, 1, WA_Activate_, 1,
                            WA_IDCMP_, idcmp, 0);
    uint32_t win = lib_call(c, IntuitionBase, "intuition", "OpenWindowTagList", 0, tags);
    if (!win) { ar_free(&A); return 0; }
    int bl = rd8(win + Window_BorderLeft), br = rd8(win + Window_BorderRight), wt = rd8(win + Window_BorderTop);
    int iw = (int16_t)rd16(win + Window_Width) - bl - br;
    uint32_t rp = rd32(win + Window_RPort);
    lib_call(c, GfxBase, "graphics", "SetAPen", rp, 1);
    for (int i = 0; i < nl; i++) {
        int x = bl + (iw - (int)strlen(lines[i]) * 8) / 2;
        lib_call(c, GfxBase, "graphics", "Move", rp, x, wt + 8 + i * 10 + 6);
        lib_call(c, GfxBase, "graphics", "Text", rp, ar_str(&A, lines[i]), (uint32_t)strlen(lines[i]));
    }
    uint32_t vi = lib_call(c, GadToolsBase, "gadtools", "GetVisualInfoA", s->scr, 0);
    uint32_t glist = ar_alloc(&A, 4);
    uint32_t g = lib_call(c, GadToolsBase, "gadtools", "CreateContext", glist);
    int sy = wt + 8 + nl * 10 + 4, by = sy + 14 + 8;
    uint32_t gs = g = ntk_new_gadget(c, &A, 12 /* STRING_KIND */, g, vi, bl + 8, sy, iw - 16, 14, 0, 1, 0,
                                     ar_tags(&A, 0x80080000u + 45, ar_str(&A, buf), 0x80080000u + 46, (uint32_t)max, 0));
    int bw = 90;
    char ok[32] = "Ok", ca[32] = "Annuler";
    if (o->gadfmt) {
        char gf[96];
        str_get(o->gadfmt, gf, sizeof gf);
        strip(gf, o->underscore);
        char *bar = strchr(gf, '|');
        if (bar) { *bar = 0; snprintf(ca, sizeof ca, "%s", bar + 1); }
        snprintf(ok, sizeof ok, "%s", gf);
    }
    g = ntk_new_gadget(c, &A, 1, g, vi, bl + 8, by, bw, 14, ok, 2, 0x10, 0);
    g = ntk_new_gadget(c, &A, 1, g, vi, bl + iw - 8 - bw, by, bw, 14, ca, 3, 0x10, 0);
    uint32_t first = rd32(glist);
    lib_call(c, IntuitionBase, "intuition", "AddGList", win, first, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    lib_call(c, IntuitionBase, "intuition", "RefreshGList", first, win, 0, 0xFFFFFFFFu);
    lib_call(c, IntuitionBase, "intuition", "ActivateGadget", gs, win, 0);
    int done = 0, result = 0;
    while (!done) {
        exec_wait(ntk_sigmask(win));
        NMsg m;
        while (!done && ntk_getmsg(win, &m)) {
            if (m.cls == IDCMP_CLOSEWINDOW || (m.cls == IDCMP_VANILLAKEY && m.code == 27)) { done = 1; break; }
            if (m.cls != IDCMP_GADGETUP) continue;
            int id = (int)rd16(m.iaddr + Gadget_GadgetID);
            if (id == 1 || id == 2) {
                char v[256];
                ntk_gadget_string(gs, v, sizeof v);
                if (!v[0] && !o->allow_empty && !numeric) continue;
                snprintf(buf, (size_t)max, "%s", v);
                result = 1;
                done = 1;
            } else if (id == 3) done = 1;
        }
    }
    lib_call(c, IntuitionBase, "intuition", "CloseWindow", win);
    lib_call(c, GadToolsBase, "gadtools", "FreeGadgets", rd32(glist));
    lib_call(c, GadToolsBase, "gadtools", "FreeVisualInfo", vi);
    ar_free(&A);
    return result;
}

extern uint32_t intuition_format(M68K *c, uint32_t fmt, uint32_t args, char *out, int max);

static void text_of(M68K *c, Opts *o, char *out, int max) {
    out[0] = 0;
    if (o->textfmt) intuition_format(c, o->textfmt, o->textargs, out, max);
}

static uint32_t rt_GetStringA(M68K *c) {
    uint32_t buffer = c->a[1], title = c->a[2], reqinfo = c->a[3], tags = c->a[0];
    int max = (int)c->d[0] + 1;
    if (max > 1024) max = 1024;
    Opts o = { 0 };
    read_opts(tags, &o);
    char buf[1024], text[512];
    str_get(buffer, buf, max);
    text_of(c, &o, text, sizeof text);
    uint32_t t = title ? title : req_title(reqinfo, &o);
    int r = run_string(c, &o, t, text, buf, max, 0);
    if (r) put_cstr(buffer, buf, max);
    return answer(&o, r ? DOSTRUE : 0);
}

static uint32_t rt_GetLongA(M68K *c) {
    uint32_t ptr = c->a[1], title = c->a[2], reqinfo = c->a[3], tags = c->a[0];
    Opts o = { 0 };
    read_opts(tags, &o);
    char buf[32], text[512];
    snprintf(buf, sizeof buf, "%d", (int32_t)rd32(ptr));
    text_of(c, &o, text, sizeof text);
    uint32_t t = title ? title : req_title(reqinfo, &o);
    for (;;) {
        if (!run_string(c, &o, t, text, buf, sizeof buf, 1)) return answer(&o, 0);
        char *end;
        long v = strtol(buf, &end, 10);
        if (end == buf) continue;
        if (o.has_min && v < o.min) v = o.min;
        if (o.has_max && v > o.max) v = o.max;
        wr32(ptr, (uint32_t)v);
        return answer(&o, DOSTRUE);
    }
}

static uint32_t rt_PaletteRequestA(M68K *c) {
    Opts o = { 0 };
    read_opts(c->a[0], &o);
    return answer(&o, 0xFFFFFFFFu);                              /* -1: cancelled */
}

static uint32_t rt_ReqHandlerA(M68K *c) {
    uint32_t hi = c->a[1];
    if (!hi) return 0;
    uint32_t r = rd32(hi + 12);
    exec_freevec(hi);
    return r;
}

/* ---- helpers for windows and screens --------------------------------------------- */

static uint32_t busy(M68K *c, uint32_t win, int on) {
    if (!win) return 0;
    Arena A = { 0 };
    lib_call(c, IntuitionBase, "intuition", "SetWindowPointerA", win, ar_tags(&A, 0x80000098u, (uint32_t)on, 0));
    ar_free(&A);
    return 0;
}
static uint32_t rt_SetWaitPointer(M68K *c) { return busy(c, c->a[0], 1); }
static uint32_t rt_LockWindow(M68K *c) {
    if (!c->a[0]) return 0;
    busy(c, c->a[0], 1);
    return exec_allocvec(8, MEMF_PUBLIC | MEMF_CLEAR);
}
static uint32_t rt_UnlockWindow(M68K *c) {
    busy(c, c->a[0], 0);
    if (c->a[1]) exec_freevec(c->a[1]);
    return 0;
}
static uint32_t rt_CloseWindowSafely(M68K *c) {
    if (c->a[0]) lib_call(c, IntuitionBase, "intuition", "CloseWindow", c->a[0]);
    return 0;
}
static uint32_t rt_ScreenToFrontSafely(M68K *c) {
    if (c->a[0]) lib_call(c, IntuitionBase, "intuition", "ScreenToFront", c->a[0]);
    return 0;
}

static uint32_t rt_GetVScreenSize(M68K *c) {
    uint32_t s = c->a[0];
    uint32_t w = s ? rd16(s + Screen_Width) : 640, h = s ? rd16(s + Screen_Height) : 256;
    if (c->a[1]) wr32(c->a[1], w);
    if (c->a[2]) wr32(c->a[2], h);
    return h > 300 ? 4 : 2;                                      /* spacing between gadgets */
}

static uint32_t rt_SetReqPosition(M68K *c) {
    uint32_t pos = c->d[0], nw = c->a[0], scr = c->a[1], win = c->a[2];
    if (!nw) return 0;
    int w = (int16_t)rd16(nw + 4), h = (int16_t)rd16(nw + 6);
    int sw = scr ? (int16_t)rd16(scr + Screen_Width) : 640, sh = scr ? (int16_t)rd16(scr + Screen_Height) : 256;
    int x = 0, y = 0;
    switch (pos) {
    case 0:                                                      /* REQPOS_POINTER */
        x = (scr ? (int16_t)rd16(scr + Screen_MouseX) : 0) - w / 2;
        y = (scr ? (int16_t)rd16(scr + Screen_MouseX - 2) : 0) - h / 2;
        break;
    case 1: case 3:                                              /* about the window */
        if (win) {
            int wx = (int16_t)rd16(win + Window_LeftEdge), wy = (int16_t)rd16(win + Window_LeftEdge + 2);
            int ww = (int16_t)rd16(win + Window_Width), wh = (int16_t)rd16(win + Window_Height);
            x = pos == 1 ? wx + (ww - w) / 2 : wx;
            y = pos == 1 ? wy + (wh - h) / 2 : wy;
            break;
        }
        /* fall through */
    case 2: x = (sw - w) / 2; y = (sh - h) / 2; break;           /* REQPOS_CENTERSCR */
    default: x = 0; y = 0; break;                                /* REQPOS_TOPLEFTSCR */
    }
    if (x + w > sw) x = sw - w;
    if (y + h > sh) y = sh - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    wr16(nw, (uint32_t)x);
    wr16(nw + 2, (uint32_t)y);
    return 0;
}

static uint32_t rt_Spread(M68K *c) {
    uint32_t pos = c->a[0], size = c->a[1];
    int32_t length = (int32_t)c->d[0], min = (int32_t)c->d[1], max = (int32_t)c->d[2], num = (int32_t)c->d[3];
    if (num <= 0) return 0;
    int32_t space = max - min - length, at = min;
    for (int32_t i = 0; i < num; i++) {
        wr32(pos + 4 * (uint32_t)i, (uint32_t)(at + (num > 1 ? space * i / (num - 1) : space / 2)));
        at += (int32_t)rd32(size + 4 * (uint32_t)i);
    }
    return 0;
}

static uint32_t rt_LockPrefs(M68K *c) { (void)c; return ReqToolsBase + RTB_PREFS; }
static uint32_t rt_Nop(M68K *c) { (void)c; return 0; }

static const NativeEntry rt_natives[] = {
    { "rtAllocRequestA", rt_AllocRequestA }, { "rtFreeRequest", rt_FreeRequest },
    { "rtFreeReqBuffer", rt_Nop }, { "rtChangeReqAttrA", rt_ChangeReqAttrA },
    { "rtFileRequestA", rt_FileRequestA }, { "rtFreeFileList", rt_FreeFileList },
    { "rtEZRequestA", rt_EZRequestA }, { "rtGetStringA", rt_GetStringA },
    { "rtGetLongA", rt_GetLongA }, { "rtLockPrefs", rt_LockPrefs }, { "rtUnlockPrefs", rt_Nop },
    { "rtFontRequestA", rt_FontRequestA }, { "rtPaletteRequestA", rt_PaletteRequestA },
    { "rtReqHandlerA", rt_ReqHandlerA }, { "rtSetWaitPointer", rt_SetWaitPointer },
    { "rtGetVScreenSize", rt_GetVScreenSize }, { "rtSetReqPosition", rt_SetReqPosition },
    { "rtSpread", rt_Spread }, { "rtScreenToFrontSafely", rt_ScreenToFrontSafely },
    { "rtScreenModeRequestA", rt_ScreenModeRequestA }, { "rtCloseWindowSafely", rt_CloseWindowSafely },
    { "rtLockWindow", rt_LockWindow }, { "rtUnlockWindow", rt_UnlockWindow },
    { 0, 0 }
};

/* after dos.library: ReqToolsBase holds its base */
void reqtools_init(void) {
    ReqToolsBase = lib_create("reqtools.library", "reqtools", RTB_SIZE, rt_natives, 38, 3, "reqtools 38.3 (AmigaMac)");
    wr32(ReqToolsBase + RTB_INTUITION, IntuitionBase);
    wr32(ReqToolsBase + RTB_GFX, GfxBase);
    wr32(ReqToolsBase + RTB_DOS, DOSBase);
    wr32(ReqToolsBase + RTB_GADTOOLS, GadToolsBase);
    wr32(ReqToolsBase + RTB_UTILITY, UtilityBase);
    wr32(ReqToolsBase + RTB_PREFS, 12 + 46 + 4 + 6 * 16);       /* ReqToolsPrefs.PrefsSize */
    lib_add(ReqToolsBase);
}
