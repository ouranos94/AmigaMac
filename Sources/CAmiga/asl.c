/* asl.library, written from the asl.library autodocs and libraries/asl.h.
 *
 * The requesters are ordinary GadTools windows built through the public
 * library calls (lib_call), and run synchronously in the caller's task, as
 * AslRequest() does. File requester: list of the drawer (drawers first),
 * Pattern/Drawer/File fields, OK / Volumes / Parent / Cancel. Font and
 * screen mode requesters: a list of what this system offers.
 *
 * A requester is one block: the public structure, then private buffers for
 * the strings it hands back (fr_Drawer, fr_File, fr_Pattern, font name). */
#include "wb.h"
#include "ntk.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

uint32_t AslBase;

#define ASL_TB 0x80080000u
#define FILE_REQ 0
#define FONT_REQ 1
#define SM_REQ 2

#define REQ_SIZE 1024
#define B_DRAWER 64
#define B_FILE 320
#define B_PATTERN 576
#define B_FONT 704
#define B_WBARG 768

/* FontRequester / ScreenModeRequester fields */
#define FO_ATTR 8
#define FO_FRONTPEN 16
#define FO_BACKPEN 17
#define FO_DRAWMODE 18
#define FO_USERDATA 20
#define FO_LEFT 24
#define FO_TATTR 32
#define SM_ID 0
#define SM_WIDTH 4
#define SM_HEIGHT 8
#define SM_DEPTH 12
#define SM_OVERSCAN 14
#define SM_AUTOSCROLL 16
#define SM_BMWIDTH 18
#define SM_BMHEIGHT 22
#define SM_LEFT 26
#define SM_USERDATA 44

/* gadtools */
#define BUTTON_KIND 1
#define LISTVIEW_KIND 4
#define STRING_KIND 12
#define GT_(n) (0x80080000u + (n))
#define GTLV_Top GT_(5)
#define GTLV_Labels GT_(6)
#define GTLV_ShowSelected GT_(53)
#define GTLV_Selected GT_(54)
#define GTST_String GT_(45)
#define GTST_MaxChars GT_(46)
#define PLACETEXT_LEFT 1
#define PLACETEXT_IN 0x10

typedef struct AslReq {
    uint32_t req;
    int type;
    char title[80], ok[32], cancel[32];
    int left, top, width, height;
    uint32_t window, screen;
    uint32_t flags1, flags2;
    uint32_t accept, reject;           /* parsed patterns (68k) */
    int fsize, fstyle, fpen, bpen;
    uint32_t smid;
    int depth;
    uint32_t lock;                     /* wa_Lock handed back */
    struct AslReq *next;
} AslReq;

static AslReq *reqs;
static AslReq *req_of(uint32_t r) { for (AslReq *a = reqs; a; a = a->next) if (a->req == r) return a; return 0; }

#define FRF_DOPATTERNS 0x1
#define FRF_DOMULTISELECT 0x8
#define FRF_DOSAVEMODE 0x20
#define FRF_DRAWERSONLY 0x1
#define FRF_REJECTICONS 0x4

static void set_flag(uint32_t *f, uint32_t bit, uint32_t on) { *f = on ? *f | bit : *f & ~bit; }

