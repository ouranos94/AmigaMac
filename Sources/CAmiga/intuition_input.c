/* intuition, second half: the input.device task (mouse, keys, window
 * dragging/sizing/depth), gadgets (boolean, string, proportional), menus,
 * requesters, and the rendering of Borders, Images and IntuiTexts.
 */
#include "os.h"
#include "intui.h"
#include "console.h"
#include "font8_extern.h"
#define CPU (&sched_current()->cpu)
#define GTYP_CUSTOM 5
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- rendering of intuition structures ------------------------------------- */

void draw_border(uint32_t rp, uint32_t b, int left, int top) {
    for (; b; b = rd32(b + Border_NextBorder)) {
        int bx = left + (int16_t)rd16(b + Border_LeftEdge), by = top + (int16_t)rd16(b + Border_LeftEdge + 2);
        int n = rd8(b + Border_Count);
        uint32_t xy = rd32(b + Border_XY);
        uint32_t ofg = rd8(rp + RastPort_FgPen), omode = rd8(rp + RastPort_DrawMode);
        wr8(rp + RastPort_FgPen, rd8(b + Border_FrontPen));
        wr8(rp + RastPort_DrawMode, rd8(b + Border_DrawMode));
        if (n > 0 && xy) {
            rp_move(rp, bx + (int16_t)rd16(xy), by + (int16_t)rd16(xy + 2));
            for (int i = 1; i < n; i++) rp_draw(rp, bx + (int16_t)rd16(xy + 4 * (uint32_t)i), by + (int16_t)rd16(xy + 4 * (uint32_t)i + 2));
            if (n == 1) rp_draw(rp, bx + (int16_t)rd16(xy), by + (int16_t)rd16(xy + 2));
        }
        wr8(rp + RastPort_FgPen, ofg);
        wr8(rp + RastPort_DrawMode, omode);
    }
}

void draw_image(uint32_t rp, uint32_t img, int left, int top, int state) {
    (void)state;
    BM b;
    if (!bm_load(rd32(rp + RastPort_BitMap), &b)) return;
    for (; img; img = rd32(img + Image_NextImage)) {
        if (rd16(img + Image_Depth) == 0xFFFF) {                /* a BOOPSI image */
            bo_draw_image(CPU, rp, img, left, top, state, 0);
            continue;
        }
        int x0 = left + (int16_t)rd16(img + Image_LeftEdge), y0 = top + (int16_t)rd16(img + Image_LeftEdge + 2);
        int w = (int16_t)rd16(img + Image_Width), h = (int16_t)rd16(img + Image_Width + 2), depth = (int16_t)rd16(img + Image_Depth);
        uint32_t data = rd32(img + Image_ImageData);
        int pick = rd8(img + Image_PlanePick), onoff = rd8(img + Image_PlaneOnOff);
        if (w <= 0 || h <= 0) continue;
        int words = (w + 15) >> 4, plane_size = words * 2 * h;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int pen = 0, k = 0;
                for (int p = 0; p < b.depth; p++) {
                    if (pick & (1 << p)) {
                        if (data && k < depth) {
                            uint32_t a = data + (uint32_t)(k * plane_size + y * words * 2 + (x >> 3));
                            if ((rd8(a) >> (7 - (x & 7))) & 1) pen |= 1 << p;
                        }
                        k++;
                    } else if (onoff & (1 << p)) pen |= 1 << p;
                }
                bm_pixel(&b, x0 + x, y0 + y, pen, JAM1, 0xFF);
            }
    }
    gfx_touch();
}

void draw_itext(uint32_t rp, uint32_t it, int left, int top) {
    for (; it; it = rd32(it + IntuiText_NextText)) {
        char buf[256];
        int n = str_get(rd32(it + IntuiText_IText), buf, sizeof buf);
        uint32_t ofg = rd8(rp + RastPort_FgPen), obg = rd8(rp + RastPort_BgPen), omode = rd8(rp + RastPort_DrawMode);
        wr8(rp + RastPort_FgPen, rd8(it + IntuiText_FrontPen));
        wr8(rp + RastPort_BgPen, rd8(it + IntuiText_BackPen));
        wr8(rp + RastPort_DrawMode, rd8(it + IntuiText_DrawMode));
        uint32_t font = rd32(rp + RastPort_Font);
        int base = font ? rd16(font + TextFont_tf_Baseline) : 6;
        rp_move(rp, left + (int16_t)rd16(it + IntuiText_LeftEdge), top + (int16_t)rd16(it + IntuiText_TopEdge) + base);
        rp_text(rp, buf, n);
        wr8(rp + RastPort_FgPen, ofg);
        wr8(rp + RastPort_BgPen, obg);
        wr8(rp + RastPort_DrawMode, omode);
    }
}

int itext_length(uint32_t it) {
    char buf[256];
    return str_get(rd32(it + IntuiText_IText), buf, sizeof buf) * 8;
}

/* ---- gadgets ----------------------------------------------------------------- */

/* ---- old-style requesters (Request/EndRequest) -----------------------------------
 * Requester: OlderRequest 0, LeftEdge 4, TopEdge 6, Width 8, Height 10, RelLeft 12,
 * RelTop 14, ReqGadget 16, ReqBorder 20, ReqText 24, Flags 28, BackFill 30, RWindow 72 */
#define RQ_GADGET 16
#define GTYP_REQGADGET 0x1000

/* the active requester whose gadget g is, or 0 */
static uint32_t req_of_gadget(NWindow *w, uint32_t g) {
    for (uint32_t r = rd32(w->win + Window_FirstRequest); r; r = rd32(r))
        for (uint32_t q = rd32(r + RQ_GADGET); q; q = rd32(q + Gadget_NextGadget))
            if (q == g) return r;
    return 0;
}

void gadget_box(NWindow *w, uint32_t g, int *x, int *y, int *gw, int *gh) {
    uint32_t fl = rd16(g + Gadget_Flags);
    int bx = 0, by = 0, bw = w->w, bh = w->h;
    if (rd16(g + Gadget_GadgetType) & GTYP_REQGADGET) {
        uint32_t r = req_of_gadget(w, g);
        if (r) { bx = (int16_t)rd16(r + 4); by = (int16_t)rd16(r + 6); bw = (int16_t)rd16(r + 8); bh = (int16_t)rd16(r + 10); }
    }
    *x = bx + (int16_t)rd16(g + Gadget_LeftEdge);
    *y = by + (int16_t)rd16(g + Gadget_LeftEdge + 2);
    *gw = (int16_t)rd16(g + Gadget_Width);
    *gh = (int16_t)rd16(g + Gadget_Width + 2);
    if (fl & GFLG_RELRIGHT) *x += bw - 1;
    if (fl & GFLG_RELBOTTOM) *y += bh - 1;
    if (fl & GFLG_RELWIDTH) *gw += bw;
    if (fl & GFLG_RELHEIGHT) *gh += bh;
}

static void ghost(NWindow *w, int x, int y, int gw, int gh) {
    BM b;
    bm_load(w->bm, &b);
    int pen = w->s->pens[BLOCKPEN];
    for (int yy = y; yy < y + gh; yy++)
        for (int xx = x + (yy & 1); xx < x + gw; xx += 2) bm_pixel(&b, xx, yy, pen, JAM1, 0xFF);
}

/* PropInfo: Flags, HorizPot, VertPot, HorizBody, VertBody, then private CWidth... */
#define PI_FREEHORIZ 0x2
#define PI_FREEVERT 0x4
#define PI_AUTOKNOB 0x1

static void prop_knob(NWindow *w, uint32_t g, int *kx, int *ky, int *kw, int *kh) {
    int x, y, gw, gh;
    gadget_box(w, g, &x, &y, &gw, &gh);
    uint32_t pi = rd32(g + Gadget_SpecialInfo);
    int fl = rd16(pi + PropInfo_Flags);
    int hb = rd16(pi + PropInfo_HorizBody), vb = rd16(pi + PropInfo_VertBody);
    int hp = rd16(pi + PropInfo_HorizPot), vp = rd16(pi + PropInfo_VertPot);
    int cw = gw - 4, ch = gh - 4;
    *kw = (fl & PI_FREEHORIZ) ? (int)((long)cw * hb / 0xFFFF) : cw;
    *kh = (fl & PI_FREEVERT) ? (int)((long)ch * vb / 0xFFFF) : ch;
    if (*kw < 6) *kw = 6;
    if (*kh < 4) *kh = 4;
    if (*kw > cw) *kw = cw;
    if (*kh > ch) *kh = ch;
    *kx = x + 2 + ((fl & PI_FREEHORIZ) ? (int)((long)(cw - *kw) * hp / 0xFFFF) : 0);
    *ky = y + 2 + ((fl & PI_FREEVERT) ? (int)((long)(ch - *kh) * vp / 0xFFFF) : 0);
}

static uint32_t active_strgad;
static NWindow *active_strwin;

