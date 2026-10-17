/* The Workbench, written from scratch for AmigaMac: the desktop of the
 * Workbench screen with the volume icons, drawer windows full of icons,
 * selection (click, shift-click, lasso), dragging icons around and into
 * other drawers, the menus, and starting programs the Workbench way
 * (a process without CLI that receives a WBStartup message).
 * Also workbench.library (AppWindows, AppIcons, AppMenuItems, OpenWorkbenchObject).
 *
 * It runs as a native process. Everything Amiga-visible (windows, menus,
 * gadgets, requesters) is made through the public library calls; icons are
 * drawn into an off-screen bitmap and copied into the window, which clips
 * them to the inner area. Dragged icons and the lasso are drawn in the
 * screen overlay, so they can travel over every window. */
#include "wb.h"
#include "ntk.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

uint32_t WorkbenchBase;
extern uint32_t SysBase, DOSBase;

#define IDCMP_WB (IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_MENUPICK | IDCMP_NEWSIZE | \
                  IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_ACTIVEWINDOW | IDCMP_VANILLAKEY)
#define SIGF_REQ (1u << 15)                       /* SIGBREAKF_CTRL_F: queued requests */
#define NO_POS 0x80000000u
#define AMTYPE_APPWINDOW 7
#define AMTYPE_APPICON 8
#define AMTYPE_APPMENUITEM 9

/* ---- state ------------------------------------------------------------------------ */

typedef struct WIcon {
    char name[256];                 /* host file name (no .info); volume name for disks */
    char path[600];                 /* Amiga path */
    char label[64];                 /* Latin-1 */
    int type;
    uint32_t dobj;
    int x, y, w, h, lw;
    int fixed, selected;
    uint32_t appicon;               /* AppIcon handle, or 0 */
    struct WIcon *next;
} WIcon;

typedef struct WBWin {
    uint32_t win;
    int backdrop;
    char path[600];                 /* Amiga path of the drawer, "" for the desktop */
    WIcon *icons;
    int sy, cw, ch;                 /* vertical scroll, content size */
    uint32_t prop, propinfo;
    uint32_t rp;                    /* 68k RastPort for off-screen drawing */
    int show_all, by_name;
    Arena A;
    struct WBWin *next;
} WBWin;

typedef struct Launch { uint32_t msg, seg; int nargs; struct Launch *next; } Launch;
typedef struct AppObj { uint32_t handle; int kind; uint32_t id, userdata, win, port; char text[64]; uint32_t dobj; struct AppObj *next; } AppObj;
typedef struct Req { int what; char name[600]; struct Req *next; } Req;
enum { REQ_OPEN = 1, REQ_UPDATE, REQ_CLOSE, REQ_INFO, REQ_APPCHANGED };

enum { M_NONE, M_PENDING, M_DRAG, M_LASSO };

static struct {
    int running;
    uint32_t task, port;
    M68K *c;
    NScreen *s;
    uint32_t vi, menus;
    Arena A;
    WBWin *wins, *active;
    Launch *launches;
    AppObj *apps;
    uint32_t appmsgs[64];
    int nappmsgs;
    Req *reqs;
    char lastmsg[256];
    /* the mouse */
    int mode;
    WBWin *mw;
    int mx0, my0, mx1, my1;          /* screen coordinates */
    WIcon *lasticon;
    uint32_t lsecs, lmicros;
} wb;

static void wb_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- paths ------------------------------------------------------------------------- */

static void join_path(char *out, int max, const char *dir, const char *name) {
    size_t l = strlen(dir);
    if (!l) snprintf(out, (size_t)max, "%s", name);
    else if (dir[l - 1] == ':' || dir[l - 1] == '/') snprintf(out, (size_t)max, "%s%s", dir, name);
    else snprintf(out, (size_t)max, "%s/%s", dir, name);
}

static int parent_path(const char *path, char *out, int max) {
    snprintf(out, (size_t)max, "%s", path);
    size_t l = strlen(out);
    if (!l || out[l - 1] == ':') return 0;
    char *slash = strrchr(out, '/'), *colon = strchr(out, ':');
    if (slash) *slash = 0; else if (colon) colon[1] = 0; else return 0;
    return 1;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/'), *c = strrchr(path, ':');
    if (s) return s + 1;
    if (c) return c + 1;
    return path;
}

static int host_of(const char *apath, char *hp, int max) { return dos_resolve(apath, hp, max); }

static const char *volume_label(const char *vol) {
    if (!strcasecmp(vol, "SYS:")) return "System";
    if (!strcasecmp(vol, "RAM:")) return "Ram Disk";
    if (!strcasecmp(vol, "Mac:")) return "Mac";
    return vol;
}

/* ---- small UI helpers ------------------------------------------------------------- */

static int easy(const char *title, const char *text, const char *gadgets) {
    Arena A = { 0 };
    uint32_t es = ar_alloc(&A, 20);
    wr32(es, 20);
    wr32(es + 8, ar_text(&A, title));
    wr32(es + 12, ar_text(&A, text));
    wr32(es + 16, ar_text(&A, gadgets));
    uint32_t win = wb.active ? wb.active->win : 0;
    int r = (int)lib_call(wb.c, IntuitionBase, "intuition", "EasyRequestArgs", win, es, 0, 0);
    ar_free(&A);
    return r;
}

static void wb_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(wb.lastmsg, sizeof wb.lastmsg, fmt, ap);
    va_end(ap);
    easy("Workbench", wb.lastmsg, "OK");
}

