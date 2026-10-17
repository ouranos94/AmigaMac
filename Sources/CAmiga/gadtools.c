/* gadtools.library, written from the GadTools autodocs and gadtools.h.
 *
 * Every gadget kind except STRING/INTEGER/GENERIC is an object of one private
 * native BOOPSI class (a custom gadget): intuition's custom-gadget input path
 * drives it with GM_GOACTIVE/GM_HANDLEINPUT, and the class sends the IDCMP
 * messages GadTools programs expect (GADGETUP with Code = new value, GADGETDOWN
 * for MX, MOUSEMOVE while a slider or scroller is dragged). The per-gadget
 * state lives on the native side, looked up by gadget address.
 * STRING/INTEGER are plain intuition string gadgets with a ridge border, so the
 * built-in editor handles them. Menus are built from NewMenu arrays into real
 * Menu/MenuItem/IntuiText structures with the GadTools UserData after each. */
#include "intui.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t GadToolsBase;

enum { GENERIC_KIND, BUTTON_KIND, CHECKBOX_KIND, INTEGER_KIND, LISTVIEW_KIND, MX_KIND, NUMBER_KIND,
       CYCLE_KIND, PALETTE_KIND, SCROLLER_KIND, SLIDER_KIND = 11, STRING_KIND, TEXT_KIND, NUM_KINDS };

#define PLACETEXT_LEFT 1
#define PLACETEXT_RIGHT 2
#define PLACETEXT_ABOVE 4
#define PLACETEXT_BELOW 8
#define PLACETEXT_IN 0x10
#define NG_HIGHLABEL 0x20

#define TAG_USER_ 0x80000000u
#define GT(n) (TAG_USER_ + 0x80000 + (n))
#define GTCB_Checked GT(4)
#define GTLV_Top GT(5)
#define GTLV_Labels GT(6)
#define GTLV_ReadOnly GT(7)
#define GTLV_ScrollWidth GT(8)
#define GTMX_Labels GT(9)
#define GTMX_Active GT(10)
#define GTTX_Text GT(11)
#define GTTX_CopyText GT(12)
#define GTNM_Number GT(13)
#define GTCY_Labels GT(14)
#define GTCY_Active GT(15)
#define GTPA_Depth GT(16)
#define GTPA_Color GT(17)
#define GTPA_ColorOffset GT(18)
#define GTPA_IndicatorWidth GT(19)
#define GTPA_IndicatorHeight GT(20)
#define GTSC_Top GT(21)
#define GTSC_Total GT(22)
#define GTSC_Visible GT(23)
#define GTSL_Min GT(38)
#define GTSL_Max GT(39)
#define GTSL_Level GT(40)
#define GTSL_MaxLevelLen GT(41)
#define GTSL_LevelFormat GT(42)
#define GTSL_LevelPlace GT(43)
#define GTST_String GT(45)
#define GTST_MaxChars GT(46)
#define GTIN_Number GT(47)
#define GTIN_MaxChars GT(48)
#define GTMN_FrontPen GT(50)
#define GTBB_Recessed GT(51)
#define GT_VisualInfo GT(52)
#define GTLV_ShowSelected GT(53)
#define GTLV_Selected GT(54)
#define GTTX_Border GT(57)
#define GTNM_Border GT(58)
#define GTSC_Arrows GT(59)
#define GTMN_Menu GT(60)
#define GTMX_Spacing GT(61)
#define GT_Underscore GT(64)
#define GTMX_TitlePlace GT(71)
#define GTTX_FrontPen GT(72)
#define GTNM_Format GT(75)
#define GTBB_FrameType GT(77)
#define GTLV_MakeVisible GT(78)
#define GTLV_ItemHeight GT(79)
#define GTPA_ColorTable GT(82)
#define GTPA_NumColors GT(70)
#define GA_Disabled_ (TAG_USER_ + 0x3000E)
#define PGA_Freedom_ (TAG_USER_ + 0x31001)
#define LORIENT_VERT 2
#define BBFT_BUTTON 1
#define BBFT_RIDGE 2
#define BBFT_ICONDROPBOX 3

/* menu flags */
#define MENUENABLED_ 0x1
#define CHECKIT_ 0x1
#define ITEMTEXT_ 0x2
#define COMMSEQ_ 0x4
#define MENUTOGGLE_ 0x8
#define ITEMENABLED_ 0x10
#define HIGHCOMP_ 0x40
#define HIGHNONE_ 0xC0
#define CHECKED_ 0x100
#define NM_TITLE 1
#define NM_ITEM 2
#define NM_SUB 3
#define NM_IGNORE 64
#define MENU_IMAGE 128
#define NM_BARLABEL 0xFFFFFFFFu
#define NM_COMMANDSTRING COMMSEQ_

#define GM_HITTEST 0
#define GM_RENDER 1
#define GM_GOACTIVE 2
#define GM_HANDLEINPUT 3
#define GM_GOINACTIVE 4
#define OM_DISPOSE 0x102
#define GMR_MEACTIVE 0
#define GMR_NOREUSE 2
#define GMR_VERIFY 8
#define GMR_GADGETHIT 4
#define IECLASS_RAWMOUSE 2
#define SELECTUP_ 0xE8

/* ---- native state ------------------------------------------------------------ */

typedef struct GT {
    uint32_t g;                     /* the gadget */
    int kind;
    uint32_t vi;
    char label[80];
    int ul;                         /* index of the underlined char, -1 */
    int place, high;
    int32_t value;                  /* checked, active, level, top, number, colour, selected */
    int32_t min, max, total, visible;
    int vert, arrows, spacing, itemh, top;
    int readonly, showsel;
    uint32_t labels;                /* STRPTR array (cycle, mx) or struct List (listview) */
    int nlabels;
    uint32_t text;                  /* TEXT: 68k string */
    char tbuf[256];
    int copytext, border, frontpen;
    char fmt[32];
    int levelplace, maxlevellen;
    int depth, coloroffset, indw, indh, ncolors;
    uint32_t colortable;
    int bw, bh;                     /* MX: one button */
    int pressed, drag, dragoff;     /* input state */
    uint32_t block;                 /* extra 68k memory (STRING/INTEGER/GENERIC) */
    struct GT *next;
} GT;

static GT *gts;
static uint32_t gt_class;

static GT *gt_of(uint32_t g) {
    for (GT *t = gts; t; t = t->next) if (t->g == g) return t;
    return 0;
}

static NScreen *vi_screen(uint32_t vi) { return vi ? nscr_of(rd32(vi)) : 0; }

/* ---- drawing helpers --------------------------------------------------------- */

static void fill(uint32_t rp, int x0, int y0, int x1, int y1, int pen) {
    if (x1 >= x0 && y1 >= y0) rp_fill(rp, x0, y0, x1, y1, pen, 0);
}

static void line(uint32_t rp, int x0, int y0, int x1, int y1, int pen) {
    BM b;
    if (bm_load(rd32(rp + RastPort_BitMap), &b)) bm_line(&b, x0, y0, x1, y1, pen, JAM1, 0xFF);
}

/* the 3.x look: light top/left, dark bottom/right (or the other way round) */
static void bevel(uint32_t rp, const uint16_t *pens, int x, int y, int w, int h, int recessed) {
    if (w < 2 || h < 2) return;
    int hi = pens[recessed ? SHADOWPEN : SHINEPEN], lo = pens[recessed ? SHINEPEN : SHADOWPEN];
    fill(rp, x, y, x + w - 2, y, hi);
    fill(rp, x, y, x, y + h - 1, hi);
    fill(rp, x + 1, y, x + 1, y + h - 2, hi);
    fill(rp, x + 1, y + h - 1, x + w - 1, y + h - 1, lo);
    fill(rp, x + w - 1, y, x + w - 1, y + h - 1, lo);
    fill(rp, x + w - 2, y + 1, x + w - 2, y + h - 1, lo);
}

static void ridge(uint32_t rp, const uint16_t *pens, int x, int y, int w, int h) {
    bevel(rp, pens, x, y, w, h, 0);
    bevel(rp, pens, x + 2, y + 1, w - 4, h - 2, 1);
}

static void text_at(uint32_t rp, int x, int y, const char *s, int n, int fg, int bg, int jam2) {
    uint32_t ofg = rd8(rp + RastPort_FgPen), obg = rd8(rp + RastPort_BgPen), om = rd8(rp + RastPort_DrawMode);
    wr8(rp + RastPort_FgPen, (uint32_t)fg);
    wr8(rp + RastPort_BgPen, (uint32_t)bg);
    wr8(rp + RastPort_DrawMode, jam2 ? JAM2 : JAM1);
    rp_move(rp, x, y + 6);
    rp_text(rp, s, n);
    wr8(rp + RastPort_FgPen, ofg);
    wr8(rp + RastPort_BgPen, obg);
    wr8(rp + RastPort_DrawMode, om);
}