static void render_gadget(NWindow *w, uint32_t g) {
    uint32_t rp = w->rp, fl = rd16(g + Gadget_Flags);
    int type = rd16(g + Gadget_GadgetType);
    if (type & GTYP_SYSGADGET) return;                         /* drawn with the frame */
    int x, y, gw, gh;
    gadget_box(w, g, &x, &y, &gw, &gh);
    NScreen *s = w->s;
    int sel = (fl & GFLG_SELECTED) != 0;
    switch (type & GTYP_GTYPEMASK) {
    case GTYP_PROPGADGET: {
        uint32_t pi = rd32(g + Gadget_SpecialInfo);
        rp_fill(rp, x, y, x + gw - 1, y + gh - 1, s->pens[BACKGROUNDPEN], 0);
        int kx, ky, kw, kh;
        if (!pi) break;
        prop_knob(w, g, &kx, &ky, &kw, &kh);
        if (!(rd16(pi + PropInfo_Flags) & 0x8)) {              /* PROPBORDERLESS */
            BM b;
            bm_load(w->bm, &b);
            bm_fill(&b, x, y, x + gw - 1, y, s->pens[SHADOWPEN], JAM1, 0xFF);
            bm_fill(&b, x, y, x, y + gh - 1, s->pens[SHADOWPEN], JAM1, 0xFF);
            bm_fill(&b, x, y + gh - 1, x + gw - 1, y + gh - 1, s->pens[SHINEPEN], JAM1, 0xFF);
            bm_fill(&b, x + gw - 1, y, x + gw - 1, y + gh - 1, s->pens[SHINEPEN], JAM1, 0xFF);
        }
        if (rd16(pi + PropInfo_Flags) & PI_AUTOKNOB) {
            rp_fill(rp, kx, ky, kx + kw - 1, ky + kh - 1, sel ? s->pens[FILLPEN] : s->pens[BACKGROUNDPEN], 0);
            BM b;
            bm_load(w->bm, &b);
            bm_fill(&b, kx, ky, kx + kw - 1, ky, s->pens[SHINEPEN], JAM1, 0xFF);
            bm_fill(&b, kx, ky, kx, ky + kh - 1, s->pens[SHINEPEN], JAM1, 0xFF);
            bm_fill(&b, kx, ky + kh - 1, kx + kw - 1, ky + kh - 1, s->pens[SHADOWPEN], JAM1, 0xFF);
            bm_fill(&b, kx + kw - 1, ky, kx + kw - 1, ky + kh - 1, s->pens[SHADOWPEN], JAM1, 0xFF);
        } else if (rd32(g + Gadget_GadgetRender)) {
            draw_image(rp, rd32(g + Gadget_GadgetRender), kx, ky, 0);
        }
        break;
    }
    case GTYP_CUSTOM:
        bo_render(CPU, w, g, 1);
        break;
    case GTYP_STRGADGET: {
        uint32_t si = rd32(g + Gadget_SpecialInfo);
        rp_fill(rp, x, y, x + gw - 1, y + gh - 1, s->pens[BACKGROUNDPEN], 0);
        if (si) {
            char buf[512];
            int n = str_get(rd32(si + StringInfo_Buffer), buf, sizeof buf);
            int disp = (int16_t)rd16(si + StringInfo_DispPos), pos = (int16_t)rd16(si + StringInfo_BufferPos);
            int cols = gw / 8;
            if (pos < disp) disp = pos;
            if (pos >= disp + cols) disp = pos - cols + 1;
            if (disp < 0) disp = 0;
            wr16(si + StringInfo_DispPos, (uint32_t)disp);
            wr16(si + StringInfo_NumChars, (uint32_t)n);
            uint32_t ofg = rd8(rp + RastPort_FgPen), obg = rd8(rp + RastPort_BgPen), omode = rd8(rp + RastPort_DrawMode);
            wr8(rp + RastPort_FgPen, s->pens[TEXTPEN]);
            wr8(rp + RastPort_BgPen, s->pens[BACKGROUNDPEN]);
            wr8(rp + RastPort_DrawMode, JAM2);
            int shown = n - disp < cols ? n - disp : cols;
            if (shown > 0) { rp_move(rp, x, y + 6 + (gh - 8) / 2); rp_text(rp, buf + disp, shown); }
            wr8(rp + RastPort_FgPen, ofg);
            wr8(rp + RastPort_BgPen, obg);
            wr8(rp + RastPort_DrawMode, omode);
            if (active_strgad == g) {                          /* cursor */
                int cx = x + (pos - disp) * 8, cy = y + (gh - 8) / 2;
                rp_fill(rp, cx, cy, cx + 7, cy + 7, 0, 1);
            }
        }
        if (rd32(g + Gadget_GadgetRender)) draw_border(rp, rd32(g + Gadget_GadgetRender), x, y);
        if (rd32(g + Gadget_GadgetText)) draw_itext(rp, rd32(g + Gadget_GadgetText), x, y);
        break;
    }
    default: {                                                 /* boolean */
        uint32_t render = rd32(g + Gadget_GadgetRender), selr = rd32(g + Gadget_SelectRender);
        int hl = fl & GFLG_GADGHIGHBITS;
        if (fl & GFLG_GADGIMAGE) {
            uint32_t im = (sel && hl == GFLG_GADGHIMAGE && selr) ? selr : render;
            if (im) draw_image(rp, im, x, y, sel);
        } else {
            uint32_t bd = (sel && hl == GFLG_GADGHIMAGE && selr) ? selr : render;
            if (bd) draw_border(rp, bd, x, y);
        }
        if (rd32(g + Gadget_GadgetText)) draw_itext(rp, rd32(g + Gadget_GadgetText), x, y);
        if (sel && hl == GFLG_GADGHCOMP) rp_fill(rp, x, y, x + gw - 1, y + gh - 1, 0, 1);
        if (sel && hl == GFLG_GADGHBOX) {
            rp_fill(rp, x - 4, y - 2, x + gw + 3, y - 1, 0, 1);
            rp_fill(rp, x - 4, y + gh, x + gw + 3, y + gh + 1, 0, 1);
            rp_fill(rp, x - 4, y, x - 1, y + gh - 1, 0, 1);
            rp_fill(rp, x + gw, y, x + gw + 3, y + gh - 1, 0, 1);
        }
        break;
    }
    }
    if (fl & GFLG_DISABLED) ghost(w, x, y, gw, gh);
}

void gadget_render_all(NWindow *w, uint32_t g, int count) {
    for (; g && count != 0; g = rd32(g + Gadget_NextGadget), count--) render_gadget(w, g);
}

static uint32_t gadget_at(NWindow *w, int lx, int ly) {
    uint32_t req = rd32(w->win + Window_FirstRequest);
    for (uint32_t g = req ? rd32(req + RQ_GADGET) : rd32(w->win + Window_FirstGadget); g; g = rd32(g + Gadget_NextGadget)) {
        if (rd16(g + Gadget_GadgetType) & GTYP_SYSGADGET) continue;
        int x, y, gw, gh;
        gadget_box(w, g, &x, &y, &gw, &gh);
        if (lx >= x && ly >= y && lx < x + gw && ly < y + gh) return g;
    }
    return 0;
}

/* ---- menus ------------------------------------------------------------------- */

#define MENUENABLED 0x1
#define ITEMTEXT 0x2
#define COMMSEQ 0x4
#define MENUTOGGLE 0x8
#define ITEMENABLED 0x10
#define HIGHFLAGS 0xC0
#define HIGHIMAGE 0x00
#define HIGHCOMP 0x40
#define HIGHBOX 0x80
#define HIGHNONE 0xC0
#define CHECKIT 0x1
#define CHECKED 0x100

typedef struct {
    NScreen *s;
    NWindow *w;
    int menu, item, sub;           /* -1 = none */
    int bx0, by0, bx1, by1;        /* item box of the current menu */
    int sx0, sy0, sx1, sy1;        /* sub-item box */
} MenuState;
static MenuState ms;
static int menu_mode;

static void ov_fill(NScreen *s, int x0, int y0, int x1, int y1, int pen) {
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= s->h) continue;
        for (int x = x0; x <= x1; x++) if (x >= 0 && x < s->w) s->ov[(size_t)y * s->w + x] = (uint8_t)pen;
    }
}
static void ov_invert(NScreen *s, int x0, int y0, int x1, int y1) {
    int m = (1 << s->depth) - 1;
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= s->h) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= s->w) continue;
            uint8_t *p = &s->ov[(size_t)y * s->w + x];
            if (*p != 0xFF) *p = (uint8_t)(*p ^ m);
        }
    }
}
static void ov_text(NScreen *s, int x, int y, const char *t, int pen) {
    for (; *t; t++, x += 8) {
        const uint8_t *g = font8_data[(uint8_t)*t];
        for (int r = 0; r < 8; r++)
            for (int col = 0; col < 8; col++)
                if ((g[r] & (0x80 >> col)) && x + col >= 0 && x + col < s->w && y + r >= 0 && y + r < s->h)
                    s->ov[(size_t)(y + r) * s->w + x + col] = (uint8_t)pen;
    }
}
static void ov_itext(NScreen *s, uint32_t it, int left, int top) {
    for (; it; it = rd32(it + IntuiText_NextText)) {
        char buf[128];
        str_get(rd32(it + IntuiText_IText), buf, sizeof buf);
        ov_text(s, left + (int16_t)rd16(it + IntuiText_LeftEdge), top + (int16_t)rd16(it + IntuiText_TopEdge), buf, rd8(it + IntuiText_FrontPen));
    }
}
static void ov_image(NScreen *s, uint32_t img, int left, int top) {
    for (; img; img = rd32(img + Image_NextImage)) {
        if (rd16(img + Image_Depth) == 0xFFFF) {                /* a BOOPSI image */
            continue;   /* BOOPSI images in menus: not yet */
        }
        int x0 = left + (int16_t)rd16(img + Image_LeftEdge), y0 = top + (int16_t)rd16(img + Image_LeftEdge + 2);
        int w = (int16_t)rd16(img + Image_Width), h = (int16_t)rd16(img + Image_Width + 2), depth = (int16_t)rd16(img + Image_Depth);
        uint32_t data = rd32(img + Image_ImageData);
        int pick = rd8(img + Image_PlanePick), onoff = rd8(img + Image_PlaneOnOff), words = (w + 15) >> 4;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int pen = 0, k = 0;
                for (int p = 0; p < s->depth; p++) {
                    if (pick & (1 << p)) {
                        if (data && k < depth && ((rd8(data + (uint32_t)(k * words * 2 * h + y * words * 2 + (x >> 3))) >> (7 - (x & 7))) & 1)) pen |= 1 << p;
                        k++;
                    } else if (onoff & (1 << p)) pen |= 1 << p;
                }
                ov_fill(s, x0 + x, y0 + y, x0 + x, y0 + y, pen);
            }
    }
}

static uint32_t nth_menu(uint32_t strip, int n) { while (strip && n-- > 0) strip = rd32(strip + Menu_NextMenu); return strip; }
static uint32_t nth_item(uint32_t item, int n) { while (item && n-- > 0) item = rd32(item + MenuItem_NextItem); return item; }