static void apply_tags(AslReq *a, uint32_t tags) {
    uint32_t st = tags, ti, r = a->req;
    while ((ti = tag_next(&st))) {
        uint32_t t = rd32(ti), d = rd32(ti + 4);
        if (t < ASL_TB || t > ASL_TB + 200) continue;
        switch (t - ASL_TB) {
        case 1: if (d) str_get(d, a->title, sizeof a->title); break;
        case 2: a->window = d; break;
        case 3: a->left = (int16_t)d; break;
        case 4: a->top = (int16_t)d; break;
        case 5: a->width = (int16_t)d; break;
        case 6: a->height = (int16_t)d; break;
        case 8: if (a->type == FILE_REQ && d) { char b[256]; str_get(d, b, sizeof b); put_cstr(r + B_FILE, b, 256); } break;
        case 9: if (a->type == FILE_REQ && d) { char b[256]; str_get(d, b, sizeof b); put_cstr(r + B_DRAWER, b, 256); } break;
        case 10: if (d) { char b[128]; str_get(d, b, sizeof b); put_cstr(r + (a->type == FILE_REQ ? B_PATTERN : B_FONT), b, a->type == FILE_REQ ? 128 : 64); } break;
        case 11: a->fsize = (int)d; break;
        case 12: a->fstyle = (int)d; break;
        case 14: a->fpen = (int)d; break;
        case 15: a->bpen = (int)d; break;
        case 18: if (d) str_get(d, a->ok, sizeof a->ok); break;
        case 19: if (d) str_get(d, a->cancel, sizeof a->cancel); break;
        case 20: if (a->type == FILE_REQ) a->flags1 = d; break;
        case 22: if (a->type == FILE_REQ) a->flags2 = d; break;
        case 40: a->screen = d; break;
        case 44: if (a->type == FILE_REQ) set_flag(&a->flags1, FRF_DOSAVEMODE, d); break;
        case 45: if (a->type == FILE_REQ) set_flag(&a->flags1, FRF_DOMULTISELECT, d); break;
        case 46: if (a->type == FILE_REQ) set_flag(&a->flags1, FRF_DOPATTERNS, d); break;
        case 47: if (a->type == FILE_REQ) set_flag(&a->flags2, FRF_DRAWERSONLY, d); break;
        case 52:
            wr32(r + (a->type == FILE_REQ ? FileRequester_fr_UserData : a->type == FONT_REQ ? FO_USERDATA : SM_USERDATA), d);
            break;
        case 60: if (a->type == FILE_REQ) set_flag(&a->flags2, FRF_REJECTICONS, d); break;
        case 61: a->reject = d; break;
        case 62: a->accept = d; break;
        case 100: a->smid = d; break;
        case 103: a->depth = (int)d; break;
        }
    }
}

/* ---- the screen and a window on it --------------------------------------------- */

static NScreen *pick_screen(AslReq *a) {
    NScreen *s = 0;
    if (a->window) s = nscr_of(rd32(a->window + Window_WScreen));
    if (!s && a->screen) s = nscr_of(a->screen);
    if (!s) s = workbench_screen(1);
    return s;
}

static uint32_t open_req_window(M68K *c, Arena *A, NScreen *s, AslReq *a, int defw, int defh) {
    int W = a->width > 0 ? a->width : defw, H = a->height > 0 ? a->height : defh;
    if (W > s->w) W = s->w;
    if (H > s->h - 12) H = s->h - 12;
    int X = a->left > 0 ? a->left : (s->w - W) / 2, Y = a->top > 0 ? a->top : (s->h - H) / 2;
    if (X + W > s->w) X = s->w - W;
    if (Y + H > s->h) Y = s->h - H;
    uint32_t idcmp = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE |
                     IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW;
    uint32_t tags = ar_tags(A, WA_Left_, X, WA_Top_, Y, WA_Width_, W, WA_Height_, H,
                            WA_Title_, ar_str(A, a->title), WA_CustomScreen_, s->scr,
                            WA_DragBar_, 1, WA_DepthGadget_, 1, WA_CloseGadget_, 1, WA_Activate_, 1,
                            WA_IDCMP_, idcmp, 0);
    return lib_call(c, IntuitionBase, "intuition", "OpenWindowTagList", 0, tags);
}

#define new_gadget ntk_new_gadget
#define set_gattr ntk_set_gattr

/* ---- list contents ----------------------------------------------------------------- */

typedef struct { char name[108]; int kind; } Ent;          /* kind: 0 file, 1 drawer, 2 volume */

typedef struct {
    Ent *e;
    int n, cap;
    uint32_t mem;                                           /* exec List + nodes + names */
} EntList;

static void ents_add(EntList *l, const char *name, int kind) {
    if (l->n == l->cap) { l->cap = l->cap * 2 + 32; l->e = realloc(l->e, sizeof(Ent) * (size_t)l->cap); }
    snprintf(l->e[l->n].name, sizeof l->e[l->n].name, "%s", name);
    l->e[l->n].kind = kind;
    l->n++;
}

static int ent_cmp(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    if (x->kind != y->kind) return y->kind - x->kind;     /* volumes, drawers, files */
    return strcasecmp(x->name, y->name);
}

