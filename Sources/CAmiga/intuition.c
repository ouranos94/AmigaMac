/* intuition.library, native: screens, windows, decorations, IDCMP and the
 * compositor that turns them into pixels for macOS.
 *
 * Each window has its own BitMap (window-sized, screen depth): it is never
 * damaged by other windows, so every window behaves like a perfect
 * SMART_REFRESH one. The display composes screen + windows back to front.
 * Structure layouts: ndk_offsets.h. Registers: intuition_lib.fd.
 */
#include "os.h"
#include "intui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t IntuitionBase;
pthread_mutex_t disp_lock = PTHREAD_MUTEX_INITIALIZER;
NScreen *screens;
NWindow *active_win;
uint32_t ireply_port;
static int next_screen_id = 1;

/* IntuitionBase public fields */
#define IB_ACTIVEWINDOW IntuitionBase_ActiveWindow
#define IB_ACTIVESCREEN IntuitionBase_ActiveScreen
#define IB_FIRSTSCREEN IntuitionBase_FirstScreen

/* DrawInfo pens: DETAIL, BLOCK, TEXT, SHINE, SHADOW, FILL, FILLTEXT, BACKGROUND,
   HIGHLIGHTTEXT, BARDETAIL, BARBLOCK, BARTRIM */
static const uint16_t wb_pens[NUMDRIPENS] = { 0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1 };   /* the standard ones: programs copy them */
/* the Workbench colours: blue-grey windows, blue active frames, a light grey desktop */
static const uint32_t wb_palette[8] = { 0xA8B8D0, 0x000000, 0xF4F8FF, 0x6A88E8,
                                        0x7A8CA8, 0x98A8C4, 0xDCDCDC, 0xE8EEFF };

int frame_pen_inactive(NScreen *s) { return s->workbench && s->depth >= 3 ? 5 : s->pens[BACKGROUNDPEN]; }
int desktop_pen(NScreen *s) { return s->workbench && s->depth >= 3 ? 6 : s->pens[BACKGROUNDPEN]; }