static void draw_items(NScreen *s, uint32_t first, int basex, int basey, int hi, int *bx0, int *by0, int *bx1, int *by1) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (uint32_t it = first; it; it = rd32(it + MenuItem_NextItem)) {
        int ix = basex + (int16_t)rd16(it + MenuItem_LeftEdge), iy = basey + (int16_t)rd16(it + MenuItem_LeftEdge + 2);
        int iw = (int16_t)rd16(it + MenuItem_Width), ih = (int16_t)rd16(it + MenuItem_Width + 2);
        if (ix < x0) x0 = ix;
        if (iy < y0) y0 = iy;
        if (ix + iw > x1) x1 = ix + iw;
        if (iy + ih > y1) y1 = iy + ih;
    }
    if (x1 < 0) return;
    x0 -= 4; y0 -= 2; x1 += 3; y1 += 1;
    ov_fill(s, x0, y0, x1, y1, s->pens[BARBLOCKPEN]);
    ov_fill(s, x0, y0, x1, y0, s->pens[BARDETAILPEN]);
    ov_fill(s, x0, y1, x1, y1, s->pens[BARDETAILPEN]);
    ov_fill(s, x0, y0, x0, y1, s->pens[BARDETAILPEN]);
    ov_fill(s, x1, y0, x1, y1, s->pens[BARDETAILPEN]);
    int n = 0;
    for (uint32_t it = first; it; it = rd32(it + MenuItem_NextItem), n++) {
        int ix = basex + (int16_t)rd16(it + MenuItem_LeftEdge), iy = basey + (int16_t)rd16(it + MenuItem_LeftEdge + 2);
        int iw = (int16_t)rd16(it + MenuItem_Width), ih = (int16_t)rd16(it + MenuItem_Width + 2);
        uint32_t fl = rd16(it + MenuItem_Flags);
        uint32_t fill = (n == hi && (fl & HIGHFLAGS) == HIGHIMAGE && rd32(it + MenuItem_SelectFill)) ? rd32(it + MenuItem_SelectFill) : rd32(it + MenuItem_ItemFill);
        if (fl & ITEMTEXT) ov_itext(s, fill, ix, iy); else ov_image(s, fill, ix, iy);
        if ((fl & CHECKIT) && (fl & CHECKED)) ov_text(s, ix, iy + (ih - 8) / 2, "\xb7", s->pens[BARDETAILPEN]);
        if (fl & COMMSEQ) {
            char key[3] = { (char)0xA4, (char)rd8(it + MenuItem_Command), 0 };   /* Amiga key + letter */
            ov_text(s, ix + iw - 18, iy + (ih - 8) / 2, key, s->pens[BARDETAILPEN]);
        }
        if (!(fl & ITEMENABLED))                               /* ghosted */
            for (int yy = iy; yy < iy + ih; yy++) for (int xx = ix + (yy & 1); xx < ix + iw; xx += 2) ov_fill(s, xx, yy, xx, yy, s->pens[BARBLOCKPEN]);
        if (n == hi && (fl & ITEMENABLED)) {
            if ((fl & HIGHFLAGS) == HIGHCOMP) ov_invert(s, ix, iy, ix + iw - 1, iy + ih - 1);
            else if ((fl & HIGHFLAGS) == HIGHBOX) {
                ov_fill(s, ix - 1, iy - 1, ix + iw, iy - 1, s->pens[BARDETAILPEN]);
                ov_fill(s, ix - 1, iy + ih, ix + iw, iy + ih, s->pens[BARDETAILPEN]);
            }
        }
    }
    *bx0 = x0; *by0 = y0; *bx1 = x1; *by1 = y1;
}

static void menu_draw(void) {
    NScreen *s = ms.s;
    memset(s->ov, 0xFF, (size_t)s->w * s->h);
    int bh = rd8(s->scr + Screen_BarHeight);
    ov_fill(s, 0, 0, s->w - 1, bh - 1, s->pens[BARBLOCKPEN]);
    ov_fill(s, 0, bh, s->w - 1, bh, s->pens[BARTRIMPEN]);
    uint32_t strip = rd32(ms.w->win + Window_MenuStrip);
    int n = 0;
    for (uint32_t m = strip; m; m = rd32(m + Menu_NextMenu), n++) {
        char name[64];
        str_get(rd32(m + Menu_MenuName), name, sizeof name);
        int mx = (int16_t)rd16(m + Menu_LeftEdge), mw = (int16_t)rd16(m + Menu_Width);
        ov_text(s, mx + 4, (bh - 8) / 2 + 1, name, s->pens[BARDETAILPEN]);
        if (!(rd16(m + Menu_Flags) & MENUENABLED))
            for (int yy = 0; yy < bh; yy++) for (int xx = mx + (yy & 1); xx < mx + mw; xx += 2) ov_fill(s, xx, yy, xx, yy, s->pens[BARBLOCKPEN]);
        if (n == ms.menu) {
            ov_invert(s, mx, 0, mx + mw - 1, bh - 1);
            int basex = mx, basey = bh + 2;
            draw_items(s, rd32(m + Menu_FirstItem), basex, basey, ms.item, &ms.bx0, &ms.by0, &ms.bx1, &ms.by1);
            uint32_t item = ms.item >= 0 ? nth_item(rd32(m + Menu_FirstItem), ms.item) : 0;
            ms.sx0 = ms.sx1 = -1;
            if (item && rd32(item + MenuItem_SubItem)) {
                int ix = basex + (int16_t)rd16(item + MenuItem_LeftEdge), iy = basey + (int16_t)rd16(item + MenuItem_LeftEdge + 2);
                draw_items(s, rd32(item + MenuItem_SubItem), ix, iy, ms.sub, &ms.sx0, &ms.sy0, &ms.sx1, &ms.sy1);
            }
        }
    }
    gfx_touch();
}

static void menu_track(int x, int y) {
    NScreen *s = ms.s;
    int bh = rd8(s->scr + Screen_BarHeight);
    uint32_t strip = rd32(ms.w->win + Window_MenuStrip);
    int menu = ms.menu, item = ms.item, sub = ms.sub;
    if (y < bh) {
        int n = 0;
        menu = -1;
        for (uint32_t m = strip; m; m = rd32(m + Menu_NextMenu), n++) {
            int mx = (int16_t)rd16(m + Menu_LeftEdge), mw = (int16_t)rd16(m + Menu_Width);
            if (x >= mx && x < mx + mw) menu = n;
        }
        if (menu < 0) menu = ms.menu;
        item = -1;
        sub = -1;
    } else if (menu >= 0) {
        uint32_t m = nth_menu(strip, menu);
        int basex = (int16_t)rd16(m + Menu_LeftEdge), basey = bh + 2;
        /* inside a sub-item box? */
        if (ms.sx1 >= 0 && x >= ms.sx0 && x <= ms.sx1 && y >= ms.sy0 && y <= ms.sy1 && item >= 0) {
            uint32_t it = nth_item(rd32(m + Menu_FirstItem), item);
            int ix = basex + (int16_t)rd16(it + MenuItem_LeftEdge), iy = basey + (int16_t)rd16(it + MenuItem_LeftEdge + 2), k = 0;
            sub = -1;
            for (uint32_t si = rd32(it + MenuItem_SubItem); si; si = rd32(si + MenuItem_NextItem), k++) {
                int sx = ix + (int16_t)rd16(si + MenuItem_LeftEdge), sy = iy + (int16_t)rd16(si + MenuItem_LeftEdge + 2);
                if (x >= sx && x < sx + (int16_t)rd16(si + MenuItem_Width) && y >= sy && y < sy + (int16_t)rd16(si + MenuItem_Width + 2)) sub = k;
            }
        } else {
            int k = 0, found = -1;
            for (uint32_t it = rd32(m + Menu_FirstItem); it; it = rd32(it + MenuItem_NextItem), k++) {
                int ix = basex + (int16_t)rd16(it + MenuItem_LeftEdge), iy = basey + (int16_t)rd16(it + MenuItem_LeftEdge + 2);
                if (x >= ix && x < ix + (int16_t)rd16(it + MenuItem_Width) && y >= iy && y < iy + (int16_t)rd16(it + MenuItem_Width + 2)) found = k;
            }
            if (found != item) sub = -1;
            item = found;
        }
    }
    if (menu != ms.menu || item != ms.item || sub != ms.sub) {
        ms.menu = menu; ms.item = item; ms.sub = sub;
        menu_draw();
    }
}

static void menu_start(NWindow *w, int x, int y) {
    memset(&ms, 0, sizeof ms);
    ms.s = w->s; ms.w = w; ms.menu = -1; ms.item = -1; ms.sub = -1; ms.sx1 = -1;
    menu_mode = 1;
    wr32(w->win + Window_Flags, rd32(w->win + Window_Flags) | WFLG_MENUSTATE);
    menu_draw();
    menu_track(x, y);
}

static uint32_t select_item(uint32_t strip, int menu, int item, int sub) {
    /* returns the MenuItem chosen (or 0) and applies CHECKIT/MutualExclude */
    uint32_t m = nth_menu(strip, menu);
    if (!m || !(rd16(m + Menu_Flags) & MENUENABLED)) return 0;
    uint32_t it = nth_item(rd32(m + Menu_FirstItem), item), parent = it;
    if (it && sub >= 0) it = nth_item(rd32(it + MenuItem_SubItem), sub);
    if (!it || !(rd16(it + MenuItem_Flags) & ITEMENABLED)) return 0;
    if (parent && rd32(parent + MenuItem_SubItem) && sub < 0) return 0;   /* header of a submenu */
    uint32_t fl = rd16(it + MenuItem_Flags);
    if (fl & CHECKIT) {
        if ((fl & MENUTOGGLE) && (fl & CHECKED)) fl &= ~CHECKED; else fl |= CHECKED;
        wr16(it + MenuItem_Flags, fl);
        uint32_t excl = rd32(it + MenuItem_MutualExclude), k = 0;
        uint32_t first = sub >= 0 ? rd32(parent + MenuItem_SubItem) : rd32(m + Menu_FirstItem);
        for (uint32_t o = first; o; o = rd32(o + MenuItem_NextItem), k++)
            if (o != it && (excl & (1u << k))) wr16(o + MenuItem_Flags, rd16(o + MenuItem_Flags) & ~CHECKED);
    }
    wr16(it + MenuItem_NextSelect, MENUNULL);
    return it;
}