/* build an exec List in 68k memory, the text padded so the tag sits at the right */
static uint32_t ents_list(EntList *l, int cols) {
    static const char *tag[] = { "", "Tir", "Vol" };
    uint32_t sz = LH_SIZE + 2 + (uint32_t)l->n * (14 + (uint32_t)cols + 2);
    uint32_t m = exec_allocvec(sz, MEMF_PUBLIC | MEMF_CLEAR);
    list_new(m);
    uint32_t p = m + LH_SIZE + 2;
    for (int i = 0; i < l->n; i++) {
        uint32_t node = p, name = p + 14;
        p += 14 + (uint32_t)cols + 2;
        char text[256];
        const char *t = tag[l->e[i].kind];
        int tl = (int)strlen(t), room = cols - (tl ? tl + 1 : 0);
        if (room < 1) room = 1;
        snprintf(text, sizeof text, "%-*.*s%s%s", tl ? room : 0, room, l->e[i].name, tl ? " " : "", t);
        put_cstr(name, text, cols + 1);
        wr32(node + LN_NAME, name);
        list_addtail(m, node);
    }
    return m;
}

static void ents_free(EntList *l) {
    if (l->mem) exec_freevec(l->mem);
    l->mem = 0;
    l->n = 0;
}

/* ---- file requester ------------------------------------------------------------------- */

enum { G_LIST = 1, G_PATTERN, G_DRAWER, G_FILE, G_OK, G_VOLUMES, G_PARENT, G_CANCEL };

static void join_path(char *out, int max, const char *dir, const char *name) {
    size_t l = strlen(dir);
    if (!l) snprintf(out, (size_t)max, "%s", name);
    else if (dir[l - 1] == ':' || dir[l - 1] == '/') snprintf(out, (size_t)max, "%s%s", dir, name);
    else snprintf(out, (size_t)max, "%s/%s", dir, name);
}

static int parsed_match(uint32_t parsed, const char *name) {
    char pat[256];
    int mark = rd8(parsed);
    str_get(parsed + (mark == 1 || mark == 2 ? 1 : 0), pat, sizeof pat);
    return dos_match_pattern(pat, name, 1);
}

static void read_drawer(AslReq *a, EntList *l, const char *drawer, const char *pattern, int volumes) {
    l->n = 0;
    if (volumes) {
        for (int i = 0; i < dos_assign_count(); i++) {
            char v[64];
            snprintf(v, sizeof v, "%s:", dos_assign_name(i));
            ents_add(l, v, 2);
        }
    } else {
        char hp[2048];
        DIR *d = dos_resolve(drawer, hp, sizeof hp) ? opendir(hp) : 0;
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (dos_hidden_name(e->d_name)) continue;
            char full[2600];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", hp, e->d_name);
            if (stat(full, &st)) continue;
            int dir = S_ISDIR(st.st_mode);
            size_t nl = strlen(e->d_name);
            if (!dir) {
                if (a->flags2 & FRF_DRAWERSONLY) continue;
                if ((a->flags2 & FRF_REJECTICONS) && nl > 5 && !strcasecmp(e->d_name + nl - 5, ".info")) continue;
                if (a->accept && !parsed_match(a->accept, e->d_name)) continue;
                if (a->reject && parsed_match(a->reject, e->d_name)) continue;
                if (pattern[0] && !dos_match_pattern(pattern, e->d_name, 1)) continue;
            }
            char l1[256];
            to_latin1(e->d_name, l1, sizeof l1);
            ents_add(l, l1, dir);
        }
        if (d) closedir(d);
    }
    qsort(l->e, (size_t)l->n, sizeof(Ent), ent_cmp);
}

static void current_drawer(char *out, int max) {
    char hp[2048];
    if (dos_resolve("", hp, sizeof hp)) dos_amiga_name(hp, out, max); else out[0] = 0;
}