/* "_Name" -> "Name", underline index 0 */
static void strip_label(const char *in, char *out, int size, int *ul, int underscore) {
    int j = 0;
    *ul = -1;
    for (int i = 0; in[i] && j < size - 1; i++) {
        if (underscore && in[i] == underscore && in[i + 1] && *ul < 0) { *ul = j; continue; }
        out[j++] = in[i];
    }
    out[j] = 0;
}

static void label_pos(GT *t, int x, int y, int w, int h, int tw, int *lx, int *ly) {
    switch (t->place) {
    case PLACETEXT_RIGHT: *lx = x + w + 8; *ly = y + (h - 8) / 2; break;
    case PLACETEXT_ABOVE: *lx = x + (w - tw) / 2; *ly = y - 10; break;
    case PLACETEXT_BELOW: *lx = x + (w - tw) / 2; *ly = y + h + 2; break;
    case PLACETEXT_IN: *lx = x + (w - tw) / 2; *ly = y + (h - 8) / 2; break;
    default: *lx = x - tw - 8; *ly = y + (h - 8) / 2; break;
    }
}

static void draw_label(GT *t, uint32_t rp, const uint16_t *pens, int x, int y, int w, int h, int pen) {
    if (!t->label[0]) return;
    int n = (int)strlen(t->label), tw = n * 8, lx, ly;
    label_pos(t, x, y, w, h, tw, &lx, &ly);
    if (pen < 0) pen = pens[t->high ? HIGHLIGHTTEXTPEN : TEXTPEN];
    text_at(rp, lx, ly, t->label, n, pen, pens[BACKGROUNDPEN], t->place != PLACETEXT_IN);
    if (t->ul >= 0) fill(rp, lx + t->ul * 8, ly + 8, lx + t->ul * 8 + 6, ly + 8, pen);
}

/* labels of CYCLE/MX: a NULL-terminated STRPTR array */
static int count_labels(uint32_t arr) {
    int n = 0;
    if (arr) while (rd32(arr + 4 * (uint32_t)n) && n < 256) n++;
    return n;
}
static int label_str(uint32_t arr, int i, char *buf, int size) {
    buf[0] = 0;
    if (!arr || i < 0) return 0;
    uint32_t p = rd32(arr + 4 * (uint32_t)i);
    return p ? str_get(p, buf, size) : 0;
}

/* LISTVIEW: an exec List of nodes, ~0 = detached */
static int lv_valid(GT *t) { return t->labels && t->labels != 0xFFFFFFFFu; }
static int lv_count(GT *t) {
    int n = 0;
    if (!lv_valid(t)) return 0;
    for (uint32_t nd = rd32(t->labels); nd && rd32(nd); nd = rd32(nd)) n++;
    return n;
}
static uint32_t lv_node(GT *t, int i) {
    if (!lv_valid(t) || i < 0) return 0;
    for (uint32_t nd = rd32(t->labels); nd && rd32(nd); nd = rd32(nd)) if (i-- == 0) return nd;
    return 0;
}

/* Amiga printf-style format of one LONG (%ld, %lu, %lx, %d, with width) */
static void fmt_long(const char *fmt, int32_t v, char *out, int size) {
    char cf[40];
    int j = 0;
    for (int i = 0; fmt[i] && j < (int)sizeof cf - 2; i++) if (fmt[i] != 'l') cf[j++] = fmt[i];
    cf[j] = 0;
    snprintf(out, (size_t)size, cf, (int)v);
}

/* ---- geometry of the proportional kinds ------------------------------------------ */

typedef struct { int cx, cy, cw, ch; int kx, ky, kw, kh; int ax0, ay0, ax1, ay1, asz; } PropGeo;

static int prop_range(GT *t) {
    if (t->kind == SLIDER_KIND) return t->max - t->min;
    if (t->kind == LISTVIEW_KIND) { int r = lv_count(t) - t->visible; return r > 0 ? r : 0; }
    int r = t->total - t->visible;
    return r > 0 ? r : 0;
}
static int prop_pos(GT *t) {
    if (t->kind == SLIDER_KIND) return t->value - t->min;
    if (t->kind == LISTVIEW_KIND) return t->top;
    return t->value;
}

static void prop_geo(GT *t, int x, int y, int w, int h, int vert, PropGeo *p) {
    memset(p, 0, sizeof *p);
    p->asz = t->arrows;
    p->cx = x; p->cy = y; p->cw = w; p->ch = h;
    if (p->asz) {
        if (vert) { p->ch = h - 2 * p->asz; p->ax0 = x; p->ay0 = y + p->ch; p->ax1 = x; p->ay1 = y + p->ch + p->asz; }
        else { p->cw = w - 2 * p->asz; p->ax0 = x + p->cw; p->ay0 = y; p->ax1 = x + p->cw + p->asz; p->ay1 = y; }
    }
    int range = prop_range(t), pos = prop_pos(t);
    int inner = vert ? p->ch - 4 : p->cw - 8;
    int knob;
    if (t->kind == SLIDER_KIND) knob = inner / (range + 1);
    else {
        int tot = t->kind == LISTVIEW_KIND ? lv_count(t) : t->total;
        knob = tot > t->visible && tot > 0 ? (int)((long)inner * t->visible / tot) : inner;
    }
    if (knob < (vert ? 4 : 6)) knob = vert ? 4 : 6;
    if (knob > inner) knob = inner;
    int off = range > 0 ? (int)((long)(inner - knob) * pos / range) : 0;
    if (vert) { p->kx = p->cx + 4; p->kw = p->cw - 8; p->ky = p->cy + 2 + off; p->kh = knob; }
    else { p->kx = p->cx + 4 + off; p->kw = knob; p->ky = p->cy + 2; p->kh = p->ch - 4; }
}

static int prop_pos_from(GT *t, PropGeo *p, int vert, int mouse) {
    int range = prop_range(t);
    int inner = vert ? p->ch - 4 : p->cw - 8, knob = vert ? p->kh : p->kw;
    if (inner - knob <= 0 || range <= 0) return 0;
    int off = mouse - (vert ? p->cy + 2 : p->cx + 4);
    int pos = (int)(((long)off * range + (inner - knob) / 2) / (inner - knob));
    return pos < 0 ? 0 : pos > range ? range : pos;
}

static void set_prop_pos(GT *t, int pos) {
    int range = prop_range(t);
    if (pos < 0) pos = 0;
    if (pos > range) pos = range;
    if (t->kind == SLIDER_KIND) t->value = t->min + pos;
    else if (t->kind == LISTVIEW_KIND) t->top = pos;
    else t->value = pos;
}

static void arrow_glyph(uint32_t rp, int x, int y, int w, int h, int dir, int pen) {
    /* dir: 0 left, 1 right, 2 up, 3 down */
    int cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < 4; i++) {
        switch (dir) {
        case 0: fill(rp, cx - 2 + i, cy - i, cx - 2 + i, cy + i, pen); break;
        case 1: fill(rp, cx + 1 - i, cy - i, cx + 1 - i, cy + i, pen); break;
        case 2: fill(rp, cx - i * 2, cy - 2 + i, cx + i * 2, cy - 2 + i, pen); break;
        case 3: fill(rp, cx - i * 2, cy + 1 - i, cx + i * 2, cy + 1 - i, pen); break;
        }
    }
}

static void draw_prop(GT *t, uint32_t rp, const uint16_t *pens, int x, int y, int w, int h, int vert) {
    PropGeo p;
    prop_geo(t, x, y, w, h, vert, &p);
    fill(rp, p.cx, p.cy, p.cx + p.cw - 1, p.cy + p.ch - 1, pens[BACKGROUNDPEN]);
    bevel(rp, pens, p.cx, p.cy, p.cw, p.ch, t->kind == SLIDER_KIND);
    fill(rp, p.kx, p.ky, p.kx + p.kw - 1, p.ky + p.kh - 1, pens[t->drag ? SHINEPEN : FILLPEN]);
    bevel(rp, pens, p.kx, p.ky, p.kw, p.kh, 0);
    if (p.asz) {
        int aw = vert ? w : p.asz, ah = vert ? p.asz : h;
        for (int i = 0; i < 2; i++) {
            int ax = i ? p.ax1 : p.ax0, ay = i ? p.ay1 : p.ay0;
            fill(rp, ax, ay, ax + aw - 1, ay + ah - 1, pens[BACKGROUNDPEN]);
            bevel(rp, pens, ax, ay, aw, ah, t->pressed == 2 + i);
            arrow_glyph(rp, ax, ay, aw, ah, vert ? 2 + i : i, pens[TEXTPEN]);
        }
    }
}

/* ---- rendering ------------------------------------------------------------------ */