static void menu_end(void) {
    NWindow *w = ms.w;
    uint32_t strip = rd32(w->win + Window_MenuStrip), code = MENUNULL;
    if (ms.menu >= 0 && ms.item >= 0 && select_item(strip, ms.menu, ms.item, ms.sub))
        code = (uint32_t)(ms.menu & 31) | (uint32_t)(ms.item & 63) << 5 | (uint32_t)((ms.sub >= 0 ? ms.sub : 31) & 31) << 11;
    memset(ms.s->ov, 0xFF, (size_t)ms.s->w * ms.s->h);
    menu_mode = 0;
    wr32(w->win + Window_Flags, rd32(w->win + Window_Flags) & ~WFLG_MENUSTATE);
    gfx_touch();
    send_idcmp(w, IDCMP_MENUPICK, code, 0, 0, w->s ? 0 : 0, 0);
}

/* Amiga-key shortcut: find an item whose Command matches */
static int menu_shortcut(NWindow *w, int ch) {
    uint32_t strip = rd32(w->win + Window_MenuStrip);
    int mn = 0;
    for (uint32_t m = strip; m; m = rd32(m + Menu_NextMenu), mn++) {
        int in = 0;
        for (uint32_t it = rd32(m + Menu_FirstItem); it; it = rd32(it + MenuItem_NextItem), in++) {
            int sn = 0;
            if ((rd16(it + MenuItem_Flags) & COMMSEQ) && tolower(rd8(it + MenuItem_Command)) == tolower(ch)) {
                if (select_item(strip, mn, in, -1)) send_idcmp(w, IDCMP_MENUPICK, (uint32_t)(mn | in << 5 | 31 << 11), 0, 0, 0, 0);
                return 1;
            }
            for (uint32_t si = rd32(it + MenuItem_SubItem); si; si = rd32(si + MenuItem_NextItem), sn++)
                if ((rd16(si + MenuItem_Flags) & COMMSEQ) && tolower(rd8(si + MenuItem_Command)) == tolower(ch)) {
                    if (select_item(strip, mn, in, sn)) send_idcmp(w, IDCMP_MENUPICK, (uint32_t)(mn | in << 5 | sn << 11), 0, 0, 0, 0);
                    return 1;
                }
        }
    }
    return 0;
}

/* ---- the input task ------------------------------------------------------------ */

enum { DRAG_NONE, DRAG_MOVE, DRAG_SIZE, DRAG_CLOSE, DRAG_DEPTH, DRAG_GADGET, DRAG_PROP, DRAG_CUSTOM, DRAG_ZOOM };
static int drag;
static NWindow *drag_win;
static uint32_t drag_gad;
static int drag_dx, drag_dy, drag_inside;
static int qualifier;

/* mouse buttons held: bit 0 left, bit 1 right (for programs that poll the hardware) */
int input_buttons(void) { return ((qualifier & 0x4000) ? 1 : 0) | ((qualifier & 0x2000) ? 2 : 0); }

static NScreen *screen_by_id(int id) {
    for (NScreen *s = screens; s; s = s->next) if (s->id == id) return s;
    return 0;
}

static NWindow *window_at(NScreen *s, int x, int y) {
    for (NWindow *w = s->wins; w; w = w->next)
        if (x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h) return w;
    return 0;
}

static int in_gadget(NWindow *w, uint32_t g, int lx, int ly) {
    int x, y, gw, gh;
    gadget_box(w, g, &x, &y, &gw, &gh);
    return lx >= x && ly >= y && lx < x + gw && ly < y + gh;
}

static void prop_set_from(NWindow *w, uint32_t g, int lx, int ly) {
    int x, y, gw, gh, kx, ky, kw, kh;
    gadget_box(w, g, &x, &y, &gw, &gh);
    prop_knob(w, g, &kx, &ky, &kw, &kh);
    uint32_t pi = rd32(g + Gadget_SpecialInfo);
    int fl = rd16(pi + PropInfo_Flags);
    if (fl & PI_FREEHORIZ) {
        int range = gw - 4 - kw, pos = lx - drag_dx - (x + 2);
        if (pos < 0) pos = 0;
        if (pos > range) pos = range;
        wr16(pi + PropInfo_HorizPot, range > 0 ? (uint32_t)((long)pos * 0xFFFF / range) : 0);
    }
    if (fl & PI_FREEVERT) {
        int range = gh - 4 - kh, pos = ly - drag_dy - (y + 2);
        if (pos < 0) pos = 0;
        if (pos > range) pos = range;
        wr16(pi + PropInfo_VertPot, range > 0 ? (uint32_t)((long)pos * 0xFFFF / range) : 0);
    }
    render_gadget(w, g);
}

/* change a boolean gadget's selected state: GADGHCOMP complements its box
   (twice = back to normal), the other modes redraw */
static void gadget_select(NWindow *w, uint32_t g, int sel) {
    uint32_t f = rd16(g + Gadget_Flags);
    if (!!(f & GFLG_SELECTED) == !!sel) return;
    wr16(g + Gadget_Flags, sel ? f | GFLG_SELECTED : f & ~GFLG_SELECTED);
    if ((f & GFLG_GADGHIGHBITS) == GFLG_GADGHCOMP) {
        int x, y, gw, gh;
        gadget_box(w, g, &x, &y, &gw, &gh);
        rp_fill(w->rp, x, y, x + gw - 1, y + gh - 1, 0, 1);
    } else render_gadget(w, g);
}

static void str_deactivate(void) {
    if (!active_strgad) return;
    uint32_t g = active_strgad;
    NWindow *w = active_strwin;
    active_strgad = 0;
    active_strwin = 0;
    if (w && nwin_of(w->win) == w) render_gadget(w, g);
}

static void str_key(int raw, int ch) {
    NWindow *w = active_strwin;
    uint32_t g = active_strgad, si = rd32(g + Gadget_SpecialInfo), buf = rd32(si + StringInfo_Buffer);
    int max = (int16_t)rd16(si + StringInfo_MaxChars), pos = (int16_t)rd16(si + StringInfo_BufferPos);
    char text[512];
    int n = str_get(buf, text, sizeof text);
    if (raw == 0x44 || raw == 0x43) {                           /* Return / Enter */
        if (rd16(g + Gadget_Activation) & GACT_LONGINT) wr32(si + StringInfo_LongInt, (uint32_t)strtol(text, 0, 10));
        str_deactivate();
        bo_gadget_changed(CPU, w, g, 0);
        if (rd16(g + Gadget_Activation) & GACT_RELVERIFY) send_idcmp(w, IDCMP_GADGETUP, 0, (uint32_t)qualifier, g, 0, 0);
        return;
    }
    if (raw == 0x41 && pos > 0) { memmove(text + pos - 1, text + pos, (size_t)(n - pos + 1)); pos--; n--; }
    else if (raw == 0x46 && pos < n) { memmove(text + pos, text + pos + 1, (size_t)(n - pos)); n--; }
    else if (raw == 0x4F && pos > 0) pos--;
    else if (raw == 0x4E && pos < n) pos++;
    else if (ch >= 32 && ch != 127 && n < max - 1) { memmove(text + pos + 1, text + pos, (size_t)(n - pos + 1)); text[pos++] = (char)ch; n++; }
    for (int i = 0; i <= n; i++) wr8(buf + (uint32_t)i, (uint8_t)text[i]);
    wr16(si + StringInfo_BufferPos, (uint32_t)pos);
    render_gadget(w, g);
}

static void mouse_rel(NWindow *w, int x, int y, int *lx, int *ly) { *lx = x - w->x; *ly = y - w->y; }