static int run_file(M68K *c, AslReq *a) {
    uint32_t r = a->req;
    NScreen *s = pick_screen(a);
    if (!s) return 0;
    Arena A = { 0 };
    int patterns = (a->flags1 & FRF_DOPATTERNS) != 0, nofile = (a->flags2 & FRF_DRAWERSONLY) != 0;
    char drawer[256], file[256], pattern[128];
    str_get(r + B_DRAWER, drawer, sizeof drawer);
    str_get(r + B_FILE, file, sizeof file);
    str_get(r + B_PATTERN, pattern, sizeof pattern);
    if (!drawer[0]) current_drawer(drawer, sizeof drawer);
    if (!pattern[0]) snprintf(pattern, sizeof pattern, "#?");
    if (!a->title[0]) snprintf(a->title, sizeof a->title, "%s", nofile ? "Choisissez un tiroir" : "Choisissez un fichier");

    uint32_t win = open_req_window(c, &A, s, a, 340, 200);
    if (!win) { ar_free(&A); return 0; }
    int bl = rd8(win + Window_BorderLeft), bt = rd8(win + Window_BorderTop), br = rd8(win + Window_BorderRight), bb = rd8(win + Window_BorderBottom);
    int W = (int16_t)rd16(win + Window_Width), H = (int16_t)rd16(win + Window_Height);
    int iw = W - bl - br;
    uint32_t vi = lib_call(c, GadToolsBase, "gadtools", "GetVisualInfoA", s->scr, 0);
    uint32_t glist = ar_alloc(&A, 4);
    uint32_t g = lib_call(c, GadToolsBase, "gadtools", "CreateContext", glist);
    int nstr = patterns + 1 + !nofile;
    int by = H - bb - 4 - 14, sy = by - 4 - nstr * 16;
    int lx = bl + 6, ly = bt + 4, lw = iw - 12, lh = ((sy - 4 - ly - 4) / 8) * 8 + 4;
    uint32_t glv = g = new_gadget(c, &A, LISTVIEW_KIND, g, vi, lx, ly, lw, lh, 0, G_LIST, 0, 0);
    int cols = (lw - 16 - 12) / 8;
    int sx = bl + 70, sw = iw - 76, yy = sy;
    uint32_t gpat = 0, gdrw, gfile = 0;
    if (patterns) { gpat = g = new_gadget(c, &A, STRING_KIND, g, vi, sx, yy, sw, 14, "Motif", G_PATTERN, PLACETEXT_LEFT, ar_tags(&A, GTST_String, ar_str(&A, pattern), GTST_MaxChars, 120, 0)); yy += 16; }
    gdrw = g = new_gadget(c, &A, STRING_KIND, g, vi, sx, yy, sw, 14, "Tiroir", G_DRAWER, PLACETEXT_LEFT, ar_tags(&A, GTST_String, ar_str(&A, drawer), GTST_MaxChars, 250, 0));
    yy += 16;
    if (!nofile) gfile = g = new_gadget(c, &A, STRING_KIND, g, vi, sx, yy, sw, 14, "Fichier", G_FILE, PLACETEXT_LEFT, ar_tags(&A, GTST_String, ar_str(&A, file), GTST_MaxChars, 250, 0));
    int bw = (iw - 12 - 3 * 8) / 4;
    const char *bl_text[4] = { a->ok[0] ? a->ok : (a->flags1 & FRF_DOSAVEMODE) ? "Enregistrer" : "OK", "Volumes", "Parent", a->cancel[0] ? a->cancel : "Annuler" };
    for (int i = 0; i < 4; i++) g = new_gadget(c, &A, BUTTON_KIND, g, vi, bl + 6 + i * (bw + 8), by, bw, 14, bl_text[i], G_OK + i, PLACETEXT_IN, 0);
    uint32_t first = rd32(glist);
    lib_call(c, IntuitionBase, "intuition", "AddGList", win, first, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    lib_call(c, IntuitionBase, "intuition", "RefreshGList", first, win, 0, 0xFFFFFFFFu);

    EntList l = { 0 };
    int volumes = 0, result = 0, done = 0, last = -1;
    uint32_t lsecs = 0, lmicros = 0;
#define RELOAD() do { \
        set_gattr(c, &A, glv, win, GTLV_Labels, 0xFFFFFFFFu); \
        ents_free(&l); \
        read_drawer(a, &l, drawer, patterns ? pattern : "", volumes); \
        l.mem = ents_list(&l, cols); \
        uint32_t t_ = ar_tags(&A, GTLV_Labels, l.mem, GTLV_Top, 0, GTLV_Selected, 0xFFFFFFFFu, 0); \
        lib_call(c, GadToolsBase, "gadtools", "GT_SetGadgetAttrsA", glv, win, 0, t_); \
        set_gattr(c, &A, gdrw, win, GTST_String, ar_str(&A, volumes ? "" : drawer)); \
        last = -1; \
    } while (0)
    RELOAD();
    while (!done) {
        exec_wait(ntk_sigmask(win));
        NMsg m;
        while (!done && ntk_getmsg(win, &m)) {
            if (m.cls == IDCMP_CLOSEWINDOW) { done = 1; break; }
            if (m.cls == IDCMP_VANILLAKEY && m.code == 27) { done = 1; break; }
            if (m.cls != IDCMP_GADGETUP) continue;
            int id = (int)rd16(m.iaddr + Gadget_GadgetID);
            switch (id) {
            case G_LIST: {
                if ((int)m.code >= l.n) break;
                Ent *e = &l.e[m.code];
                if (e->kind == 2) { snprintf(drawer, sizeof drawer, "%s", e->name); volumes = 0; RELOAD(); break; }
                if (e->kind == 1) { char nd[256]; join_path(nd, sizeof nd, drawer, e->name); snprintf(drawer, sizeof drawer, "%s", nd); RELOAD(); break; }
                if (gfile) set_gattr(c, &A, gfile, win, GTST_String, ar_str(&A, e->name));
                if (last == (int)m.code && ntk_double_click(lsecs, lmicros, m.secs, m.micros)) { result = 1; done = 1; }
                last = (int)m.code; lsecs = m.secs; lmicros = m.micros;
                break;
            }
            case G_PATTERN: ntk_gadget_string(gpat, pattern, sizeof pattern); RELOAD(); break;
            case G_DRAWER: ntk_gadget_string(gdrw, drawer, sizeof drawer); volumes = 0; RELOAD(); break;
            case G_FILE: result = 1; done = 1; break;
            case G_OK: result = 1; done = 1; break;
            case G_VOLUMES: volumes = 1; RELOAD(); break;
            case G_PARENT: {
                if (volumes) break;
                size_t n = strlen(drawer);
                if (!n) { current_drawer(drawer, sizeof drawer); n = strlen(drawer); }
                if (n && drawer[n - 1] == ':') { volumes = 1; RELOAD(); break; }
                char *slash = strrchr(drawer, '/'), *colon = strchr(drawer, ':');
                if (slash) *slash = 0; else if (colon) colon[1] = 0; else drawer[0] = 0;
                RELOAD();
                break;
            }
            case G_CANCEL: done = 1; break;
            }
        }
    }
    if (result) {
        if (!volumes) ntk_gadget_string(gdrw, drawer, sizeof drawer);
        if (gfile) ntk_gadget_string(gfile, file, sizeof file); else file[0] = 0;
        put_cstr(r + B_DRAWER, drawer, 256);
        put_cstr(r + B_FILE, file, 256);
        if (a->lock) dos_unlock(a->lock);
        a->lock = dos_lock(drawer);
        wr32(r + B_WBARG + WBArg_wa_Lock, a->lock);
        wr32(r + B_WBARG + WBArg_wa_Name, r + B_FILE);
        wr32(r + FileRequester_fr_NumArgs, 1);
        wr32(r + FileRequester_fr_ArgList, r + B_WBARG);
    }
    if (patterns) put_cstr(r + B_PATTERN, pattern, 128);
    wr16(r + FileRequester_fr_LeftEdge, rd16(win + Window_LeftEdge));
    wr16(r + FileRequester_fr_TopEdge, rd16(win + Window_LeftEdge + 2));
    wr16(r + FileRequester_fr_Width, rd16(win + Window_Width));
    wr16(r + FileRequester_fr_Height, rd16(win + Window_Height));
    lib_call(c, IntuitionBase, "intuition", "CloseWindow", win);
    lib_call(c, GadToolsBase, "gadtools", "FreeGadgets", rd32(glist));
    lib_call(c, GadToolsBase, "gadtools", "FreeVisualInfo", vi);
    ents_free(&l);
    free(l.e);
    ar_free(&A);
    return result;
#undef RELOAD
}