static void palette_geo(GT *t, int x, int y, int w, int h, int *gx, int *gy, int *cols, int *rows, int *cw, int *ch) {
    int n = t->ncolors;
    *gx = x; *gy = y;
    if (t->indw) { *gx += t->indw + 4; w -= t->indw + 4; }
    else if (t->indh) { *gy += t->indh + 4; h -= t->indh + 4; }
    int best = 1, bestd = 1 << 30;
    for (int r = 1; r <= n; r++) {
        if (n % r) continue;
        int c = n / r, cwi = w / c, chi = h / r;
        int d = abs(cwi - chi * 2);
        if (cwi >= 2 && chi >= 2 && d < bestd) { bestd = d; best = r; }
    }
    *rows = best; *cols = n / best;
    *cw = w / *cols; *ch = h / *rows;
}
static int palette_pen(GT *t, int i) {
    if (t->colortable) return rd8(t->colortable + (uint32_t)i);
    return t->coloroffset + i;
}

static void gt_render(GT *t, NWindow *w, uint32_t rp) {
    const uint16_t *pens = w->s->pens;
    int x, y, gw, gh;
    gadget_box(w, t->g, &x, &y, &gw, &gh);
    int bg = pens[BACKGROUNDPEN];
    char buf[256];
    switch (t->kind) {
    case BUTTON_KIND:
        fill(rp, x, y, x + gw - 1, y + gh - 1, t->pressed ? pens[FILLPEN] : bg);
        bevel(rp, pens, x, y, gw, gh, t->pressed);
        if (t->place == PLACETEXT_IN) {
            int n = (int)strlen(t->label), lx, ly;
            label_pos(t, x, y, gw, gh, n * 8, &lx, &ly);
            int pen = t->pressed ? pens[FILLTEXTPEN] : pens[t->high ? HIGHLIGHTTEXTPEN : TEXTPEN];
            text_at(rp, lx, ly, t->label, n, pen, bg, 0);
            if (t->ul >= 0) fill(rp, lx + t->ul * 8, ly + 8, lx + t->ul * 8 + 6, ly + 8, pen);
        } else draw_label(t, rp, pens, x, y, gw, gh, -1);
        break;
    case CHECKBOX_KIND:
        fill(rp, x, y, x + gw - 1, y + gh - 1, bg);
        bevel(rp, pens, x, y, gw, gh, t->pressed);
        if (t->value) {
            int pen = pens[TEXTPEN], cx = x + gw / 2, by = y + gh - 3;
            for (int k = 0; k < 2; k++) {
                line(rp, cx - 6 + k, y + gh / 2, cx - 2 + k, by, pen);
                line(rp, cx - 2 + k, by, cx + 6 + k, y + 2, pen);
            }
        }
        draw_label(t, rp, pens, x, y, gw, gh, -1);
        break;
    case MX_KIND: {
        int n = t->nlabels;
        int maxw = 0;
        for (int i = 0; i < n; i++) { int l = label_str(t->labels, i, buf, sizeof buf); if (l > maxw) maxw = l; }
        for (int i = 0; i < n; i++) {
            int by = y + i * (t->bh + t->spacing);
            fill(rp, x, by, x + t->bw - 1, by + t->bh - 1, bg);
            bevel(rp, pens, x, by, t->bw, t->bh, i == t->value);
            if (i == t->value) fill(rp, x + 4, by + 2, x + t->bw - 5, by + t->bh - 3, pens[FILLPEN]);
            int l = label_str(t->labels, i, buf, sizeof buf);
            int lx = t->place == PLACETEXT_LEFT ? x - l * 8 - 8 : x + t->bw + 8;
            text_at(rp, lx, by + (t->bh - 8) / 2, buf, l, pens[TEXTPEN], bg, 1);
        }
        (void)maxw;
        break;
    }
    case CYCLE_KIND: {
        fill(rp, x, y, x + gw - 1, y + gh - 1, t->pressed ? pens[FILLPEN] : bg);
        bevel(rp, pens, x, y, gw, gh, t->pressed);
        int pen = pens[t->pressed ? FILLTEXTPEN : TEXTPEN], cy = y + gh / 2;
        /* the cycle glyph: a loop with an arrow head */
        line(rp, x + 6, cy - 3, x + 6, cy + 2, pen); line(rp, x + 7, cy - 3, x + 7, cy + 2, pen);
        line(rp, x + 8, cy - 4, x + 13, cy - 4, pen);
        line(rp, x + 14, cy - 3, x + 14, cy - 1, pen); line(rp, x + 15, cy - 3, x + 15, cy - 1, pen);
        line(rp, x + 12, cy - 1, x + 17, cy - 1, pen); line(rp, x + 13, cy, x + 16, cy, pen); line(rp, x + 14, cy + 1, x + 15, cy + 1, pen);
        line(rp, x + 8, cy + 3, x + 15, cy + 3, pen);
        fill(rp, x + 20, y + 2, x + 20, y + gh - 3, pens[SHADOWPEN]);
        fill(rp, x + 21, y + 2, x + 21, y + gh - 3, pens[SHINEPEN]);
        int l = label_str(t->labels, t->value, buf, sizeof buf);
        int maxc = (gw - 28) / 8;
        if (l > maxc) l = maxc;
        text_at(rp, x + 22 + (gw - 22 - l * 8) / 2, y + (gh - 8) / 2, buf, l, pen, bg, 0);
        draw_label(t, rp, pens, x, y, gw, gh, -1);
        break;
    }
    case SLIDER_KIND: case SCROLLER_KIND:
        draw_prop(t, rp, pens, x, y, gw, gh, t->vert);
        draw_label(t, rp, pens, x, y, gw, gh, -1);
        if (t->kind == SLIDER_KIND && t->fmt[0]) {
            char lv[64];
            fmt_long(t->fmt, t->value, lv, sizeof lv);
            int n = (int)strlen(lv), mw = t->maxlevellen * 8;
            if (n > t->maxlevellen) n = t->maxlevellen;
            int lx, ly;
            GT tmp = *t;
            tmp.place = t->levelplace;
            label_pos(&tmp, x, y, gw, gh, mw, &lx, &ly);
            fill(rp, lx, ly, lx + mw - 1, ly + 7, bg);
            text_at(rp, lx + mw - n * 8, ly, lv, n, pens[TEXTPEN], bg, 1);
        }
        break;
    case LISTVIEW_KIND: {
        int sw = 16, lw = gw - sw;
        fill(rp, x, y, x + lw - 1, y + gh - 1, bg);
        bevel(rp, pens, x, y, lw, gh, t->readonly);
        int rows = (gh - 4) / t->itemh;
        t->visible = rows;
        int cnt = lv_count(t);
        if (t->top > cnt - rows) t->top = cnt - rows > 0 ? cnt - rows : 0;
        for (int r = 0; r < rows; r++) {
            uint32_t nd = lv_node(t, t->top + r);
            if (!nd) break;
            int ry = y + 2 + r * t->itemh, sel = t->showsel && t->top + r == t->value;
            if (sel) fill(rp, x + 4, ry, x + lw - 5, ry + t->itemh - 1, pens[FILLPEN]);
            int l = rd32(nd + LN_NAME) ? str_get(rd32(nd + LN_NAME), buf, sizeof buf) : 0;
            int maxc = (lw - 12) / 8;
            if (l > maxc) l = maxc;
            text_at(rp, x + 6, ry + (t->itemh - 8) / 2, buf, l, pens[sel ? FILLTEXTPEN : TEXTPEN], bg, 0);
        }
        GT sc = *t;
        sc.kind = LISTVIEW_KIND;
        sc.arrows = 8;
        draw_prop(&sc, rp, pens, x + lw, y, sw, gh, 1);
        draw_label(t, rp, pens, x, y, gw, gh, -1);
        break;
    }
    case PALETTE_KIND: {
        int gx, gy, cols, rows, cw, ch;
        palette_geo(t, x, y, gw, gh, &gx, &gy, &cols, &rows, &cw, &ch);
        fill(rp, x, y, x + gw - 1, y + gh - 1, bg);
        if (t->indw || t->indh) {
            int iw = t->indw ? t->indw : gw, ih = t->indh ? t->indh : gh;
            bevel(rp, pens, x, y, iw, ih, 1);
            fill(rp, x + 3, y + 2, x + iw - 4, y + ih - 3, t->value);
        }
        for (int i = 0; i < t->ncolors; i++) {
            int cx = gx + (i % cols) * cw, cy = gy + (i / cols) * ch;
            int pen = palette_pen(t, i);
            if (pen == t->value) {
                fill(rp, cx, cy, cx + cw - 1, cy + ch - 1, pens[TEXTPEN]);
                fill(rp, cx + 1, cy + 1, cx + cw - 2, cy + ch - 2, pens[SHINEPEN]);
                fill(rp, cx + 2, cy + 2, cx + cw - 3, cy + ch - 3, pen);
            } else {
                fill(rp, cx + 1, cy + 1, cx + cw - 2, cy + ch - 2, pen);
            }
        }
        draw_label(t, rp, pens, x, y, gw, gh, -1);
        break;
    }
    case TEXT_KIND: case NUMBER_KIND: {
        fill(rp, x, y, x + gw - 1, y + gh - 1, bg);
        if (t->border) bevel(rp, pens, x, y, gw, gh, 1);
        int l;
        if (t->kind == NUMBER_KIND) { fmt_long(t->fmt[0] ? t->fmt : "%ld", t->value, buf, sizeof buf); l = (int)strlen(buf); }
        else if (t->copytext) { snprintf(buf, sizeof buf, "%s", t->tbuf); l = (int)strlen(buf); }
        else l = t->text ? str_get(t->text, buf, sizeof buf) : 0;
        int maxc = (gw - (t->border ? 12 : 0)) / 8;
        if (l > maxc) l = maxc;
        text_at(rp, x + (t->border ? 6 : 0), y + (gh - 8) / 2, buf, l, t->frontpen >= 0 ? t->frontpen : pens[TEXTPEN], bg, 0);
        draw_label(t, rp, pens, x, y, gw, gh, -1);
        break;
    }
    }
    gfx_touch();
}