/* a string requester: returns 1 with buf filled, 0 if cancelled */
static int ask_string(const char *title, const char *prompt, char *buf, int max) {
    M68K *c = wb.c;
    NScreen *s = wb.s;
    Arena A = { 0 };
    int W = 360, H = 70;
    uint32_t tags = ar_tags(&A, WA_Left_, (s->w - W) / 2, WA_Top_, (s->h - H) / 2, WA_Width_, W, WA_Height_, H,
                            WA_Title_, ar_text(&A, title), WA_CustomScreen_, s->scr, WA_DragBar_, 1, WA_DepthGadget_, 1,
                            WA_CloseGadget_, 1, WA_Activate_, 1, WA_IDCMP_, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_VANILLAKEY, 0);
    uint32_t win = lib_call(c, IntuitionBase, "intuition", "OpenWindowTagList", 0, tags);
    if (!win) { ar_free(&A); return 0; }
    int bl = rd8(win + Window_BorderLeft), bt = rd8(win + Window_BorderTop), br = rd8(win + Window_BorderRight);
    int iw = W - bl - br;
    uint32_t glist = ar_alloc(&A, 4);
    uint32_t g = lib_call(c, GadToolsBase, "gadtools", "CreateContext", glist);
    char p1[256], b1[512];
    to_latin1(prompt, p1, sizeof p1);
    to_latin1(buf, b1, sizeof b1);
    g = ntk_new_gadget(c, &A, 13 /* TEXT */, g, wb.vi, bl + 6, bt + 4, iw - 12, 10, 0, 0, 0,
                       ar_tags(&A, 0x8008000Bu /* GTTX_Text */, ar_str(&A, p1), 0));
    uint32_t gs = g = ntk_new_gadget(c, &A, 12 /* STRING */, g, wb.vi, bl + 6, bt + 16, iw - 12, 14, 0, 1, 0,
                                     ar_tags(&A, 0x8008002Du /* GTST_String */, ar_str(&A, b1), 0x8008002Eu, (uint32_t)max - 1, 0));
    int bw = 100;
    g = ntk_new_gadget(c, &A, 1, g, wb.vi, bl + 6, bt + 34, bw, 14, "OK", 2, 0x10, 0);
    g = ntk_new_gadget(c, &A, 1, g, wb.vi, bl + iw - 6 - bw, bt + 34, bw, 14, "Annuler", 3, 0x10, 0);
    uint32_t first = rd32(glist);
    lib_call(c, IntuitionBase, "intuition", "AddGList", win, first, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    lib_call(c, IntuitionBase, "intuition", "RefreshGList", first, win, 0, 0xFFFFFFFFu);
    lib_call(c, IntuitionBase, "intuition", "ActivateGadget", gs, win, 0);
    int done = 0, ok = 0;
    while (!done) {
        exec_wait(ntk_sigmask(win));
        NMsg m;
        while (!done && ntk_getmsg(win, &m)) {
            if (m.cls == IDCMP_CLOSEWINDOW || (m.cls == IDCMP_VANILLAKEY && m.code == 27)) done = 1;
            else if (m.cls == IDCMP_GADGETUP) {
                int id = (int)rd16(m.iaddr + Gadget_GadgetID);
                if (id == 1 || id == 2) ok = done = 1;
                else if (id == 3) done = 1;
            }
        }
    }
    if (ok) {
        char l1[512];
        ntk_gadget_string(gs, l1, sizeof l1);
        /* back to UTF-8 for the host side */
        int k = 0;
        for (const unsigned char *p = (const unsigned char *)l1; *p && k < max - 2; p++) {
            if (*p >= 0x80) { buf[k++] = (char)(0xC0 | (*p >> 6)); buf[k++] = (char)(0x80 | (*p & 0x3F)); }
            else buf[k++] = (char)*p;
        }
        buf[k] = 0;
    }
    lib_call(c, IntuitionBase, "intuition", "CloseWindow", win);
    lib_call(c, GadToolsBase, "gadtools", "FreeGadgets", rd32(glist));
    ar_free(&A);
    return ok;
}

/* ---- reading a drawer --------------------------------------------------------------- */

static void free_icons(WBWin *w) {
    for (WIcon *i = w->icons, *n; i; i = n) {
        n = i->next;
        if (i->dobj && !i->appicon) icon_free(i->dobj);
        free(i);
    }
    w->icons = 0;
}

static WIcon *add_icon(WBWin *w, const char *name, const char *path, const char *label, uint32_t dobj, int type) {
    WIcon *ic = calloc(1, sizeof *ic);
    snprintf(ic->name, sizeof ic->name, "%s", name);
    snprintf(ic->path, sizeof ic->path, "%s", path);
    char l1[256];
    to_latin1(label, l1, sizeof l1);
    if (strlen(l1) > 22) { l1[20] = '.'; l1[21] = '.'; l1[22] = 0; }
    snprintf(ic->label, sizeof ic->label, "%s", l1);
    ic->dobj = dobj;
    ic->type = type;
    icon_size(dobj, &ic->w, &ic->h);
    ic->lw = (int)strlen(ic->label) * 8;
    uint32_t cx = rd32(dobj + DiskObject_do_CurrentX), cy = rd32(dobj + DiskObject_do_CurrentY);
    if (cx != NO_POS && cy != NO_POS && (int32_t)cx > -1000 && (int32_t)cx < 4000 && (int32_t)cy > -1000 && (int32_t)cy < 4000) {
        ic->x = (int32_t)cx; ic->y = (int32_t)cy; ic->fixed = 1;
    }
    /* append, keeping the reading order */
    WIcon **pp = &w->icons;
    while (*pp) pp = &(*pp)->next;
    *pp = ic;
    return ic;
}

typedef struct { char name[256]; int dir, info; } DEnt;
static int dent_cmp(const void *a, const void *b) {
    const DEnt *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void read_desktop(WBWin *w) {
    static const char *vols[] = { "SYS:", "RAM:", "Mac:" };
    int y = 4;
    for (int i = 0; i < 3; i++) {
        uint32_t d = icon_get(vols[i]);
        if (!d) d = icon_default(WBDISK);
        WIcon *ic = add_icon(w, vols[i], vols[i], volume_label(vols[i]), d, WBDISK);
        if (!ic->fixed) { ic->x = wb.s->w - 60 - ic->w / 2; ic->y = y; ic->fixed = 1; }
        y += ic->h + 22;
    }
    for (AppObj *a = wb.apps; a; a = a->next) {
        if (a->kind != AMTYPE_APPICON) continue;
        WIcon *ic = add_icon(w, a->text, "", a->text, a->dobj, WBAPPICON);
        ic->appicon = a->handle;
        if (!ic->fixed) { ic->x = wb.s->w - 60 - ic->w / 2; ic->y = y; ic->fixed = 1; }
        y += ic->h + 22;
    }
}

static void read_drawer(WBWin *w) {
    free_icons(w);
    if (w->backdrop) { read_desktop(w); return; }
    char hp[2048];
    if (!host_of(w->path, hp, sizeof hp)) return;
    DIR *d = opendir(hp);
    if (!d) return;
    DEnt *e = 0;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        size_t l = strlen(de->d_name);
        if (l > 5 && !strcasecmp(de->d_name + l - 5, ".info")) continue;
        char full[2600], info[2600];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", hp, de->d_name);
        if (stat(full, &st)) continue;
        snprintf(info, sizeof info, "%s.info", full);
        int has = access(info, F_OK) == 0;
        if (!w->show_all && !has) continue;
        if (n == cap) { cap = cap * 2 + 64; e = realloc(e, sizeof(DEnt) * (size_t)cap); }
        snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
        e[n].dir = S_ISDIR(st.st_mode);
        e[n].info = has;
        n++;
    }
    closedir(d);
    qsort(e, (size_t)n, sizeof(DEnt), dent_cmp);
    for (int i = 0; i < n; i++) {
        char ap[600];
        join_path(ap, sizeof ap, w->path, e[i].name);
        uint32_t dobj = e[i].info ? icon_get(ap) : 0;
        int type = dobj ? rd8(dobj + DiskObject_do_Type) : 0;
        if (!dobj) { type = e[i].dir ? WBDRAWER : icon_type_of(ap); if (!type) type = WBPROJECT; dobj = icon_default(type); }
        if (!dobj) continue;
        add_icon(w, e[i].name, ap, e[i].name, dobj, type);
    }
    free(e);
}

/* ---- layout and drawing ---------------------------------------------------------------- */

static void inner(WBWin *w, int *bl, int *bt, int *iw, int *ih) {
    uint32_t win = w->win;
    *bl = rd8(win + Window_BorderLeft);
    *bt = rd8(win + Window_BorderTop);
    *iw = (int16_t)rd16(win + Window_Width) - *bl - rd8(win + Window_BorderRight);
    *ih = (int16_t)rd16(win + Window_Height) - *bt - rd8(win + Window_BorderBottom);
}

static void icon_bounds(WIcon *ic, int *x0, int *y0, int *x1, int *y1) {
    int lx = ic->x + (ic->w - ic->lw) / 2;
    *x0 = ic->x < lx ? ic->x : lx;
    *x1 = ic->x + ic->w > lx + ic->lw ? ic->x + ic->w : lx + ic->lw;
    *y0 = ic->y;
    *y1 = ic->y + ic->h + 10;
}

static void layout(WBWin *w) {
    int bl, bt, iw, ih;
    inner(w, &bl, &bt, &iw, &ih);
    if (w->by_name) {
        int y = 2;
        for (WIcon *ic = w->icons; ic; ic = ic->next) { ic->y = y; y += 9; }
        w->cw = iw;
        w->ch = y + 2;
    } else {
        /* auto-placed icons flow in rows, each row bottom-aligned */
        int x = 8, y = 4;
        WIcon *row[256];
        int nrow = 0, rowh = 0;
        for (WIcon *ic = w->icons;; ic = ic->next) {
            int cell = ic ? (ic->w > ic->lw ? ic->w : ic->lw) + 14 : 0;
            if (!ic || (nrow && x + cell > iw - 4) || nrow == 256) {
                for (int k = 0; k < nrow; k++) row[k]->y = y + (rowh - row[k]->h);
                y += rowh + 16;
                x = 8; nrow = 0; rowh = 0;
                if (!ic) break;
            }
            if (ic->fixed) continue;
            int bw = ic->w > ic->lw ? ic->w : ic->lw;
            ic->x = x + (bw - ic->w) / 2;
            if (ic->lw > ic->w) ic->x = x + (ic->lw - ic->w) / 2;
            x += cell;
            if (ic->h > rowh) rowh = ic->h;
            row[nrow++] = ic;
        }
        int maxx = 0, maxy = 0;
        for (WIcon *ic = w->icons; ic; ic = ic->next) {
            int x0, y0, x1, y1;
            icon_bounds(ic, &x0, &y0, &x1, &y1);
            if (x1 > maxx) maxx = x1;
            if (y1 > maxy) maxy = y1;
        }
        w->cw = maxx + 4;
        w->ch = maxy + 4;
    }
    if (w->sy > w->ch - ih) w->sy = w->ch - ih;
    if (w->sy < 0) w->sy = 0;
}

static void update_prop(WBWin *w) {
    if (!w->prop) return;
    int bl, bt, iw, ih;
    inner(w, &bl, &bt, &iw, &ih);
    uint32_t pi = w->propinfo;
    int total = w->ch, range = total - ih;
    wr16(pi + PropInfo_VertBody, total <= ih ? 0xFFFF : (uint32_t)((long)ih * 0xFFFF / total));
    wr16(pi + PropInfo_VertPot, range <= 0 ? 0 : (uint32_t)((long)w->sy * 0xFFFF / range));
    lib_call(wb.c, IntuitionBase, "intuition", "RefreshGList", w->prop, w->win, 0, 1);
}

static void redraw(WBWin *w) {
    int bl, bt, iw, ih;
    inner(w, &bl, &bt, &iw, &ih);
    if (iw <= 0 || ih <= 0) return;
    NScreen *s = wb.s;
    uint32_t bm = gfx_alloc_bitmap(iw, ih, s->depth, 1);
    wr32(w->rp + RastPort_BitMap, bm);
    rp_fill(w->rp, 0, 0, iw - 1, ih - 1, s->pens[BACKGROUNDPEN], 0);
    if (w->by_name && !w->backdrop) {
        for (WIcon *ic = w->icons; ic; ic = ic->next) {
            int y = ic->y - w->sy;
            if (y + 9 < 0 || y > ih) continue;
            char line[160], size[32] = "";
            if (ic->type == WBDRAWER) snprintf(size, sizeof size, "(tiroir)");
            else {
                char hp[2048];
                struct stat st;
                if (host_of(ic->path, hp, sizeof hp) && !stat(hp, &st)) snprintf(size, sizeof size, "%lld", (long long)st.st_size);
            }
            snprintf(line, sizeof line, "%-28.28s %12s", ic->label, size);
            if (ic->selected) rp_fill(w->rp, 2, y, iw - 3, y + 8, s->pens[FILLPEN], 0);
            wr8(w->rp + RastPort_FgPen, ic->selected ? s->pens[FILLTEXTPEN] : s->pens[TEXTPEN]);
            wr8(w->rp + RastPort_DrawMode, JAM1);
            rp_move(w->rp, 6, y + 7);
            rp_text(w->rp, line, (int)strlen(line));
        }
    } else {
        for (WIcon *ic = w->icons; ic; ic = ic->next) {
            int x0, y0, x1, y1;
            icon_bounds(ic, &x0, &y0, &x1, &y1);
            if (y1 - w->sy < 0 || y0 - w->sy > ih) continue;
            icon_draw(w->rp, ic->dobj, ic->label, ic->x, ic->y - w->sy, ic->selected, s->pens);
        }
    }
    NWindow *nw = nwin_of(w->win);
    BM src, dst;
    if (nw && bm_load(bm, &src) && bm_load(nw->bm, &dst)) bm_copy(&src, 0, 0, &dst, bl, bt, iw, ih, 0xC0, 0xFF);
    wr32(w->rp + RastPort_BitMap, 0);
    gfx_free_bitmap(bm);
    gfx_touch();
}

static void relayout(WBWin *w) { layout(w); update_prop(w); redraw(w); }

/* ---- windows ------------------------------------------------------------------------- */

static WBWin *wbwin_of(uint32_t win) { for (WBWin *w = wb.wins; w; w = w->next) if (w->win == win) return w; return 0; }

static void attach(WBWin *w) {
    wr32(w->win + Window_UserPort, wb.port);
    lib_call(wb.c, IntuitionBase, "intuition", "ModifyIDCMP", w->win, (uint32_t)IDCMP_WB);
    if (wb.menus) lib_call(wb.c, IntuitionBase, "intuition", "SetMenuStrip", w->win, wb.menus);
    w->rp = ar_alloc(&w->A, RastPort_SIZEOF + 4);
    rp_init(w->rp);
    w->next = wb.wins;
    wb.wins = w;
}

static void detach_close(WBWin *w) {
    /* our port is shared: take this window's messages out before closing it */
    uint32_t port = wb.port;
    for (uint32_t m = rd32(port + MP_MSGLIST), n; m && (n = rd32(m)); m = n)
        if (rd8(m + LN_TYPE) == NT_MESSAGE && rd32(m + IntuiMessage_IDCMPWindow) == w->win) { list_remove(m); exec_reply_msg(m); }
    lib_call(wb.c, IntuitionBase, "intuition", "ClearMenuStrip", w->win);
    wr32(w->win + Window_UserPort, 0);
    lib_call(wb.c, IntuitionBase, "intuition", "ModifyIDCMP", w->win, 0);
    lib_call(wb.c, IntuitionBase, "intuition", "CloseWindow", w->win);
    WBWin **pp = &wb.wins;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    if (wb.active == w) wb.active = 0;
    if (wb.mw == w) { wb.mw = 0; wb.mode = M_NONE; }
    free_icons(w);
    ar_free(&w->A);
    free(w);
}

static void open_desktop(void) {
    NScreen *s = wb.s;
    WBWin *w = calloc(1, sizeof *w);
    w->backdrop = 1;
    int top = rd8(s->scr + Screen_BarHeight) + 1;
    uint32_t tags = ar_tags(&w->A, WA_Left_, 0, WA_Top_, top, WA_Width_, s->w, WA_Height_, s->h - top,
                            WA_CustomScreen_, s->scr, WA_Backdrop_, 1, WA_Borderless_, 1, WA_ReportMouse_, 1,
                            WA_Activate_, 1, 0);
    w->win = lib_call(wb.c, IntuitionBase, "intuition", "OpenWindowTagList", 0, tags);
    if (!w->win) { free(w); return; }
    attach(w);
    read_drawer(w);
    relayout(w);
}

static WBWin *find_drawer(const char *path) {
    for (WBWin *w = wb.wins; w; w = w->next) if (!w->backdrop && !strcasecmp(w->path, path)) return w;
    return 0;
}

static void open_drawer(const char *path, uint32_t dobj) {
    WBWin *w = find_drawer(path);
    if (w) {
        lib_call(wb.c, IntuitionBase, "intuition", "WindowToFront", w->win);
        lib_call(wb.c, IntuitionBase, "intuition", "ActivateWindow", w->win);
        return;
    }
    char hp[2048];
    struct stat st;
    if (!host_of(path, hp, sizeof hp) || stat(hp, &st) || !S_ISDIR(st.st_mode)) { wb_message("Impossible d'ouvrir le tiroir\n%s", path); return; }
    NScreen *s = wb.s;
    static int cascade;
    int x = 30 + (cascade % 8) * 16, y = 20 + (cascade % 8) * 10, W = 380, H = 130;
    cascade++;
    uint32_t dd = dobj ? rd32(dobj + DiskObject_do_DrawerData) : 0;
    if (dd) {
        int dx = (int16_t)rd16(dd), dy = (int16_t)rd16(dd + 2), dw = (int16_t)rd16(dd + 4), dh = (int16_t)rd16(dd + 6);
        if (dw >= 80 && dh >= 40 && dx >= 0 && dy >= 0 && dx + dw <= s->w && dy + dh <= s->h) { x = dx; y = dy; W = dw; H = dh; }
    }
    if (x + W > s->w) x = s->w - W;
    if (y + H > s->h) y = s->h - H;
    w = calloc(1, sizeof *w);
    snprintf(w->path, sizeof w->path, "%s", path);
    w->show_all = 1;
    char title[600];
    size_t l = strlen(path);
    if (l && path[l - 1] == ':') { char v[64]; snprintf(v, sizeof v, "%s", volume_label(path)); snprintf(title, sizeof title, "%s", v); }
    else snprintf(title, sizeof title, "%s", path);
    uint32_t tags = ar_tags(&w->A, WA_Left_, x, WA_Top_, y, WA_Width_, W, WA_Height_, H,
                            WA_Title_, ar_text(&w->A, title), WA_CustomScreen_, s->scr,
                            WA_DragBar_, 1, WA_DepthGadget_, 1, WA_CloseGadget_, 1, WA_SizeGadget_, 1,
                            WA_MinWidth_, 120, WA_MinHeight_, 60, WA_MaxWidth_, 0xFFFF, WA_MaxHeight_, 0xFFFF,
                            WA_ReportMouse_, 1, WA_Activate_, 1, 0);
    w->win = lib_call(wb.c, IntuitionBase, "intuition", "OpenWindowTagList", 0, tags);
    if (!w->win) { free(w); return; }
    attach(w);
    /* a vertical scroller in the right border */
    int bt = rd8(w->win + Window_BorderTop), br = rd8(w->win + Window_BorderRight);
    uint32_t g = ar_alloc(&w->A, Gadget_SIZEOF + 4), pi = ar_alloc(&w->A, PropInfo_SIZEOF + 4), img = ar_alloc(&w->A, Image_SIZEOF);
    wr16(g + Gadget_LeftEdge, (uint32_t)(-br + 5));
    wr16(g + Gadget_LeftEdge + 2, (uint32_t)(bt + 1));
    wr16(g + Gadget_Width, (uint32_t)(br - 8));
    wr16(g + Gadget_Width + 2, (uint32_t)(-bt - 13));                 /* stop above the size gadget */
    wr16(g + Gadget_Flags, GFLG_RELRIGHT | GFLG_RELHEIGHT);
    wr16(g + Gadget_Activation, GACT_RELVERIFY | GACT_IMMEDIATE | GACT_FOLLOWMOUSE | 0x10 /* RIGHTBORDER */);
    wr16(g + Gadget_GadgetType, GTYP_PROPGADGET);
    wr32(g + Gadget_GadgetRender, img);
    wr32(g + Gadget_SpecialInfo, pi);
    wr16(g + Gadget_GadgetID, 1);
    wr16(pi + PropInfo_Flags, 0x1 | 0x4 | 0x10);             /* AUTOKNOB | FREEVERT | PROPNEWLOOK */
    wr16(pi + PropInfo_VertBody, 0xFFFF);
    wr16(pi + PropInfo_HorizBody, 0xFFFF);
    w->prop = g;
    w->propinfo = pi;
    lib_call(wb.c, IntuitionBase, "intuition", "AddGList", w->win, g, 0xFFFFFFFFu, 1, 0);
    read_drawer(w);
    relayout(w);
}

/* ---- starting programs -------------------------------------------------------------- */

static int launch(const char *tool, const char *project) {
    char hp[2048];
    if (!host_of(tool, hp, sizeof hp)) { wb_message("Programme introuvable :\n%s", tool); return 0; }
    uint32_t seg = dos_loadseg(hp);
    if (!seg) { wb_message("Impossible de charger\n%s", tool); return 0; }
    uint32_t stack = 8192;
    uint32_t dobj = icon_get(tool);
    if (dobj) {
        uint32_t st = rd32(dobj + DiskObject_do_StackSize);
        if (st > stack && st < (1u << 20)) stack = st;
        icon_free(dobj);
    }
    char tdir[600], pdir[600];
    if (!parent_path(tool, tdir, sizeof tdir)) snprintf(tdir, sizeof tdir, "%s", tool);
    int nargs = project ? 2 : 1;
    Arena A = { 0 };
    uint32_t tags = ar_tags(&A, 0x800003E9u /* NP_Seglist */, seg, 0x800003EAu /* NP_FreeSeglist */, 0,
                            0x800003F4u /* NP_Name */, ar_text(&A, base_name(tool)), 0x800003F3u /* NP_StackSize */, stack,
                            0x800003F2u /* NP_CurrentDir */, dos_lock(tdir), 0x800003FAu /* NP_Cli */, 0, 0);
    uint32_t msg = exec_allocmem(WBStartup_SIZEOF + 8 * 2 + 2 * 256, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t args = msg + WBStartup_SIZEOF, names = args + 16;
    wr8(msg + LN_TYPE, NT_MESSAGE);
    wr32(msg + MN_REPLYPORT, wb.port);
    wr16(msg + MN_LENGTH, WBStartup_SIZEOF);
    wr32(msg + WBStartup_sm_Segment, seg);
    wr32(msg + WBStartup_sm_NumArgs, (uint32_t)nargs);
    wr32(msg + WBStartup_sm_ArgList, args);
    char n1[256];
    to_latin1(base_name(tool), n1, sizeof n1);
    put_cstr(names, n1, 256);
    wr32(args + WBArg_wa_Lock, dos_lock(tdir));
    wr32(args + WBArg_wa_Name, names);
    if (project) {
        if (!parent_path(project, pdir, sizeof pdir)) snprintf(pdir, sizeof pdir, "%s", project);
        to_latin1(base_name(project), n1, sizeof n1);
        put_cstr(names + 256, n1, 256);
        wr32(args + 8 + WBArg_wa_Lock, dos_lock(pdir));
        wr32(args + 8 + WBArg_wa_Name, names + 256);
    }
    uint32_t pr = lib_call(wb.c, DOSBase, "dos", "CreateNewProc", tags);
    ar_free(&A);
    if (!pr) {
        for (int i = 0; i < nargs; i++) dos_unlock(rd32(args + 8 * (uint32_t)i));
        exec_freemem(msg, WBStartup_SIZEOF + 8 * 2 + 2 * 256);
        dos_unloadseg(seg);
        wb_message("Impossible de lancer\n%s", tool);
        return 0;
    }
    wr32(msg + WBStartup_sm_Process, pr + PR_MSGPORT);
    Launch *l = calloc(1, sizeof *l);
    l->msg = msg;
    l->seg = seg;
    l->nargs = nargs;
    l->next = wb.launches;
    wb.launches = l;
    lib_call(wb.c, SysBase, "exec", "PutMsg", pr + PR_MSGPORT, msg);
    return 1;
}

/* a program ended: its startup code replied the WBStartup */
static int launch_ended(uint32_t msg) {
    for (Launch **pp = &wb.launches; *pp; pp = &(*pp)->next) {
        Launch *l = *pp;
        if (l->msg != msg) continue;
        uint32_t args = rd32(msg + WBStartup_sm_ArgList);
        for (int i = 0; i < l->nargs; i++) dos_unlock(rd32(args + 8 * (uint32_t)i));
        dos_unloadseg(l->seg);
        exec_freemem(msg, WBStartup_SIZEOF + 8 * 2 + 2 * 256);
        *pp = l->next;
        free(l);
        return 1;
    }
    return 0;
}

static int find_tool(const char *tool, const char *projdir, char *out, int max) {
    if (strchr(tool, ':') || strchr(tool, '/')) { snprintf(out, (size_t)max, "%s", tool); return 1; }
    const char *dirs[] = { projdir, "C:", "SYS:Utilities", "SYS:Tools", "SYS:" };
    for (int i = 0; i < 5; i++) {
        char p[600], hp[2048];
        join_path(p, sizeof p, dirs[i], tool);
        struct stat st;
        if (host_of(p, hp, sizeof hp) && !stat(hp, &st) && S_ISREG(st.st_mode)) { snprintf(out, (size_t)max, "%s", p); return 1; }
    }
    return 0;
}

static void send_appmsg(AppObj *a, int type, WIcon **icons, int n, int mx, int my);

static void open_icon(WIcon *ic) {
    switch (ic->type) {
    case WBDISK: case WBDRAWER: case WBGARBAGE: case WBDEVICE:
        open_drawer(ic->path, ic->dobj);
        break;
    case WBTOOL:
        launch(ic->path, 0);
        break;
    case WBAPPICON:
        for (AppObj *a = wb.apps; a; a = a->next) if (a->handle == ic->appicon) send_appmsg(a, AMTYPE_APPICON, 0, 0, 0, 0);
        break;
    default: {
        char tool[600] = "", dir[600];
        uint32_t dt = rd32(ic->dobj + DiskObject_do_DefaultTool);
        if (!parent_path(ic->path, dir, sizeof dir)) dir[0] = 0;
        if (dt) {
            char t[256];
            str_get(dt, t, sizeof t);
            if (t[0] && find_tool(t, dir, tool, sizeof tool)) { launch(tool, ic->path); return; }
        }
        char hp[2048];
        if (host.open_file && host_of(ic->path, hp, sizeof hp)) { host.open_file(hp); return; }
        wb_message("%s n'a pas d'outil par d\xc3\xa9" "faut.", ic->name);
    }
    }
}

/* ---- AppWindows, AppIcons, AppMenuItems ----------------------------------------------- */

static void send_appmsg(AppObj *a, int type, WIcon **icons, int n, int mx, int my) {
    uint32_t size = AppMessage_SIZEOF + 8 * (uint32_t)(n + 1) + 256 * (uint32_t)(n + 1);
    uint32_t m = exec_allocmem(size, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t args = m + AppMessage_SIZEOF, names = args + 8 * (uint32_t)(n + 1);
    wr8(m + LN_TYPE, NT_MESSAGE);
    wr32(m + MN_REPLYPORT, wb.port);
    wr16(m + MN_LENGTH, AppMessage_SIZEOF);
    wr16(m + AppMessage_am_Type, (uint32_t)type);
    wr32(m + AppMessage_am_UserData, a->userdata);
    wr32(m + AppMessage_am_ID, a->id);
    wr32(m + AppMessage_am_NumArgs, (uint32_t)n);
    wr32(m + AppMessage_am_ArgList, args);
    wr16(m + AppMessage_am_Version, 1);
    wr16(m + AppMessage_am_MouseX, (uint32_t)mx);
    wr16(m + AppMessage_am_MouseY, (uint32_t)my);
    wr32(m + AppMessage_am_Seconds, timer_systime_secs());
    for (int i = 0; i < n; i++) {
        char dir[600], n1[256];
        if (!parent_path(icons[i]->path, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s", icons[i]->path);
        int vol = icons[i]->type == WBDISK;
        to_latin1(vol ? "" : base_name(icons[i]->path), n1, sizeof n1);
        put_cstr(names + 256 * (uint32_t)i, n1, 256);
        wr32(args + 8 * (uint32_t)i + WBArg_wa_Lock, dos_lock(vol ? icons[i]->path : dir));
        wr32(args + 8 * (uint32_t)i + WBArg_wa_Name, names + 256 * (uint32_t)i);
    }
    wr32(m + AppMessage_SIZEOF - 4, size);                       /* remember the size (reserved field) */
    if (wb.nappmsgs < 64) wb.appmsgs[wb.nappmsgs++] = m;
    lib_call(wb.c, SysBase, "exec", "PutMsg", a->port, m);
}

static int appmsg_returned(uint32_t m) {
    for (int i = 0; i < wb.nappmsgs; i++) {
        if (wb.appmsgs[i] != m) continue;
        uint32_t n = rd32(m + AppMessage_am_NumArgs), args = rd32(m + AppMessage_am_ArgList);
        for (uint32_t k = 0; k < n; k++) dos_unlock(rd32(args + 8 * k));
        exec_freemem(m, rd32(m + AppMessage_SIZEOF - 4));
        wb.appmsgs[i] = wb.appmsgs[--wb.nappmsgs];
        return 1;
    }
    return 0;
}

/* ---- menus ----------------------------------------------------------------------------- */

enum {
    A_EXECUTE = 1, A_REDRAWALL, A_UPDATEALL, A_LASTMSG, A_ABOUT, A_QUIT,
    A_NEWDRAWER, A_PARENT, A_CLOSE, A_UPDATE, A_SELECTALL, A_CLEANUP, A_SHOWICONS, A_SHOWALL, A_BYICON, A_BYNAME,
    A_OPEN, A_COPY, A_RENAME, A_INFO, A_SNAPSHOT, A_DELETE,
    A_RUNPROGRAM, A_RESET, A_APPMENU = 1000
};

typedef struct { int type; const char *label, *key; int flags, excl, action; } MDef;

static void build_menus(void) {
    static const MDef defs[] = {
        { 1, "Workbench", 0, 0, 0, 0 },
        { 2, "Exécuter une commande...", "E", 0, 0, A_EXECUTE },
        { 2, "Redessiner tout", 0, 0, 0, A_REDRAWALL },
        { 2, "Mettre à jour tout", 0, 0, 0, A_UPDATEALL },
        { 2, "Dernier message", 0, 0, 0, A_LASTMSG },
        { 2, "À propos...", "?", 0, 0, A_ABOUT },
        { 2, 0, 0, 0, 0, 0 },
        { 2, "Quitter le Workbench", "Q", 0, 0, A_QUIT },
        { 1, "Fenêtre", 0, 0, 0, 0 },
        { 2, "Nouveau tiroir...", "N", 0, 0, A_NEWDRAWER },
        { 2, "Ouvrir le parent", "P", 0, 0, A_PARENT },
        { 2, "Fermer", "K", 0, 0, A_CLOSE },
        { 2, "Mettre à jour", 0, 0, 0, A_UPDATE },
        { 2, "Tout sélectionner", "A", 0, 0, A_SELECTALL },
        { 2, "Ranger", ".", 0, 0, A_CLEANUP },
        { 2, 0, 0, 0, 0, 0 },
        { 2, "Afficher", 0, 0, 0, 0 },
        { 3, "Icônes seulement", 0, 1 | 8, 2, A_SHOWICONS },
        { 3, "Tous les fichiers", 0, 1 | 8 | 0x100, 1, A_SHOWALL },
        { 2, "Voir par", 0, 0, 0, 0 },
        { 3, "Icône", 0, 1 | 8 | 0x100, 2, A_BYICON },
        { 3, "Nom", 0, 1 | 8, 1, A_BYNAME },
        { 1, "Icônes", 0, 0, 0, 0 },
        { 2, "Ouvrir", "O", 0, 0, A_OPEN },
        { 2, "Copier", "C", 0, 0, A_COPY },
        { 2, "Renommer...", "R", 0, 0, A_RENAME },
        { 2, "Informations...", "I", 0, 0, A_INFO },
        { 2, "Figer", "S", 0, 0, A_SNAPSHOT },
        { 2, 0, 0, 0, 0, 0 },
        { 2, "Supprimer...", "D", 0, 0, A_DELETE },
        { 1, "Outils", 0, 0, 0, 0 },
        { 2, "Lancer un programme...", "L", 0, 0, A_RUNPROGRAM },
        { 2, "Réinitialiser le Workbench", 0, 0, 0, A_RESET },
    };
    int n = (int)(sizeof defs / sizeof *defs), napp = 0;
    for (AppObj *a = wb.apps; a; a = a->next) if (a->kind == AMTYPE_APPMENUITEM) napp++;
    uint32_t nm = ar_alloc(&wb.A, NewMenu_SIZEOF * (uint32_t)(n + napp + 2));
    int k = 0;
    for (int i = 0; i < n; i++, k++) {
        uint32_t e = nm + NewMenu_SIZEOF * (uint32_t)k;
        wr8(e + NewMenu_nm_Type, (uint32_t)defs[i].type);
        wr32(e + NewMenu_nm_Label, defs[i].label ? ar_text(&wb.A, defs[i].label) : 0xFFFFFFFFu);
        wr32(e + NewMenu_nm_CommKey, defs[i].key ? ar_str(&wb.A, defs[i].key) : 0);
        wr16(e + NewMenu_nm_Flags, (uint32_t)defs[i].flags);
        wr32(e + NewMenu_nm_MutualExclude, (uint32_t)defs[i].excl);
        wr32(e + NewMenu_nm_UserData, (uint32_t)defs[i].action);
    }
    for (AppObj *a = wb.apps; a; a = a->next) {
        if (a->kind != AMTYPE_APPMENUITEM) continue;
        uint32_t e = nm + NewMenu_SIZEOF * (uint32_t)k++;
        wr8(e + NewMenu_nm_Type, 2);
        wr32(e + NewMenu_nm_Label, ar_str(&wb.A, a->text));
        wr32(e + NewMenu_nm_UserData, A_APPMENU);
        wr32(e + NewMenu_nm_MutualExclude, 0);
    }
    wb.menus = lib_call(wb.c, GadToolsBase, "gadtools", "CreateMenusA", nm, 0);
    if (wb.menus) lib_call(wb.c, GadToolsBase, "gadtools", "LayoutMenusA", wb.menus, wb.vi, 0);
}

static void rebuild_menus(void) {
    for (WBWin *w = wb.wins; w; w = w->next) lib_call(wb.c, IntuitionBase, "intuition", "ClearMenuStrip", w->win);
    if (wb.menus) lib_call(wb.c, GadToolsBase, "gadtools", "FreeMenus", wb.menus);
    build_menus();
    for (WBWin *w = wb.wins; w; w = w->next) lib_call(wb.c, IntuitionBase, "intuition", "SetMenuStrip", w->win, wb.menus);
}

/* the CHECKIT items follow the active window */
static void sync_checks(WBWin *w) {
    if (!wb.menus || !w) return;
    for (uint32_t m = wb.menus; m; m = rd32(m + Menu_NextMenu))
        for (uint32_t it = rd32(m + Menu_FirstItem); it; it = rd32(it + MenuItem_NextItem))
            for (uint32_t sub = rd32(it + MenuItem_SubItem); sub; sub = rd32(sub + MenuItem_NextItem)) {
                uint32_t act = rd32(sub + MenuItem_SIZEOF), fl = rd16(sub + MenuItem_Flags) & ~0x100u;
                int on = (act == A_SHOWALL && w->show_all) || (act == A_SHOWICONS && !w->show_all) ||
                         (act == A_BYNAME && w->by_name) || (act == A_BYICON && !w->by_name);
                wr16(sub + MenuItem_Flags, on ? fl | 0x100 : fl);
            }
}

/* ---- file operations ---------------------------------------------------------------------- */

static int selected_icons(WBWin *w, WIcon **out, int max) {
    int n = 0;
    for (WIcon *ic = w->icons; ic && n < max; ic = ic->next) if (ic->selected) out[n++] = ic;
    return n;
}

static void update_path(const char *dir) {
    for (WBWin *w = wb.wins; w; w = w->next)
        if (!w->backdrop && !strcasecmp(w->path, dir)) { read_drawer(w); relayout(w); }
}

static int copy_host_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return 0; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    fclose(a);
    fclose(b);
    return ok;
}

static void move_into(WIcon **icons, int n, const char *destdir) {
    char dhp[2048];
    if (!host_of(destdir, dhp, sizeof dhp)) return;
    char srcdir[600] = "";
    for (int i = 0; i < n; i++) {
        WIcon *ic = icons[i];
        if (ic->type == WBDISK || ic->type == WBAPPICON) continue;
        char shp[2048], to[2600];
        if (!host_of(ic->path, shp, sizeof shp)) continue;
        if (!strncmp(dhp, shp, strlen(shp)) && (dhp[strlen(shp)] == '/' || !dhp[strlen(shp)])) {
            wb_message("Impossible de d\xc3\xa9placer un tiroir dans lui-m\xc3\xaame.");
            continue;
        }
        snprintf(to, sizeof to, "%s/%s", dhp, ic->name);
        if (access(to, F_OK) == 0) { wb_message("%s existe d\xc3\xa9j\xc3\xa0 dans la destination.", ic->name); continue; }
        if (rename(shp, to)) { wb_message("D\xc3\xa9placement impossible :\n%s", strerror(errno)); continue; }
        char si[2100], ti[2700];
        snprintf(si, sizeof si, "%s.info", shp);
        snprintf(ti, sizeof ti, "%s.info", to);
        if (access(si, F_OK) == 0) rename(si, ti);
        parent_path(ic->path, srcdir, sizeof srcdir);
    }
    if (srcdir[0]) update_path(srcdir);
    update_path(destdir);
}

static void do_copy(WBWin *w) {
    WIcon *sel[256];
    int n = selected_icons(w, sel, 256);
    for (int i = 0; i < n; i++) {
        WIcon *ic = sel[i];
        if (ic->type == WBDRAWER || ic->type == WBDISK) { wb_message("La copie de tiroirs n'est pas g\xc3\xa9r\xc3\xa9" "e."); continue; }
        char hp[2048], dir[2048], to[2600];
        if (!host_of(ic->path, hp, sizeof hp)) continue;
        snprintf(dir, sizeof dir, "%s", hp);
        char *sl = strrchr(dir, '/');
        if (!sl) continue;
        *sl = 0;
        snprintf(to, sizeof to, "%s/copy of %s", dir, ic->name);
        for (int k = 2; access(to, F_OK) == 0 && k < 100; k++) snprintf(to, sizeof to, "%s/copy %d of %s", dir, k, ic->name);
        if (!copy_host_file(hp, to)) { wb_message("Copie impossible de\n%s", ic->name); continue; }
        char si[2100], ti[2700];
        snprintf(si, sizeof si, "%s.info", hp);
        snprintf(ti, sizeof ti, "%s.info", to);
        if (access(si, F_OK) == 0) copy_host_file(si, ti);
    }
    if (n) { read_drawer(w); relayout(w); }
}

static void do_rename(WBWin *w) {
    WIcon *sel[256];
    int n = selected_icons(w, sel, 256);
    for (int i = 0; i < n; i++) {
        WIcon *ic = sel[i];
        if (ic->type == WBDISK || ic->type == WBAPPICON) continue;
        char nn[256];
        snprintf(nn, sizeof nn, "%s", ic->name);
        if (!ask_string("Renommer", "Nouveau nom :", nn, sizeof nn) || !nn[0] || strchr(nn, '/') || strchr(nn, ':')) continue;
        char hp[2048], to[2600];
        if (!host_of(ic->path, hp, sizeof hp)) continue;
        snprintf(to, sizeof to, "%.*s/%s", (int)(strrchr(hp, '/') - hp), hp, nn);
        if (rename(hp, to)) { wb_message("Renommage impossible :\n%s", strerror(errno)); continue; }
        char si[2100], ti[2700];
        snprintf(si, sizeof si, "%s.info", hp);
        snprintf(ti, sizeof ti, "%s.info", to);
        if (access(si, F_OK) == 0) rename(si, ti);
    }
    if (n) { read_drawer(w); relayout(w); }
}

static void do_delete(WBWin *w) {
    WIcon *sel[256];
    int n = selected_icons(w, sel, 256);
    if (!n) return;
    char q[400];
    if (n == 1) snprintf(q, sizeof q, "Supprimer d\xc3\xa9" "finitivement\n\xc2\xab %s \xc2\xbb ?", sel[0]->name);
    else snprintf(q, sizeof q, "Supprimer d\xc3\xa9" "finitivement\nces %d objets ?", n);
    if (easy("Supprimer", q, "Supprimer|Annuler") != 1) return;
    for (int i = 0; i < n; i++) {
        WIcon *ic = sel[i];
        if (ic->type == WBDISK || ic->type == WBAPPICON) continue;
        char hp[2048], si[2100];
        if (!host_of(ic->path, hp, sizeof hp)) continue;
        int r = ic->type == WBDRAWER ? rmdir(hp) : unlink(hp);
        if (r) { wb_message("Impossible de supprimer %s :\n%s", ic->name, errno == ENOTEMPTY ? "le tiroir n'est pas vide" : strerror(errno)); continue; }
        snprintf(si, sizeof si, "%s.info", hp);
        unlink(si);
    }
    read_drawer(w);
    relayout(w);
}

static void do_info(WIcon *ic) {
    static const char *types[] = { "?", "Disque", "Tiroir", "Outil", "Projet", "Corbeille", "P\xc3\xa9riph\xc3\xa9rique", "Kickstart", "AppIcon" };
    char hp[2048], text[1500], sz[64] = "-", tool[256] = "-", tts[600] = "";
    struct stat st;
    if (host_of(ic->path, hp, sizeof hp) && !stat(hp, &st)) {
        if (S_ISDIR(st.st_mode)) snprintf(sz, sizeof sz, "tiroir");
        else snprintf(sz, sizeof sz, "%lld octets", (long long)st.st_size);
    }
    uint32_t dt = rd32(ic->dobj + DiskObject_do_DefaultTool), tt = rd32(ic->dobj + DiskObject_do_ToolTypes);
    if (dt) str_get(dt, tool, sizeof tool);
    for (uint32_t i = 0; tt && rd32(tt + 4 * i) && i < 8; i++) {
        char t[128];
        str_get(rd32(tt + 4 * i), t, sizeof t);
        size_t l = strlen(tts);
        snprintf(tts + l, sizeof tts - l, "\n   %s", t);
    }
    /* tool and tool types are Latin-1 already: keep the text in UTF-8 for easy() */
    char tool8[512], tts8[1200];
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)tool; *p && k < 500; p++) { if (*p >= 0x80) { tool8[k++] = (char)(0xC0 | (*p >> 6)); tool8[k++] = (char)(0x80 | (*p & 0x3F)); } else tool8[k++] = (char)*p; }
    tool8[k] = 0;
    k = 0;
    for (const unsigned char *p = (const unsigned char *)tts; *p && k < 1190; p++) { if (*p >= 0x80) { tts8[k++] = (char)(0xC0 | (*p >> 6)); tts8[k++] = (char)(0x80 | (*p & 0x3F)); } else tts8[k++] = (char)*p; }
    tts8[k] = 0;
    snprintf(text, sizeof text, "Nom : %s\nType : %s\nTaille : %s\nPile : %u\nOutil par d\xc3\xa9" "faut : %s%s%s",
             ic->name, types[ic->type >= 0 && ic->type <= 8 ? ic->type : 0], sz,
             rd32(ic->dobj + DiskObject_do_StackSize), tool8, tts8[0] ? "\nTypes d'outil :" : "", tts8);
    easy("Informations", text, "OK");
}

static void do_snapshot(WBWin *w) {
    WIcon *sel[256];
    int n = selected_icons(w, sel, 256);
    for (int i = 0; i < n; i++) {
        WIcon *ic = sel[i];
        if (ic->type == WBAPPICON) continue;
        wr32(ic->dobj + DiskObject_do_CurrentX, (uint32_t)ic->x);
        wr32(ic->dobj + DiskObject_do_CurrentY, (uint32_t)ic->y);
        ic->fixed = 1;
        if (!icon_put(ic->path, ic->dobj)) wb_message("Impossible d'\xc3\xa9" "crire l'ic\xc3\xb4ne de\n%s", ic->name);
    }
}

static void run_program(void) {
    Arena A = { 0 };
    uint32_t tags = ar_tags(&A, 0x80080001u /* ASLFR_TitleText */, ar_text(&A, "Lancer un programme"),
                            0x80080009u /* ASLFR_InitialDrawer */, ar_str(&A, "SYS:C"), 0);
    uint32_t fr = lib_call(wb.c, AslBase, "asl", "AllocAslRequest", 0, tags);
    if (fr && lib_call(wb.c, AslBase, "asl", "AslRequest", fr, 0)) {
        char d[256], f[256], p[600];
        str_get(rd32(fr + FileRequester_fr_Drawer), d, sizeof d);
        str_get(rd32(fr + FileRequester_fr_File), f, sizeof f);
        join_path(p, sizeof p, d, f);
        if (f[0]) {
            if (icon_type_of(p) == WBTOOL) launch(p, 0);
            else wb_message("%s n'est pas un programme.", f);
        }
    }
    if (fr) lib_call(wb.c, AslBase, "asl", "FreeAslRequest", fr);
    ar_free(&A);
}

static void quit_workbench(void);

static void menu_action(WBWin *w, int act) {
    WIcon *sel[256];
    int n;
    switch (act) {
    case A_EXECUTE: {
        char cmd[400] = "";
        if (ask_string("Ex\xc3\xa9" "cuter une commande", "Commande \xc3\xa0 ex\xc3\xa9" "cuter :", cmd, sizeof cmd) && cmd[0]) {
            char l1[400];
            to_latin1(cmd, l1, sizeof l1);
            shell_run_background(l1);
        }
        break;
    }
    case A_REDRAWALL: for (WBWin *x = wb.wins; x; x = x->next) redraw(x); break;
    case A_UPDATEALL: for (WBWin *x = wb.wins; x; x = x->next) { read_drawer(x); relayout(x); } break;
    case A_LASTMSG: easy("Dernier message", wb.lastmsg[0] ? wb.lastmsg : "Aucun message.", "OK"); break;
    case A_ABOUT:
        easy("\xc3\x80 propos", "AmigaMac Workbench 1.0\n\nUn Workbench r\xc3\xa9" "\xc3\xa9" "crit de z\xc3\xa9ro,\nnatif pour macOS.", "OK");
        break;
    case A_QUIT:
        if (easy("Workbench", "Quitter le Workbench ?", "Quitter|Annuler") == 1) quit_workbench();
        break;
    case A_NEWDRAWER: {
        if (!w || w->backdrop) break;
        char nm[256] = "Sans titre";
        if (!ask_string("Nouveau tiroir", "Nom du nouveau tiroir :", nm, sizeof nm) || !nm[0]) break;
        char ap[600], hp[2048];
        join_path(ap, sizeof ap, w->path, nm);
        if (!host_of(ap, hp, sizeof hp) || mkdir(hp, 0755)) { wb_message("Cr\xc3\xa9" "ation impossible :\n%s", strerror(errno)); break; }
        read_drawer(w);
        relayout(w);
        break;
    }
    case A_PARENT: {
        char p[600];
        if (w && !w->backdrop && parent_path(w->path, p, sizeof p)) open_drawer(p, 0);
        break;
    }
    case A_CLOSE: if (w && !w->backdrop) detach_close(w); break;
    case A_UPDATE: if (w) { read_drawer(w); relayout(w); } break;
    case A_SELECTALL: if (w) { for (WIcon *ic = w->icons; ic; ic = ic->next) ic->selected = 1; redraw(w); } break;
    case A_CLEANUP: if (w && !w->backdrop) { for (WIcon *ic = w->icons; ic; ic = ic->next) ic->fixed = 0; relayout(w); } break;
    case A_SHOWICONS: case A_SHOWALL:
        if (w && !w->backdrop) { w->show_all = act == A_SHOWALL; read_drawer(w); relayout(w); sync_checks(w); }
        break;
    case A_BYICON: case A_BYNAME:
        if (w && !w->backdrop) { w->by_name = act == A_BYNAME; w->sy = 0; relayout(w); sync_checks(w); }
        break;
    case A_OPEN: if (w) { n = selected_icons(w, sel, 256); for (int i = 0; i < n; i++) open_icon(sel[i]); } break;
    case A_COPY: if (w) do_copy(w); break;
    case A_RENAME: if (w) do_rename(w); break;
    case A_INFO: if (w) { n = selected_icons(w, sel, 256); for (int i = 0; i < n; i++) do_info(sel[i]); } break;
    case A_SNAPSHOT: if (w) do_snapshot(w); break;
    case A_DELETE: if (w && !w->backdrop) do_delete(w); break;
    case A_RUNPROGRAM: run_program(); break;
    case A_RESET: {
        while (wb.wins) detach_close(wb.wins);
        open_desktop();
        break;
    }
    }
}

static void menu_pick(WBWin *w, uint32_t code) {
    while (code != 0xFFFF && wb.menus && wb.running) {
        uint32_t item = lib_call(wb.c, IntuitionBase, "intuition", "ItemAddress", wb.menus, code);
        if (!item) break;
        uint32_t next = rd16(item + MenuItem_NextSelect);
        int act = (int)rd32(item + MenuItem_SIZEOF);
        if (act == A_APPMENU) {
            /* which AppMenuItem: by its text */
            char t[64];
            str_get(rd32(rd32(item + MenuItem_ItemFill) + IntuiText_IText), t, sizeof t);
            for (AppObj *a = wb.apps; a; a = a->next)
                if (a->kind == AMTYPE_APPMENUITEM && !strcmp(a->text, t)) {
                    WIcon *sel[64];
                    int n = 0;
                    for (WBWin *x = wb.wins; x; x = x->next) n += selected_icons(x, sel + n, 64 - n);
                    send_appmsg(a, AMTYPE_APPMENUITEM, sel, n, 0, 0);
                }
        } else menu_action(wbwin_of(w ? w->win : 0) ? w : 0, act);
        if (!wb.running || (w && !wbwin_of(w->win))) break;
        code = next;
    }
}

/* ---- the mouse: selection, lasso, dragging ------------------------------------------- */

static WIcon *hit(WBWin *w, int mx, int my) {
    int bl, bt, iw, ih;
    inner(w, &bl, &bt, &iw, &ih);
    if (mx < bl || my < bt || mx >= bl + iw || my >= bt + ih) return 0;
    int cx = mx - bl, cy = my - bt + w->sy;
    WIcon *found = 0;
    for (WIcon *ic = w->icons; ic; ic = ic->next) {
        if (w->by_name && !w->backdrop) { if (cy >= ic->y && cy < ic->y + 9) found = ic; continue; }
        int lx = ic->x + (ic->w - ic->lw) / 2;
        if ((cx >= ic->x && cx < ic->x + ic->w && cy >= ic->y && cy < ic->y + ic->h) ||
            (cx >= lx && cx < lx + ic->lw && cy >= ic->y + ic->h && cy < ic->y + ic->h + 10)) found = ic;
    }
    return found;
}

static void deselect_all(WBWin *except) {
    for (WBWin *w = wb.wins; w; w = w->next) {
        if (w == except) continue;
        int any = 0;
        for (WIcon *ic = w->icons; ic; ic = ic->next) if (ic->selected) { ic->selected = 0; any = 1; }
        if (any) redraw(w);
    }
}

static void win_origin(WBWin *w, int *x, int *y) {
    *x = (int16_t)rd16(w->win + Window_LeftEdge);
    *y = (int16_t)rd16(w->win + Window_TopEdge);
}

static void ov_clear(void) { memset(wb.s->ov, 0xFF, (size_t)wb.s->w * wb.s->h); gfx_touch(); }

static void ov_px(int x, int y, int pen) {
    NScreen *s = wb.s;
    if (x >= 0 && y >= 0 && x < s->w && y < s->h) s->ov[(size_t)y * s->w + x] = (uint8_t)pen;
}

/* an icon image in the overlay, colour 0 left transparent */
static void ov_icon(uint32_t dobj, int x0, int y0) {
    uint32_t img = rd32(dobj + DiskObject_do_Gadget + Gadget_GadgetRender);
    if (!img || rd16(img + Image_Depth) == 0xFFFF) return;
    int w = (int16_t)rd16(img + Image_Width), h = (int16_t)rd16(img + Image_Width + 2), depth = (int16_t)rd16(img + Image_Depth);
    uint32_t data = rd32(img + Image_ImageData);
    int pick = rd8(img + Image_PlanePick), onoff = rd8(img + Image_PlaneOnOff), words = (w + 15) >> 4;
    x0 += (int16_t)rd16(img + Image_LeftEdge);
    y0 += (int16_t)rd16(img + Image_LeftEdge + 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int pen = 0, k = 0;
            for (int p = 0; p < wb.s->depth; p++) {
                if (pick & (1 << p)) {
                    if (data && k < depth && ((rd8(data + (uint32_t)(k * words * 2 * h + y * words * 2 + (x >> 3))) >> (7 - (x & 7))) & 1)) pen |= 1 << p;
                    k++;
                } else if (onoff & (1 << p)) pen |= 1 << p;
            }
            if (pen) ov_px(x0 + x, y0 + y, pen);
        }
}

static void show_drag(void) {
    memset(wb.s->ov, 0xFF, (size_t)wb.s->w * wb.s->h);
    WBWin *w = wb.mw;
    int ox, oy, bl, bt, iw, ih;
    win_origin(w, &ox, &oy);
    inner(w, &bl, &bt, &iw, &ih);
    int dx = wb.mx1 - wb.mx0, dy = wb.my1 - wb.my0;
    for (WIcon *ic = w->icons; ic; ic = ic->next)
        if (ic->selected) ov_icon(ic->dobj, ox + bl + ic->x + dx, oy + bt + ic->y - w->sy + dy);
    gfx_touch();
}

static void show_lasso(void) {
    memset(wb.s->ov, 0xFF, (size_t)wb.s->w * wb.s->h);
    int x0 = wb.mx0 < wb.mx1 ? wb.mx0 : wb.mx1, x1 = wb.mx0 < wb.mx1 ? wb.mx1 : wb.mx0;
    int y0 = wb.my0 < wb.my1 ? wb.my0 : wb.my1, y1 = wb.my0 < wb.my1 ? wb.my1 : wb.my0;
    int pen = wb.s->pens[TEXTPEN];
    for (int x = x0; x <= x1; x++) { if ((x & 2) == 0) { ov_px(x, y0, pen); ov_px(x, y1, pen); } }
    for (int y = y0; y <= y1; y++) { if ((y & 2) == 0) { ov_px(x0, y, pen); ov_px(x1, y, pen); } }
    gfx_touch();
}

/* the WB or App window under a screen point (front to back) */
static uint32_t window_at(int x, int y) {
    for (NWindow *n = wb.s->wins; n; n = n->next)
        if (x >= n->x && y >= n->y && x < n->x + n->w && y < n->y + n->h) return n->win;
    return 0;
}

static void drop(void) {
    WBWin *src = wb.mw;
    int dx = wb.mx1 - wb.mx0, dy = wb.my1 - wb.my0;
    uint32_t tw = window_at(wb.mx1, wb.my1);
    WBWin *dst = wbwin_of(tw);
    WIcon *sel[256];
    int n = selected_icons(src, sel, 256);
    if (!n) return;
    if (!dst) {
        for (AppObj *a = wb.apps; a; a = a->next)
            if (a->kind == AMTYPE_APPWINDOW && a->win == tw) {
                int ox = (int16_t)rd16(tw + Window_LeftEdge), oy = (int16_t)rd16(tw + Window_TopEdge);
                send_appmsg(a, AMTYPE_APPWINDOW, sel, n, wb.mx1 - ox, wb.my1 - oy);
                return;
            }
        return;
    }
    int ox, oy;
    win_origin(dst, &ox, &oy);
    WIcon *on = hit(dst, wb.mx1 - ox, wb.my1 - oy);
    if (on && on->selected && dst == src) on = 0;
    if (on && on->type == WBAPPICON) {
        for (AppObj *a = wb.apps; a; a = a->next) if (a->handle == on->appicon) send_appmsg(a, AMTYPE_APPICON, sel, n, 0, 0);
        return;
    }
    if (on && (on->type == WBDRAWER || on->type == WBDISK)) { move_into(sel, n, on->path); return; }
    if (on && on->type == WBTOOL && src != dst) { launch(on->path, sel[0]->path); return; }
    if (on && on->type == WBTOOL && src == dst) { launch(on->path, sel[0]->path); return; }
    if (dst == src) {
        if (src->by_name) return;
        for (int i = 0; i < n; i++) { sel[i]->x += dx; sel[i]->y += dy; sel[i]->fixed = 1; }
        relayout(src);
        return;
    }
    if (dst->backdrop || src->backdrop) return;                      /* no "leave out" */
    move_into(sel, n, dst->path);
}

static void mouse_button(WBWin *w, NMsg *m) {
    int ox, oy;
    win_origin(w, &ox, &oy);
    int sx = ox + m->mx, sy = oy + m->my;
    if (m->code == SELECTDOWN) {
        WIcon *ic = hit(w, m->mx, m->my);
        int shift = (m->qual & 3) != 0;
        if (ic) {
            if (ic == wb.lasticon && ntk_double_click(wb.lsecs, wb.lmicros, m->secs, m->micros)) {
                wb.lasticon = 0;
                wb.mode = M_NONE;
                open_icon(ic);
                return;
            }
            wb.lasticon = ic;
            wb.lsecs = m->secs;
            wb.lmicros = m->micros;
            if (!ic->selected) {
                if (!shift) deselect_all(0);
                ic->selected = 1;
            } else if (shift) {
                ic->selected = 0;
                redraw(w);
                return;
            }
            redraw(w);
            wb.mode = M_PENDING;
        } else {
            int bl, bt, iw, ih;
            inner(w, &bl, &bt, &iw, &ih);
            if (m->mx < bl || m->my < bt || m->mx >= bl + iw || m->my >= bt + ih) return;
            if (!shift) deselect_all(0);
            wb.lasticon = 0;
            wb.mode = M_LASSO;
        }
        wb.mw = w;
        wb.mx0 = wb.mx1 = sx;
        wb.my0 = wb.my1 = sy;
        return;
    }
    if (m->code == SELECTUP) {
        WBWin *mw = wb.mw;
        int mode = wb.mode;
        wb.mode = M_NONE;
        if (!mw || !wbwin_of(mw->win)) return;
        wb.mx1 = sx;
        wb.my1 = sy;
        if (mode == M_DRAG) { ov_clear(); drop(); }
        else if (mode == M_LASSO) {
            ov_clear();
            int mox, moy, bl, bt, iw, ih;
            win_origin(mw, &mox, &moy);
            inner(mw, &bl, &bt, &iw, &ih);
            int x0 = (wb.mx0 < wb.mx1 ? wb.mx0 : wb.mx1) - mox - bl, x1 = (wb.mx0 < wb.mx1 ? wb.mx1 : wb.mx0) - mox - bl;
            int y0 = (wb.my0 < wb.my1 ? wb.my0 : wb.my1) - moy - bt + mw->sy, y1 = (wb.my0 < wb.my1 ? wb.my1 : wb.my0) - moy - bt + mw->sy;
            for (WIcon *ic = mw->icons; ic; ic = ic->next) {
                int a0, b0, a1, b1;
                if (mw->by_name && !mw->backdrop) { a0 = 0; a1 = iw; b0 = ic->y; b1 = ic->y + 9; }
                else icon_bounds(ic, &a0, &b0, &a1, &b1);
                if (a1 >= x0 && a0 <= x1 && b1 >= y0 && b0 <= y1) ic->selected = 1;
            }
            redraw(mw);
        }
    }
}

static void mouse_move(WBWin *w, NMsg *m) {
    if (wb.mode == M_NONE || !wb.mw) return;
    int ox, oy;
    win_origin(w, &ox, &oy);
    wb.mx1 = ox + m->mx;
    wb.my1 = oy + m->my;
    if (wb.mode == M_PENDING && (abs(wb.mx1 - wb.mx0) > 3 || abs(wb.my1 - wb.my0) > 2)) wb.mode = M_DRAG;
    if (wb.mode == M_DRAG) show_drag();
    else if (wb.mode == M_LASSO) show_lasso();
}

/* ---- the event loop ------------------------------------------------------------------------ */

static void handle_requests(void) {
    while (wb.reqs) {
        Req *r = wb.reqs;
        wb.reqs = r->next;
        switch (r->what) {
        case REQ_OPEN: {
            int t = icon_type_of(r->name);
            if (t == WBDRAWER || t == WBDISK) open_drawer(r->name, 0);
            else if (t == WBTOOL) launch(r->name, 0);
            else if (t == WBPROJECT) {
                WIcon tmp;
                memset(&tmp, 0, sizeof tmp);
                snprintf(tmp.path, sizeof tmp.path, "%s", r->name);
                snprintf(tmp.name, sizeof tmp.name, "%s", base_name(r->name));
                tmp.type = WBPROJECT;
                tmp.dobj = icon_get_new(r->name);
                if (tmp.dobj) { open_icon(&tmp); icon_free(tmp.dobj); }
            }
            break;
        }
        case REQ_UPDATE: update_path(r->name); break;
        case REQ_CLOSE: { WBWin *w = find_drawer(r->name); if (w) detach_close(w); break; }
        case REQ_INFO: {
            WIcon tmp;
            memset(&tmp, 0, sizeof tmp);
            snprintf(tmp.path, sizeof tmp.path, "%s", r->name);
            snprintf(tmp.name, sizeof tmp.name, "%s", base_name(r->name));
            tmp.dobj = icon_get_new(r->name);
            tmp.type = tmp.dobj ? rd8(tmp.dobj + DiskObject_do_Type) : WBPROJECT;
            if (tmp.dobj) { do_info(&tmp); icon_free(tmp.dobj); }
            break;
        }
        case REQ_APPCHANGED:
            rebuild_menus();
            for (WBWin *w = wb.wins; w; w = w->next) if (w->backdrop) { read_drawer(w); relayout(w); }
            break;
        }
        free(r);
        if (!wb.running) break;
    }
}

static void wb_loop(void) {
    while (wb.running) {
        uint32_t got = exec_wait((1u << rd8(wb.port + MP_SIGBIT)) | SIGF_REQ);
        if (got & SIGF_REQ) handle_requests();
        uint32_t msg;
        while (wb.running && (msg = list_remhead(wb.port + MP_MSGLIST))) {
            if (rd8(msg + LN_TYPE) == NT_REPLYMSG) {               /* our WBStartups and AppMessages coming back */
                if (!launch_ended(msg)) appmsg_returned(msg);
                continue;
            }
            NMsg m;
            m.cls = rd32(msg + IntuiMessage_Class);
            m.code = rd16(msg + IntuiMessage_Code);
            m.qual = rd16(msg + IntuiMessage_Qualifier);
            m.iaddr = rd32(msg + IntuiMessage_IAddress);
            m.mx = (int16_t)rd16(msg + IntuiMessage_MouseX);
            m.my = (int16_t)rd16(msg + IntuiMessage_MouseY);
            m.secs = rd32(msg + IntuiMessage_Seconds);
            m.micros = rd32(msg + IntuiMessage_Micros);
            uint32_t win = rd32(msg + IntuiMessage_IDCMPWindow);
            exec_reply_msg(msg);
            WBWin *w = wbwin_of(win);
            if (!w) continue;
            switch (m.cls) {
            case IDCMP_ACTIVEWINDOW: wb.active = w; sync_checks(w); break;
            case IDCMP_MOUSEBUTTONS: wb.active = w; mouse_button(w, &m); break;
            case IDCMP_MOUSEMOVE:
                if (m.iaddr && m.iaddr == w->prop) goto scroll;
                mouse_move(w, &m);
                break;
            case IDCMP_GADGETDOWN: case IDCMP_GADGETUP:
            scroll:
                if (w->prop) {
                    int bl, bt, iw, ih;
                    inner(w, &bl, &bt, &iw, &ih);
                    int range = w->ch - ih;
                    int sy = range > 0 ? (int)((long)rd16(w->propinfo + PropInfo_VertPot) * range / 0xFFFF) : 0;
                    if (sy != w->sy) { w->sy = sy; redraw(w); }
                }
                break;
            case IDCMP_NEWSIZE: relayout(w); break;
            case IDCMP_CLOSEWINDOW: if (!w->backdrop) detach_close(w); break;
            case IDCMP_MENUPICK: wb.active = w; menu_pick(w, m.code); break;
            case IDCMP_VANILLAKEY:
                if (m.code == 13) { WIcon *sel[64]; int n = selected_icons(w, sel, 64); for (int i = 0; i < n; i++) open_icon(sel[i]); }
                break;
            }
        }
    }
}

static void quit_workbench(void) {
    while (wb.wins) detach_close(wb.wins);
    wb.running = 0;
}

static void wb_main(void *arg) {
    (void)arg;
    NTask *me = sched_current();
    wb.c = &me->cpu;
    wb.task = me->task;
    wb.s = workbench_screen(1);
    if (!wb.s) { wb.running = 0; return; }
    wb.port = lib_call(wb.c, SysBase, "exec", "CreateMsgPort");
    wb.vi = lib_call(wb.c, GadToolsBase, "gadtools", "GetVisualInfoA", wb.s->scr, 0);
    build_menus();
    open_desktop();
    open_drawer("SYS:", 0);
    wb_loop();
    /* still running programs reply later: wait for them before the port goes */
    while (wb.launches || wb.nappmsgs) {
        exec_wait(1u << rd8(wb.port + MP_SIGBIT));
        uint32_t msg;
        while ((msg = list_remhead(wb.port + MP_MSGLIST))) {
            if (rd8(msg + LN_TYPE) == NT_REPLYMSG) { if (!launch_ended(msg)) appmsg_returned(msg); }
            else exec_reply_msg(msg);
        }
    }
    if (wb.menus) lib_call(wb.c, GadToolsBase, "gadtools", "FreeMenus", wb.menus);
    lib_call(wb.c, GadToolsBase, "gadtools", "FreeVisualInfo", wb.vi);
    lib_call(wb.c, SysBase, "exec", "DeleteMsgPort", wb.port);
    ar_free(&wb.A);
    wb.menus = 0;
    wb.port = 0;
    wb.task = 0;
}

int workbench_running(void) { return wb.running; }

void workbench_start(void) {
    if (wb.running) return;
    wb.running = 1;
    uint32_t pr = dos_create_cli("Workbench", wb_main, 0);
    wr8(pr + LN_PRI, 1);
}

/* ---- workbench.library ------------------------------------------------------------------------- */

static void queue(int what, const char *name) {
    if (!wb.running || !wb.task) return;
    Req *r = calloc(1, sizeof *r);
    r->what = what;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    Req **pp = &wb.reqs;
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    sched_signal(wb.task, SIGF_REQ);
}

static uint32_t new_app(int kind, uint32_t id, uint32_t ud, uint32_t win, uint32_t port, uint32_t text, uint32_t dobj) {
    AppObj *a = calloc(1, sizeof *a);
    a->handle = exec_allocmem(8, MEMF_PUBLIC | MEMF_CLEAR);
    a->kind = kind;
    a->id = id;
    a->userdata = ud;
    a->win = win;
    a->port = port;
    a->dobj = dobj;
    if (text) str_get(text, a->text, sizeof a->text);
    a->next = wb.apps;
    wb.apps = a;
    if (kind != AMTYPE_APPWINDOW) queue(REQ_APPCHANGED, 0);
    return a->handle;
}

static uint32_t rem_app(uint32_t handle) {
    for (AppObj **pp = &wb.apps; *pp; pp = &(*pp)->next) {
        AppObj *a = *pp;
        if (a->handle != handle) continue;
        *pp = a->next;
        if (a->kind != AMTYPE_APPWINDOW) queue(REQ_APPCHANGED, 0);
        exec_freemem(a->handle, 8);
        free(a);
        return DOSTRUE;
    }
    return 0;
}

static uint32_t wbl_AddAppWindowA(M68K *c) { return wb.running ? new_app(AMTYPE_APPWINDOW, c->d[0], c->d[1], c->a[0], c->a[1], 0, 0) : 0; }
static uint32_t wbl_AddAppIconA(M68K *c) { return wb.running ? new_app(AMTYPE_APPICON, c->d[0], c->d[1], 0, c->a[1], c->a[0], c->a[3]) : 0; }
static uint32_t wbl_AddAppMenuItemA(M68K *c) { return wb.running ? new_app(AMTYPE_APPMENUITEM, c->d[0], c->d[1], 0, c->a[1], c->a[0], 0) : 0; }
static uint32_t wbl_Remove(M68K *c) { return rem_app(c->a[0]); }

static void name_of_lock(uint32_t lock, uint32_t name, char *out, int max) {
    char dir[600] = "", n[256] = "";
    const char *hp = lock ? dos_lock_host_path(lock) : 0;
    if (hp) dos_amiga_name(hp, dir, sizeof dir);
    if (name) str_get(name, n, sizeof n);
    join_path(out, max, dir, n);
}

static uint32_t wbl_OpenWorkbenchObjectA(M68K *c) {
    if (!wb.running) return 0;
    char n[600];
    str_get(c->a[0], n, sizeof n);
    queue(REQ_OPEN, n);
    return DOSTRUE;
}
static uint32_t wbl_CloseWorkbenchObjectA(M68K *c) {
    char n[600];
    str_get(c->a[0], n, sizeof n);
    queue(REQ_CLOSE, n);
    return wb.running ? DOSTRUE : 0;
}
static uint32_t wbl_UpdateWorkbench(M68K *c) {
    char p[600], dir[600] = "";
    const char *hp = c->a[1] ? dos_lock_host_path(c->a[1]) : 0;
    if (hp) dos_amiga_name(hp, dir, sizeof dir);
    (void)p;
    queue(REQ_UPDATE, dir);
    return 0;
}
static uint32_t wbl_WBInfo(M68K *c) {
    char p[600];
    name_of_lock(c->a[0], c->a[1], p, sizeof p);
    queue(REQ_INFO, p);
    return DOSTRUE;
}
static uint32_t wbl_WorkbenchControlA(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t wbl_Zero(M68K *c) { (void)c; return 0; }

static const NativeEntry wb_natives[] = {
    { "UpdateWorkbench", wbl_UpdateWorkbench }, { "AddAppWindowA", wbl_AddAppWindowA },
    { "RemoveAppWindow", wbl_Remove }, { "AddAppIconA", wbl_AddAppIconA }, { "RemoveAppIcon", wbl_Remove },
    { "AddAppMenuItemA", wbl_AddAppMenuItemA }, { "RemoveAppMenuItem", wbl_Remove }, { "WBInfo", wbl_WBInfo },
    { "OpenWorkbenchObjectA", wbl_OpenWorkbenchObjectA }, { "CloseWorkbenchObjectA", wbl_CloseWorkbenchObjectA },
    { "WorkbenchControlA", wbl_WorkbenchControlA }, { "AddAppWindowDropZoneA", wbl_Zero },
    { "RemoveAppWindowDropZone", wbl_Zero }, { "ChangeWorkbenchSelectionA", wbl_Zero },
    { "MakeWorkbenchObjectVisibleA", wbl_Zero }, { "WhichWorkbenchObjectA", wbl_Zero },
    { 0, 0 }
};

void workbench_init(void) {
    WorkbenchBase = lib_create("workbench.library", "wb", 64, wb_natives, 44, 1, "workbench 44.1 (AmigaMac)");
    lib_add(WorkbenchBase);
}