/* ---- a list to choose from (fonts, screen modes) ------------------------------------- */

static int run_list(M68K *c, AslReq *a, const char *deftitle, const char **items, int n, int sel) {
    NScreen *s = pick_screen(a);
    if (!s) return -1;
    Arena A = { 0 };
    if (!a->title[0]) snprintf(a->title, sizeof a->title, "%s", deftitle);
    uint32_t win = open_req_window(c, &A, s, a, 260, 140);
    if (!win) { ar_free(&A); return -1; }
    int bl = rd8(win + Window_BorderLeft), bt = rd8(win + Window_BorderTop), br = rd8(win + Window_BorderRight), bb = rd8(win + Window_BorderBottom);
    int W = (int16_t)rd16(win + Window_Width), H = (int16_t)rd16(win + Window_Height), iw = W - bl - br;
    uint32_t vi = lib_call(c, GadToolsBase, "gadtools", "GetVisualInfoA", s->scr, 0);
    uint32_t glist = ar_alloc(&A, 4);
    uint32_t g = lib_call(c, GadToolsBase, "gadtools", "CreateContext", glist);
    int by = H - bb - 4 - 14, lh = ((by - 4 - bt - 4 - 4) / 8) * 8 + 4;
    EntList l = { 0 };
    for (int i = 0; i < n; i++) ents_add(&l, items[i], 0);
    int lw = iw - 12;
    l.mem = ents_list(&l, (lw - 28) / 8);
    uint32_t glv = g = new_gadget(c, &A, LISTVIEW_KIND, g, vi, bl + 6, bt + 4, lw, lh, 0, G_LIST, 0,
                                  ar_tags(&A, GTLV_Labels, l.mem, GTLV_ShowSelected, 0, GTLV_Selected, (uint32_t)sel, 0));
    (void)glv;
    int bw = (iw - 12 - 8) / 2;
    g = new_gadget(c, &A, BUTTON_KIND, g, vi, bl + 6, by, bw, 14, a->ok[0] ? a->ok : "OK", G_OK, PLACETEXT_IN, 0);
    g = new_gadget(c, &A, BUTTON_KIND, g, vi, bl + 6 + bw + 8, by, bw, 14, a->cancel[0] ? a->cancel : "Annuler", G_CANCEL, PLACETEXT_IN, 0);
    uint32_t first = rd32(glist);
    lib_call(c, IntuitionBase, "intuition", "AddGList", win, first, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    lib_call(c, IntuitionBase, "intuition", "RefreshGList", first, win, 0, 0xFFFFFFFFu);
    int done = 0, result = -1, cur = sel, last = -1;
    uint32_t ls = 0, lm = 0;
    while (!done) {
        exec_wait(ntk_sigmask(win));
        NMsg m;
        while (!done && ntk_getmsg(win, &m)) {
            if (m.cls == IDCMP_CLOSEWINDOW || (m.cls == IDCMP_VANILLAKEY && m.code == 27)) { done = 1; break; }
            if (m.cls != IDCMP_GADGETUP) continue;
            int id = (int)rd16(m.iaddr + Gadget_GadgetID);
            if (id == G_LIST) {
                cur = (int)m.code;
                if (last == cur && ntk_double_click(ls, lm, m.secs, m.micros)) { result = cur; done = 1; }
                last = cur; ls = m.secs; lm = m.micros;
            } else if (id == G_OK) { result = cur; done = 1; }
            else if (id == G_CANCEL) done = 1;
        }
    }
    lib_call(c, IntuitionBase, "intuition", "CloseWindow", win);
    lib_call(c, GadToolsBase, "gadtools", "FreeGadgets", rd32(glist));
    lib_call(c, GadToolsBase, "gadtools", "FreeVisualInfo", vi);
    ents_free(&l);
    free(l.e);
    ar_free(&A);
    return result;
}

static int run_font(M68K *c, AslReq *a) {
    static const char *items[] = { "topaz 8", "topaz 9" };
    uint32_t r = a->req;
    int sel = a->fsize == 9 ? 1 : 0;
    int i = run_list(c, a, "Choisissez une police", items, 2, sel);
    if (i < 0) return 0;
    put_cstr(r + B_FONT, "topaz.font", 64);
    for (int k = 0; k < 2; k++) {
        uint32_t ta = r + (k ? FO_TATTR : FO_ATTR);
        wr32(ta, r + B_FONT);
        wr16(ta + 4, i ? 9 : 8);
        wr8(ta + 6, (uint32_t)a->fstyle);
        wr8(ta + 7, 0);
    }
    wr8(r + FO_FRONTPEN, (uint32_t)a->fpen);
    wr8(r + FO_BACKPEN, (uint32_t)a->bpen);
    wr8(r + FO_DRAWMODE, JAM1);
    return 1;
}

static int run_screenmode(M68K *c, AslReq *a) {
    static const char *items[] = { "PAL : basse résolution", "PAL : haute résolution", "PAL : basse résolution entrelacée", "PAL : haute résolution entrelacée" };
    static const uint32_t ids[] = { 0x00021000, 0x00029000, 0x00021004, 0x00029004 };
    static const int ws[] = { 320, 640, 320, 640 }, hs[] = { 256, 256, 512, 512 };
    char l1[4][64];
    const char *it[4];
    for (int i = 0; i < 4; i++) {                              /* the font is Latin-1 */
        int k = 0;
        for (const unsigned char *p = (const unsigned char *)items[i]; *p && k < 63; p++) {
            if (*p == 0xC3 && p[1]) { l1[i][k++] = (char)(p[1] + 0x40); p++; } else l1[i][k++] = (char)*p;
        }
        l1[i][k] = 0;
        it[i] = l1[i];
    }
    int sel = 1;
    for (int i = 0; i < 4; i++) if (ids[i] == a->smid) sel = i;
    int i = run_list(c, a, "Choisissez un mode d'\xe9" "cran", it, 4, sel);
    if (i < 0) return 0;
    uint32_t r = a->req;
    wr32(r + SM_ID, ids[i]);
    wr32(r + SM_WIDTH, (uint32_t)ws[i]);
    wr32(r + SM_HEIGHT, (uint32_t)hs[i]);
    wr16(r + SM_DEPTH, (uint32_t)(a->depth > 0 ? a->depth : 3));
    wr16(r + SM_OVERSCAN, 1);
    wr16(r + SM_AUTOSCROLL, 1);
    wr32(r + SM_BMWIDTH, (uint32_t)ws[i]);
    wr32(r + SM_BMHEIGHT, (uint32_t)hs[i]);
    return 1;
}

/* ---- library vectors -------------------------------------------------------------------- */

static uint32_t alloc_req(int type, uint32_t tags) {
    if (type < 0 || type > SM_REQ) return 0;
    uint32_t r = exec_allocvec(REQ_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    AslReq *a = calloc(1, sizeof *a);
    a->req = r;
    a->type = type;
    a->fsize = 8;
    a->fpen = 1;
    a->depth = 3;
    a->smid = 0x00029000;
    a->next = reqs;
    reqs = a;
    if (type == FILE_REQ) {
        wr32(r + FileRequester_fr_File, r + B_FILE);
        wr32(r + FileRequester_fr_Drawer, r + B_DRAWER);
        wr32(r + FileRequester_fr_Pattern, r + B_PATTERN);
        put_cstr(r + B_PATTERN, "#?", 128);
    } else if (type == FONT_REQ) {
        put_cstr(r + B_FONT, "topaz.font", 64);
        wr32(r + FO_ATTR, r + B_FONT);
        wr16(r + FO_ATTR + 4, 8);
        wr32(r + FO_TATTR, r + B_FONT);
        wr16(r + FO_TATTR + 4, 8);
    }
    apply_tags(a, tags);
    return r;
}

static uint32_t asl_AllocAslRequest(M68K *c) { return alloc_req((int)c->d[0], c->a[0]); }
static uint32_t asl_AllocFileRequest(M68K *c) { (void)c; return alloc_req(FILE_REQ, 0); }
static uint32_t asl_FreeAslRequest(M68K *c) {
    AslReq **pp = &reqs;
    while (*pp && (*pp)->req != c->a[0]) pp = &(*pp)->next;
    if (!*pp) return 0;
    AslReq *a = *pp;
    *pp = a->next;
    if (a->lock) dos_unlock(a->lock);
    exec_freevec(a->req);
    free(a);
    return 0;
}
static uint32_t asl_AslRequest(M68K *c) {
    AslReq *a = req_of(c->a[0]);
    if (!a) return 0;
    apply_tags(a, c->a[1]);
    int r = a->type == FILE_REQ ? run_file(c, a) : a->type == FONT_REQ ? run_font(c, a) : run_screenmode(c, a);
    return r ? DOSTRUE : 0;
}
static uint32_t asl_RequestFile(M68K *c) { c->a[1] = 0; return asl_AslRequest(c); }
static uint32_t asl_Nop(M68K *c) { (void)c; return 0; }

static const NativeEntry asl_natives[] = {
    { "AllocFileRequest", asl_AllocFileRequest }, { "FreeFileRequest", asl_FreeAslRequest },
    { "RequestFile", asl_RequestFile }, { "AllocAslRequest", asl_AllocAslRequest },
    { "FreeAslRequest", asl_FreeAslRequest }, { "AslRequest", asl_AslRequest },
    { "AbortAslRequest", asl_Nop }, { "ActivateAslRequest", asl_Nop },
    { 0, 0 }
};

void asl_init(void) {
    AslBase = lib_create("asl.library", "asl", 64, asl_natives, 44, 1, "asl 44.1 (AmigaMac)");
    lib_add(AslBase);
}