/* ---- input ---------------------------------------------------------------------- */

static int mx_index(GT *t, int my) {
    int pitch = t->bh + t->spacing;
    if (my < 0 || pitch <= 0) return -1;
    int i = my / pitch;
    if (i >= t->nlabels || my - i * pitch >= t->bh) return -1;
    return i;
}

static int palette_index(GT *t, NWindow *w, int mx, int my) {
    int x, y, gw, gh, gx, gy, cols, rows, cw, ch;
    gadget_box(w, t->g, &x, &y, &gw, &gh);
    palette_geo(t, x, y, gw, gh, &gx, &gy, &cols, &rows, &cw, &ch);
    int ax = x + mx - gx, ay = y + my - gy;
    if (ax < 0 || ay < 0 || cw <= 0 || ch <= 0) return -1;
    int c = ax / cw, r = ay / ch;
    if (c >= cols || r >= rows) return -1;
    return r * cols + c;
}

/* which part of a prop kind: 0 container, 1 knob, 2 first arrow, 3 second arrow, -1 none */
static int prop_part(GT *t, PropGeo *p, int vert, int ax, int ay) {
    if (p->asz) {
        int aw = vert ? p->cw : p->asz, ah = vert ? p->asz : p->ch;
        if (ax >= p->ax0 && ax < p->ax0 + aw && ay >= p->ay0 && ay < p->ay0 + ah) return 2;
        if (ax >= p->ax1 && ax < p->ax1 + aw && ay >= p->ay1 && ay < p->ay1 + ah) return 3;
    }
    if (ax >= p->kx && ax < p->kx + p->kw && ay >= p->ky && ay < p->ky + p->kh) return 1;
    if (ax >= p->cx && ax < p->cx + p->cw && ay >= p->cy && ay < p->cy + p->ch) return 0;
    (void)t;
    return -1;
}

static void lv_scroller_box(GT *t, NWindow *w, int *sx, int *sy, int *sw, int *sh) {
    int x, y, gw, gh;
    gadget_box(w, t->g, &x, &y, &gw, &gh);
    *sx = x + gw - 16; *sy = y; *sw = 16; *sh = gh;
}

static void prop_geo_for(GT *t, NWindow *w, PropGeo *p, int *vert, GT *tmp) {
    int x, y, gw, gh;
    *tmp = *t;
    if (t->kind == LISTVIEW_KIND) {
        lv_scroller_box(t, w, &x, &y, &gw, &gh);
        tmp->arrows = 8;
        *vert = 1;
    } else {
        gadget_box(w, t->g, &x, &y, &gw, &gh);
        *vert = t->vert;
    }
    prop_geo(tmp, x, y, gw, gh, *vert, p);
}

static void redraw(GT *t, NWindow *w) { gadget_render_all(w, t->g, 1); }

static void send_value(NWindow *w, GT *t, uint32_t cls, int mx, int my) {
    int x, y, gw, gh;
    gadget_box(w, t->g, &x, &y, &gw, &gh);
    send_idcmp(w, cls, (uint32_t)(t->value & 0xFFFF), 0, t->g, x + mx, y + my);
}

static uint32_t gt_goactive(GT *t, NWindow *w, uint32_t msg) {
    int mx = (int16_t)rd16(msg + 16), my = (int16_t)rd16(msg + 18);
    uint32_t term = rd32(msg + 12);
    int x, y, gw, gh;
    gadget_box(w, t->g, &x, &y, &gw, &gh);
    switch (t->kind) {
    case BUTTON_KIND: case CHECKBOX_KIND: case CYCLE_KIND:
        t->pressed = 1;
        redraw(t, w);
        return GMR_MEACTIVE;
    case MX_KIND: {
        int i = mx_index(t, my);
        if (i < 0) return GMR_NOREUSE;
        if (i != t->value) { t->value = i; redraw(t, w); }
        send_value(w, t, IDCMP_GADGETDOWN, mx, my);
        return GMR_NOREUSE;
    }
    case PALETTE_KIND: {
        int i = palette_index(t, w, mx, my);
        if (i < 0 || i >= t->ncolors) return GMR_NOREUSE;
        t->value = palette_pen(t, i);
        redraw(t, w);
        if (term) wr32(term, (uint32_t)t->value);
        return GMR_NOREUSE | GMR_VERIFY;
    }
    case LISTVIEW_KIND: {
        int sx, sy, sw, sh;
        lv_scroller_box(t, w, &sx, &sy, &sw, &sh);
        if (x + mx < sx) {                                    /* the list itself */
            int r = (my - 2) / t->itemh, ord = t->top + r;
            if (t->readonly || my < 2 || r >= t->visible || ord >= lv_count(t)) return GMR_NOREUSE;
            t->value = ord;
            redraw(t, w);
            if (term) wr32(term, (uint32_t)ord);
            return GMR_NOREUSE | GMR_VERIFY;
        }
    }
    /* fall through: the scroller of a listview */
    case SLIDER_KIND: case SCROLLER_KIND: {
        PropGeo p;
        GT tmp;
        int vert;
        prop_geo_for(t, w, &p, &vert, &tmp);
        int ax = x + mx, ay = y + my;
        int part = prop_part(t, &p, vert, ax, ay);
        int pos = prop_pos(t);
        int step = t->kind == SLIDER_KIND ? 1 : (t->visible > 1 ? t->visible - 1 : 1);
        switch (part) {
        case 1:
            t->drag = 1;
            t->dragoff = vert ? ay - p.ky : ax - p.kx;
            redraw(t, w);
            if (t->kind != LISTVIEW_KIND) send_value(w, t, IDCMP_GADGETDOWN, mx, my);
            return GMR_MEACTIVE;
        case 0: {
            int before = vert ? ay < p.ky : ax < p.kx;
            set_prop_pos(t, pos + (before ? -step : step));
            break;
        }
        case 2: case 3:
            set_prop_pos(t, pos + (part == 2 ? -1 : 1));
            break;
        default: return GMR_NOREUSE;
        }
        redraw(t, w);
        if (t->kind == LISTVIEW_KIND) return GMR_NOREUSE;
        if (term) wr32(term, (uint32_t)t->value);
        send_value(w, t, IDCMP_GADGETDOWN, mx, my);
        return GMR_NOREUSE | GMR_VERIFY;
    }
    }
    return GMR_NOREUSE;
}

static uint32_t gt_handleinput(GT *t, NWindow *w, uint32_t msg) {
    uint32_t ie = rd32(msg + 8), term = rd32(msg + 12);
    int mx = (int16_t)rd16(msg + 16), my = (int16_t)rd16(msg + 18);
    int up = rd8(ie + InputEvent_ie_Class) == IECLASS_RAWMOUSE && rd16(ie + InputEvent_ie_Code) == SELECTUP_;
    int x, y, gw, gh;
    gadget_box(w, t->g, &x, &y, &gw, &gh);
    if (t->drag) {
        PropGeo p;
        GT tmp;
        int vert;
        prop_geo_for(t, w, &p, &vert, &tmp);
        int m = (vert ? y + my : x + mx) - t->dragoff;
        int old = prop_pos(t);
        int pos = prop_pos_from(t, &p, vert, m);
        if (pos != old) {
            set_prop_pos(t, pos);
            redraw(t, w);
            if (t->kind != LISTVIEW_KIND) send_value(w, t, IDCMP_MOUSEMOVE, mx, my);
        }
        if (!up) return GMR_MEACTIVE;
        t->drag = 0;
        redraw(t, w);
        if (t->kind == LISTVIEW_KIND) return GMR_NOREUSE;
        if (term) wr32(term, (uint32_t)t->value);
        return GMR_NOREUSE | GMR_VERIFY;
    }
    int inside = mx >= 0 && my >= 0 && mx < gw && my < gh;
    if (inside != t->pressed) { t->pressed = inside; redraw(t, w); }
    if (!up) return GMR_MEACTIVE;
    t->pressed = 0;
    if (!inside) { redraw(t, w); return GMR_NOREUSE; }
    if (t->kind == CHECKBOX_KIND) t->value = !t->value;
    if (t->kind == CYCLE_KIND && t->nlabels) {
        int shift = rd16(ie + InputEvent_ie_Qualifier) & 3;
        t->value = (t->value + (shift ? t->nlabels - 1 : 1)) % t->nlabels;
    }
    redraw(t, w);
    if (term) wr32(term, t->kind == BUTTON_KIND ? 0 : (uint32_t)t->value);
    return GMR_NOREUSE | GMR_VERIFY;
}