static void handle_button(NScreen *s, int x, int y, int code) {
    NWindow *w;
    int lx, ly;
    if (code == 1) {                                            /* select down */
        qualifier |= 0x4000;
        w = window_at(s, x, y);
        if (!w) return;
        if (w != active_win) win_activate(w);
        mouse_rel(w, x, y, &lx, &ly);
        uint32_t fl = rd32(w->win + Window_Flags);
        int bt = (int8_t)rd8(w->win + Window_BorderTop);
        int cw = (fl & WFLG_CLOSEGADGET) ? 20 : 0, dw = (fl & WFLG_DEPTHGADGET) ? 24 : 0;
        uint32_t g = gadget_at(w, lx, ly);
        if (active_strgad && g != active_strgad) str_deactivate();
        if (!g && rd32(w->win + Window_FirstRequest) && !(ly < bt && !(fl & WFLG_BORDERLESS))) { drag = DRAG_NONE; return; }
        drag_win = w;
        if (g && !(rd16(g + Gadget_Flags) & GFLG_DISABLED)) {
            int type = rd16(g + Gadget_GadgetType) & GTYP_GTYPEMASK, act = rd16(g + Gadget_Activation);
            drag_gad = g;
            if (type == GTYP_CUSTOM) {
                int gx, gy, gw, gh;
                gadget_box(w, g, &gx, &gy, &gw, &gh);
                if (!bo_hittest(CPU, w, g, lx - gx, ly - gy)) { drag = DRAG_NONE; return; }
                if (act & GACT_IMMEDIATE) send_idcmp(w, IDCMP_GADGETDOWN, 0, (uint32_t)qualifier, g, lx, ly);
                uint32_t code = 0;
                uint32_t r = bo_input(CPU, w, g, 2 /* GM_GOACTIVE */, 2, 0x68, qualifier, lx - gx, ly - gy, &code);
                if (r == 0) { drag = DRAG_CUSTOM; return; }          /* GMR_MEACTIVE */
                if (r & 8) send_idcmp(w, IDCMP_GADGETUP, code, (uint32_t)qualifier, g, lx, ly);
                bo_goinactive(CPU, w, g, 0);
                drag = DRAG_NONE;
                return;
            }
            if (act & GACT_IMMEDIATE) send_idcmp(w, IDCMP_GADGETDOWN, 0, (uint32_t)qualifier, g, lx, ly);
            if (type == GTYP_STRGADGET) {
                uint32_t si = rd32(g + Gadget_SpecialInfo);
                int gx, gy, gw, gh;
                gadget_box(w, g, &gx, &gy, &gw, &gh);
                char text[512];
                int n = str_get(rd32(si + StringInfo_Buffer), text, sizeof text);
                int pos = (int16_t)rd16(si + StringInfo_DispPos) + (lx - gx) / 8;
                wr16(si + StringInfo_BufferPos, (uint32_t)(pos > n ? n : pos));
                active_strgad = g;
                active_strwin = w;
                render_gadget(w, g);
                drag = DRAG_NONE;
                return;
            }
            if (type == GTYP_PROPGADGET) {
                int kx, ky, kw, kh;
                prop_knob(w, g, &kx, &ky, &kw, &kh);
                uint32_t pi = rd32(g + Gadget_SpecialInfo);
                if (lx >= kx && ly >= ky && lx < kx + kw && ly < ky + kh) {
                    drag = DRAG_PROP;
                    drag_dx = lx - kx;
                    drag_dy = ly - ky;
                    wr16(g + Gadget_Flags, rd16(g + Gadget_Flags) | GFLG_SELECTED);
                    render_gadget(w, g);
                } else {                                         /* page towards the click */
                    int fl2 = rd16(pi + PropInfo_Flags);
                    if (fl2 & PI_FREEHORIZ) {
                        int step = rd16(pi + PropInfo_HorizBody), pot = rd16(pi + PropInfo_HorizPot) + (lx < kx ? -step : step);
                        wr16(pi + PropInfo_HorizPot, (uint32_t)(pot < 0 ? 0 : pot > 0xFFFF ? 0xFFFF : pot));
                    }
                    if (fl2 & PI_FREEVERT) {
                        int step = rd16(pi + PropInfo_VertBody), pot = rd16(pi + PropInfo_VertPot) + (ly < ky ? -step : step);
                        wr16(pi + PropInfo_VertPot, (uint32_t)(pot < 0 ? 0 : pot > 0xFFFF ? 0xFFFF : pot));
                    }
                    render_gadget(w, g);
                    if (act & GACT_RELVERIFY) send_idcmp(w, IDCMP_GADGETUP, 0, (uint32_t)qualifier, g, lx, ly);
                    drag = DRAG_NONE;
                }
                return;
            }
            /* boolean */
            gadget_select(w, g, (act & GACT_TOGGLESELECT) ? !(rd16(g + Gadget_Flags) & GFLG_SELECTED) : 1);
            drag = DRAG_GADGET;
            drag_inside = 1;
            return;
        }
        if (!(fl & WFLG_BORDERLESS) && ly < bt) {
            if (cw && lx < cw) { drag = DRAG_CLOSE; return; }
            if (dw && lx >= w->w - dw) { drag = DRAG_DEPTH; return; }
            if (zoom_width(w) && lx >= w->w - dw - zoom_width(w) && lx < w->w - dw) { drag = DRAG_ZOOM; return; }
            if (fl & WFLG_DRAGBAR) { drag = DRAG_MOVE; drag_dx = lx; drag_dy = ly; return; }
        }
        if ((fl & WFLG_SIZEGADGET) && lx >= w->w - 18 && ly >= w->h - 10) {
            drag = DRAG_SIZE;
            drag_dx = w->w - lx;
            drag_dy = w->h - ly;
            return;
        }
        drag = DRAG_NONE;
        send_idcmp(w, IDCMP_MOUSEBUTTONS, SELECTDOWN, (uint32_t)qualifier, 0, lx, ly);
        return;
    }
    if (code == -1) {                                           /* select up */
        qualifier &= ~0x4000;
        w = drag_win;
        int d = drag;
        drag = DRAG_NONE;
        if (!w || nwin_of(w->win) != w) { drag_win = 0; return; }
        mouse_rel(w, x, y, &lx, &ly);
        int bt = (int8_t)rd8(w->win + Window_BorderTop);
        uint32_t fl = rd32(w->win + Window_Flags);
        int cw = (fl & WFLG_CLOSEGADGET) ? 20 : 0, dw = (fl & WFLG_DEPTHGADGET) ? 24 : 0;
        switch (d) {
        case DRAG_CUSTOM: {
            uint32_t g = drag_gad, code = 0;
            int gx, gy, gw, gh;
            gadget_box(w, g, &gx, &gy, &gw, &gh);
            uint32_t r = bo_input(CPU, w, g, 3 /* GM_HANDLEINPUT */, 2, 0x68 | 0x80, qualifier, lx - gx, ly - gy, &code);
            if (r == 0) { drag = DRAG_CUSTOM; return; }
            if (r & 8) send_idcmp(w, IDCMP_GADGETUP, code, (uint32_t)qualifier, g, lx, ly);
            bo_goinactive(CPU, w, g, 0);
            return;
        }
        case DRAG_CLOSE:
            if (lx >= 0 && lx < cw && ly >= 0 && ly < bt) send_idcmp(w, IDCMP_CLOSEWINDOW, 0, (uint32_t)qualifier, 0, lx, ly);
            return;
        case DRAG_DEPTH:
            if (lx >= w->w - dw && lx < w->w && ly >= 0 && ly < bt) {
                if (w->s->wins == w) win_to_back(w); else win_to_front(w);
            }
            return;
        case DRAG_ZOOM:
            if (lx >= w->w - dw - zoom_width(w) && lx < w->w - dw && ly >= 0 && ly < bt) win_zip(w);
            return;
        case DRAG_MOVE: case DRAG_SIZE: return;
        case DRAG_PROP: {
            uint32_t g = drag_gad;
            wr16(g + Gadget_Flags, rd16(g + Gadget_Flags) & ~GFLG_SELECTED);
            render_gadget(w, g);
            bo_gadget_changed(CPU, w, g, 0);
            if (rd16(g + Gadget_Activation) & GACT_RELVERIFY) send_idcmp(w, IDCMP_GADGETUP, 0, (uint32_t)qualifier, g, lx, ly);
            return;
        }
        case DRAG_GADGET: {
            uint32_t g = drag_gad;
            int inside = in_gadget(w, g, lx, ly), act = rd16(g + Gadget_Activation);
            if (!(act & GACT_TOGGLESELECT)) gadget_select(w, g, 0);
            if (inside && (act & GACT_RELVERIFY)) send_idcmp(w, IDCMP_GADGETUP, 0, (uint32_t)qualifier, g, lx, ly);
            return;
        }
        }
        if (active_win) {
            mouse_rel(active_win, x, y, &lx, &ly);
            send_idcmp(active_win, IDCMP_MOUSEBUTTONS, SELECTUP, (uint32_t)qualifier, 0, lx, ly);
        }
        return;
    }
    if (code == 2) {                                            /* menu down */
        qualifier |= 0x2000;
        w = active_win;
        if (!w || w->s != s) return;
        if (rd32(w->win + Window_Flags) & WFLG_RMBTRAP) {
            mouse_rel(w, x, y, &lx, &ly);
            send_idcmp(w, IDCMP_MOUSEBUTTONS, MENUDOWN, (uint32_t)qualifier, 0, lx, ly);
            return;
        }
        if (rd32(w->win + Window_MenuStrip)) menu_start(w, x, y);
        return;
    }
    if (code == -2) {                                           /* menu up */
        qualifier &= ~0x2000;
        if (menu_mode) { menu_end(); return; }
        w = active_win;
        if (w && (rd32(w->win + Window_Flags) & WFLG_RMBTRAP)) {
            mouse_rel(w, x, y, &lx, &ly);
            send_idcmp(w, IDCMP_MOUSEBUTTONS, MENUUP, (uint32_t)qualifier, 0, lx, ly);
        }
    }
}

static void handle_move(NScreen *s, int x, int y) {
    wr16(s->scr + Screen_MouseX, (uint32_t)x);
    wr16(s->scr + Screen_MouseY, (uint32_t)y);
    wr16(IntuitionBase + IntuitionBase_MouseX, (uint32_t)x);
    wr16(IntuitionBase + IntuitionBase_MouseY, (uint32_t)y);
    for (NWindow *w = s->wins; w; w = w->next) {
        wr16(w->win + Window_MouseX, (uint32_t)(x - w->x));
        wr16(w->win + Window_MouseY, (uint32_t)(y - w->y));
        wr16(w->win + Window_GZZMouseX, (uint32_t)(x - w->x - (int8_t)rd8(w->win + Window_BorderLeft)));
        wr16(w->win + Window_GZZMouseY, (uint32_t)(y - w->y - (int8_t)rd8(w->win + Window_BorderTop)));
    }
    if (menu_mode) { if (ms.s == s) menu_track(x, y); return; }
    NWindow *w = drag_win;
    switch (drag) {
    case DRAG_MOVE: win_move(w, x - drag_dx, y - drag_dy); return;
    case DRAG_SIZE: win_resize(w, x - w->x + drag_dx, y - w->y + drag_dy); return;
    case DRAG_CUSTOM: {
        int lx, ly, gx, gy, gw, gh;
        uint32_t code = 0;
        mouse_rel(w, x, y, &lx, &ly);
        gadget_box(w, drag_gad, &gx, &gy, &gw, &gh);
        uint32_t r = bo_input(CPU, w, drag_gad, 3, 2, 0xFF /* IECODE_NOBUTTON */, qualifier, lx - gx, ly - gy, &code);
        if (r != 0) {
            if (r & 8) send_idcmp(w, IDCMP_GADGETUP, code, (uint32_t)qualifier, drag_gad, lx, ly);
            bo_goinactive(CPU, w, drag_gad, 0);
            drag = DRAG_NONE;
        }
        return;
    }
    case DRAG_PROP: {
        int lx, ly;
        mouse_rel(w, x, y, &lx, &ly);
        prop_set_from(w, drag_gad, lx, ly);
        bo_gadget_changed(CPU, w, drag_gad, 1);
        if (rd16(drag_gad + Gadget_Activation) & GACT_FOLLOWMOUSE)
            send_idcmp(w, IDCMP_MOUSEMOVE, 0, (uint32_t)qualifier, drag_gad, lx, ly);
        return;
    }
    case DRAG_GADGET: {
        int lx, ly;
        mouse_rel(w, x, y, &lx, &ly);
        int inside = in_gadget(w, drag_gad, lx, ly);
        if (inside != drag_inside && !(rd16(drag_gad + Gadget_Activation) & GACT_TOGGLESELECT)) {
            drag_inside = inside;
            gadget_select(w, drag_gad, inside);
        }
        return;
    }
    }
    w = active_win;
    if (w && w->s == s && (rd32(w->win + Window_Flags) & WFLG_REPORTMOUSE))
        send_idcmp(w, IDCMP_MOUSEMOVE, 0, (uint32_t)qualifier, 0, x - w->x, y - w->y);
}