static void set_pal(NScreen *s, int n, uint32_t rgb);
/* a pen for a colour: one already showing it, else a free one set to it, else the nearest */
int screen_obtain_pen(NScreen *s, uint32_t rgb) {
    int n = s->depth >= 8 ? 256 : 1 << s->depth;
    for (int i = 0; i < n; i++) if ((i < 8 || s->pen_used[i]) && s->pal[i] == rgb) return i;
    for (int i = 8; i < n; i++) {
        if (i >= 16 && i <= 19) continue;                           /* the pointer's colours */
        if (!s->pen_used[i]) { s->pen_used[i] = 1; set_pal(s, i, rgb); return i; }
    }
    int best = 0;
    long bd = 1L << 30;
    for (int i = 0; i < n; i++) {
        if (i >= 8 && !s->pen_used[i]) continue;
        long dr = (long)(s->pal[i] >> 16 & 0xFF) - (rgb >> 16 & 0xFF), dg = (long)(s->pal[i] >> 8 & 0xFF) - (rgb >> 8 & 0xFF), db = (long)(s->pal[i] & 0xFF) - (rgb & 0xFF);
        long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

NWindow *nwin_of(uint32_t win) {
    for (NScreen *s = screens; s; s = s->next)
        for (NWindow *w = s->wins; w; w = w->next) if (w->win == win) return w;
    return 0;
}

NScreen *nscr_of(uint32_t scr) {
    for (NScreen *s = screens; s; s = s->next) if (s->scr == scr) return s;
    return 0;
}

uint32_t *gfx_palette_of_vp(uint32_t vp) {
    for (NScreen *s = screens; s; s = s->next) if (s->scr + Screen_ViewPort == vp) return s->pal;
    return 0;
}

int border_top(NScreen *s) { return rd8(s->scr + Screen_BarHeight) + 1; }

/* ---- drawing helpers on a BitMap ------------------------------------------- */

static void hline(BM *b, int x0, int x1, int y, int pen) { bm_fill(b, x0, y, x1, y, pen, JAM1, 0xFF); }
static void vline(BM *b, int x, int y0, int y1, int pen) { bm_fill(b, x, y0, x, y1, pen, JAM1, 0xFF); }

/* raised (or recessed) 3D box */
static void bevel(BM *b, int x0, int y0, int x1, int y1, int shine, int shadow, int recessed) {
    int t = recessed ? shadow : shine, d = recessed ? shine : shadow;
    hline(b, x0, x1, y0, t);
    vline(b, x0, y0, y1, t);
    hline(b, x0 + 1, x1, y1, d);
    vline(b, x1, y0 + 1, y1, d);
}

static void bm_text(BM *b, int x, int y, const char *s, int pen) {
    /* y = top of the 8-pixel cell, built-in font */
    for (; *s; s++, x += 8) {
        extern const uint8_t font8_data[256][8];
        const uint8_t *g = font8_data[(uint8_t)*s];
        for (int r = 0; r < 8; r++)
            for (int col = 0; col < 8; col++)
                if (g[r] & (0x80 >> col)) bm_pixel(b, x + col, y + r, pen, JAM1, 0xFF);
    }
}

/* ---- screens ---------------------------------------------------------------- */

void draw_screen_bar(NScreen *s) {
    BM b;
    bm_load(s->scr + Screen_BitMap, &b);
    int bh = rd8(s->scr + Screen_BarHeight);
    if (!(rd16(s->scr + Screen_Flags) & 0x10)) return;          /* SHOWTITLE off */
    bm_fill(&b, 0, 0, s->w - 1, bh - 1, s->pens[BARBLOCKPEN], JAM1, 0xFF);
    hline(&b, 0, s->w - 1, bh, s->pens[BARTRIMPEN]);
    char title[128];
    str_get(rd32(s->scr + Screen_Title), title, sizeof title);
    bm_text(&b, rd8(s->scr + Screen_BarHBorder), (bh - 8) / 2 + 1, title, s->pens[BARDETAILPEN]);
    /* screen depth gadget */
    int gx = s->w - 23;
    bevel(&b, gx, 0, s->w - 1, bh - 1, s->pens[SHINEPEN], s->pens[SHADOWPEN], 0);
    bm_fill(&b, gx + 5, 2, gx + 13, bh - 5, s->pens[SHADOWPEN], JAM1, 0xFF);
    bm_fill(&b, gx + 9, 4, gx + 17, bh - 3, s->pens[SHINEPEN], JAM1, 0xFF);
    gfx_touch();
}

static void set_pal(NScreen *s, int n, uint32_t rgb) {
    if (n >= 256) return;
    s->pal[n] = rgb;
    uint32_t cm = rd32(s->scr + Screen_ViewPort + ViewPort_ColorMap);
    if (cm && n < (int)rd16(cm + ColorMap_Count))
        wr16(rd32(cm + ColorMap_ColorTable) + 2 * (uint32_t)n, (rgb >> 20 & 15) << 8 | (rgb >> 12 & 15) << 4 | (rgb >> 4 & 15));
}

static uint32_t tag_value(uint32_t tags, uint32_t tag, uint32_t def) {
    while (tags) {
        uint32_t t = rd32(tags), d = rd32(tags + 4);
        if (t == 0) break;
        if (t == 2) { tags = d; continue; }               /* TAG_MORE */
        if (t == 3) { tags += 8 * (d + 1); continue; }    /* TAG_SKIP */
        if (t == tag) return d;
        tags += 8;
    }
    return def;
}

#define SA_Left 0x80000021
#define SA_Top 0x80000022
#define SA_Width 0x80000023
#define SA_Height 0x80000024
#define SA_Depth 0x80000025
#define SA_DetailPen 0x80000026
#define SA_BlockPen 0x80000027
#define SA_Title 0x80000028
#define SA_Colors 0x80000029
#define SA_Type 0x8000002D
#define SA_PubName 0x8000002F
#define SA_DisplayID 0x80000032
#define SA_ShowTitle 0x80000036
#define SA_Behind 0x80000037
#define SA_Pens 0x8000003A
#define SA_Colors32 0x80000043
#define SA_LikeWorkbench 0x80000047
#define NOTAG 0xDEADBEEF

extern int gfx_mode_info(uint32_t id, int *w, int *h, int *res, int *lace);

static NScreen *open_screen(int w, int h, int depth, uint32_t modeid, const char *title,
                            int type, int showtitle, uint32_t pens, int workbench) {
    if (depth < 1) depth = 1;
    if (depth > 8) depth = 8;
    int mw, mh, hires = 1, lace = 0;
    if (!gfx_mode_info(modeid, &mw, &mh, &hires, &lace)) { modeid = 0x00029000; hires = 1; lace = 0; }
    NScreen *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->depth = depth; s->hires = hires; s->lace = lace; s->modeid = modeid;
    s->workbench = workbench;
    s->id = next_screen_id++;
    uint32_t scr = exec_allocmem(Screen_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    s->scr = scr;
    wr16(scr + Screen_Width, w);
    wr16(scr + Screen_Height, h);
    wr16(scr + Screen_Flags, (uint32_t)(type & 0xF) | (showtitle ? 0x10 : 0) | (hires ? 0x200 : 0));  /* SHOWTITLE, SCREENHIRES */
    uint32_t t = str_alloc(title);
    wr32(scr + Screen_Title, t);
    wr32(scr + Screen_DefaultTitle, t);
    uint32_t font = gfx_default_font();
    int fh = rd16(font + TextFont_tf_YSize);
    wr8(scr + Screen_BarHeight, fh + 4);                      /* bars with room around the text */
    wr8(scr + Screen_BarVBorder, 1);
    wr8(scr + Screen_BarHBorder, 5);
    wr8(scr + Screen_MenuVBorder, 2);
    wr8(scr + Screen_MenuHBorder, 4);
    wr8(scr + Screen_WBorTop, 2);
    wr8(scr + Screen_WBorLeft, 4);
    wr8(scr + Screen_WBorRight, 4);
    wr8(scr + Screen_WBorBottom, 2);
    /* the screen's TextAttr */
    uint32_t ta = exec_allocmem(8, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(ta, rd32(font + LN_NAME));
    wr16(ta + 4, (uint32_t)fh);
    wr32(scr + Screen_Font, ta);
    /* ViewPort + ColorMap */
    uint32_t vp = scr + Screen_ViewPort;
    wr16(vp + ViewPort_DWidth, w);
    wr16(vp + ViewPort_DHeight, h);
    wr16(vp + ViewPort_Modes, (hires ? 0x8000 : 0) | (lace ? 4 : 0) | (hires == 2 ? 0x20 : 0));
    int ncol = 1 << depth;
    uint32_t cm = exec_allocmem(ColorMap_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(cm + ColorMap_Type, 1);
    wr16(cm + ColorMap_Count, ncol < 32 ? 32 : ncol);
    wr32(cm + ColorMap_ColorTable, exec_allocmem(2u * (ncol < 32 ? 32 : ncol), MEMF_PUBLIC | MEMF_CLEAR));
    wr32(cm + 36, modeid);                                          /* cm_VPModeID */
    wr32(vp + ViewPort_ColorMap, cm);
    /* embedded BitMap and RastPort */
    uint32_t bm = scr + Screen_BitMap;
    int bpr = ((w + 15) >> 4) * 2;
    wr16(bm + BitMap_BytesPerRow, bpr);
    wr16(bm + BitMap_Rows, h);
    wr8(bm + BitMap_Depth, depth);
    for (int i = 0; i < depth; i++) wr32(bm + BitMap_Planes + 4 * i, exec_allocmem((uint32_t)(bpr * h), MEMF_CHIP | MEMF_CLEAR));
    uint32_t rp = scr + Screen_RastPort;
    rp_init(rp);
    wr32(rp + RastPort_BitMap, bm);
    /* pens and palette */
    for (int i = 0; i < NUMDRIPENS; i++) s->pens[i] = wb_pens[i];
    if (pens) for (int i = 0; i < NUMDRIPENS && rd16(pens + 2 * (uint32_t)i) != 0xFFFF; i++) s->pens[i] = (uint16_t)rd16(pens + 2 * (uint32_t)i);
    for (int i = 0; i < 256; i++) set_pal(s, i, i < 8 ? wb_palette[i] : (uint32_t)((i * 37) & 0xFF) << 16 | (uint32_t)((i * 91) & 0xFF) << 8 | (uint32_t)((i * 53) & 0xFF));
    set_pal(s, 17, 0xEE4444); set_pal(s, 18, 0x000000); set_pal(s, 19, 0xEEEECC);   /* the pointer's colours */
    wr8(scr + Screen_DetailPen, s->pens[DETAILPEN]);
    wr8(scr + Screen_BlockPen, s->pens[BLOCKPEN]);
    /* DrawInfo */
    uint32_t di = exec_allocmem(DrawInfo_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR), dp = exec_allocmem(2 * (NUMDRIPENS + 1), MEMF_PUBLIC);
    for (int i = 0; i < NUMDRIPENS; i++) wr16(dp + 2 * (uint32_t)i, s->pens[i]);
    wr16(dp + 2 * NUMDRIPENS, 0xFFFF);
    wr16(di + DrawInfo_dri_Version, 2);
    wr16(di + DrawInfo_dri_NumPens, NUMDRIPENS);
    wr32(di + DrawInfo_dri_Pens, dp);
    wr32(di + DrawInfo_dri_Font, font);
    wr16(di + DrawInfo_dri_Depth, (uint32_t)depth);
    /* ticks per pixel: a hires non-laced pixel is twice as tall as wide */
    wr16(di + DrawInfo_dri_Resolution, hires == 0 ? 44 : hires == 2 ? 11 : 22);
    wr16(di + DrawInfo_dri_Resolution + 2, hires == 3 || lace ? 22 : 44);
    wr32(di + DrawInfo_dri_Flags, 1);                                /* DRIF_NEWLOOK */
    s->drawinfo = di;
    s->ov = malloc((size_t)w * h);
    memset(s->ov, 0xFF, (size_t)w * h);

    pthread_mutex_lock(&disp_lock);
    s->next = screens;
    screens = s;
    wr32(scr + Screen_NextScreen, rd32(IntuitionBase + IB_FIRSTSCREEN));
    wr32(IntuitionBase + IB_FIRSTSCREEN, scr);
    wr32(IntuitionBase + IB_ACTIVESCREEN, scr);
    pthread_mutex_unlock(&disp_lock);
    draw_screen_bar(s);
    host_screen_opened(s, title);
    return s;
}

/* ScreenMode preferences: FORM PREF, PRHD, SCRM {Reserved[4], DisplayID, Width, Height, Depth, Control} */
static int read_screenmode_prefs(const char *apath, uint32_t *id, int *w, int *h, int *depth) {
    char hp[1024];
    if (!dos_resolve(apath, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, "rb");
    if (!f) return 0;
    uint8_t b[512];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "PREF", 4)) return 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t sz = (uint32_t)b[p + 4] << 24 | (uint32_t)b[p + 5] << 16 | (uint32_t)b[p + 6] << 8 | b[p + 7];
        if (!memcmp(b + p, "SCRM", 4) && sz >= 28 && p + 8 + 28 <= n) {
            const uint8_t *d = b + p + 8;
            *id = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
            *w = d[20] << 8 | d[21];
            *h = d[22] << 8 | d[23];
            *depth = d[24] << 8 | d[25];
            return 1;
        }
        p += 8 + ((sz + 1) & ~1u);
    }
    return 0;
}

NScreen *workbench_screen(int open_it) {
    for (NScreen *s = screens; s; s = s->next) if (s->workbench) return s;
    if (!open_it) return 0;
    uint32_t id = 0x50031000;                                   /* AmigaMac: 1280x720 unless the prefs say otherwise */
    int w = 0, h = 0, depth = 8, mw, mh, res, lace;            /* 256 colours: colour icons */
    if (!read_screenmode_prefs("ENV:Sys/screenmode.prefs", &id, &w, &h, &depth))
        read_screenmode_prefs("ENVARC:Sys/screenmode.prefs", &id, &w, &h, &depth);
    if (!gfx_mode_info(id, &mw, &mh, &res, &lace)) { id = 0x00029000; mw = 640; mh = 256; }
    if (w < 320 || w > 4096) w = mw;                           /* 0 or STDSCREENWIDTH: the mode's size */
    if (h < 200 || h > 4096) h = mh;
    if (depth < 1 || depth > 8) depth = 8;
    return open_screen(w, h, depth, id, "AmigaMac Workbench", 1 /* WBENCHSCREEN */, 1, 0, 1);
}

static uint32_t i_OpenScreenTagList(M68K *c) {
    uint32_t ns = c->a[0], tags = c->a[1];
    int w = 640, h = 256, depth = 2, modes = 0x8000, type = 0xF;
    uint32_t title = 0, pens = 0;
    if (ns) {
        w = (int16_t)rd16(ns + NewScreen_Width);
        h = (int16_t)rd16(ns + NewScreen_Width + 2);
        depth = (int16_t)rd16(ns + NewScreen_Depth);
        modes = rd16(ns + NewScreen_ViewModes);
        type = rd16(ns + NewScreen_Type);
        title = rd32(ns + NewScreen_DefaultTitle);
        if (type & 0x1000) tags = tags ? tags : rd32(ns + ExtNewScreen_Extension);   /* NS_EXTENDED */
    }
    w = (int)tag_value(tags, SA_Width, (uint32_t)w);
    h = (int)tag_value(tags, SA_Height, (uint32_t)h);
    depth = (int)tag_value(tags, SA_Depth, (uint32_t)depth);
    title = tag_value(tags, SA_Title, title);
    pens = tag_value(tags, SA_Pens, 0);
    uint32_t id = tag_value(tags, SA_DisplayID, NOTAG);
    if (id == NOTAG) id = 0x00021000 | (modes & (0x8000 | 0x20 | 4));    /* ViewModes: HIRES, SUPERHIRES, LACE */
    if (tag_value(tags, SA_LikeWorkbench, 0)) {
        NScreen *wbs = workbench_screen(1);
        id = wbs->modeid;
        w = wbs->w;
        h = wbs->h;
        depth = (int)tag_value(tags, SA_Depth, (uint32_t)wbs->depth);
    }
    int mw = 640, mh = 256, res, lace;
    if (!gfx_mode_info(id, &mw, &mh, &res, &lace)) { id = 0x00021000 | (modes & (0x8000 | 4)); gfx_mode_info(id, &mw, &mh, &res, &lace); }
    if (w <= 0 || w > 4096) w = mw;                           /* STDSCREENWIDTH (-1) */
    if (h <= 0 || h > 4096) h = mh;
    char t[128] = "";
    if (title) str_get(title, t, sizeof t);
    int showtitle = (int)tag_value(tags, SA_ShowTitle, 1);
    uint32_t cbm = (ns && (type & 0x40)) ? rd32(ns + NewScreen_CustomBitMap) : 0;   /* CUSTOMBITMAP */
    cbm = tag_value(tags, 0x8000002E /* SA_BitMap */, cbm);
    if (cbm) depth = rd8(cbm + BitMap_Depth);
    NScreen *s = open_screen(w, h, depth, id, t, type & 0xF, showtitle, pens, 0);
    if (cbm) {
        /* the program's BitMap is the display: the screen's own planes go */
        uint32_t bm = s->scr + Screen_BitMap;
        BM b;
        bm_load(bm, &b);
        for (int i = 0; i < b.depth; i++) exec_freemem(b.planes[i], (uint32_t)(b.bpr * b.rows));
        for (int i = 0; i < BitMap_SIZEOF; i++) wr8(bm + (uint32_t)i, rd8(cbm + (uint32_t)i));
        wr32(s->scr + Screen_RastPort + RastPort_BitMap, cbm);
        s->custom_bm = cbm;
    }
    /* SA_Colors: ColorSpec {index, r, g, b (4 bits)} until index -1 */
    uint32_t cs = tag_value(tags, SA_Colors, 0);
    for (; cs && (int16_t)rd16(cs) != -1; cs += 8)
        set_pal(s, (int16_t)rd16(cs), (rd16(cs + 2) & 15) * 0x110000u + (rd16(cs + 4) & 15) * 0x1100u + (rd16(cs + 6) & 15) * 0x11u);
    uint32_t c32 = tag_value(tags, SA_Colors32, 0);
    while (c32) {
        uint32_t hdr = rd32(c32), count = hdr >> 16, first = hdr & 0xFFFF;
        if (!count) break;
        c32 += 4;
        for (uint32_t i = 0; i < count; i++, c32 += 12)
            set_pal(s, (int)(first + i), (rd32(c32) >> 24) << 16 | (rd32(c32 + 4) >> 24) << 8 | rd32(c32 + 8) >> 24);
    }
    draw_screen_bar(s);
    return s->scr;
}

static uint32_t i_OpenScreen(M68K *c) { c->a[1] = 0; return i_OpenScreenTagList(c); }

/* QueryOverscan(displayID, rect, oScanType): the whole PAL display */
static uint32_t i_QueryOverscan(M68K *c) {
    uint32_t id = c->a[0], r = c->a[1];
    int w, h, res, lace;
    if (!gfx_mode_info(id, &w, &h, &res, &lace)) { w = (id & 0x8000) ? 640 : 320; h = (id & 4) ? 512 : 256; }
    wr16(r, 0); wr16(r + 2, 0); wr16(r + 4, (uint32_t)(w - 1)); wr16(r + 6, (uint32_t)(h - 1));
    return DOSTRUE;
}

static void free_screen(NScreen *s) {
    pthread_mutex_lock(&disp_lock);
    NScreen **pp = &screens;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    /* unlink from IntuitionBase's chain */
    uint32_t prev = 0, cur = rd32(IntuitionBase + IB_FIRSTSCREEN);
    while (cur && cur != s->scr) { prev = cur; cur = rd32(cur + Screen_NextScreen); }
    if (cur) {
        if (prev) wr32(prev + Screen_NextScreen, rd32(cur + Screen_NextScreen));
        else wr32(IntuitionBase + IB_FIRSTSCREEN, rd32(cur + Screen_NextScreen));
    }
    wr32(IntuitionBase + IB_ACTIVESCREEN, rd32(IntuitionBase + IB_FIRSTSCREEN));
    pthread_mutex_unlock(&disp_lock);
    host_screen_closed(s);
    uint32_t bm = s->scr + Screen_BitMap;
    BM b;
    bm_load(bm, &b);
    if (!s->custom_bm) for (int i = 0; i < b.depth; i++) exec_freemem(b.planes[i], (uint32_t)(b.bpr * b.rows));
    free(s->ov);
    exec_freemem(s->scr, Screen_SIZEOF);
    free(s);
}

static uint32_t i_CloseScreen(M68K *c) {
    NScreen *s = nscr_of(c->a[0]);
    if (!s) return 0;
    if (s->wins) return 0;                                      /* windows still open */
    free_screen(s);
    return DOSTRUE;
}

/* ---- window decorations ------------------------------------------------------ */

static void sys_gadgets(NWindow *w, int *close_w, int *depth_w) {
    uint32_t fl = rd32(w->win + Window_Flags);
    *close_w = (fl & WFLG_CLOSEGADGET) ? 20 : 0;
    *depth_w = (fl & WFLG_DEPTHGADGET) ? 24 : 0;
}
int zoom_width(NWindow *w) { return (rd32(w->win + Window_Flags) & WFLG_HASZOOM) ? 23 : 0; }

/* an outlined box, filled */
static void box(BM *b, int x0, int y0, int x1, int y1, int line, int inside) {
    bm_fill(b, x0, y0, x1, y1, line, JAM1, 0xFF);
    if (x1 - x0 > 1 && y1 - y0 > 1) bm_fill(b, x0 + 1, y0 + 1, x1 - 1, y1 - 1, inside, JAM1, 0xFF);
}

void draw_frame(NWindow *w) {
    uint32_t win = w->win, fl = rd32(win + Window_Flags);
    if (fl & WFLG_BORDERLESS) return;
    NScreen *s = w->s;
    BM b;
    bm_load(w->bm, &b);
    int W = w->w, H = w->h;
    int bl = (int8_t)rd8(win + Window_BorderLeft), bt = (int8_t)rd8(win + Window_BorderTop);
    int br = (int8_t)rd8(win + Window_BorderRight), bb = (int8_t)rd8(win + Window_BorderBottom);
    int active = (fl & WFLG_WINDOWACTIVE) != 0;
    int shine = s->pens[SHINEPEN], shadow = s->pens[SHADOWPEN];
    int fill = active ? s->pens[FILLPEN] : frame_pen_inactive(s);
    /* the frame: title bar and borders in the window's colour */
    bm_fill(&b, 0, 0, W - 1, bt - 1, fill, JAM1, 0xFF);
    bm_fill(&b, 0, 0, bl - 1, H - 1, fill, JAM1, 0xFF);
    bm_fill(&b, W - br, 0, W - 1, H - 1, fill, JAM1, 0xFF);
    bm_fill(&b, 0, H - bb, W - 1, H - 1, fill, JAM1, 0xFF);
    bevel(&b, 0, 0, W - 1, H - 1, shine, shadow, 0);
    if (bl > 1 || bt > 1) bevel(&b, bl - 1, bt - 1, W - br, H - bb, shine, shadow, 1);
    int cw, dw, zw = zoom_width(w);
    sys_gadgets(w, &cw, &dw);
    if (bt > 2) {
        /* the gadgets' glyphs keep two pixels from the bar's edges */
        int gh = bt - 2, cy = gh / 2, top = 2, bot = gh - 2, ih = bot - top;
        if (cw) {                                               /* close: a small box with a dot */
            bevel(&b, 0, 0, cw - 1, gh, shine, shadow, 0);
            int cx = cw / 2, r = ih / 2 > 3 ? 3 : ih / 2;
            box(&b, cx - r, cy - r, cx + r, cy + r, shadow, shine);
            bm_fill(&b, cx - 1, cy - 1, cx + 1, cy + 1, shadow, JAM1, 0xFF);
        }
        if (dw) {                                               /* depth: two overlapping windows */
            int gx = W - dw;
            bevel(&b, gx, 0, W - 1, gh, shine, shadow, 0);
            int x0 = gx + 5, bh = ih * 2 / 3;
            box(&b, x0, top, x0 + 9, top + bh, shadow, fill);
            box(&b, x0 + 5, bot - bh, x0 + 14, bot, shadow, shine);
        }
        if (zw) {                                               /* zoom: a small window in a bigger one */
            int gx = W - dw - zw;
            bevel(&b, gx, 0, gx + zw - 1, gh, shine, shadow, 0);
            int x0 = gx + 5;
            box(&b, x0, top, x0 + 13, bot, shadow, shine);
            box(&b, x0, top, x0 + 6, top + ih / 2, shadow, fill);
        }
        char title[128];
        str_get(rd32(win + Window_Title), title, sizeof title);
        int maxc = (W - cw - dw - zw - 8) / 8;
        if (maxc < 0) maxc = 0;
        if ((int)strlen(title) > maxc) title[maxc] = 0;
        bm_text(&b, cw + 6, (bt - 8) / 2, title, active ? s->pens[FILLTEXTPEN] : s->pens[TEXTPEN]);
    }
    if (fl & WFLG_SIZEGADGET) {                                 /* size: a corner, bottom right */
        int gx = W - (br > 4 ? br : 18), gy = H - (bb > 2 ? bb : 10);
        bm_fill(&b, gx, gy, W - 1, H - 1, fill, JAM1, 0xFF);
        bevel(&b, gx, gy, W - 1, H - 1, shine, shadow, 0);
        int x1 = W - 4, y1 = H - 3, n = (H - gy) - 5;
        for (int i = 0; i < n; i++) hline(&b, x1 - i * 2, x1, y1 - i, shadow);
        for (int i = 0; i < n; i++) bm_pixel(&b, x1 - i * 2 - 1, y1 - i, shine, JAM1, 0xFF);
    }
    gfx_touch();
}

/* ---- windows ---------------------------------------------------------------- */

#define WA_Left 0x80000064
#define WA_Top 0x80000065
#define WA_Width 0x80000066
#define WA_Height 0x80000067
#define WA_DetailPen 0x80000068
#define WA_BlockPen 0x80000069
#define WA_IDCMP 0x8000006A
#define WA_Flags 0x8000006B
#define WA_Gadgets 0x8000006C
#define WA_Title 0x8000006E
#define WA_ScreenTitle 0x8000006F
#define WA_CustomScreen 0x80000070
#define WA_MinWidth 0x80000072
#define WA_MinHeight 0x80000073
#define WA_MaxWidth 0x80000074
#define WA_MaxHeight 0x80000075
#define WA_InnerWidth 0x80000076
#define WA_InnerHeight 0x80000077
#define WA_PubScreenName 0x80000078
#define WA_PubScreen 0x80000079
#define WA_SizeGadget 0x80000081
#define WA_DragBar 0x80000082
#define WA_DepthGadget 0x80000083
#define WA_CloseGadget 0x80000084
#define WA_Backdrop 0x80000085
#define WA_ReportMouse 0x80000086
#define WA_NoCareRefresh 0x80000087
#define WA_Borderless 0x80000088
#define WA_Activate 0x80000089
#define WA_RMBTrap 0x8000008A
#define WA_SimpleRefresh 0x8000008C
#define WA_SmartRefresh 0x8000008D
#define WA_SizeBRight 0x8000008E
#define WA_SizeBBottom 0x8000008F
#define WA_GimmeZeroZero 0x80000091
#define WA_NewLookMenus 0x80000093

static uint32_t flag_tag(uint32_t tags, uint32_t tag, uint32_t flags, uint32_t bit) {
    uint32_t v = tag_value(tags, tag, NOTAG);
    if (v == NOTAG) return flags;
    return v ? flags | bit : flags & ~bit;
}

static uint32_t new_port_for(uint32_t task) {
    /* a MsgPort signalling 'task' with a fresh signal of that task */
    uint32_t p = exec_allocmem(MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t alloc = rd32(task + TC_SIGALLOC);
    int sig;
    for (sig = 31; sig >= 16 && (alloc & (1u << sig)); sig--) ;
    if (sig < 16) sig = 16;
    wr32(task + TC_SIGALLOC, alloc | (1u << sig));
    wr32(task + TC_SIGRECVD, rd32(task + TC_SIGRECVD) & ~(1u << sig));
    wr8(p + LN_TYPE, NT_MSGPORT);
    wr8(p + MP_FLAGS, PA_SIGNAL);
    wr8(p + MP_SIGBIT, (uint32_t)sig);
    wr32(p + MP_SIGTASK, task);
    list_new(p + MP_MSGLIST);
    return p;
}

static void free_port(uint32_t p) {
    uint32_t task = rd32(p + MP_SIGTASK);
    if (task) wr32(task + TC_SIGALLOC, rd32(task + TC_SIGALLOC) & ~(1u << rd8(p + MP_SIGBIT)));
    exec_freemem(p, MP_SIZE);
}

/* drop this window's queued messages from a (possibly shared) port */
static void strip_messages(uint32_t port, uint32_t win) {
    uint32_t n = rd32(port + MP_MSGLIST + LH_HEAD);
    while (rd32(n + LN_SUCC)) {
        uint32_t next = rd32(n + LN_SUCC);
        if (rd32(n + IntuiMessage_IDCMPWindow) == win) { list_remove(n); exec_freemem(n, 56); }
        n = next;
    }
}

uint32_t open_window(M68K *c, uint32_t nw, uint32_t tags) {
    (void)c;
    int x = 0, y = 0, w = 200, h = 100, detail = 0xFF, block = 0xFF;
    uint32_t idcmp = 0, flags = 0, gadgets = 0, title = 0, scr = 0, checkmark = 0;
    int minw = 0, minh = 0, maxw = 0, maxh = 0, type = 1;
    if (nw) {
        x = (int16_t)rd16(nw + NewWindow_LeftEdge);
        y = (int16_t)rd16(nw + NewWindow_LeftEdge + 2);
        w = (int16_t)rd16(nw + NewWindow_Width);
        h = (int16_t)rd16(nw + NewWindow_Width + 2);
        detail = rd8(nw + NewWindow_DetailPen);
        block = rd8(nw + NewWindow_DetailPen + 1);
        idcmp = rd32(nw + NewWindow_IDCMPFlags);
        flags = rd32(nw + NewWindow_Flags);
        gadgets = rd32(nw + NewWindow_FirstGadget);
        checkmark = rd32(nw + NewWindow_CheckMark);
        title = rd32(nw + NewWindow_Title);
        scr = rd32(nw + NewWindow_Screen);
        minw = (int16_t)rd16(nw + NewWindow_MinWidth);
        minh = (int16_t)rd16(nw + NewWindow_MinWidth + 2);
        maxw = rd16(nw + NewWindow_MaxWidth);
        maxh = rd16(nw + NewWindow_MaxWidth + 2);
        type = rd16(nw + NewWindow_Type);
        if ((flags & WFLG_NW_EXTENDED) && !tags) tags = rd32(nw + ExtNewWindow_Extension);
    }
    x = (int)tag_value(tags, WA_Left, (uint32_t)x);
    y = (int)tag_value(tags, WA_Top, (uint32_t)y);
    w = (int)tag_value(tags, WA_Width, (uint32_t)w);
    h = (int)tag_value(tags, WA_Height, (uint32_t)h);
    detail = (int)tag_value(tags, WA_DetailPen, (uint32_t)detail);
    block = (int)tag_value(tags, WA_BlockPen, (uint32_t)block);
    idcmp = tag_value(tags, WA_IDCMP, idcmp);
    flags = tag_value(tags, WA_Flags, flags);
    gadgets = tag_value(tags, WA_Gadgets, gadgets);
    title = tag_value(tags, WA_Title, title);
    minw = (int)tag_value(tags, WA_MinWidth, (uint32_t)minw);
    minh = (int)tag_value(tags, WA_MinHeight, (uint32_t)minh);
    maxw = (int)tag_value(tags, WA_MaxWidth, (uint32_t)maxw);
    maxh = (int)tag_value(tags, WA_MaxHeight, (uint32_t)maxh);
    flags = flag_tag(tags, WA_SizeGadget, flags, WFLG_SIZEGADGET);
    flags = flag_tag(tags, WA_DragBar, flags, WFLG_DRAGBAR);
    flags = flag_tag(tags, WA_DepthGadget, flags, WFLG_DEPTHGADGET);
    flags = flag_tag(tags, WA_CloseGadget, flags, WFLG_CLOSEGADGET);
    flags = flag_tag(tags, WA_Backdrop, flags, WFLG_BACKDROP);
    flags = flag_tag(tags, WA_ReportMouse, flags, WFLG_REPORTMOUSE);
    flags = flag_tag(tags, WA_NoCareRefresh, flags, WFLG_NOCAREREFRESH);
    flags = flag_tag(tags, WA_Borderless, flags, WFLG_BORDERLESS);
    flags = flag_tag(tags, WA_Activate, flags, WFLG_ACTIVATE);
    flags = flag_tag(tags, WA_RMBTrap, flags, WFLG_RMBTRAP);
    flags = flag_tag(tags, WA_SizeBRight, flags, WFLG_SIZEBRIGHT);
    flags = flag_tag(tags, WA_SizeBBottom, flags, WFLG_SIZEBBOTTOM);
    flags = flag_tag(tags, WA_GimmeZeroZero, flags, WFLG_GIMMEZEROZERO);
    uint32_t zoom = tag_value(tags, 0x8000007D /* WA_Zoom */, 0);
    if (zoom) flags |= WFLG_HASZOOM;
    if (tag_value(tags, WA_SimpleRefresh, 0)) flags |= WFLG_SIMPLE_REFRESH;

    /* which screen */
    NScreen *s = 0;
    uint32_t cs = tag_value(tags, WA_CustomScreen, 0), ps = tag_value(tags, WA_PubScreen, 0);
    if (cs) s = nscr_of(cs);
    else if (ps) s = nscr_of(ps);
    else if ((type & 0xF) == 0xF && scr) s = nscr_of(scr);     /* CUSTOMSCREEN */
    if (!s) s = workbench_screen(1);

    /* borders */
    int bl = 0, bt = 0, br = 0, bb = 0;
    if (!(flags & WFLG_BORDERLESS)) {
        bl = rd8(s->scr + Screen_WBorLeft);
        br = rd8(s->scr + Screen_WBorRight);
        bb = rd8(s->scr + Screen_WBorBottom);
        bt = (title || (flags & (WFLG_DRAGBAR | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET))) ? border_top(s) : rd8(s->scr + Screen_WBorTop);
        if (flags & WFLG_SIZEGADGET) {
            if (flags & WFLG_SIZEBBOTTOM) bb = 10;
            if ((flags & WFLG_SIZEBRIGHT) || !(flags & WFLG_SIZEBBOTTOM)) br = 18;
        }
    }
    uint32_t iw = tag_value(tags, WA_InnerWidth, NOTAG), ih = tag_value(tags, WA_InnerHeight, NOTAG);
    if (iw != NOTAG) w = (int)iw + bl + br;
    if (ih != NOTAG) h = (int)ih + bt + bb;
    if (w < bl + br + 1) w = bl + br + 1;
    if (h < bt + bb + 1) h = bt + bb + 1;
    if (w > s->w) w = s->w;
    if (h > s->h) h = s->h;
    if (x + w > s->w) x = s->w - w;
    if (y + h > s->h) y = s->h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    uint32_t win = exec_allocmem(Window_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t rp = exec_allocmem(RastPort_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t layer = exec_allocmem(Layer_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    int shared = (flags & WFLG_BORDERLESS) && x == 0 && y == 0 && !s->workbench;
    /* never reallocated; a borderless window from 0,0 of a custom screen is the screen's bitmap, as on
       the Amiga where a visible window is the screen's bitmap: programs draw into either */
    uint32_t bm = shared ? rd32(s->scr + Screen_RastPort + RastPort_BitMap)
                         : gfx_alloc_bitmap(w > s->w ? w : s->w, h > s->h ? h : s->h, s->depth, 1);
    NWindow *nwn = calloc(1, sizeof *nwn);
    nwn->win = win; nwn->rp = rp; nwn->layer = layer; nwn->bm = bm; nwn->s = s;
    nwn->shared_bm = shared;
    if (getenv("AMIGAMAC_TRACE")) os_log("OpenWindow : %d,%d %dx%d flags %08X%s", x, y, w, h, flags, shared ? " (bitmap de l'écran)" : "");
    nwn->task = this_task();
    nwn->x = x; nwn->y = y; nwn->w = w; nwn->h = h;

    wr16(win + Window_LeftEdge, (uint32_t)x);
    wr16(win + Window_TopEdge, (uint32_t)y);
    wr16(win + Window_Width, (uint32_t)w);
    wr16(win + Window_Height, (uint32_t)h);
    wr16(win + Window_MinWidth, (uint32_t)(minw ? minw : w));
    wr16(win + Window_MinHeight, (uint32_t)(minh ? minh : h));
    wr16(win + Window_MaxWidth, (uint32_t)(maxw ? maxw : w));
    wr16(win + Window_MaxHeight, (uint32_t)(maxh ? maxh : h));
    wr32(win + Window_Flags, flags & ~WFLG_WINDOWACTIVE);
    wr32(win + Window_Title, title);
    wr32(win + Window_WScreen, s->scr);
    wr32(win + Window_RPort, rp);
    wr8(win + Window_BorderLeft, (uint32_t)bl);
    wr8(win + Window_BorderTop, (uint32_t)bt);
    wr8(win + Window_BorderRight, (uint32_t)br);
    wr8(win + Window_BorderBottom, (uint32_t)bb);
    wr32(win + Window_BorderRPort, rp);
    wr32(win + Window_FirstGadget, gadgets);
    wr32(win + Window_IDCMPFlags, idcmp);
    wr8(win + Window_DetailPen, (uint32_t)(detail == 0xFF ? s->pens[DETAILPEN] : detail));
    wr8(win + Window_BlockPen, (uint32_t)(block == 0xFF ? s->pens[BLOCKPEN] : block));
    wr32(win + Window_CheckMark, checkmark);
    wr32(win + Window_ScreenTitle, tag_value(tags, WA_ScreenTitle, 0));
    wr16(win + Window_GZZWidth, (uint32_t)(w - bl - br));
    wr16(win + Window_GZZHeight, (uint32_t)(h - bt - bb));
    wr32(win + Window_WLayer, layer);
    wr32(win + Window_IFont, gfx_default_font());
    wr32(win + Window_WindowPort, ireply_port);
    if (idcmp) { wr32(win + Window_UserPort, new_port_for(nwn->task)); nwn->own_port = 1; }
    if (flags & WFLG_GIMMEZEROZERO) {
        /* the program draws into its own inner bitmap, laid over the frame */
        int iw2 = w - bl - br, ih2 = h - bt - bb;
        nwn->ibm = gfx_alloc_bitmap(s->w, s->h, s->depth, 1);
        nwn->irp = exec_allocmem(RastPort_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
        rp_init(nwn->irp);
        wr32(nwn->irp + RastPort_BitMap, nwn->ibm);
        wr32(nwn->irp + RastPort_Layer, layer);
        wr8(nwn->irp + RastPort_FgPen, s->pens[TEXTPEN]);
        wr32(win + Window_RPort, nwn->irp);
        BM ib;
        bm_load(nwn->ibm, &ib);
        bm_fill(&ib, 0, 0, iw2 - 1, ih2 - 1, s->pens[BACKGROUNDPEN], JAM1, 0xFF);
    }

    rp_init(rp);
    wr32(rp + RastPort_BitMap, bm);
    wr32(rp + RastPort_Layer, layer);
    wr8(rp + RastPort_FgPen, s->pens[TEXTPEN]);
    wr8(rp + RastPort_BgPen, s->pens[BACKGROUNDPEN]);
    wr32(layer + Layer_rp, rp);
    wr16(layer + Layer_bounds, (uint32_t)x);
    wr16(layer + Layer_bounds + 2, (uint32_t)y);
    wr16(layer + Layer_bounds + 4, (uint32_t)(x + w - 1));
    wr16(layer + Layer_bounds + 6, (uint32_t)(y + h - 1));
    wr32(layer + Layer_Window, win);
    wr16(layer + Layer_Width, (uint32_t)w);
    wr16(layer + Layer_Height, (uint32_t)h);
    wr16(layer + Layer_Flags, 0x02);                            /* LAYERSMART */
    {   /* background pen 0 is the window's backfill */
        BM b;
        bm_load(bm, &b);
        bm_fill(&b, 0, 0, w - 1, h - 1, s->pens[BACKGROUNDPEN], JAM1, 0xFF);
    }

    pthread_mutex_lock(&disp_lock);
    if (flags & WFLG_BACKDROP) {                                /* behind every other window */
        NWindow **pp = &s->wins;
        while (*pp) pp = &(*pp)->next;
        *pp = nwn;
    } else {
        nwn->next = s->wins;
        s->wins = nwn;
    }
    wr32(win + Window_NextWindow, rd32(s->scr + Screen_FirstWindow));
    wr32(s->scr + Screen_FirstWindow, win);
    pthread_mutex_unlock(&disp_lock);

    if (zoom) {                                                     /* the other box: Left, Top, Width, Height */
        nwn->zx = (int16_t)rd16(zoom); nwn->zy = (int16_t)rd16(zoom + 2);
        nwn->zw = (int16_t)rd16(zoom + 4); nwn->zh = (int16_t)rd16(zoom + 6);
        if (nwn->zx < 0) nwn->zx = x;                               /* ~0: keep the position */
        if (nwn->zy < 0) nwn->zy = y;
    }
    if (tags && tag_value(tags, 0x80000098 /* WA_BusyPointer */, 0)) nwn->ptr_kind = 2;
    draw_frame(nwn);
    gadget_render_all(nwn, gadgets, -1);
    if (flags & WFLG_ACTIVATE) win_activate(nwn);
    gfx_touch();
    return win;
}

static uint32_t i_OpenWindow(M68K *c) { return open_window(c, c->a[0], 0); }
static uint32_t i_OpenWindowTagList(M68K *c) { return open_window(c, c->a[0], c->a[1]); }

void close_window(NWindow *w) {
    uint32_t win = w->win;
    NScreen *s = w->s;
    if (active_win == w) {
        active_win = 0;
        wr32(IntuitionBase + IB_ACTIVEWINDOW, 0);
    }
    pthread_mutex_lock(&disp_lock);
    NWindow **pp = &s->wins;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    uint32_t prev = 0, cur = rd32(s->scr + Screen_FirstWindow);
    while (cur && cur != win) { prev = cur; cur = rd32(cur + Window_NextWindow); }
    if (cur) {
        if (prev) wr32(prev + Window_NextWindow, rd32(cur + Window_NextWindow));
        else wr32(s->scr + Screen_FirstWindow, rd32(cur + Window_NextWindow));
    }
    pthread_mutex_unlock(&disp_lock);
    uint32_t port = rd32(win + Window_UserPort);
    if (port) {
        strip_messages(port, win);
        if (w->own_port) free_port(port);
    }
    if (!w->shared_bm) gfx_free_bitmap(w->bm);
    exec_freemem(w->rp, RastPort_SIZEOF);
    if (w->ibm) { gfx_free_bitmap(w->ibm); exec_freemem(w->irp, RastPort_SIZEOF); }
    exec_freemem(w->layer, Layer_SIZEOF);
    exec_freemem(win, Window_SIZEOF);
    free(w->ptr_pix);
    free(w);
    /* the next window becomes active, like on the Workbench */
    if (!active_win && s->wins) win_activate(s->wins);
    if (!active_win) pointer_update();
    gfx_touch();
}

static uint32_t i_CloseWindow(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (w) close_window(w);
    return 0;
}

void win_activate(NWindow *w) {
    if (active_win == w) return;
    NWindow *old = active_win;
    if (old) {
        wr32(old->win + Window_Flags, rd32(old->win + Window_Flags) & ~WFLG_WINDOWACTIVE);
        draw_frame(old);
        gadget_render_all(old, rd32(old->win + Window_FirstGadget), -1);
        send_idcmp(old, IDCMP_INACTIVEWINDOW, 0, 0, 0, 0, 0);
    }
    active_win = w;
    wr32(IntuitionBase + IB_ACTIVEWINDOW, w ? w->win : 0);
    if (w) {
        wr32(w->win + Window_Flags, rd32(w->win + Window_Flags) | WFLG_WINDOWACTIVE);
        wr32(IntuitionBase + IB_ACTIVESCREEN, w->s->scr);
        draw_frame(w);
        gadget_render_all(w, rd32(w->win + Window_FirstGadget), -1);
        send_idcmp(w, IDCMP_ACTIVEWINDOW, 0, 0, 0, 0, 0);
    }
    pointer_update();
}

void win_to_front(NWindow *w) {
    NScreen *s = w->s;
    if (rd32(w->win + Window_Flags) & WFLG_BACKDROP) return;
    pthread_mutex_lock(&disp_lock);
    NWindow **pp = &s->wins;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) { *pp = w->next; w->next = s->wins; s->wins = w; }
    pthread_mutex_unlock(&disp_lock);
    gfx_touch();
}

void win_to_back(NWindow *w) {
    NScreen *s = w->s;
    pthread_mutex_lock(&disp_lock);
    NWindow **pp = &s->wins;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) {
        *pp = w->next;
        w->next = 0;
        /* in front of the backdrop windows */
        pp = &s->wins;
        while (*pp && !(rd32((*pp)->win + Window_Flags) & WFLG_BACKDROP)) pp = &(*pp)->next;
        w->next = *pp;
        *pp = w;
    }
    pthread_mutex_unlock(&disp_lock);
    gfx_touch();
}

void win_move(NWindow *w, int x, int y) {
    NScreen *s = w->s;
    if (x + w->w > s->w) x = s->w - w->w;
    if (y + w->h > s->h) y = s->h - w->h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    pthread_mutex_lock(&disp_lock);
    w->x = x; w->y = y;
    pthread_mutex_unlock(&disp_lock);
    wr16(w->win + Window_LeftEdge, (uint32_t)x);
    wr16(w->win + Window_TopEdge, (uint32_t)y);
    wr16(w->layer + Layer_bounds, (uint32_t)x);
    wr16(w->layer + Layer_bounds + 2, (uint32_t)y);
    wr16(w->layer + Layer_bounds + 4, (uint32_t)(x + w->w - 1));
    wr16(w->layer + Layer_bounds + 6, (uint32_t)(y + w->h - 1));
    send_idcmp(w, IDCMP_CHANGEWINDOW, 0, 0, 0, 0, 0);
    gfx_touch();
}

void win_resize(NWindow *w, int width, int height) {
    uint32_t win = w->win;
    int minw = rd16(win + Window_MinWidth), minh = rd16(win + Window_MinHeight);
    int maxw = rd16(win + Window_MaxWidth), maxh = rd16(win + Window_MaxHeight);
    if (maxw == 0xFFFF || maxw == 0) maxw = w->s->w;
    if (maxh == 0xFFFF || maxh == 0) maxh = w->s->h;
    if (width < minw) width = minw;
    if (height < minh) height = minh;
    if (width > maxw) width = maxw;
    if (height > maxh) height = maxh;
    if (w->x + width > w->s->w) width = w->s->w - w->x;
    if (w->y + height > w->s->h) height = w->s->h - w->y;
    if (width == w->w && height == w->h) return;
    /* the bitmaps are screen-sized from the start and never move (programs keep
       copies of the RastPort): only what the old frame covered is cleared */
    int bl = (int8_t)rd8(win + Window_BorderLeft), bt = (int8_t)rd8(win + Window_BorderTop);
    int br = (int8_t)rd8(win + Window_BorderRight), bb = (int8_t)rd8(win + Window_BorderBottom);
    int ow = w->w, oh = w->h, bg = w->s->pens[BACKGROUNDPEN];
    BM b;
    bm_load(w->bm, &b);
    /* old right and bottom borders, and anything newly uncovered */
    bm_fill(&b, ow - br, 0, width - 1, height - 1, bg, JAM1, 0xFF);
    bm_fill(&b, 0, oh - bb, width - 1, height - 1, bg, JAM1, 0xFF);
    if (w->ibm) {
        BM ib;
        bm_load(w->ibm, &ib);
        bm_fill(&ib, ow - bl - br, 0, width - 1, height - 1, bg, JAM1, 0xFF);
        bm_fill(&ib, 0, oh - bt - bb, width - 1, height - 1, bg, JAM1, 0xFF);
    }
    pthread_mutex_lock(&disp_lock);
    w->w = width;
    w->h = height;
    pthread_mutex_unlock(&disp_lock);
    wr16(win + Window_Width, (uint32_t)width);
    wr16(win + Window_Height, (uint32_t)height);
    wr16(win + Window_GZZWidth, (uint32_t)(width - bl - br));
    wr16(win + Window_GZZHeight, (uint32_t)(height - bt - bb));
    wr16(w->layer + Layer_Width, (uint32_t)width);
    wr16(w->layer + Layer_Height, (uint32_t)height);
    wr16(w->layer + Layer_bounds + 4, (uint32_t)(w->x + width - 1));
    wr16(w->layer + Layer_bounds + 6, (uint32_t)(w->y + height - 1));
    draw_frame(w);
    gadget_render_all(w, rd32(win + Window_FirstGadget), -1);
    send_idcmp(w, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
    send_idcmp(w, IDCMP_REFRESHWINDOW, 0, 0, 0, 0, 0);
    send_idcmp(w, IDCMP_CHANGEWINDOW, 0, 0, 0, 0, 0);
    gfx_touch();
}

/* ---- IDCMP ---------------------------------------------------------------- */

void send_idcmp(NWindow *w, uint32_t cls, uint32_t code, uint32_t qual, uint32_t iaddr, int mx, int my) {
    uint32_t win = w->win, port = rd32(win + Window_UserPort);
    if (w->hook && cls == IDCMP_CLOSEWINDOW) w->hook(w, HOOK_CLOSE, 0, 0, 0);
    if (w->hook && cls == IDCMP_NEWSIZE) w->hook(w, HOOK_NEWSIZE, 0, 0, 0);
    if (!port || !(rd32(win + Window_IDCMPFlags) & cls)) return;
    uint32_t m = exec_allocmem(56, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(m + LN_TYPE, NT_MESSAGE);
    wr32(m + MN_REPLYPORT, ireply_port);
    wr16(m + MN_LENGTH, 56);
    wr32(m + IntuiMessage_Class, cls);
    wr16(m + IntuiMessage_Code, code);
    wr16(m + IntuiMessage_Qualifier, qual);
    wr32(m + IntuiMessage_IAddress, iaddr);
    wr16(m + IntuiMessage_MouseX, (uint32_t)mx);
    wr16(m + IntuiMessage_MouseY, (uint32_t)my);
    uint32_t secs = timer_systime_secs();
    wr32(m + IntuiMessage_Seconds, secs);
    wr32(m + IntuiMessage_Micros, (uint32_t)(now_us() % 1000000u));
    wr32(m + IntuiMessage_IDCMPWindow, win);
    /* like PutMsg */
    list_addtail(port + MP_MSGLIST, m);
    if ((rd8(port + MP_FLAGS) & 3) == PA_SIGNAL) sched_signal(rd32(port + MP_SIGTASK), 1u << rd8(port + MP_SIGBIT));
}

static uint32_t i_ModifyIDCMP(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (!w) return 0;
    uint32_t flags = c->d[0], port = rd32(w->win + Window_UserPort);
    wr32(w->win + Window_IDCMPFlags, flags);
    if (flags && !port) { wr32(w->win + Window_UserPort, new_port_for(this_task())); w->own_port = 1; }
    if (!flags && port && w->own_port) {
        strip_messages(port, w->win);
        free_port(port);
        wr32(w->win + Window_UserPort, 0);
        w->own_port = 0;
    }
    return DOSTRUE;
}

/* ---- window manipulation from programs ------------------------------------ */

static uint32_t i_WindowToFront(M68K *c) { NWindow *w = nwin_of(c->a[0]); if (w) win_to_front(w); return 0; }
static uint32_t i_WindowToBack(M68K *c) { NWindow *w = nwin_of(c->a[0]); if (w) win_to_back(w); return 0; }
static uint32_t i_ActivateWindow(M68K *c) { NWindow *w = nwin_of(c->a[0]); if (w) win_activate(w); return 0; }
static uint32_t i_MoveWindow(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (w) win_move(w, w->x + (int16_t)c->d[0], w->y + (int16_t)c->d[1]);
    return 0;
}
static uint32_t i_SizeWindow(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (w) win_resize(w, w->w + (int16_t)c->d[0], w->h + (int16_t)c->d[1]);
    return 0;
}
static uint32_t i_ChangeWindowBox(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (!w) return 0;
    win_move(w, (int16_t)c->d[0], (int16_t)c->d[1]);
    win_resize(w, (int16_t)c->d[2], (int16_t)c->d[3]);
    return 0;
}
static uint32_t i_WindowLimits(M68K *c) {
    uint32_t win = c->a[0];
    if (c->d[0]) wr16(win + Window_MinWidth, c->d[0]);
    if (c->d[1]) wr16(win + Window_MinHeight, c->d[1]);
    if (c->d[2]) wr16(win + Window_MaxWidth, c->d[2]);
    if (c->d[3]) wr16(win + Window_MaxHeight, c->d[3]);
    return DOSTRUE;
}
static uint32_t i_SetWindowTitles(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (!w) return 0;
    if (c->a[1] != 0xFFFFFFFF) wr32(w->win + Window_Title, c->a[1]);
    if (c->a[2] != 0xFFFFFFFF) wr32(w->win + Window_ScreenTitle, c->a[2]);
    draw_frame(w);
    gadget_render_all(w, rd32(w->win + Window_FirstGadget), -1);
    return 0;
}
static uint32_t i_RefreshWindowFrame(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (w) { draw_frame(w); gadget_render_all(w, rd32(w->win + Window_FirstGadget), -1); }
    return 0;
}
static uint32_t i_BeginRefresh(M68K *c) { (void)c; return 0; }
static uint32_t i_EndRefresh(M68K *c) { (void)c; return 0; }
static uint32_t i_ReportMouse(M68K *c) {
    /* ReportMouse(flag D0, window A0) */
    uint32_t win = c->a[0], fl = rd32(win + Window_Flags);
    wr32(win + Window_Flags, c->d[0] ? fl | WFLG_REPORTMOUSE : fl & ~WFLG_REPORTMOUSE);
    return 0;
}
static uint32_t i_ViewPortAddress(M68K *c) { return rd32(c->a[0] + Window_WScreen) + Screen_ViewPort; }
static uint32_t i_ViewAddress(M68K *c) { (void)c; return IntuitionBase + 34; }
static uint32_t i_ScrollWindowRaster(M68K *c) {
    NWindow *w = nwin_of(c->a[1]);
    if (!w) return 0;
    uint32_t sa1 = c->a[1];
    c->a[1] = w->rp;
    cpu_call(c, GfxBase - lvo_of("graphics", "ScrollRaster"));
    c->a[1] = sa1;
    return 0;
}

/* ---- screens from programs ------------------------------------------------- */

/* PubScreenStatus(screen, flags): the old flags; the Workbench screen stays public */
static uint32_t i_PubScreenStatus(M68K *c) { (void)c; return 0; }

static uint32_t i_LockPubScreen(M68K *c) {
    char name[64] = "";
    if (c->a[0]) str_get(c->a[0], name, sizeof name);
    if (c->a[0] && strcasecmp(name, "Workbench")) {
        os_log("LockPubScreen(\"%s\") : écran public inconnu", name);
        return 0;
    }
    NScreen *s = workbench_screen(1);
    s->locks++;
    return s->scr;
}
static uint32_t i_UnlockPubScreen(M68K *c) {
    NScreen *s = c->a[1] ? nscr_of(c->a[1]) : workbench_screen(0);
    if (s && s->locks) s->locks--;
    return 0;
}
/* reopen the Workbench screen in the current preferences; 0 while other windows use it */
int intuition_reopen_workbench(void) {
    NScreen *s = workbench_screen(0);
    if (!s) return 1;
    if (s->wins || s->locks) return 0;
    free_screen(s);
    workbench_screen(1);
    return 1;
}

/* ENV:Sys/screenmode.prefs was written (ScreenMode "Use" or "Save") */
void intuition_screenmode_changed(void) {
    extern int workbench_running(void);
    extern void workbench_reset_screen(void);
    if (workbench_running()) workbench_reset_screen();
    else intuition_reopen_workbench();
}

static uint32_t i_OpenWorkBench(M68K *c) { (void)c; return workbench_screen(1)->scr; }
static uint32_t i_CloseWorkBench(M68K *c) {
    (void)c;
    NScreen *s = workbench_screen(0);
    if (!s) return DOSTRUE;
    if (s->wins || s->locks) return 0;
    free_screen(s);
    return DOSTRUE;
}
static uint32_t i_GetScreenData(M68K *c) {
    /* buffer A0, size D0, type D1, screen A1 */
    NScreen *s = (c->d[1] & 0xF) == 0xF && c->a[1] ? nscr_of(c->a[1]) : workbench_screen(1);
    if (!s) return 0;
    uint32_t n = c->d[0] < Screen_SIZEOF ? c->d[0] : Screen_SIZEOF;
    for (uint32_t i = 0; i < n; i++) wr8(c->a[0] + i, rd8(s->scr + i));
    return DOSTRUE;
}
static uint32_t i_GetDefaultPubScreen(M68K *c) {
    const char *n = "Workbench";
    for (int i = 0; i <= (int)strlen(n); i++) wr8(c->a[0] + (uint32_t)i, (uint8_t)n[i]);
    return 0;
}
static uint32_t i_ScreenToFront(M68K *c) {
    NScreen *s = nscr_of(c->a[0]);
    if (s) wr32(IntuitionBase + IB_ACTIVESCREEN, s->scr);
    return 0;
}
static uint32_t i_ShowTitle(M68K *c) {
    NScreen *s = nscr_of(c->a[0]);
    if (!s) return 0;
    uint32_t fl = rd16(s->scr + Screen_Flags);
    wr16(s->scr + Screen_Flags, c->d[0] ? fl | 0x10 : fl & ~0x10u);
    draw_screen_bar(s);
    return 0;
}
static uint32_t i_GetScreenDrawInfo(M68K *c) {
    NScreen *s = nscr_of(c->a[0]);
    return s ? s->drawinfo : 0;
}
static uint32_t i_FreeScreenDrawInfo(M68K *c) { (void)c; return 0; }
/* ---- the pointer ------------------------------------------------------------------
 * Shown as the Mac cursor over the screen windows. Sprite data: posctl words,
 * then per line two words (plane 0, plane 1); pens 1-3 are colours 17-19. */

static uint8_t spr0_pix[16 * 128];
static int spr0_h, spr0_on;

/* the busy pointer: a stopwatch (k black, w light, r red) */
static const char *busy_art[] = {
    "......kkkk......", ".....kwrrwk.....", "......kkkk......", "....kkwwwwkk....",
    "...kwwwwwwwwk...", "..kwwwwkwwwwwk..", ".kwwwwwkwwwwwwk.", ".kwwwwwkwwwwwwk.",
    ".kwwwwwkkkkwwwk.", ".kwwwwwwwwwwwwk.", ".kwwwwwwwwwwwwk.", "..kwwwwwwwwwwk..",
    "...kwwwwwwwwk...", "....kkwwwwkk....", "......kkkk......", "................",
};

static void sprite_read(uint32_t data, int h, uint8_t *pix) {
    for (int y = 0; y < h; y++) {
        uint16_t p0 = (uint16_t)rd16(data + 4 + 4 * (uint32_t)y), p1 = (uint16_t)rd16(data + 6 + 4 * (uint32_t)y);
        for (int x = 0; x < 16; x++) pix[y * 16 + x] = (uint8_t)(((p0 >> (15 - x)) & 1) | (((p1 >> (15 - x)) & 1) << 1));
    }
}

void pointer_update(void) {
    if (!host.pointer) return;
    NWindow *w = active_win;
    NScreen *s = w ? w->s : screens;
    int sid = s ? s->id : 0;
    const uint8_t *pix = 0;
    uint8_t busy[16 * 16];
    int pw = 16, ph = 0, hx = 0, hy = 0;
    if (spr0_on) {
        int any = 0;
        for (int i = 0; i < 16 * spr0_h; i++) any |= spr0_pix[i];
        if (!any) { host.pointer(sid, 2, 0, 0, 0, 0, 0); return; }
        pix = spr0_pix; ph = spr0_h;
    } else if (w && w->ptr_kind == 1 && w->ptr_pix) {
        pix = w->ptr_pix; pw = w->ptr_w; ph = w->ptr_h; hx = w->ptr_hx; hy = w->ptr_hy;
    } else if (w && w->ptr_kind == 2) {
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                char ch = busy_art[y][x];
                busy[y * 16 + x] = ch == 'r' ? 1 : ch == 'k' ? 2 : ch == 'w' ? 3 : 0;
            }
        pix = busy; ph = 16; hx = 7; hy = 8;
    } else { host.pointer(sid, 0, 0, 0, 0, 0, 0); return; }
    uint32_t argb[16 * 128];
    for (int i = 0; i < pw * ph && i < 16 * 128; i++)
        argb[i] = pix[i] && s ? 0xFF000000u | s->pal[16 + pix[i]] : 0;
    host.pointer(sid, 1, argb, pw, ph, hx, hy);
}

void pointer_sprite0(const uint8_t *pix, int h, int on) {
    spr0_on = on;
    if (on) { spr0_h = h > 128 ? 128 : h; memcpy(spr0_pix, pix, (size_t)(16 * spr0_h)); }
    pointer_update();
}

/* SetPointer(window, data, height, width, xoffset, yoffset) */
static uint32_t i_SetPointer(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (!w) return 0;
    int h = (int)(c->d[0] & 0xFFFF);
    if (h > 128) h = 128;
    free(w->ptr_pix);
    w->ptr_pix = calloc(16, (size_t)(h ? h : 1));
    if (c->a[1] && h) sprite_read(c->a[1], h, w->ptr_pix);
    w->ptr_kind = 1;
    w->ptr_w = 16;
    w->ptr_h = h;
    w->ptr_hx = -(int16_t)c->d[2];
    w->ptr_hy = -(int16_t)c->d[3];
    if (w == active_win) pointer_update();
    return 0;
}
static uint32_t i_ClearPointer(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (!w) return 0;
    w->ptr_kind = 0;
    if (w == active_win) pointer_update();
    return 0;
}
/* SetWindowPointerA(window, tags): WA_Pointer (an object: the system pointer here), WA_BusyPointer */
#define WA_Pointer_ 0x80000097
#define WA_BusyPointer_ 0x80000098
static uint32_t i_SetWindowPointerA(M68K *c) {
    NWindow *w = nwin_of(c->a[0]);
    if (!w) return 0;
    w->ptr_kind = tag_value(c->a[1], WA_BusyPointer_, 0) ? 2 : 0;
    if (w == active_win) pointer_update();
    return 0;
}

/* the zoom gadget and ZipWindow(): switch to the other box */
void win_zip(NWindow *w) {
    if (!w->zw || !w->zh) return;
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    win_move(w, w->zx, w->zy);
    win_resize(w, w->zw, w->zh);
    w->zx = x; w->zy = y; w->zw = ww; w->zh = hh;
    send_idcmp(w, 0x02000000 /* IDCMP_CHANGEWINDOW */, 0, 0, 0, 0, 0);
}
static uint32_t i_ZipWindow(M68K *c) { NWindow *w = nwin_of(c->a[0]); if (w) win_zip(w); return 0; }

static uint32_t i_nop(M68K *c) { (void)c; return 0; }
static uint32_t i_LockIBase(M68K *c) { (void)c; return 0; }

static uint32_t i_DisplayBeep(M68K *c) {
    (void)c;
    if (host.beep) host.beep();
    return 0;
}
static uint32_t i_CurrentTime(M68K *c) {
    uint32_t secs = timer_systime_secs();
    if (c->a[0]) wr32(c->a[0], secs);
    if (c->a[1]) wr32(c->a[1], (uint32_t)(now_us() % 1000000u));
    return 0;
}
static uint32_t i_DoubleClick(M68K *c) {
    int64_t a = (int64_t)c->d[0] * 1000000 + c->d[1], b = (int64_t)c->d[2] * 1000000 + c->d[3];
    return (b - a) >= 0 && (b - a) < 500000 ? DOSTRUE : 0;
}

/* Remember lists: {NextRemember, RememberSize, Memory} */
static uint32_t i_AllocRemember(M68K *c) {
    uint32_t key = c->a[0], size = c->d[0];
    uint32_t mem_ = exec_allocmem(size, c->d[1]);
    if (!mem_) return 0;
    uint32_t r = exec_allocmem(12, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(r, rd32(key));
    wr32(r + 4, size);
    wr32(r + 8, mem_);
    wr32(key, r);
    return mem_;
}
static uint32_t i_FreeRemember(M68K *c) {
    uint32_t key = c->a[0], r = rd32(key);
    while (r) {
        uint32_t n = rd32(r);
        if (c->d[0]) exec_freemem(rd32(r + 8), rd32(r + 4));
        exec_freemem(r, 12);
        r = n;
    }
    wr32(key, 0);
    return 0;
}

static uint32_t i_GetPrefs(M68K *c) {
    /* Preferences: zero it, then the few fields programs look at */
    uint32_t buf = c->a[0], n = c->d[0];
    for (uint32_t i = 0; i < n; i++) wr8(buf + i, 0);
    if (n > 0) wr8(buf, 8);                                     /* FontHeight = TOPAZ_EIGHTY */
    return buf;
}
static uint32_t i_DisplayAlert(M68K *c) {
    char text[256];
    str_get(c->a[0] + 3, text, sizeof text);
    os_log("DisplayAlert($%08X) : %s", c->d[0], text);
    return 0;
}

static const NativeEntry intui_natives[] = {
    { "OpenScreen", i_OpenScreen }, { "OpenScreenTagList", i_OpenScreenTagList }, { "CloseScreen", i_CloseScreen },
    { "QueryOverscan", i_QueryOverscan },
    { "OpenWindow", i_OpenWindow }, { "OpenWindowTagList", i_OpenWindowTagList }, { "CloseWindow", i_CloseWindow },
    { "ModifyIDCMP", i_ModifyIDCMP },
    { "WindowToFront", i_WindowToFront }, { "WindowToBack", i_WindowToBack }, { "ActivateWindow", i_ActivateWindow },
    { "MoveWindow", i_MoveWindow }, { "SizeWindow", i_SizeWindow }, { "ChangeWindowBox", i_ChangeWindowBox },
    { "WindowLimits", i_WindowLimits }, { "SetWindowTitles", i_SetWindowTitles },
    { "RefreshWindowFrame", i_RefreshWindowFrame }, { "BeginRefresh", i_BeginRefresh }, { "EndRefresh", i_EndRefresh },
    { "ReportMouse", i_ReportMouse }, { "ViewPortAddress", i_ViewPortAddress }, { "ViewAddress", i_ViewAddress },
    { "ScrollWindowRaster", i_ScrollWindowRaster },
    { "LockPubScreen", i_LockPubScreen }, { "UnlockPubScreen", i_UnlockPubScreen },
    { "OpenWorkBench", i_OpenWorkBench }, { "PubScreenStatus", i_PubScreenStatus }, { "CloseWorkBench", i_CloseWorkBench },
    { "GetScreenData", i_GetScreenData }, { "GetDefaultPubScreen", i_GetDefaultPubScreen },
    { "ScreenToFront", i_ScreenToFront }, { "ScreenToBack", i_nop }, { "ScreenDepth", i_nop },
    { "WBenchToFront", i_nop }, { "WBenchToBack", i_nop }, { "MoveScreen", i_nop },
    { "ShowTitle", i_ShowTitle }, { "GetScreenDrawInfo", i_GetScreenDrawInfo },
    { "FreeScreenDrawInfo", i_FreeScreenDrawInfo },
    { "MakeScreen", i_nop }, { "RethinkDisplay", i_nop }, { "RemakeDisplay", i_nop },
    { "LockIBase", i_LockIBase }, { "UnlockIBase", i_nop },
    { "ZipWindow", i_ZipWindow }, { "SetPointer", i_SetPointer }, { "ClearPointer", i_ClearPointer }, { "SetWindowPointerA", i_SetWindowPointerA },
    { "DisplayBeep", i_DisplayBeep }, { "CurrentTime", i_CurrentTime }, { "DoubleClick", i_DoubleClick },
    { "AllocRemember", i_AllocRemember }, { "FreeRemember", i_FreeRemember },
    { "GetPrefs", i_GetPrefs }, { "GetDefPrefs", i_GetPrefs }, { "SetPrefs", i_nop },
    { "DisplayAlert", i_DisplayAlert }, { "TimedDisplayAlert", i_DisplayAlert },
    { "HelpControl", i_nop }, { "SetPubScreenModes", i_nop },
    { 0, 0 }
};

/* ---- the compositor ------------------------------------------------------- */

static void blit_planar(uint32_t *out, int ow, int oh, BM *b, int ox, int oy, int w, int h, const uint32_t *pal) {
    for (int y = 0; y < h; y++) {
        int sy = y, dy = oy + y;
        if (dy < 0 || dy >= oh || sy >= b->rows) continue;
        uint32_t *row = out + (size_t)dy * ow;
        for (int x = 0; x < w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= ow) continue;
            int pen = 0;
            uint32_t off = (uint32_t)(sy * b->bpr + (x >> 3));
            int sh = 7 - (x & 7);
            for (int p = 0; p < b->depth; p++) {
                uint32_t pl = b->planes[p];
                if (pl == 0xFFFFFFFF) pen |= 1 << p;
                else if (pl && pl + off < mem_size) pen |= ((mem[pl + off] >> sh) & 1) << p;
            }
            row[dx] = 0xFF000000u | pal[pen];
        }
    }
}

/* host: render screen 'id' into an RGBA buffer of its size; returns 0 if gone */
int amiga_display_render(int id, uint32_t *out, int ow, int oh) {
    pthread_mutex_lock(&disp_lock);
    NScreen *s = screens;
    while (s && s->id != id) s = s->next;
    if (!s) { pthread_mutex_unlock(&disp_lock); return 0; }
    BM b;
    bm_load(s->custom_bm ? s->custom_bm : s->scr + Screen_BitMap, &b);
    blit_planar(out, ow, oh, &b, 0, 0, s->w, s->h, s->pal);
    /* windows back to front */
    NWindow *stack[256];
    int n = 0;
    for (NWindow *w = s->wins; w && n < 256; w = w->next) stack[n++] = w;
    for (int i = n - 1; i >= 0; i--) {
        NWindow *w = stack[i];
        BM wb;
        if (bm_load(w->bm, &wb)) blit_planar(out, ow, oh, &wb, w->x, w->y, w->w, w->h, s->pal);
        if (w->ibm && bm_load(w->ibm, &wb)) {
            int bl = (int8_t)rd8(w->win + Window_BorderLeft), bt = (int8_t)rd8(w->win + Window_BorderTop);
            int br = (int8_t)rd8(w->win + Window_BorderRight), bb = (int8_t)rd8(w->win + Window_BorderBottom);
            blit_planar(out, ow, oh, &wb, w->x + bl, w->y + bt, w->w - bl - br, w->h - bt - bb, s->pal);
        }
    }
    /* menu overlay */
    for (int y = 0; y < s->h && y < oh; y++)
        for (int x = 0; x < s->w && x < ow; x++) {
            uint8_t p = s->ov[(size_t)y * s->w + x];
            if (p != 0xFF) out[(size_t)y * ow + x] = 0xFF000000u | s->pal[p];
        }
    pthread_mutex_unlock(&disp_lock);
    return 1;
}

int amiga_display_size(int id, int *w, int *h, int *hires, int *lace) {
    pthread_mutex_lock(&disp_lock);
    NScreen *s = screens;
    while (s && s->id != id) s = s->next;
    if (s) { *w = s->w; *h = s->h; *hires = s->hires; *lace = s->lace; }
    pthread_mutex_unlock(&disp_lock);
    return s != 0;
}

uint32_t amiga_display_serial(void) {
    extern uint32_t gfx_changes(void);
    return gfx_changes();
}

int intui_active_screen_id(void) {
    uint32_t scr = rd32(IntuitionBase + IB_ACTIVESCREEN);
    NScreen *s = nscr_of(scr);
    return s ? s->id : (screens ? screens->id : 0);
}

void host_screen_opened(NScreen *s, const char *title) { if (host.screen_opened) host.screen_opened(s->id, title); }
void host_screen_closed(NScreen *s) { if (host.screen_closed) host.screen_closed(s->id); }

/* ---- init ------------------------------------------------------------------ */

void intuition_init(void) {
    /* the table merges this file's functions and intuition_input.c's */
    static NativeEntry all[256];
    int n = 0;
    for (const NativeEntry *e = intui_natives; e->name; e++) all[n++] = *e;
    for (const NativeEntry *e = intui_input_natives; e->name; e++) all[n++] = *e;
    for (const NativeEntry *e = boopsi_natives; e->name; e++) all[n++] = *e;
    all[n].name = 0;
    IntuitionBase = lib_create("intuition.library", "intuition", IntuitionBase_SIZEOF + 64, all, 40, 1,
                               "intuition 40.1 (AmigaMac)");
    lib_add(IntuitionBase);
    boopsi_init();
    ireply_port = exec_allocmem(MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(ireply_port + LN_TYPE, NT_MSGPORT);
    wr8(ireply_port + MP_FLAGS, PA_IGNORE);
    list_new(ireply_port + MP_MSGLIST);
    /* input.device's task: high priority, as on the Amiga */
    uint32_t t = exec_allocmem(TC_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(t + LN_TYPE, NT_TASK);
    wr8(t + LN_PRI, 20);
    wr32(t + LN_NAME, str_alloc("input.device"));
    wr32(t + TC_SIGALLOC, 0xFFFF);
    list_new(t + TC_MEMENTRY);
    uint32_t stack = exec_allocmem(32768, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(t + TC_SPLOWER, stack);
    wr32(t + TC_SPUPPER, stack + 32768);
    sched_add(t, input_task_main, 0, 0, 0);
}