static uint32_t gt_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    GT *t = gt_of(obj);
    uint32_t id = rd32(msg);
    if (t) {
        uint32_t gi = rd32(msg + 4), win = 0;
        if (id <= GM_GOINACTIVE && gi) win = rd32(gi + GadgetInfo_gi_Window);
        NWindow *w = win ? nwin_of(win) : 0;
        switch (id) {
        case GM_HITTEST: {
            int mx = (int16_t)rd16(msg + 8), my = (int16_t)rd16(msg + 10);
            if (t->kind == TEXT_KIND || t->kind == NUMBER_KIND) return 0;
            if (t->kind == MX_KIND) return mx_index(t, my) >= 0 && mx < t->bw ? GMR_GADGETHIT : 0;
            return GMR_GADGETHIT;
        }
        case GM_RENDER:
            if (w && rd32(msg + 8)) gt_render(t, w, rd32(msg + 8));
            return 0;
        case GM_GOACTIVE: return w ? gt_goactive(t, w, msg) : GMR_NOREUSE;
        case GM_HANDLEINPUT: return w ? gt_handleinput(t, w, msg) : GMR_NOREUSE;
        case GM_GOINACTIVE:
            if (t->pressed || t->drag) { t->pressed = t->drag = 0; if (w) redraw(t, w); }
            return 0;
        }
    }
    return bo_super(c, cl, obj, msg);
}

/* ---- creation --------------------------------------------------------------------- */

static void apply_tags(GT *t, uint32_t tags, int creating) {
    uint32_t st = tags, ti;
    while ((ti = tag_next(&st))) {
        uint32_t d = rd32(ti + 4);
        switch (rd32(ti)) {
        case GA_Disabled_: {
            uint32_t f = rd16(t->g + Gadget_Flags);
            wr16(t->g + Gadget_Flags, d ? f | GFLG_DISABLED : f & ~GFLG_DISABLED);
            break;
        }
        case GTCB_Checked: if (t->kind == CHECKBOX_KIND) t->value = d != 0; break;
        case GTMX_Active: if (t->kind == MX_KIND) t->value = (int32_t)d; break;
        case GTMX_Labels: if (t->kind == MX_KIND) { t->labels = d; t->nlabels = count_labels(d); } break;
        case GTMX_Spacing: t->spacing = (int)d; break;
        case GTCY_Active: if (t->kind == CYCLE_KIND) t->value = (int32_t)d; break;
        case GTCY_Labels: if (t->kind == CYCLE_KIND) { t->labels = d; t->nlabels = count_labels(d); } break;
        case GTSL_Min: t->min = (int32_t)(int16_t)d; break;
        case GTSL_Max: t->max = (int32_t)(int16_t)d; break;
        case GTSL_Level: if (t->kind == SLIDER_KIND) t->value = (int32_t)(int16_t)d; break;
        case GTSL_LevelFormat: if (d) str_get(d, t->fmt, sizeof t->fmt); break;
        case GTSL_MaxLevelLen: t->maxlevellen = (int)d; break;
        case GTSL_LevelPlace: t->levelplace = (int)d; break;
        case PGA_Freedom_: t->vert = d == LORIENT_VERT; break;
        case GTSC_Top: if (t->kind == SCROLLER_KIND) t->value = (int32_t)d; break;
        case GTSC_Total: t->total = (int32_t)d; break;
        case GTSC_Visible: t->visible = (int32_t)d; break;
        case GTSC_Arrows: t->arrows = (int)d; break;
        case GTLV_Labels: if (t->kind == LISTVIEW_KIND) t->labels = d; break;
        case GTLV_Top: t->top = (int)d; break;
        case GTLV_MakeVisible: {
            int v = (int)d;
            if (v < t->top) t->top = v;
            else if (t->visible && v >= t->top + t->visible) t->top = v - t->visible + 1;
            break;
        }
        case GTLV_Selected: if (t->kind == LISTVIEW_KIND) t->value = (int32_t)d; break;
        case GTLV_ShowSelected: t->showsel = 1; break;
        case GTLV_ReadOnly: t->readonly = d != 0; break;
        case GTLV_ItemHeight: if (d) t->itemh = (int)d; break;
        case GTTX_Text:
            t->text = d;
            if (t->copytext) { if (d) str_get(d, t->tbuf, sizeof t->tbuf); else t->tbuf[0] = 0; }
            break;
        case GTTX_CopyText: if (creating) t->copytext = d != 0; break;
        case GTTX_Border: case GTNM_Border: t->border = d != 0; break;
        case GTTX_FrontPen: t->frontpen = (int)d; break;
        case GTNM_Number: if (t->kind == NUMBER_KIND) t->value = (int32_t)d; break;
        case GTNM_Format: if (d) str_get(d, t->fmt, sizeof t->fmt); break;
        case GTPA_Depth: t->depth = (int)d; t->ncolors = 1 << t->depth; break;
        case GTPA_NumColors: t->ncolors = (int)d; break;
        case GTPA_Color: if (t->kind == PALETTE_KIND) t->value = (int32_t)d; break;
        case GTPA_ColorOffset: t->coloroffset = (int)d; break;
        case GTPA_IndicatorWidth: t->indw = (int)d; break;
        case GTPA_IndicatorHeight: t->indh = (int)d; break;
        case GTPA_ColorTable: t->colortable = d; break;
        }
    }
    if (t->kind == SLIDER_KIND) { if (t->value < t->min) t->value = t->min; if (t->value > t->max) t->value = t->max; }
    if (t->kind == SCROLLER_KIND) { int r = prop_range(t); if (t->value > r) t->value = r; if (t->value < 0) t->value = 0; }
    if ((t->kind == CYCLE_KIND || t->kind == MX_KIND) && t->nlabels && t->value >= t->nlabels) t->value = t->nlabels - 1;
}

/* the ridge of a string gadget, drawn by intuition from a Border chain */
static uint32_t make_ridge(uint32_t mem, const uint16_t *pens, int w, int h) {
    /* four L-shaped borders: outer light/dark, inner dark/light */
    static const int sh[4][2] = { { SHINEPEN, 0 }, { SHADOWPEN, 1 }, { SHADOWPEN, 2 }, { SHINEPEN, 3 } };
    uint32_t xy = mem + 4 * Border_SIZEOF;
    for (int k = 0; k < 4; k++) {
        uint32_t b = mem + (uint32_t)k * Border_SIZEOF, p = xy + (uint32_t)k * 12;
        int inset = k >= 2, x0 = -6 + 2 * inset, y0 = -3 + inset, x1 = -6 + w - 1 - 2 * inset, y1 = -3 + h - 1 - inset;
        int16_t pts[6];
        if (sh[k][1] == 0 || sh[k][1] == 2) { pts[0] = (int16_t)x0; pts[1] = (int16_t)y1; pts[2] = (int16_t)x0; pts[3] = (int16_t)y0; pts[4] = (int16_t)x1; pts[5] = (int16_t)y0; }
        else { pts[0] = (int16_t)(x0 + 1); pts[1] = (int16_t)y1; pts[2] = (int16_t)x1; pts[3] = (int16_t)y1; pts[4] = (int16_t)x1; pts[5] = (int16_t)y0; }
        for (int i = 0; i < 6; i++) wr16(p + 2 * (uint32_t)i, (uint16_t)pts[i]);
        wr8(b + Border_FrontPen, pens[sh[k][0]]);
        wr8(b + Border_DrawMode, JAM1);
        wr8(b + Border_Count, 3);
        wr32(b + Border_XY, p);
        wr32(b + Border_NextBorder, k < 3 ? b + Border_SIZEOF : 0);
    }
    return mem;
}

static uint32_t make_itext(uint32_t it, uint32_t str, int pen, int left, int top) {
    wr8(it + IntuiText_FrontPen, (uint32_t)pen);
    wr8(it + IntuiText_DrawMode, JAM1);
    wr16(it + IntuiText_LeftEdge, (uint32_t)left);
    wr16(it + IntuiText_TopEdge, (uint32_t)top);
    wr32(it + IntuiText_IText, str);
    return it;
}