/* the keyboard matrix keyboard.device reports: a bit per raw key held down */
static uint8_t key_matrix[16];
int input_qualifier(void) { return qualifier; }

void input_key_matrix(uint8_t out[16]) {
    memcpy(out, key_matrix, 16);
    for (int i = 0; i < 8; i++)                                  /* modifiers: raw keys $60-$67 */
        if (qualifier & (1 << i)) out[12] |= (uint8_t)(1 << i); else out[12] &= (uint8_t)~(1 << i);
}

static void handle_key(int raw, int ch, int qual) {
    qualifier = (qualifier & 0x7000) | (qual & 0xFF);
    int up = raw & 0x80;
    if ((raw & 0x7F) < 0x60) {
        if (up) key_matrix[(raw & 0x7F) >> 3] &= (uint8_t)~(1 << (raw & 7));
        else key_matrix[raw >> 3] |= (uint8_t)(1 << (raw & 7));
    }
    NWindow *w = active_win;
    if (!up) keymap_learn(raw, ch, qual);
    if (!w) return;
    if (!up && active_strgad && active_strwin == w) { str_key(raw, ch); return; }
    if (!up && (qual & 0xC0) && ch && rd32(w->win + Window_MenuStrip) && menu_shortcut(w, ch)) return;
    if (w->hook) w->hook(w, HOOK_KEY, raw, ch, qualifier);
    uint32_t idcmp = rd32(w->win + Window_IDCMPFlags);
    if (!up && ch && (idcmp & IDCMP_VANILLAKEY)) { send_idcmp(w, IDCMP_VANILLAKEY, (uint32_t)ch, (uint32_t)qualifier, 0, 0, 0); return; }
    if (idcmp & IDCMP_RAWKEY) send_idcmp(w, IDCMP_RAWKEY, (uint32_t)raw, (uint32_t)qualifier, 0, 0, 0);
}

static void handle_tick(void) {
    NWindow *w = active_win;
    if (!w) return;
    uint32_t port = rd32(w->win + Window_UserPort);
    if (!port || !(rd32(w->win + Window_IDCMPFlags) & IDCMP_INTUITICKS)) return;
    /* one tick at a time in the port */
    for (uint32_t n = rd32(port + MP_MSGLIST + LH_HEAD); rd32(n + LN_SUCC); n = rd32(n + LN_SUCC))
        if (rd32(n + IntuiMessage_Class) == IDCMP_INTUITICKS && rd32(n + IntuiMessage_IDCMPWindow) == w->win) return;
    send_idcmp(w, IDCMP_INTUITICKS, 0, (uint32_t)qualifier, 0, 0, 0);
}

static int last_mx, last_my;

/* one event to Intuition (after the input handlers) */
static void dispatch(InputEv *ev) {
    if (ev->type == IEV_TICK) { handle_tick(); return; }
    if (ev->type == IEV_KEY) { handle_key(ev->code, ev->ch, ev->qual); return; }
    NScreen *s = screen_by_id(ev->screen);
    if (!s) return;
    if (ev->type == IEV_MOUSE) handle_move(s, ev->x, ev->y);
    else if (ev->type == IEV_BUTTON) handle_button(s, ev->x, ev->y, ev->code);
    else if (ev->type == IEV_CLOSE_SCREEN)
        for (NWindow *w = s->wins; w; w = w->next) send_idcmp(w, IDCMP_CLOSEWINDOW, 0, 0, 0, 0, 0);
}

static int cur_screen;
/* events a commodity puts in the stream, in place of the one it took (input task only) */
void input_dispatch_now(InputEv *ev) {
    if (ev->type == IEV_BUTTON && !ev->screen) ev->screen = cur_screen;
    if (ev->type == IEV_BUTTON) { ev->x = last_mx; ev->y = last_my; }
    dispatch(ev);
}

void input_task_main(void *arg) {
    (void)arg;
    InputEv ev;
    for (;;) {
        sched_input_read(&ev);
        uint32_t m;
        while ((m = list_remhead(ireply_port + MP_MSGLIST))) exec_freemem(m, 56);   /* replied IntuiMessages */
        extern int input_handlers_filter(InputEv *ev);
        if (ev.screen) cur_screen = ev.screen;
        if (ev.type == IEV_MOUSE) { last_mx = ev.x; last_my = ev.y; }
        if (!input_handlers_filter(&ev)) continue;              /* a program's input handler ate it */
        dispatch(&ev);
    }
}

/* ---- library functions: gadgets, menus, drawing ---------------------------------- */

static uint32_t i_DrawBorder(M68K *c) { draw_border(c->a[0], c->a[1], (int16_t)c->d[0], (int16_t)c->d[1]); return 0; }
static uint32_t i_DrawImage(M68K *c) { draw_image(c->a[0], c->a[1], (int16_t)c->d[0], (int16_t)c->d[1], 0); return 0; }
static uint32_t i_DrawImageState(M68K *c) { draw_image(c->a[0], c->a[1], (int16_t)c->d[0], (int16_t)c->d[1], (int)c->d[2]); return 0; }
static uint32_t i_EraseImage(M68K *c) {
    uint32_t img = c->a[1];
    int x = (int16_t)c->d[0] + (int16_t)rd16(img + Image_LeftEdge), y = (int16_t)c->d[1] + (int16_t)rd16(img + Image_LeftEdge + 2);
    rp_fill(c->a[0], x, y, x + (int16_t)rd16(img + Image_Width) - 1, y + (int16_t)rd16(img + Image_Width + 2) - 1, 0, 0);
    return 0;
}
static uint32_t i_PrintIText(M68K *c) { draw_itext(c->a[0], c->a[1], (int16_t)c->d[0], (int16_t)c->d[1]); return 0; }
static uint32_t i_IntuiTextLength(M68K *c) { return (uint32_t)itext_length(c->a[0]); }

static uint32_t i_AddGList(M68K *c) {
    /* window A0, gadget A1, position D0, numGad D1 (-1 = all), requester A2 */
    uint32_t win = c->a[0], g = c->a[1];
    int pos = (int16_t)c->d[0], num = (int16_t)c->d[1];
    if (!g) return 0;
    uint32_t last = g;
    for (int k = 1; (num < 0 || k < num) && rd32(last + Gadget_NextGadget); k++) last = rd32(last + Gadget_NextGadget);
    uint32_t prev = 0, cur = rd32(win + Window_FirstGadget);
    int i = 0;
    while (cur && (pos < 0 || i < pos)) { prev = cur; cur = rd32(cur + Gadget_NextGadget); i++; }
    wr32(last + Gadget_NextGadget, cur);
    if (prev) wr32(prev + Gadget_NextGadget, g); else wr32(win + Window_FirstGadget, g);
    return (uint32_t)i;
}
static uint32_t i_AddGadget(M68K *c) { c->d[1] = 1; return i_AddGList(c); }
static uint32_t i_RemoveGList(M68K *c) {
    /* window A0, gadget A1, numGad D0 */
    uint32_t win = c->a[0], g = c->a[1];
    int num = (int16_t)c->d[0];
    uint32_t prev = 0, cur = rd32(win + Window_FirstGadget);
    int i = 0;
    while (cur && cur != g) { prev = cur; cur = rd32(cur + Gadget_NextGadget); i++; }
    if (!cur) return 0xFFFFFFFF;
    uint32_t last = g;
    for (int k = 1; (num < 0 || k < num) && rd32(last + Gadget_NextGadget); k++) last = rd32(last + Gadget_NextGadget);
    uint32_t after = rd32(last + Gadget_NextGadget);
    if (prev) wr32(prev + Gadget_NextGadget, after); else wr32(win + Window_FirstGadget, after);
    wr32(last + Gadget_NextGadget, 0);
    if (active_strgad == g) str_deactivate();
    return (uint32_t)i;
}
static uint32_t i_RemoveGadget(M68K *c) { c->d[0] = 1; return i_RemoveGList(c); }
static uint32_t i_RefreshGList(M68K *c) {
    NWindow *w = nwin_of(c->a[1]);
    if (w) gadget_render_all(w, c->a[0], (int16_t)c->d[0]);
    return 0;
}
static uint32_t i_RefreshGadgets(M68K *c) { c->d[0] = 0xFFFF; return i_RefreshGList(c); }
static uint32_t i_OnGadget(M68K *c) {
    wr16(c->a[0] + Gadget_Flags, rd16(c->a[0] + Gadget_Flags) & ~GFLG_DISABLED);
    NWindow *w = nwin_of(c->a[1]);
    if (w) {                                                   /* clear the ghosting first */
        int x, y, gw, gh;
        gadget_box(w, c->a[0], &x, &y, &gw, &gh);
        rp_fill(w->rp, x, y, x + gw - 1, y + gh - 1, w->s->pens[BACKGROUNDPEN], 0);
        render_gadget(w, c->a[0]);
    }
    return 0;
}
static uint32_t i_OffGadget(M68K *c) {
    wr16(c->a[0] + Gadget_Flags, rd16(c->a[0] + Gadget_Flags) | GFLG_DISABLED);
    NWindow *w = nwin_of(c->a[1]);
    if (w) render_gadget(w, c->a[0]);
    return 0;
}
static uint32_t i_ActivateGadget(M68K *c) {
    NWindow *w = nwin_of(c->a[1]);
    uint32_t g = c->a[0];
    if (!w || (rd16(g + Gadget_GadgetType) & GTYP_GTYPEMASK) != GTYP_STRGADGET) return 0;
    str_deactivate();
    active_strgad = g;
    active_strwin = w;
    render_gadget(w, g);
    return DOSTRUE;
}
static uint32_t i_NewModifyProp(M68K *c) {
    /* gadget A0, window A1, req A2, flags D0, hpot D1, vpot D2, hbody D3, vbody D4 */
    uint32_t pi = rd32(c->a[0] + Gadget_SpecialInfo);
    if (!pi) return 0;
    wr16(pi + PropInfo_Flags, (rd16(pi + PropInfo_Flags) & 0xFF00) | (c->d[0] & 0xFF));
    wr16(pi + PropInfo_HorizPot, c->d[1]);
    wr16(pi + PropInfo_VertPot, c->d[2]);
    wr16(pi + PropInfo_HorizBody, c->d[3]);
    wr16(pi + PropInfo_VertBody, c->d[4]);
    NWindow *w = nwin_of(c->a[1]);
    if (w) render_gadget(w, c->a[0]);
    return 0;
}