static uint32_t gt_CreateGadgetA(M68K *c) {
    int kind = (int)c->d[0];
    uint32_t prev = c->a[0], ng = c->a[1], tags = c->a[2];
    if (!prev || !ng || kind < 0 || kind >= NUM_KINDS || kind == 10) return 0;
    uint32_t vi = rd32(ng + NewGadget_ng_VisualInfo);
    if (tag_find(tags, GT_VisualInfo)) vi = tag_get(tags, GT_VisualInfo, vi);
    NScreen *s = vi_screen(vi);
    if (!s) { os_log("gadtools : VisualInfo invalide"); return 0; }
    int x = (int16_t)rd16(ng + NewGadget_ng_LeftEdge), y = (int16_t)rd16(ng + NewGadget_ng_TopEdge);
    int w = (int16_t)rd16(ng + NewGadget_ng_Width), h = (int16_t)rd16(ng + NewGadget_ng_Height);
    uint32_t flags = rd32(ng + NewGadget_ng_Flags), gtext = rd32(ng + NewGadget_ng_GadgetText);

    GT *t = calloc(1, sizeof *t);
    t->kind = kind;
    t->vi = vi;
    t->frontpen = -1;
    t->max = 15;
    t->visible = kind == SCROLLER_KIND ? 2 : 0;
    t->spacing = 1;
    t->itemh = 8;
    t->value = kind == LISTVIEW_KIND ? -1 : kind == PALETTE_KIND ? 1 : 0;
    t->depth = 1; t->ncolors = 2;
    t->maxlevellen = 2;
    t->levelplace = PLACETEXT_LEFT;
    t->place = flags & 0x1F;
    t->high = (flags & NG_HIGHLABEL) != 0;
    if (!t->place) t->place = kind == BUTTON_KIND ? PLACETEXT_IN : kind == LISTVIEW_KIND ? PLACETEXT_ABOVE : PLACETEXT_LEFT;
    int us = (int)tag_get(tags, GT_Underscore, 0);
    if (gtext) { char raw[80]; str_get(gtext, raw, sizeof raw); strip_label(raw, t->label, sizeof t->label, &t->ul, us); }
    else t->ul = -1;

    uint32_t g;
    if (kind == STRING_KIND || kind == INTEGER_KIND || kind == GENERIC_KIND) {
        int maxc = (int)tag_get(tags, kind == STRING_KIND ? GTST_MaxChars : GTIN_MaxChars, kind == STRING_KIND ? 64 : 10);
        if (maxc < 1) maxc = 1;
        uint32_t size = Gadget_SIZEOF + StringInfo_SIZEOF + 2 * IntuiText_SIZEOF + 4 * Border_SIZEOF + 48 + 2 * (uint32_t)(maxc + 2) + 96;
        uint32_t blk = exec_allocvec(size, MEMF_PUBLIC | MEMF_CLEAR);
        g = blk;
        t->block = blk;
        uint32_t si = blk + Gadget_SIZEOF, itx = si + StringInfo_SIZEOF, brd = itx + 2 * IntuiText_SIZEOF;
        uint32_t buf = brd + 4 * Border_SIZEOF + 48, undo = buf + (uint32_t)maxc + 2, lab = undo + (uint32_t)maxc + 2;
        for (int i = 0; t->label[i] && i < 95; i++) wr8(lab + (uint32_t)i, (uint8_t)t->label[i]);
        int tw = (int)strlen(t->label) * 8, lx, ly;
        if (kind == GENERIC_KIND) {
            wr16(g + Gadget_LeftEdge, (uint32_t)x); wr16(g + Gadget_LeftEdge + 2, (uint32_t)y);
            wr16(g + Gadget_Width, (uint32_t)w); wr16(g + Gadget_Width + 2, (uint32_t)h);
            wr16(g + Gadget_Flags, GFLG_GADGHCOMP);
            wr16(g + Gadget_Activation, GACT_RELVERIFY);
            wr16(g + Gadget_GadgetType, GTYP_BOOLGADGET);
            label_pos(t, 0, 0, w, h, tw, &lx, &ly);
            if (t->label[0]) wr32(g + Gadget_GadgetText, make_itext(itx, lab, s->pens[t->high ? HIGHLIGHTTEXTPEN : TEXTPEN], lx, ly));
        } else {
            int ix = x + 6, iy = y + 3, iw = w - 12, ih = h - 6;
            wr16(g + Gadget_LeftEdge, (uint32_t)ix); wr16(g + Gadget_LeftEdge + 2, (uint32_t)iy);
            wr16(g + Gadget_Width, (uint32_t)iw); wr16(g + Gadget_Width + 2, (uint32_t)ih);
            wr16(g + Gadget_Flags, GFLG_GADGHCOMP);
            wr16(g + Gadget_Activation, GACT_RELVERIFY | (kind == INTEGER_KIND ? GACT_LONGINT : 0));
            wr16(g + Gadget_GadgetType, GTYP_STRGADGET);
            wr32(g + Gadget_SpecialInfo, si);
            wr32(si + StringInfo_Buffer, buf);
            wr32(si + StringInfo_UndoBuffer, undo);
            wr16(si + StringInfo_MaxChars, (uint32_t)maxc + 1);
            wr32(g + Gadget_GadgetRender, make_ridge(brd, s->pens, w, h));
            label_pos(t, -6, -3, w, h, tw, &lx, &ly);
            if (t->label[0]) wr32(g + Gadget_GadgetText, make_itext(itx, lab, s->pens[t->high ? HIGHLIGHTTEXTPEN : TEXTPEN], lx, ly));
            char init[512] = "";
            if (kind == STRING_KIND) { uint32_t sp = tag_get(tags, GTST_String, 0); if (sp) str_get(sp, init, sizeof init); }
            else { int32_t v = (int32_t)tag_get(tags, GTIN_Number, 0); snprintf(init, sizeof init, "%d", (int)v); wr32(si + StringInfo_LongInt, (uint32_t)v); }
            int n = (int)strlen(init);
            if (n > maxc) n = maxc;
            for (int i = 0; i < n; i++) wr8(buf + (uint32_t)i, (uint8_t)init[i]);
            wr16(si + StringInfo_NumChars, (uint32_t)n);
        }
        if (tag_get(tags, GA_Disabled_, 0)) wr16(g + Gadget_Flags, rd16(g + Gadget_Flags) | GFLG_DISABLED);
    } else {
        g = bo_new(c, gt_class, 0);
        if (!g) { free(t); return 0; }
        wr16(g + Gadget_LeftEdge, (uint32_t)x); wr16(g + Gadget_LeftEdge + 2, (uint32_t)y);
        wr16(g + Gadget_Width, (uint32_t)w); wr16(g + Gadget_Width + 2, (uint32_t)h);
        wr16(g + Gadget_Flags, GFLG_GADGHNONE);
        wr16(g + Gadget_Activation, GACT_RELVERIFY);
        t->g = g;
        apply_tags(t, tags, 1);
        if (kind == MX_KIND) {
            t->bw = w > 0 ? w : 17;
            t->bh = h > 0 ? h : 9;
            int n = t->nlabels ? t->nlabels : 1;
            wr16(g + Gadget_Width, (uint32_t)t->bw);
            wr16(g + Gadget_Width + 2, (uint32_t)(n * (t->bh + t->spacing) - t->spacing));
        }
        if (kind == SLIDER_KIND && !tag_find(tags, GTSL_MaxLevelLen)) {
            char a[32], b[32];
            fmt_long(t->fmt[0] ? t->fmt : "%ld", t->min, a, sizeof a);
            fmt_long(t->fmt[0] ? t->fmt : "%ld", t->max, b, sizeof b);
            t->maxlevellen = (int)(strlen(a) > strlen(b) ? strlen(a) : strlen(b));
        }
    }
    wr16(g + Gadget_GadgetID, rd16(ng + NewGadget_ng_GadgetID));
    wr32(g + Gadget_UserData, rd32(ng + NewGadget_ng_UserData));
    t->g = g;
    t->next = gts;
    gts = t;
    /* append after prev */
    wr32(g + Gadget_NextGadget, rd32(prev + Gadget_NextGadget));
    wr32(prev + Gadget_NextGadget, g);
    return g;
}

static uint32_t gt_CreateContext(M68K *c) {
    uint32_t pp = c->a[0];
    uint32_t g = exec_allocvec(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr16(g + Gadget_Flags, GFLG_GADGHNONE);
    wr16(g + Gadget_GadgetType, GTYP_BOOLGADGET);
    GT *t = calloc(1, sizeof *t);
    t->g = g;
    t->kind = -1;
    t->block = g;
    t->next = gts;
    gts = t;
    if (pp) wr32(pp, g);
    return g;
}

static void free_gt(M68K *c, GT *t) {
    GT **pp = &gts;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    if (t->block) exec_freevec(t->block);
    else {
        uint32_t g = t->g;
        free(t);
        uint32_t m[1] = { OM_DISPOSE };
        bo_domethod(c, g, 1, m);
        return;
    }
    free(t);
}

static uint32_t gt_FreeGadgets(M68K *c) {
    uint32_t g = c->a[0];
    while (g) {
        uint32_t next = rd32(g + Gadget_NextGadget);
        GT *t = gt_of(g);
        if (t) free_gt(c, t);
        g = next;
    }
    return 0;
}

static uint32_t gt_SetGadgetAttrsA(M68K *c) {
    uint32_t g = c->a[0], win = c->a[1], tags = c->a[3];
    GT *t = gt_of(g);
    if (!t) return 0;
    NWindow *w = win ? nwin_of(win) : 0;
    if (t->kind == STRING_KIND || t->kind == INTEGER_KIND) {
        uint32_t si = rd32(g + Gadget_SpecialInfo), buf = rd32(si + StringInfo_Buffer);
        int maxc = (int)rd16(si + StringInfo_MaxChars) - 1;
        char s[512] = "";
        int set = 0;
        uint32_t ti;
        if ((ti = tag_find(tags, GTST_String)) && t->kind == STRING_KIND) { if (rd32(ti + 4)) str_get(rd32(ti + 4), s, sizeof s); set = 1; }
        if ((ti = tag_find(tags, GTIN_Number)) && t->kind == INTEGER_KIND) {
            snprintf(s, sizeof s, "%d", (int)(int32_t)rd32(ti + 4));
            wr32(si + StringInfo_LongInt, rd32(ti + 4));
            set = 1;
        }
        if (set) {
            int n = (int)strlen(s);
            if (n > maxc) n = maxc;
            for (int i = 0; i < n; i++) wr8(buf + (uint32_t)i, (uint8_t)s[i]);
            wr8(buf + (uint32_t)n, 0);
            wr16(si + StringInfo_NumChars, (uint32_t)n);
            wr16(si + StringInfo_BufferPos, 0);
            wr16(si + StringInfo_DispPos, 0);
        }
        if ((ti = tag_find(tags, GA_Disabled_))) {
            uint32_t f = rd16(g + Gadget_Flags);
            wr16(g + Gadget_Flags, rd32(ti + 4) ? f | GFLG_DISABLED : f & ~GFLG_DISABLED);
        }
    } else if (t->kind >= 0) apply_tags(t, tags, 0);
    if (w) gadget_render_all(w, g, 1);
    return 0;
}

static uint32_t gt_GetGadgetAttrsA(M68K *c) {
    uint32_t g = c->a[0], tags = c->a[3];
    GT *t = gt_of(g);
    if (!t) return 0;
    uint32_t st = tags, ti, n = 0;
    uint32_t si = (t->kind == STRING_KIND || t->kind == INTEGER_KIND) ? rd32(g + Gadget_SpecialInfo) : 0;
    while ((ti = tag_next(&st))) {
        uint32_t dst = rd32(ti + 4), v;
        switch (rd32(ti)) {
        case GA_Disabled_: v = (rd16(g + Gadget_Flags) & GFLG_DISABLED) != 0; break;
        case GTCB_Checked: case GTMX_Active: case GTCY_Active: case GTSL_Level: case GTSC_Top:
        case GTLV_Selected: case GTNM_Number: case GTPA_Color: v = (uint32_t)t->value; break;
        case GTCY_Labels: case GTMX_Labels: case GTLV_Labels: v = t->labels; break;
        case GTSL_Min: v = (uint32_t)t->min; break;
        case GTSL_Max: v = (uint32_t)t->max; break;
        case GTSC_Total: v = (uint32_t)t->total; break;
        case GTSC_Visible: v = (uint32_t)t->visible; break;
        case GTLV_Top: v = (uint32_t)t->top; break;
        case GTTX_Text: v = t->text; break;
        case GTST_String: if (!si) continue; v = rd32(si + StringInfo_Buffer); break;
        case GTIN_Number: if (!si) continue; v = rd32(si + StringInfo_LongInt); break;
        default: continue;
        }
        if (dst) wr32(dst, v);
        n++;
    }
    return n;
}

/* ---- messages and refresh --------------------------------------------------------- */

static uint32_t gt_GetIMsg(M68K *c) { return list_remhead(c->a[0] + MP_MSGLIST); }
static uint32_t gt_ReplyIMsg(M68K *c) { if (c->a[1]) exec_reply_msg(c->a[1]); return 0; }
static uint32_t gt_FilterIMsg(M68K *c) { return c->a[1]; }
static uint32_t gt_PostFilterIMsg(M68K *c) { return c->a[1]; }
static uint32_t gt_RefreshWindow(M68K *c) {
    NWindow *w = c->a[0] ? nwin_of(c->a[0]) : 0;
    if (w) gadget_render_all(w, rd32(w->win + Window_FirstGadget), -1);
    return 0;
}
static uint32_t gt_BeginRefresh(M68K *c) { (void)c; return 0; }
static uint32_t gt_EndRefresh(M68K *c) { (void)c; return 0; }

/* ---- visual info, bevel boxes ------------------------------------------------------- */

static uint32_t gt_GetVisualInfoA(M68K *c) {
    NScreen *s = c->a[0] ? nscr_of(c->a[0]) : 0;
    if (!s) return 0;
    uint32_t vi = exec_allocvec(16, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(vi, s->scr);
    wr32(vi + 4, s->drawinfo);
    wr32(vi + 8, 0x47545649);                                  /* 'GTVI' */
    return vi;
}
static uint32_t gt_FreeVisualInfo(M68K *c) { if (c->a[0]) exec_freevec(c->a[0]); return 0; }

static uint32_t gt_DrawBevelBoxA(M68K *c) {
    uint32_t rp = c->a[0], tags = c->a[1];
    int x = (int16_t)c->d[0], y = (int16_t)c->d[1], w = (int16_t)c->d[2], h = (int16_t)c->d[3];
    NScreen *s = vi_screen(tag_get(tags, GT_VisualInfo, 0));
    if (!s) s = workbench_screen(0);
    if (!s || !rp) return 0;
    int rec = tag_get(tags, GTBB_Recessed, 0) != 0, ft = (int)tag_get(tags, GTBB_FrameType, BBFT_BUTTON);
    if (ft == BBFT_RIDGE) ridge(rp, s->pens, x, y, w, h);
    else if (ft == BBFT_ICONDROPBOX) { bevel(rp, s->pens, x, y, w, h, rec); bevel(rp, s->pens, x + 4, y + 2, w - 8, h - 4, !rec); }
    else bevel(rp, s->pens, x, y, w, h, rec);
    gfx_touch();
    return 0;
}

/* ---- menus ------------------------------------------------------------------------- */

#define MENU_BLK (Menu_SIZEOF + 4)                        /* + GTMENU_USERDATA */
#define ITEM_BLK (MenuItem_SIZEOF + 4 + 2 * IntuiText_SIZEOF + Image_SIZEOF)

static uint32_t sub_arrow;                                /* "»" */

static uint32_t gt_CreateMenusA(M68K *c) {
    uint32_t nm = c->a[0], tags = c->a[1];
    int pen = (int)tag_get(tags, GTMN_FrontPen, 1);
    uint32_t err = tag_get(tags, GT(63) /* GTMN_SecondaryError */, 0);
    if (err) wr32(err, 0);
    uint32_t size = 0;
    int n = 0;
    for (uint32_t e = nm; rd8(e + NewMenu_nm_Type) != 0; e += NewMenu_SIZEOF, n++) {
        int type = rd8(e + NewMenu_nm_Type);
        if (type & NM_IGNORE) continue;
        size += (type & 0x7F) == NM_TITLE ? MENU_BLK : ITEM_BLK;
        if (n > 4096) return 0;
    }
    if (!size) return 0;
    uint32_t blk = exec_allocvec(size, MEMF_PUBLIC | MEMF_CLEAR), p = blk;
    uint32_t menu = 0, lastmenu = 0, lastitem = 0, lastsub = 0, item = 0;
    for (uint32_t e = nm; rd8(e + NewMenu_nm_Type) != 0; e += NewMenu_SIZEOF) {
        int type = rd8(e + NewMenu_nm_Type);
        if (type & NM_IGNORE) continue;
        int image = (type & MENU_IMAGE) != 0;
        type &= 0x7F;
        uint32_t label = rd32(e + NewMenu_nm_Label), key = rd32(e + NewMenu_nm_CommKey);
        uint32_t fl = rd16(e + NewMenu_nm_Flags);
        if (type == NM_TITLE) {
            menu = p; p += MENU_BLK;
            wr32(menu + Menu_MenuName, label);
            wr16(menu + Menu_Flags, (fl & MENUENABLED_) ? 0 : MENUENABLED_);   /* NM_MENUDISABLED */
            wr32(menu + Menu_SIZEOF, rd32(e + NewMenu_nm_UserData));
            if (lastmenu) wr32(lastmenu + Menu_NextMenu, menu);
            lastmenu = menu;
            lastitem = 0;
            item = 0;
            continue;
        }
        if (!menu || (type == NM_SUB && !item)) {
            if (err) wr32(err, 1);
            exec_freevec(blk);
            return 0;
        }
        uint32_t it = p;
        p += ITEM_BLK;
        uint32_t itx = it + MenuItem_SIZEOF + 4, img = itx + 2 * IntuiText_SIZEOF;
        uint32_t f = HIGHCOMP_ | ((fl & ITEMENABLED_) ? 0 : ITEMENABLED_) | (fl & (CHECKIT_ | CHECKED_ | MENUTOGGLE_));
        if (label == NM_BARLABEL) {
            f = HIGHNONE_;
            wr16(img + Image_Width, 8);
            wr16(img + Image_Width + 2, 1);
            wr16(img + Image_LeftEdge + 2, 2);
            wr8(img + Image_PlaneOnOff, (uint32_t)pen);
            wr32(it + MenuItem_ItemFill, img);
        } else if (image) {
            wr32(it + MenuItem_ItemFill, label);
        } else {
            f |= ITEMTEXT_;
            make_itext(itx, label, pen, 0, 1);
            wr32(it + MenuItem_ItemFill, itx);
        }
        if (key && !(fl & NM_COMMANDSTRING) && label != NM_BARLABEL) { f |= COMMSEQ_; wr8(it + MenuItem_Command, rd8(key)); }
        wr16(it + MenuItem_Flags, f);
        wr32(it + MenuItem_MutualExclude, rd32(e + NewMenu_nm_MutualExclude));
        wr32(it + MenuItem_SIZEOF, rd32(e + NewMenu_nm_UserData));
        if (type == NM_ITEM) {
            if (lastitem) wr32(lastitem + MenuItem_NextItem, it); else wr32(menu + Menu_FirstItem, it);
            lastitem = item = it;
            lastsub = 0;
        } else {
            if (lastsub) wr32(lastsub + MenuItem_NextItem, it); else wr32(item + MenuItem_SubItem, it);
            lastsub = it;
        }
    }
    return blk;
}

static uint32_t gt_FreeMenus(M68K *c) { if (c->a[0]) exec_freevec(c->a[0]); return 0; }

static int item_text_width(uint32_t it) {
    uint32_t fl = rd16(it + MenuItem_Flags), fill = rd32(it + MenuItem_ItemFill);
    if (!fill) return 0;
    if (fl & ITEMTEXT_) {
        char b[128];
        return rd32(fill + IntuiText_IText) ? str_get(rd32(fill + IntuiText_IText), b, sizeof b) * 8 : 0;
    }
    if (rd16(fill + Image_Width + 2) <= 2) return 0;          /* a bar */
    return (int16_t)rd16(fill + Image_Width);
}
static int item_height(uint32_t it) {
    uint32_t fl = rd16(it + MenuItem_Flags), fill = rd32(it + MenuItem_ItemFill);
    if (fl & ITEMTEXT_) return 10;
    if (!fill) return 10;
    int h = (int16_t)rd16(fill + Image_Width + 2);
    return h <= 2 ? 6 : h + 2;
}

/* lay out a chain of items; returns its width */
static int layout_items(uint32_t first, int minw, int left, int screenw, int basex) {
    int check = 0, comm = 0, tw = 0, sub = 0;
    for (uint32_t it = first; it; it = rd32(it + MenuItem_NextItem)) {
        uint32_t fl = rd16(it + MenuItem_Flags);
        if (fl & CHECKIT_) check = 1;
        if (fl & COMMSEQ_) comm = 1;
        if (rd32(it + MenuItem_SubItem)) sub = 1;
        int w = item_text_width(it);
        if (w > tw) tw = w;
    }
    int cw = check ? 12 : 0;
    int width = cw + tw + 4 + (comm ? 28 : 0) + (sub ? 16 : 0);
    if (width < minw) width = minw;
    if (basex + left + width + 4 > screenw) left = screenw - width - 4 - basex;
    int y = 0;
    for (uint32_t it = first; it; it = rd32(it + MenuItem_NextItem)) {
        uint32_t fl = rd16(it + MenuItem_Flags), fill = rd32(it + MenuItem_ItemFill);
        int h = item_height(it);
        wr16(it + MenuItem_LeftEdge, (uint32_t)left);
        wr16(it + MenuItem_LeftEdge + 2, (uint32_t)y);
        wr16(it + MenuItem_Width, (uint32_t)width);
        wr16(it + MenuItem_Width + 2, (uint32_t)h);
        if (fl & ITEMTEXT_) {
            wr16(fill + IntuiText_LeftEdge, (uint32_t)cw);
            if (rd32(it + MenuItem_SubItem)) {
                uint32_t arrow = fill + IntuiText_SIZEOF;
                make_itext(arrow, sub_arrow, rd8(fill + IntuiText_FrontPen), width - 10, 1);
                wr32(fill + IntuiText_NextText, arrow);
            }
        } else if (fill && h == 6) {
            wr16(fill + Image_Width, (uint32_t)width);
        }
        if (rd32(it + MenuItem_SubItem)) layout_items(rd32(it + MenuItem_SubItem), 0, width - 4, screenw, basex + left);
        y += h;
    }
    return width;
}

static uint32_t gt_LayoutMenusA(M68K *c) {
    uint32_t menu = c->a[0];
    NScreen *s = vi_screen(c->a[1]);
    if (!s) return 0;
    int x = 4, bh = rd8(s->scr + Screen_BarHeight);
    char b[128];
    for (uint32_t m = menu; m; m = rd32(m + Menu_NextMenu)) {
        int tw = rd32(m + Menu_MenuName) ? str_get(rd32(m + Menu_MenuName), b, sizeof b) * 8 : 0;
        wr16(m + Menu_LeftEdge, (uint32_t)x);
        wr16(m + Menu_LeftEdge + 2, 0);
        wr16(m + Menu_Width, (uint32_t)(tw + 8));
        wr16(m + Menu_Width + 2, (uint32_t)bh);
        layout_items(rd32(m + Menu_FirstItem), tw + 8, 0, s->w, x);
        x += tw + 16;
    }
    return DOSTRUE;
}

static uint32_t gt_LayoutMenuItemsA(M68K *c) {
    NScreen *s = vi_screen(c->a[1]);
    uint32_t m = tag_get(c->a[2], GTMN_Menu, 0);
    int basex = m ? (int16_t)rd16(m + Menu_LeftEdge) : 0;
    layout_items(c->a[0], 0, 0, s ? s->w : 640, basex);
    return DOSTRUE;
}

static uint32_t gt_SetDesignFontA(M68K *c) { (void)c; return 0; }
static uint32_t gt_ScaleGadgetRectA(M68K *c) { (void)c; return 0; }

static const NativeEntry gt_natives[] = {
    { "CreateGadgetA", gt_CreateGadgetA }, { "FreeGadgets", gt_FreeGadgets },
    { "GT_SetGadgetAttrsA", gt_SetGadgetAttrsA }, { "CreateMenusA", gt_CreateMenusA },
    { "FreeMenus", gt_FreeMenus }, { "LayoutMenuItemsA", gt_LayoutMenuItemsA },
    { "LayoutMenusA", gt_LayoutMenusA }, { "GT_GetIMsg", gt_GetIMsg }, { "GT_ReplyIMsg", gt_ReplyIMsg },
    { "GT_RefreshWindow", gt_RefreshWindow }, { "GT_BeginRefresh", gt_BeginRefresh },
    { "GT_EndRefresh", gt_EndRefresh }, { "GT_FilterIMsg", gt_FilterIMsg },
    { "GT_PostFilterIMsg", gt_PostFilterIMsg }, { "CreateContext", gt_CreateContext },
    { "DrawBevelBoxA", gt_DrawBevelBoxA }, { "GetVisualInfoA", gt_GetVisualInfoA },
    { "FreeVisualInfo", gt_FreeVisualInfo }, { "SetDesignFontA", gt_SetDesignFontA },
    { "ScaleGadgetRectA", gt_ScaleGadgetRectA }, { "GT_GetGadgetAttrsA", gt_GetGadgetAttrsA },
    { 0, 0 }
};

void gadtools_init(void) {
    gt_class = bo_native_class("_gadtools", "gadgetclass", 0, gt_dispatch);
    sub_arrow = str_alloc("\xbb");
    GadToolsBase = lib_create("gadtools.library", "gadtools", 64, gt_natives, 40, 1, "gadtools 40.1 (AmigaMac)");
    lib_add(GadToolsBase);
}