static uint32_t i_SetMenuStrip(M68K *c) { wr32(c->a[0] + Window_MenuStrip, c->a[1]); return DOSTRUE; }
static uint32_t i_ClearMenuStrip(M68K *c) { wr32(c->a[0] + Window_MenuStrip, 0); return 0; }
static uint32_t i_ItemAddress(M68K *c) {
    uint32_t num = c->d[0];
    if ((num & 0xFFFF) == MENUNULL) return 0;
    int menu = (int)(num & 31), item = (int)((num >> 5) & 63), sub = (int)((num >> 11) & 31);
    uint32_t m = nth_menu(c->a[0], menu);
    if (!m) return 0;
    uint32_t it = nth_item(rd32(m + Menu_FirstItem), item);
    if (it && sub != 31) it = nth_item(rd32(it + MenuItem_SubItem), sub);
    return it;
}
static void on_off_menu(uint32_t win, uint32_t num, int on) {
    int menu = (int)(num & 31), item = (int)((num >> 5) & 63), sub = (int)((num >> 11) & 31);
    uint32_t m = nth_menu(rd32(win + Window_MenuStrip), menu);
    if (!m) return;
    if (item == 63) { uint32_t f = rd16(m + Menu_Flags); wr16(m + Menu_Flags, on ? f | MENUENABLED : f & ~MENUENABLED); return; }
    uint32_t it = nth_item(rd32(m + Menu_FirstItem), item);
    if (it && sub != 31) it = nth_item(rd32(it + MenuItem_SubItem), sub);
    if (it) { uint32_t f = rd16(it + MenuItem_Flags); wr16(it + MenuItem_Flags, on ? f | ITEMENABLED : f & ~ITEMENABLED); }
}
static uint32_t i_OnMenu(M68K *c) { on_off_menu(c->a[0], c->d[0], 1); return 0; }
static uint32_t i_OffMenu(M68K *c) { on_off_menu(c->a[0], c->d[0], 0); return 0; }

/* ---- requesters ---------------------------------------------------------------- */

/* RawDoFmt into a host buffer; returns the argument pointer after the used ones */
static uint32_t format(M68K *c, uint32_t fmt, uint32_t args, char *out, int max) {
    static uint32_t scratch, putch;
    if (!scratch) {
        scratch = exec_allocmem(4096, MEMF_PUBLIC);
        putch = exec_allocmem(4, MEMF_PUBLIC);
        wr16(putch, 0x16C0); wr16(putch + 2, 0x4E75);           /* move.b d0,(a3)+ ; rts */
    }
    uint32_t sd[8], sa[8];
    memcpy(sd, c->d, sizeof sd); memcpy(sa, c->a, sizeof sa);
    c->a[0] = fmt; c->a[1] = args; c->a[2] = putch; c->a[3] = scratch;
    cpu_call(c, SysBase - lvo_of("exec", "RawDoFmt"));
    uint32_t next = c->d[0];
    memcpy(c->d, sd, sizeof sd); memcpy(c->a, sa, sizeof sa);
    str_get(scratch, out, max);
    return next;
}

typedef struct { uint32_t win; uint32_t gads; int n; } SysReq;

static uint32_t mk_itext(const char *t, int pen) {
    uint32_t it = exec_allocmem(IntuiText_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(it + IntuiText_FrontPen, (uint32_t)pen);
    wr8(it + IntuiText_DrawMode, JAM1);
    wr32(it + IntuiText_IText, str_alloc(t));
    return it;
}

/* body: lines separated by '\n'; buttons: labels; returns the window */
static uint32_t build_request(M68K *c, uint32_t refwin, const char *title, const char *body, char labels[][64], int nb) {
    NScreen *s = refwin && nwin_of(refwin) ? nwin_of(refwin)->s : workbench_screen(1);
    int lines = 1, maxw = 0, cur = 0;
    for (const char *p = body; *p; p++) {
        if (*p == '\n') { lines++; cur = 0; }
        else if (++cur > maxw) maxw = cur;
    }
    int bw = 0;
    for (int i = 0; i < nb; i++) bw += ((int)strlen(labels[i]) * 8 + 16) + 12;
    int bt = border_top(s);
    int width = (maxw * 8 + 40 > bw + 20 ? maxw * 8 + 40 : bw + 20);
    int height = bt + 12 + lines * 9 + 10 + 16 + 6;
    if (width > s->w) width = s->w;
    /* window through the normal path, so input and IDCMP work the same way */
    uint32_t tags = exec_allocmem(8 * 12, MEMF_PUBLIC | MEMF_CLEAR), t = tags;
#define TAG(k, v) do { wr32(t, k); wr32(t + 4, (uint32_t)(v)); t += 8; } while (0)
    TAG(0x80000064, (s->w - width) / 2);
    TAG(0x80000065, (s->h - height) / 2);
    TAG(0x80000066, width);
    TAG(0x80000067, height);
    TAG(0x8000006E, str_alloc(title));
    TAG(0x8000006A, IDCMP_GADGETUP | IDCMP_RAWKEY);
    TAG(0x80000082, 1);                                         /* drag bar */
    TAG(0x80000083, 1);                                         /* depth gadget */
    TAG(0x80000089, 1);                                         /* activate */
    TAG(0x80000079, s->scr);                                    /* public screen */
#undef TAG
    uint32_t win = open_window(c, 0, tags);
    exec_freemem(tags, 8 * 12);
    NWindow *w = nwin_of(win);
    /* body text */
    uint32_t rp = w->rp;
    wr8(rp + RastPort_FgPen, s->pens[TEXTPEN]);
    wr8(rp + RastPort_DrawMode, JAM1);
    int y = bt + 12 + 6, ln = 0;
    char line[256];
    for (const char *p = body;; p++) {
        if (*p == '\n' || !*p) {
            line[ln] = 0;
            int lw = ln * 8;
            rp_move(rp, (width - lw) / 2, y);
            rp_text(rp, line, ln);
            y += 9;
            ln = 0;
            if (!*p) break;
        } else if (ln < 255) line[ln++] = *p;
    }
    /* buttons: ids 1..n-1 left to right, the last one is 0 */
    int gx = 10, gy = height - 6 - 14, step = nb > 1 ? (width - 20 - (int)(strlen(labels[nb - 1]) * 8 + 16)) / (nb - 1) : 0;
    uint32_t first = 0, prev = 0;
    for (int i = 0; i < nb; i++) {
        int gw = (int)strlen(labels[i]) * 8 + 16;
        int x = nb == 1 ? (width - gw) / 2 : (i == nb - 1 ? width - 10 - gw : gx + i * step);
        uint32_t g = exec_allocmem(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
        uint32_t xy = exec_allocmem(40, MEMF_PUBLIC | MEMF_CLEAR), bd = exec_allocmem(Border_SIZEOF * 2, MEMF_PUBLIC | MEMF_CLEAR);
        /* raised border: shine top-left, shadow bottom-right */
        int pts[10] = { 0, 13, 0, 0, gw - 1, 0, gw - 1, 13, 1, 13 };
        for (int k = 0; k < 10; k++) wr16(xy + 2 * (uint32_t)k, (uint32_t)pts[k]);
        wr8(bd + Border_FrontPen, s->pens[SHINEPEN]);
        wr8(bd + Border_DrawMode, JAM1);
        wr8(bd + Border_Count, 3);
        wr32(bd + Border_XY, xy);
        wr32(bd + Border_NextBorder, bd + Border_SIZEOF);
        wr8(bd + Border_SIZEOF + Border_FrontPen, s->pens[SHADOWPEN]);
        wr8(bd + Border_SIZEOF + Border_DrawMode, JAM1);
        wr8(bd + Border_SIZEOF + Border_Count, 3);
        wr32(bd + Border_SIZEOF + Border_XY, xy + 8);
        uint32_t it = mk_itext(labels[i], s->pens[TEXTPEN]);
        wr16(it + IntuiText_LeftEdge, 8);
        wr16(it + IntuiText_TopEdge, 3);
        wr16(g + Gadget_LeftEdge, (uint32_t)x);
        wr16(g + Gadget_LeftEdge + 2, (uint32_t)gy);
        wr16(g + Gadget_Width, (uint32_t)gw);
        wr16(g + Gadget_Width + 2, 14);
        wr16(g + Gadget_Flags, GFLG_GADGHCOMP);
        wr16(g + Gadget_Activation, GACT_RELVERIFY);
        wr16(g + Gadget_GadgetType, GTYP_BOOLGADGET);
        wr32(g + Gadget_GadgetRender, bd);
        wr32(g + Gadget_GadgetText, it);
        wr16(g + Gadget_GadgetID, (uint32_t)(i == nb - 1 ? 0 : i + 1));
        if (prev) wr32(prev + Gadget_NextGadget, g); else first = g;
        prev = g;
    }
    wr32(win + Window_FirstGadget, first);
    gadget_render_all(w, first, -1);
    return win;
}

/* InitRequester(req): cleared */
static uint32_t i_InitRequester(M68K *c) { for (uint32_t i = 0; i < 112; i++) wr8(c->a[0] + i, 0); return 0; }

typedef struct { uint32_t req; NWindow *w; int x, y, wd, ht; uint8_t *pens; } ReqSave;
static ReqSave rsaves[16];

/* Request(req, window): shown in the window, which then only answers its gadgets */
static uint32_t i_Request(M68K *c) {
    uint32_t req = c->a[0], win = c->a[1];
    NWindow *w = nwin_of(win);
    if (!req || !w) return 0;
    uint16_t flags = (uint16_t)rd16(req + 28);
    int wd = (int16_t)rd16(req + 8), ht = (int16_t)rd16(req + 10);
    if (flags & 1) {                                             /* POINTREL: about the window's centre */
        wr16(req + 4, (uint16_t)((w->w - wd) / 2 + (int16_t)rd16(req + 12)));
        wr16(req + 6, (uint16_t)((w->h - ht) / 2 + (int16_t)rd16(req + 14)));
    }
    int x = (int16_t)rd16(req + 4), y = (int16_t)rd16(req + 6);
    /* what is under it comes back at EndRequest */
    ReqSave *sv = 0;
    for (int i = 0; i < 16 && !sv; i++) if (!rsaves[i].req) sv = &rsaves[i];
    BM b;
    if (sv && wd > 0 && ht > 0 && bm_load(rd32(w->rp + RastPort_BitMap), &b)) {
        sv->req = req; sv->w = w; sv->x = x; sv->y = y; sv->wd = wd; sv->ht = ht;
        sv->pens = malloc((size_t)wd * ht);
        for (int j = 0; j < ht; j++) for (int i = 0; i < wd; i++) sv->pens[j * wd + i] = (uint8_t)bm_read(&b, x + i, y + j);
    }
    wr32(req, rd32(win + Window_FirstRequest));                  /* OlderRequest */
    wr32(win + Window_FirstRequest, req);
    wr16(win + Window_ReqCount, rd16(win + Window_ReqCount) + 1);
    wr32(req + 72, win);                                         /* RWindow */
    wr16(req + 28, flags | 0x2000);                              /* REQACTIVE */
    if (!(flags & 0x40) && wd > 0 && ht > 0) rp_fill(w->rp, x, y, x + wd - 1, y + ht - 1, rd8(req + 30), 0);   /* BackFill */
    if (rd32(req + 20)) draw_border(w->rp, rd32(req + 20), x, y);
    if (rd32(req + 24)) draw_itext(w->rp, rd32(req + 24), x, y);
    for (uint32_t g = rd32(req + RQ_GADGET); g; g = rd32(g + Gadget_NextGadget)) render_gadget(w, g);
    gfx_touch();
    send_idcmp(w, 0x80 /* IDCMP_REQSET */, 0, (uint32_t)qualifier, req, 0, 0);
    return DOSTRUE;
}

/* EndRequest(req, window) */
static uint32_t i_EndRequest(M68K *c) {
    uint32_t req = c->a[0], win = c->a[1];
    NWindow *w = nwin_of(win);
    if (!req || !w) return 0;
    for (uint32_t prev = win + Window_FirstRequest, r = rd32(prev); r; prev = r, r = rd32(r))
        if (r == req) { wr32(prev, rd32(r)); wr16(win + Window_ReqCount, rd16(win + Window_ReqCount) - 1); break; }
    wr16(req + 28, rd16(req + 28) & ~0x2000u);
    if (active_strgad && req_of_gadget(w, active_strgad) == 0 && (rd16(active_strgad + Gadget_GadgetType) & GTYP_REQGADGET)) str_deactivate();
    BM b;
    for (int i = 0; i < 16; i++) {
        ReqSave *sv = &rsaves[i];
        if (sv->req != req) continue;
        if (sv->w == w && bm_load(rd32(w->rp + RastPort_BitMap), &b))
            for (int j = 0; j < sv->ht; j++) for (int k = 0; k < sv->wd; k++) bm_pixel(&b, sv->x + k, sv->y + j, sv->pens[j * sv->wd + k], JAM1, 0xFF);
        free(sv->pens);
        memset(sv, 0, sizeof *sv);
    }
    gfx_touch();
    send_idcmp(w, 0x1000 /* IDCMP_REQCLEAR */, 0, (uint32_t)qualifier, req, 0, 0);
    return 0;
}

static void free_request(uint32_t win) {
    NWindow *w = nwin_of(win);
    if (!w) return;
    uint32_t g = rd32(win + Window_FirstGadget);
    wr32(win + Window_FirstGadget, 0);
    close_window(w);
    while (g) {
        uint32_t n = rd32(g + Gadget_NextGadget);
        exec_freemem(g, Gadget_SIZEOF);
        g = n;
    }
}

/* wait for a button; returns its id, or -2 when 'wait' is 0 and nothing came */
static int request_wait(uint32_t win, int wait) {
    uint32_t port = rd32(win + Window_UserPort);
    for (;;) {
        uint32_t m = list_remhead(port + MP_MSGLIST);
        if (!m) {
            if (!wait) return -2;
            exec_waitport(port);
            continue;
        }
        uint32_t cls = rd32(m + IntuiMessage_Class), ia = rd32(m + IntuiMessage_IAddress);
        uint32_t code = rd16(m + IntuiMessage_Code);
        exec_freemem(m, 56);
        if (cls == IDCMP_GADGETUP) return (int)rd16(ia + Gadget_GadgetID);
        if (cls == IDCMP_RAWKEY && (code == 0x44 || code == 0x43)) return 1;   /* Return: first button */
        if (cls == IDCMP_RAWKEY && code == 0x45) return 0;                     /* Esc: last button */
    }
}

static int split_labels(const char *s, char out[][64], int max) {
    int n = 0, l = 0;
    for (;; s++) {
        if (*s == '|' || !*s) {
            out[n][l] = 0;
            if (++n == max || !*s) break;
            l = 0;
        } else if (l < 63) out[n][l++] = *s;
    }
    return n;
}

uint32_t intuition_format(M68K *c, uint32_t fmt, uint32_t args, char *out, int max) { return format(c, fmt, args, out, max); }

static uint32_t easy_build(M68K *c, uint32_t refwin, uint32_t es, uint32_t args) {
    char title[128] = "Requ\xeate syst\xe8me", fmtd[1024], gads[256];
    if (rd32(es + EasyStruct_es_Title)) str_get(rd32(es + EasyStruct_es_Title), title, sizeof title);
    else if (refwin && rd32(refwin + Window_Title)) str_get(rd32(refwin + Window_Title), title, sizeof title);
    uint32_t next = format(c, rd32(es + EasyStruct_es_TextFormat), args, fmtd, sizeof fmtd);
    format(c, rd32(es + EasyStruct_es_GadgetFormat), next, gads, sizeof gads);
    char labels[8][64];
    int nb = split_labels(gads, labels, 8);
    return build_request(c, refwin, title, fmtd, labels, nb);
}

static uint32_t i_EasyRequestArgs(M68K *c) {
    uint32_t win = easy_build(c, c->a[0], c->a[1], c->a[3]);
    int r = request_wait(win, 1);
    free_request(win);
    return (uint32_t)r;
}
static uint32_t i_BuildEasyRequestArgs(M68K *c) { return easy_build(c, c->a[0], c->a[1], c->a[3]); }
static uint32_t i_SysReqHandler(M68K *c) { return (uint32_t)request_wait(c->a[0], (int)c->d[0]); }
static uint32_t i_FreeSysRequest(M68K *c) { free_request(c->a[0]); return 0; }

static void itext_chain(uint32_t it, char *out, int max) {
    out[0] = 0;
    for (; it; it = rd32(it + IntuiText_NextText)) {
        char t[256];
        str_get(rd32(it + IntuiText_IText), t, sizeof t);
        if (out[0]) strncat(out, "\n", (size_t)max - strlen(out) - 1);
        strncat(out, t, (size_t)max - strlen(out) - 1);
    }
}

static uint32_t i_AutoRequest(M68K *c) {
    char body[1024], labels[2][64];
    int nb = 0;
    itext_chain(c->a[1], body, sizeof body);
    if (c->a[2]) { itext_chain(c->a[2], labels[nb], 64); nb++; }
    if (c->a[3]) { itext_chain(c->a[3], labels[nb], 64); nb++; }
    if (!nb) { strcpy(labels[0], "OK"); nb = 1; }
    uint32_t win = build_request(c, c->a[0], "Requ\xeate syst\xe8me", body, labels, nb);
    int r = request_wait(win, 1);
    free_request(win);
    return nb == 2 ? (r == 1 ? DOSTRUE : 0) : (c->a[2] ? DOSTRUE : 0);
}
static uint32_t i_BuildSysRequest(M68K *c) {
    char body[1024], labels[2][64];
    int nb = 0;
    itext_chain(c->a[1], body, sizeof body);
    if (c->a[2]) { itext_chain(c->a[2], labels[nb], 64); nb++; }
    if (c->a[3]) { itext_chain(c->a[3], labels[nb], 64); nb++; }
    if (!nb) { strcpy(labels[0], "OK"); nb = 1; }
    return build_request(c, c->a[0], "Requ\xeate syst\xe8me", body, labels, nb);
}

const NativeEntry intui_input_natives[] = {
    { "DrawBorder", i_DrawBorder }, { "DrawImage", i_DrawImage }, { "DrawImageState", i_DrawImageState },
    { "EraseImage", i_EraseImage }, { "PrintIText", i_PrintIText }, { "IntuiTextLength", i_IntuiTextLength },
    { "AddGadget", i_AddGadget }, { "AddGList", i_AddGList }, { "RemoveGadget", i_RemoveGadget },
    { "RemoveGList", i_RemoveGList }, { "RefreshGList", i_RefreshGList }, { "RefreshGadgets", i_RefreshGadgets },
    { "OnGadget", i_OnGadget }, { "OffGadget", i_OffGadget }, { "ActivateGadget", i_ActivateGadget },
    { "NewModifyProp", i_NewModifyProp }, { "ModifyProp", i_NewModifyProp },
    { "SetMenuStrip", i_SetMenuStrip }, { "ResetMenuStrip", i_SetMenuStrip }, { "ClearMenuStrip", i_ClearMenuStrip },
    { "ItemAddress", i_ItemAddress }, { "OnMenu", i_OnMenu }, { "OffMenu", i_OffMenu },
    { "EasyRequestArgs", i_EasyRequestArgs }, { "BuildEasyRequestArgs", i_BuildEasyRequestArgs },
    { "SysReqHandler", i_SysReqHandler }, { "FreeSysRequest", i_FreeSysRequest },
    { "AutoRequest", i_AutoRequest }, { "InitRequester", i_InitRequester },
    { "Request", i_Request }, { "EndRequest", i_EndRequest }, { "BuildSysRequest", i_BuildSysRequest },
    { 0, 0 }
};
