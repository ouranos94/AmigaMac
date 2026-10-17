/* graphics.library, native, on planar BitMaps in 68k memory.
 * Structure layouts come from ndk_offsets.h (measured with vbcc + NDK).
 * Register conventions follow graphics_lib.fd.
 */
#include "os.h"
#include "sched.h"
#include "gfx.h"
#include <stdlib.h>
#include <string.h>

uint32_t GfxBase;
static uint32_t topaz8, topaz9;
static volatile uint32_t gfx_serial;            /* bumped by every drawing call */

void gfx_touch(void) { gfx_serial++; }
uint32_t gfx_changes(void) { return gfx_serial; }

/* ---- BitMaps --------------------------------------------------------------- */

int bm_load(uint32_t bm, BM *b) {
    if (!bm) return 0;
    b->addr = bm;
    b->bpr = rd16(bm + BitMap_BytesPerRow);
    b->rows = rd16(bm + BitMap_Rows);
    b->depth = rd8(bm + BitMap_Depth);
    if (b->depth > 8) b->depth = 8;
    for (int i = 0; i < 8; i++) b->planes[i] = i < b->depth ? rd32(bm + BitMap_Planes + 4 * i) : 0;
    return b->bpr > 0 && b->rows > 0;
}

/* plane pointers 0 and -1 mean "all zeros" / "all ones", never written */
static inline int plane_ok(uint32_t p) { return p && p != 0xFFFFFFFF; }

void bm_pixel(BM *b, int x, int y, int pen, int mode, int mask) {
    if (x < 0 || y < 0 || x >= b->bpr * 8 || y >= b->rows) return;
    uint32_t off = (uint32_t)(y * b->bpr + (x >> 3));
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    for (int p = 0; p < b->depth; p++) {
        if (!(mask & (1 << p)) || !plane_ok(b->planes[p])) continue;
        uint32_t a = b->planes[p] + off;
        if (!mem_ok(a, 1)) continue;
        if (mode & COMPLEMENT) mem[a] ^= bit;
        else if (pen & (1 << p)) mem[a] |= bit;
        else mem[a] &= (uint8_t)~bit;
    }
}

int bm_read(BM *b, int x, int y) {
    if (x < 0 || y < 0 || x >= b->bpr * 8 || y >= b->rows) return 0;
    uint32_t off = (uint32_t)(y * b->bpr + (x >> 3));
    int pen = 0;
    for (int p = 0; p < b->depth; p++) {
        uint32_t pl = b->planes[p];
        int v = pl == 0xFFFFFFFF ? 1 : pl && mem_ok(pl + off, 1) ? (mem[pl + off] >> (7 - (x & 7))) & 1 : 0;
        pen |= v << p;
    }
    return pen;
}

void bm_fill(BM *b, int x0, int y0, int x1, int y1, int pen, int mode, int mask) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= b->bpr * 8) x1 = b->bpr * 8 - 1;
    if (y1 >= b->rows) y1 = b->rows - 1;
    if (x0 > x1 || y0 > y1) return;
    int bx0 = x0 >> 3, bx1 = x1 >> 3;
    uint8_t m0 = (uint8_t)(0xFF >> (x0 & 7)), m1 = (uint8_t)(0xFF << (7 - (x1 & 7)));
    for (int p = 0; p < b->depth; p++) {
        if (!(mask & (1 << p)) || !plane_ok(b->planes[p])) continue;
        int set = (pen >> p) & 1;
        for (int y = y0; y <= y1; y++) {
            uint32_t row = b->planes[p] + (uint32_t)(y * b->bpr);
            if (!mem_ok(row + bx0, (uint32_t)(bx1 - bx0 + 1))) continue;
            uint8_t *r = mem + row;
            for (int bx = bx0; bx <= bx1; bx++) {
                uint8_t m = 0xFF;
                if (bx == bx0) m &= m0;
                if (bx == bx1) m &= m1;
                if (mode & COMPLEMENT) r[bx] ^= m;
                else if (set) r[bx] |= m;
                else r[bx] &= (uint8_t)~m;
            }
        }
    }
}

void bm_line(BM *b, int x0, int y0, int x1, int y1, int pen, int mode, int mask) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        bm_pixel(b, x0, y0, pen, mode, mask);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* blitter-style copy: D = f(B = source, C = destination) per minterm, the
   A channel being all ones (BltBitMap's model) */
void bm_copy(BM *s, int sx, int sy, BM *d, int dx, int dy, int w, int h, int minterm, int mask) {
    /* clip against both bitmaps */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > s->bpr * 8) w = s->bpr * 8 - sx;
    if (sy + h > s->rows) h = s->rows - sy;
    if (dx + w > d->bpr * 8) w = d->bpr * 8 - dx;
    if (dy + h > d->rows) h = d->rows - dy;
    if (w <= 0 || h <= 0) return;
    minterm &= 0xFF;
    int depth = d->depth;
    uint8_t *tmp = malloc((size_t)w * h);
    /* read the source first: overlapping copies within one bitmap stay right */
    for (int p = 0; p < depth; p++) {
        if (!(mask & (1 << p)) || !plane_ok(d->planes[p])) continue;
        uint32_t sp = p < s->depth ? s->planes[p] : 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sxp = sx + x, syp = sy + y;
                int bsrc = sp == 0xFFFFFFFF ? 1 : sp ? (rd8(sp + (uint32_t)(syp * s->bpr + (sxp >> 3))) >> (7 - (sxp & 7))) & 1 : 0;
                tmp[y * w + x] = (uint8_t)bsrc;
            }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int dxp = dx + x, dyp = dy + y;
                uint32_t a = d->planes[p] + (uint32_t)(dyp * d->bpr + (dxp >> 3));
                if (!mem_ok(a, 1)) continue;
                int bit = 7 - (dxp & 7);
                int c = (mem[a] >> bit) & 1, bsrc = tmp[y * w + x];
                int r = (minterm >> (4 + 2 * bsrc + c)) & 1;
                if (r) mem[a] |= (uint8_t)(1 << bit); else mem[a] &= (uint8_t)~(1 << bit);
            }
    }
    free(tmp);
}

uint32_t gfx_alloc_bitmap(int w, int h, int depth, int clear) {
    uint32_t bm = exec_allocmem(BitMap_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    int bpr = ((w + 15) >> 4) * 2;
    wr16(bm + BitMap_BytesPerRow, bpr);
    wr16(bm + BitMap_Rows, h);
    wr8(bm + BitMap_Depth, depth);
    for (int i = 0; i < depth && i < 8; i++)
        wr32(bm + BitMap_Planes + 4 * i, exec_allocmem((uint32_t)(bpr * h), MEMF_CHIP | (clear ? MEMF_CLEAR : 0)));
    return bm;
}

void gfx_free_bitmap(uint32_t bm) {
    if (!bm) return;
    BM b;
    bm_load(bm, &b);
    for (int i = 0; i < b.depth; i++) if (plane_ok(b.planes[i])) exec_freemem(b.planes[i], (uint32_t)(b.bpr * b.rows));
    exec_freemem(bm, BitMap_SIZEOF);
}

/* ---- RastPort -------------------------------------------------------------- */

typedef struct { BM bm; int fg, bg, mode, mask; uint32_t rp; } RP;

static int rp_get(uint32_t rp, RP *r) {
    r->rp = rp;
    if (!bm_load(rd32(rp + RastPort_BitMap), &r->bm)) return 0;
    r->fg = rd8(rp + RastPort_FgPen);
    r->bg = rd8(rp + RastPort_BgPen);
    r->mode = rd8(rp + RastPort_DrawMode);
    r->mask = rd8(rp + RastPort_Mask);
    if (r->mode & INVERSVID) { int t = r->fg; r->fg = r->bg; r->bg = t; }
    return 1;
}

void rp_init(uint32_t rp) {
    for (int i = 0; i < RastPort_SIZEOF; i++) wr8(rp + i, 0);
    wr8(rp + RastPort_Mask, 0xFF);
    wr8(rp + RastPort_FgPen, 0xFF);
    wr8(rp + RastPort_BgPen, 0);
    wr8(rp + RastPort_AOlPen, 0xFF);
    wr8(rp + RastPort_DrawMode, JAM2);
    wr16(rp + RastPort_LinePtrn, 0xFFFF);
    wr16(rp + RastPort_Flags, 0);
    uint32_t f = gfx_default_font();
    wr32(rp + RastPort_Font, f);
    wr16(rp + RastPort_TxHeight, rd16(f + TextFont_tf_YSize));
    wr16(rp + RastPort_TxWidth, rd16(f + TextFont_tf_XSize));
    wr16(rp + RastPort_TxBaseline, rd16(f + TextFont_tf_Baseline));
}

/* a pixel with the RastPort's pattern/mode rules; on = "foreground" */
static void rp_plot(RP *r, int x, int y, int on) {
    if (r->mode & COMPLEMENT) { if (on) bm_pixel(&r->bm, x, y, 0, COMPLEMENT, r->mask); return; }
    if (on) bm_pixel(&r->bm, x, y, r->fg, JAM1, r->mask);
    else if (r->mode & JAM2) bm_pixel(&r->bm, x, y, r->bg, JAM1, r->mask);
}

static void rp_rectfill(RP *r, int x0, int y0, int x1, int y1) {
    uint32_t ptrn = rd32(r->rp + RastPort_AreaPtrn);
    int8_t ptsz = (int8_t)rd8(r->rp + RastPort_AreaPtSz);
    if (!ptrn || ptsz < 0) { bm_fill(&r->bm, x0, y0, x1, y1, r->fg, r->mode & COMPLEMENT ? COMPLEMENT : JAM1, r->mask); return; }
    int rowsmask = (1 << ptsz) - 1;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) {
        uint32_t word = rd16(ptrn + 2 * (uint32_t)(y & rowsmask));
        for (int x = x0; x <= x1; x++) rp_plot(r, x, y, (word >> (15 - (x & 15))) & 1);
    }
}

/* ---- text ------------------------------------------------------------------ */

#include "font8_extern.h"

static uint32_t make_font(const char *name, int ysize) {
    /* chars 32..255, 8 pixels each, side by side in tf_CharData */
    int lo = 32, hi = 255, n = hi - lo + 1, mod = n;           /* one byte per char per row */
    uint32_t tf = exec_allocmem(TextFont_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t data = exec_allocmem((uint32_t)(mod * ysize), MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t loc = exec_allocmem((uint32_t)(4 * (n + 1)), MEMF_PUBLIC | MEMF_CLEAR);
    for (int c = lo; c <= hi; c++)
        for (int row = 0; row < 8 && row < ysize; row++)
            wr8(data + (uint32_t)(row * mod + (c - lo)), font8_data[c][row]);
    for (int i = 0; i <= n; i++) wr32(loc + 4 * i, (uint32_t)(i < n ? i * 8 : ('?' - lo) * 8) << 16 | 8);
    wr8(tf + LN_TYPE, NT_FONT);
    wr32(tf + LN_NAME, str_alloc(name));
    wr16(tf + TextFont_tf_YSize, ysize);
    wr8(tf + TextFont_tf_Style, 0);
    wr8(tf + TextFont_tf_Flags, 0x41);                         /* FPF_ROMFONT | FPF_DESIGNED */
    wr16(tf + TextFont_tf_XSize, 8);
    wr16(tf + TextFont_tf_Baseline, 6);
    wr16(tf + TextFont_tf_BoldSmear, 1);
    wr8(tf + TextFont_tf_LoChar, lo);
    wr8(tf + TextFont_tf_HiChar, hi);
    wr32(tf + TextFont_tf_CharData, data);
    wr16(tf + TextFont_tf_Modulo, mod);
    wr32(tf + TextFont_tf_CharLoc, loc);
    return tf;
}

uint32_t gfx_default_font(void) { return topaz8; }

uint32_t gfx_open_font(const char *name, int ysize) {
    (void)name;
    /* only the built-in topaz replacement for now: nearest size */
    uint32_t f = ysize >= 9 ? topaz9 : topaz8;
    wr16(f + TextFont_tf_Accessors, rd16(f + TextFont_tf_Accessors) + 1);
    return f;
}

static int glyph_index(uint32_t tf, int c) {
    int lo = rd8(tf + TextFont_tf_LoChar), hi = rd8(tf + TextFont_tf_HiChar);
    return (c < lo || c > hi) ? hi - lo + 1 : c - lo;
}

static int char_advance(uint32_t rp, uint32_t tf, int c) {
    uint32_t space = rd32(tf + TextFont_tf_CharSpace), kern = rd32(tf + TextFont_tf_CharKern);
    int gi = glyph_index(tf, c);
    int adv = space ? (int16_t)rd16(space + 2 * (uint32_t)gi) : rd16(tf + TextFont_tf_XSize);
    if (kern) adv += (int16_t)rd16(kern + 2 * (uint32_t)gi);
    return adv + (int16_t)rd16(rp + RastPort_TxSpacing);
}

int rp_text_length(uint32_t rp, const char *s, int n) {
    uint32_t tf = rd32(rp + RastPort_Font);
    if (!tf) tf = topaz8;
    int w = 0;
    for (int i = 0; i < n; i++) w += char_advance(rp, tf, (uint8_t)s[i]);
    return w;
}

void rp_text(uint32_t rp, const char *s, int n) {
    RP r;
    if (!rp_get(rp, &r)) return;
    uint32_t tf = rd32(rp + RastPort_Font);
    if (!tf) tf = topaz8;
    int ysize = rd16(tf + TextFont_tf_YSize), base = rd16(tf + TextFont_tf_Baseline);
    uint32_t data = rd32(tf + TextFont_tf_CharData), loc = rd32(tf + TextFont_tf_CharLoc);
    uint32_t kern = rd32(tf + TextFont_tf_CharKern);
    int mod = rd16(tf + TextFont_tf_Modulo), style = rd8(rp + RastPort_AlgoStyle);
    int x = (int16_t)rd16(rp + RastPort_cp_x), y = (int16_t)rd16(rp + RastPort_cp_y) - base;
    for (int i = 0; i < n; i++) {
        int c = (uint8_t)s[i], gi = glyph_index(tf, c);
        uint32_t l = rd32(loc + 4 * (uint32_t)gi);
        int boff = (int)(l >> 16), bw = (int)(l & 0xFFFF);
        int adv = char_advance(rp, tf, c);
        int gx = x + (kern ? (int16_t)rd16(kern + 2 * (uint32_t)gi) : 0);
        /* JAM2 fills the whole cell */
        if ((r.mode & 3) == JAM2) bm_fill(&r.bm, x, y, x + adv - 1, y + ysize - 1, r.bg, JAM1, r.mask);
        for (int row = 0; row < ysize; row++)
            for (int col = 0; col < bw; col++) {
                int bitpos = boff + col;
                int on = (rd8(data + (uint32_t)(row * mod + (bitpos >> 3))) >> (7 - (bitpos & 7))) & 1;
                if (style & 2) on |= col > 0 && ((rd8(data + (uint32_t)(row * mod + ((bitpos - 1) >> 3))) >> (7 - ((bitpos - 1) & 7))) & 1);  /* bold */
                if (on) rp_plot(&r, gx + col + ((style & 4) ? (base - row) / 2 : 0), y + row, 1);
            }
        if (style & 1) bm_fill(&r.bm, x, y + base + 1, x + adv - 1, y + base + 1, r.fg, r.mode & COMPLEMENT ? COMPLEMENT : JAM1, r.mask);
        x += adv;
    }
    wr16(rp + RastPort_cp_x, (uint32_t)x);
    gfx_touch();
}

/* ---- library functions ------------------------------------------------------- */

#define RPA(c) ((c)->a[1])

static uint32_t g_InitRastPort(M68K *c) { rp_init(c->a[1]); return 0; }
static uint32_t g_InitBitMap(M68K *c) {
    uint32_t bm = c->a[0];
    wr16(bm + BitMap_BytesPerRow, ((c->d[1] + 15) >> 4) * 2);
    wr16(bm + BitMap_Rows, c->d[2]);
    wr8(bm + BitMap_Depth, c->d[0]);
    wr8(bm + BitMap_Flags, 0);
    wr16(bm + 6, 0);
    return 0;
}
static uint32_t g_AllocBitMap(M68K *c) {
    int depth = (int)c->d[2];
    if (depth > 8) depth = 8;
    return gfx_alloc_bitmap((int)c->d[0], (int)c->d[1], depth, c->d[3] & 1);
}
static uint32_t g_FreeBitMap(M68K *c) { gfx_free_bitmap(c->a[0]); return 0; }
static uint32_t g_AllocRaster(M68K *c) {
    uint32_t bytes = ((c->d[0] + 15) >> 4) * 2 * c->d[1];
    return exec_allocmem(bytes, MEMF_CHIP);
}
static uint32_t g_FreeRaster(M68K *c) { exec_freemem(c->a[0], ((c->d[0] + 15) >> 4) * 2 * c->d[1]); return 0; }
static uint32_t g_GetBitMapAttr(M68K *c) {
    uint32_t bm = c->a[0];
    switch (c->d[1]) {
    case 0: return rd16(bm + BitMap_Rows);                  /* BMA_HEIGHT */
    case 4: return rd8(bm + BitMap_Depth);                  /* BMA_DEPTH */
    case 8: return rd16(bm + BitMap_BytesPerRow) * 8u;      /* BMA_WIDTH */
    case 12: return 0;                                      /* BMA_FLAGS */
    }
    return 0;
}

static uint32_t g_SetAPen(M68K *c) { wr8(RPA(c) + RastPort_FgPen, c->d[0]); return 0; }
static uint32_t g_SetBPen(M68K *c) { wr8(RPA(c) + RastPort_BgPen, c->d[0]); return 0; }
static uint32_t g_SetDrMd(M68K *c) { wr8(RPA(c) + RastPort_DrawMode, c->d[0]); return 0; }
static uint32_t g_SetOutlinePen(M68K *c) {
    uint32_t old = rd8(c->a[0] + RastPort_AOlPen);
    wr8(c->a[0] + RastPort_AOlPen, c->d[0]);
    wr16(c->a[0] + RastPort_Flags, rd16(c->a[0] + RastPort_Flags) | 0x08);   /* AREAOUTLINE */
    return old;
}
static uint32_t g_SetWriteMask(M68K *c) { wr8(c->a[0] + RastPort_Mask, c->d[0]); return 1; }
static uint32_t g_GetAPen(M68K *c) { return rd8(c->a[0] + RastPort_FgPen); }
static uint32_t g_GetBPen(M68K *c) { return rd8(c->a[0] + RastPort_BgPen); }
static uint32_t g_GetDrMd(M68K *c) { return rd8(c->a[0] + RastPort_DrawMode); }
static uint32_t g_GetOutlinePen(M68K *c) { return rd8(c->a[0] + RastPort_AOlPen); }

static uint32_t g_Move(M68K *c) {
    wr16(RPA(c) + RastPort_cp_x, c->d[0]);
    wr16(RPA(c) + RastPort_cp_y, c->d[1]);
    return 0;
}

void rp_draw(uint32_t rp, int x1, int y1) {
    RP r;
    int x0 = (int16_t)rd16(rp + RastPort_cp_x), y0 = (int16_t)rd16(rp + RastPort_cp_y);
    wr16(rp + RastPort_cp_x, (uint32_t)x1);
    wr16(rp + RastPort_cp_y, (uint32_t)y1);
    if (!rp_get(rp, &r)) return;
    uint32_t ptrn = rd16(rp + RastPort_LinePtrn);
    int cnt = rd8(rp + 30);                                  /* linpatcnt */
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        int on = ptrn == 0xFFFF ? 1 : (ptrn >> (15 - (cnt & 15))) & 1;
        rp_plot(&r, x0, y0, on);
        cnt++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    wr8(rp + 30, (uint32_t)(cnt & 15));
    gfx_touch();
}

void rp_move(uint32_t rp, int x, int y) {
    wr16(rp + RastPort_cp_x, (uint32_t)x);
    wr16(rp + RastPort_cp_y, (uint32_t)y);
}

/* filled rectangle with an explicit pen (mode JAM1) or complemented */
void rp_fill(uint32_t rp, int x0, int y0, int x1, int y1, int pen, int complement) {
    BM b;
    if (!bm_load(rd32(rp + RastPort_BitMap), &b)) return;
    bm_fill(&b, x0, y0, x1, y1, pen, complement ? COMPLEMENT : JAM1, 0xFF);
    gfx_touch();
}

static uint32_t g_Draw(M68K *c) { rp_draw(RPA(c), (int16_t)c->d[0], (int16_t)c->d[1]); return 0; }
static uint32_t g_PolyDraw(M68K *c) {
    uint32_t rp = c->a[1], arr = c->a[0];
    for (uint32_t i = 0; i < (c->d[0] & 0xFFFF); i++)
        rp_draw(rp, (int16_t)rd16(arr + 4 * i), (int16_t)rd16(arr + 4 * i + 2));
    return 0;
}
static uint32_t g_RectFill(M68K *c) {
    RP r;
    if (!rp_get(RPA(c), &r)) return 0;
    rp_rectfill(&r, (int16_t)c->d[0], (int16_t)c->d[1], (int16_t)c->d[2], (int16_t)c->d[3]);
    gfx_touch();
    return 0;
}
static uint32_t g_EraseRect(M68K *c) {
    RP r;
    if (!rp_get(RPA(c), &r)) return 0;
    bm_fill(&r.bm, (int16_t)c->d[0], (int16_t)c->d[1], (int16_t)c->d[2], (int16_t)c->d[3], 0, JAM1, r.mask);
    gfx_touch();
    return 0;
}
static uint32_t g_WritePixel(M68K *c) {
    RP r;
    if (!rp_get(RPA(c), &r)) return 0xFFFFFFFF;
    rp_plot(&r, (int16_t)c->d[0], (int16_t)c->d[1], 1);
    gfx_touch();
    return 0;
}
static uint32_t g_ReadPixel(M68K *c) {
    RP r;
    if (!rp_get(RPA(c), &r)) return 0xFFFFFFFF;
    int x = (int16_t)c->d[0], y = (int16_t)c->d[1];
    if (x < 0 || y < 0 || x >= r.bm.bpr * 8 || y >= r.bm.rows) return 0xFFFFFFFF;
    return (uint32_t)bm_read(&r.bm, x, y);
}
static uint32_t g_SetRast(M68K *c) {
    BM b;
    if (!bm_load(rd32(RPA(c) + RastPort_BitMap), &b)) return 0;
    bm_fill(&b, 0, 0, b.bpr * 8 - 1, b.rows - 1, (int)c->d[0], JAM1, 0xFF);
    gfx_touch();
    return 0;
}
static uint32_t g_ScrollRaster(M68K *c) {
    RP r;
    if (!rp_get(RPA(c), &r)) return 0;
    int dx = (int16_t)c->d[0], dy = (int16_t)c->d[1];
    int x0 = (int16_t)c->d[2], y0 = (int16_t)c->d[3], x1 = (int16_t)c->d[4], y1 = (int16_t)c->d[5];
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w <= 0 || h <= 0) return 0;
    if (abs(dx) < w && abs(dy) < h) {
        int sx = dx > 0 ? x0 + dx : x0, sy = dy > 0 ? y0 + dy : y0;
        int tx = dx > 0 ? x0 : x0 - dx, ty = dy > 0 ? y0 : y0 - dy;
        bm_copy(&r.bm, sx, sy, &r.bm, tx, ty, w - abs(dx), h - abs(dy), 0xC0, r.mask);
    }
    /* vacated area gets the background pen */
    if (dx > 0) bm_fill(&r.bm, x1 - dx + 1, y0, x1, y1, r.bg, JAM1, r.mask);
    if (dx < 0) bm_fill(&r.bm, x0, y0, x0 - dx - 1, y1, r.bg, JAM1, r.mask);
    if (dy > 0) bm_fill(&r.bm, x0, y1 - dy + 1, x1, y1, r.bg, JAM1, r.mask);
    if (dy < 0) bm_fill(&r.bm, x0, y0, x1, y0 - dy - 1, r.bg, JAM1, r.mask);
    gfx_touch();
    return 0;
}

static uint32_t g_BltBitMap(M68K *c) {
    BM s, d;
    if (!bm_load(c->a[0], &s) || !bm_load(c->a[1], &d)) return 0;
    bm_copy(&s, (int16_t)c->d[0], (int16_t)c->d[1], &d, (int16_t)c->d[2], (int16_t)c->d[3],
            (int16_t)c->d[4], (int16_t)c->d[5], (int)c->d[6], (int)c->d[7]);
    gfx_touch();
    return (uint32_t)d.depth;
}
static uint32_t g_BltBitMapRastPort(M68K *c) {
    BM s;
    RP r;
    if (!bm_load(c->a[0], &s) || !rp_get(c->a[1], &r)) return 0;
    bm_copy(&s, (int16_t)c->d[0], (int16_t)c->d[1], &r.bm, (int16_t)c->d[2], (int16_t)c->d[3],
            (int16_t)c->d[4], (int16_t)c->d[5], (int)c->d[6], r.mask);
    gfx_touch();
    return 0;
}
static uint32_t g_ClipBlit(M68K *c) {
    RP s, d;
    if (!rp_get(c->a[0], &s) || !rp_get(c->a[1], &d)) return 0;
    bm_copy(&s.bm, (int16_t)c->d[0], (int16_t)c->d[1], &d.bm, (int16_t)c->d[2], (int16_t)c->d[3],
            (int16_t)c->d[4], (int16_t)c->d[5], (int)c->d[6], d.mask);
    gfx_touch();
    return 0;
}
static uint32_t g_BltTemplate(M68K *c) {
    /* source A0 (word-aligned bitplane), srcX D0, srcMod D1, rp A1, dest D2/D3, size D4/D5 */
    RP r;
    if (!rp_get(c->a[1], &r)) return 0;
    uint32_t src = c->a[0];
    int sx = (int)c->d[0], mod = (int16_t)c->d[1], dx = (int16_t)c->d[2], dy = (int16_t)c->d[3];
    int w = (int16_t)c->d[4], h = (int16_t)c->d[5];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int bit = sx + x;
            int on = (rd8(src + (uint32_t)(y * mod + (bit >> 3))) >> (7 - (bit & 7))) & 1;
            if (on || (r.mode & 3) == JAM2) rp_plot(&r, dx + x, dy + y, on);
        }
    gfx_touch();
    return 0;
}
static uint32_t g_BltClear(M68K *c) {
    uint32_t n = c->d[0];
    if (c->d[1] & 2) n = (n >> 16) * (n & 0xFFFF);
    if (mem_ok(c->a[1], n)) memset(mem + c->a[1], 0, n);
    return 0;
}
static uint32_t g_BltPattern(M68K *c) {
    /* rp A1, mask A0 (may be NULL), xl D0, yl D1, maxx D2, maxy D3, bytecnt D4 */
    RP r;
    if (!rp_get(c->a[1], &r)) return 0;
    uint32_t maskp = c->a[0];
    int x0 = (int16_t)c->d[0], y0 = (int16_t)c->d[1], x1 = (int16_t)c->d[2], y1 = (int16_t)c->d[3], bpr = (int16_t)c->d[4];
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (maskp) {
                int bx = x - x0;
                if (!((rd8(maskp + (uint32_t)((y - y0) * bpr + (bx >> 3))) >> (7 - (bx & 7))) & 1)) continue;
            }
            uint32_t ptrn = rd32(c->a[1] + RastPort_AreaPtrn);
            int on = 1;
            if (ptrn) {
                int rows = (1 << (int8_t)rd8(c->a[1] + RastPort_AreaPtSz)) - 1;
                on = (rd16(ptrn + 2 * (uint32_t)(y & rows)) >> (15 - (x & 15))) & 1;
            }
            rp_plot(&r, x, y, on);
        }
    gfx_touch();
    return 0;
}

static uint32_t g_DrawEllipse(M68K *c) {
    RP r;
    if (!rp_get(RPA(c), &r)) return 0;
    int xc = (int16_t)c->d[0], yc = (int16_t)c->d[1], a = (int16_t)c->d[2], b = (int16_t)c->d[3];
    /* midpoint ellipse */
    long a2 = (long)a * a, b2 = (long)b * b;
    long x = 0, y = b, dx = 0, dy = 2 * a2 * y, err = b2 - a2 * b + a2 / 4;
    while (dx < dy) {
        rp_plot(&r, xc + (int)x, yc + (int)y, 1); rp_plot(&r, xc - (int)x, yc + (int)y, 1);
        rp_plot(&r, xc + (int)x, yc - (int)y, 1); rp_plot(&r, xc - (int)x, yc - (int)y, 1);
        x++; dx += 2 * b2;
        if (err < 0) err += b2 + dx;
        else { y--; dy -= 2 * a2; err += b2 + dx - dy; }
    }
    err = (long)(b2 * (x + 0.5) * (x + 0.5) + a2 * (y - 1) * (y - 1) - a2 * b2);
    while (y >= 0) {
        rp_plot(&r, xc + (int)x, yc + (int)y, 1); rp_plot(&r, xc - (int)x, yc + (int)y, 1);
        rp_plot(&r, xc + (int)x, yc - (int)y, 1); rp_plot(&r, xc - (int)x, yc - (int)y, 1);
        y--; dy -= 2 * a2;
        if (err > 0) err += a2 - dy;
        else { x++; dx += 2 * b2; err += a2 - dy + dx; }
    }
    gfx_touch();
    return 0;
}

/* ---- area fill (AreaMove/AreaDraw/AreaEllipse/AreaEnd) -------------------------- */

static uint32_t g_InitArea(M68K *c) {
    uint32_t ai = c->a[0], buf = c->a[1];
    int max = (int16_t)c->d[0];
    wr32(ai + AreaInfo_VctrTbl, buf);
    wr32(ai + AreaInfo_VctrPtr, buf);
    wr32(ai + AreaInfo_FlagTbl, buf + 4 * (uint32_t)max);
    wr32(ai + AreaInfo_FlagPtr, buf + 4 * (uint32_t)max);
    wr16(ai + AreaInfo_Count, 0);
    wr16(ai + AreaInfo_MaxCount, (uint32_t)max);
    return 0;
}

static int area_add(uint32_t ai, int x, int y, int flag) {
    int count = (int16_t)rd16(ai + AreaInfo_Count), max = (int16_t)rd16(ai + AreaInfo_MaxCount);
    if (count >= max) return -1;
    uint32_t v = rd32(ai + AreaInfo_VctrPtr), f = rd32(ai + AreaInfo_FlagPtr);
    wr16(v, (uint32_t)x); wr16(v + 2, (uint32_t)y);
    wr8(f, (uint32_t)flag);
    wr32(ai + AreaInfo_VctrPtr, v + 4);
    wr32(ai + AreaInfo_FlagPtr, f + 1);
    wr16(ai + AreaInfo_Count, (uint32_t)(count + 1));
    return 0;
}

static uint32_t g_AreaMove(M68K *c) {
    uint32_t ai = rd32(RPA(c) + RastPort_AreaInfo);
    if (!ai) return 0xFFFFFFFF;
    wr16(ai + AreaInfo_FirstX, c->d[0]);
    wr16(ai + AreaInfo_FirstY, c->d[1]);
    return (uint32_t)area_add(ai, (int16_t)c->d[0], (int16_t)c->d[1], 0x00);     /* new polygon */
}
static uint32_t g_AreaDraw(M68K *c) {
    uint32_t ai = rd32(RPA(c) + RastPort_AreaInfo);
    if (!ai) return 0xFFFFFFFF;
    return (uint32_t)area_add(ai, (int16_t)c->d[0], (int16_t)c->d[1], 0x01);
}
static uint32_t g_AreaEllipse(M68K *c) {
    uint32_t ai = rd32(RPA(c) + RastPort_AreaInfo);
    if (!ai) return 0xFFFFFFFF;
    int xc = (int16_t)c->d[0], yc = (int16_t)c->d[1], a = (int16_t)c->d[2], b = (int16_t)c->d[3];
    if (area_add(ai, xc, yc, 0x02) || area_add(ai, a, b, 0x02)) return 0xFFFFFFFF;
    return 0;
}

/* even-odd scanline fill of all polygons gathered since InitArea */
static uint32_t g_AreaEnd(M68K *c) {
    uint32_t rp = RPA(c), ai = rd32(rp + RastPort_AreaInfo);
    if (!ai) return 0xFFFFFFFF;
    RP r;
    int count = (int16_t)rd16(ai + AreaInfo_Count);
    uint32_t vt = rd32(ai + AreaInfo_VctrTbl), ft = rd32(ai + AreaInfo_FlagTbl);
    if (!rp_get(rp, &r) || count < 2) goto reset;
    {
        /* edges */
        typedef struct { int x0, y0, x1, y1; } Edge;
        Edge *e = malloc(sizeof(Edge) * (size_t)(count * 64 + 8));
        int ne = 0, start = 0, ymin = 1 << 30, ymax = -(1 << 30);
        for (int i = 0; i < count; i++) {
            int x = (int16_t)rd16(vt + 4 * (uint32_t)i), y = (int16_t)rd16(vt + 4 * (uint32_t)i + 2);
            int fl = rd8(ft + (uint32_t)i);
            if (fl == 0x02) {                                 /* ellipse: centre then radii */
                int a = (int16_t)rd16(vt + 4 * (uint32_t)(i + 1)), b = (int16_t)rd16(vt + 4 * (uint32_t)(i + 1) + 2);
                int px = x + a, py = y;
                for (int k = 1; k <= 64; k++) {
                    double t = k * 6.283185307179586 / 64;
                    int nx = x + (int)(a * __builtin_cos(t)), ny = y + (int)(b * __builtin_sin(t));
                    e[ne++] = (Edge){ px, py, nx, ny };
                    px = nx; py = ny;
                }
                if (y - b < ymin) ymin = y - b;
                if (y + b > ymax) ymax = y + b;
                i++;
                start = i + 1;
                continue;
            }
            if (fl == 0x00) start = i;
            int nx, ny;
            int last = (i + 1 >= count) || rd8(ft + (uint32_t)(i + 1)) != 0x01;
            if (last) { nx = (int16_t)rd16(vt + 4 * (uint32_t)start); ny = (int16_t)rd16(vt + 4 * (uint32_t)start + 2); }
            else { nx = (int16_t)rd16(vt + 4 * (uint32_t)(i + 1)); ny = (int16_t)rd16(vt + 4 * (uint32_t)(i + 1) + 2); }
            e[ne++] = (Edge){ x, y, nx, ny };
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
        int *xs = malloc(sizeof(int) * (size_t)(ne + 1));
        for (int y = ymin; y <= ymax; y++) {
            int nx = 0;
            for (int k = 0; k < ne; k++) {
                Edge g = e[k];
                if (g.y0 == g.y1) continue;
                int ylo = g.y0 < g.y1 ? g.y0 : g.y1, yhi = g.y0 < g.y1 ? g.y1 : g.y0;
                if (y < ylo || y >= yhi) continue;
                xs[nx++] = g.x0 + (int)((long)(y - g.y0) * (g.x1 - g.x0) / (g.y1 - g.y0));
            }
            for (int a = 1; a < nx; a++) for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
            for (int k = 0; k + 1 < nx; k += 2) rp_rectfill(&r, xs[k], y, xs[k + 1], y);
        }
        /* outline when AREAOUTLINE is set */
        if (rd16(rp + RastPort_Flags) & 0x08) {
            int fg = r.fg;
            r.fg = rd8(rp + RastPort_AOlPen);
            for (int k = 0; k < ne; k++) bm_line(&r.bm, e[k].x0, e[k].y0, e[k].x1, e[k].y1, r.fg, JAM1, r.mask);
            r.fg = fg;
        }
        free(xs);
        free(e);
    }
reset:
    wr32(ai + AreaInfo_VctrPtr, vt);
    wr32(ai + AreaInfo_FlagPtr, ft);
    wr16(ai + AreaInfo_Count, 0);
    gfx_touch();
    return 0;
}
static uint32_t g_InitTmpRas(M68K *c) {
    wr32(c->a[0] + TmpRas_RasPtr, c->a[1]);
    wr32(c->a[0] + TmpRas_Size, c->d[0]);
    return c->a[0];
}
static uint32_t g_Flood(M68K *c) {
    /* mode D2: 0 = outline (stop at AOlPen), 1 = colour (replace the colour at x,y) */
    RP r;
    if (!rp_get(RPA(c), &r)) return 0;
    int sx = (int16_t)c->d[0], sy = (int16_t)c->d[1], mode = (int)c->d[2];
    int w = r.bm.bpr * 8, h = r.bm.rows;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return 0;
    int target = bm_read(&r.bm, sx, sy), outline = rd8(RPA(c) + RastPort_AOlPen);
    uint8_t *seen = calloc((size_t)w * h, 1);
    int *stack = malloc(sizeof(int) * (size_t)w * h * 2 + 16), sp = 0;
    stack[sp++] = sx; stack[sp++] = sy;
    while (sp) {
        int y = stack[--sp], x = stack[--sp];
        if (x < 0 || y < 0 || x >= w || y >= h || seen[y * w + x]) continue;
        int p = bm_read(&r.bm, x, y);
        if (mode == 1 ? p != target : p == outline) continue;
        seen[y * w + x] = 1;
        stack[sp++] = x + 1; stack[sp++] = y; stack[sp++] = x - 1; stack[sp++] = y;
        stack[sp++] = x; stack[sp++] = y + 1; stack[sp++] = x; stack[sp++] = y - 1;
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (seen[y * w + x]) rp_plot(&r, x, y, 1);
    free(seen);
    free(stack);
    gfx_touch();
    return 1;
}

/* ---- text functions -------------------------------------------------------- */

static uint32_t g_Text(M68K *c) {
    char buf[1024];
    int n = (int)(c->d[0] & 0xFFFF);
    if (n > (int)sizeof buf) n = sizeof buf;
    for (int i = 0; i < n; i++) buf[i] = (char)rd8(c->a[0] + (uint32_t)i);
    rp_text(c->a[1], buf, n);
    return 0;
}
static uint32_t g_TextLength(M68K *c) {
    char buf[1024];
    int n = (int)(c->d[0] & 0xFFFF);
    if (n > (int)sizeof buf) n = sizeof buf;
    for (int i = 0; i < n; i++) buf[i] = (char)rd8(c->a[0] + (uint32_t)i);
    return (uint32_t)rp_text_length(c->a[1], buf, n);
}
static uint32_t g_TextExtent(M68K *c) {
    char buf[1024];
    int n = (int)(c->d[0] & 0xFFFF);
    if (n > (int)sizeof buf) n = sizeof buf;
    for (int i = 0; i < n; i++) buf[i] = (char)rd8(c->a[0] + (uint32_t)i);
    uint32_t rp = c->a[1], te = c->a[2], tf = rd32(rp + RastPort_Font);
    if (!tf) tf = topaz8;
    int w = rp_text_length(rp, buf, n), h = rd16(tf + TextFont_tf_YSize), base = rd16(tf + TextFont_tf_Baseline);
    wr16(te + TextExtent_te_Width, (uint32_t)w);
    wr16(te + TextExtent_te_Height, (uint32_t)h);
    wr16(te + TextExtent_te_Extent, 0);
    wr16(te + TextExtent_te_Extent + 2, (uint32_t)-base);
    wr16(te + TextExtent_te_Extent + 4, (uint32_t)(w - 1));
    wr16(te + TextExtent_te_Extent + 6, (uint32_t)(h - 1 - base));
    return 0;
}
static uint32_t g_TextFit(M68K *c) {
    /* rp A1, string A0, count D0, te A2, constrainingTE A3, direction D1, width D2, height D3 */
    uint32_t rp = c->a[1], tf = rd32(rp + RastPort_Font);
    if (!tf) tf = topaz8;
    int n = (int)(c->d[0] & 0xFFFF), maxw = (int)(c->d[2] & 0xFFFF), w = 0, k = 0;
    for (; k < n; k++) {
        int adv = char_advance(rp, tf, rd8(c->a[0] + (uint32_t)k));
        if (w + adv > maxw) break;
        w += adv;
    }
    if (c->a[2]) {
        wr16(c->a[2] + TextExtent_te_Width, (uint32_t)w);
        wr16(c->a[2] + TextExtent_te_Height, rd16(tf + TextFont_tf_YSize));
    }
    return (uint32_t)k;
}
static uint32_t g_SetFont(M68K *c) {
    uint32_t rp = c->a[1], tf = c->a[0];
    if (!tf) return 0;
    wr32(rp + RastPort_Font, tf);
    wr16(rp + RastPort_TxHeight, rd16(tf + TextFont_tf_YSize));
    wr16(rp + RastPort_TxWidth, rd16(tf + TextFont_tf_XSize));
    wr16(rp + RastPort_TxBaseline, rd16(tf + TextFont_tf_Baseline));
    return 0;
}
static uint32_t g_OpenFont(M68K *c) {
    uint32_t ta = c->a[0];
    char name[64];
    str_get(rd32(ta + TextAttr_ta_Name), name, sizeof name);
    return gfx_open_font(name, rd16(ta + TextAttr_ta_YSize));
}
static uint32_t g_CloseFont(M68K *c) {
    uint32_t f = c->a[1];
    if (f && rd16(f + TextFont_tf_Accessors)) wr16(f + TextFont_tf_Accessors, rd16(f + TextFont_tf_Accessors) - 1);
    return 0;
}
static uint32_t g_AskFont(M68K *c) {
    uint32_t rp = c->a[1], ta = c->a[0], tf = rd32(rp + RastPort_Font);
    wr32(ta + TextAttr_ta_Name, rd32(tf + LN_NAME));
    wr16(ta + TextAttr_ta_YSize, rd16(tf + TextFont_tf_YSize));
    wr8(ta + TextAttr_ta_Style, rd8(tf + TextFont_tf_Style));
    wr8(ta + TextAttr_ta_Flags, rd8(tf + TextFont_tf_Flags));
    return 0;
}
static uint32_t g_SetSoftStyle(M68K *c) {
    uint32_t rp = c->a[1];
    uint32_t st = (rd8(rp + RastPort_AlgoStyle) & ~c->d[1]) | (c->d[0] & c->d[1] & 7);
    wr8(rp + RastPort_AlgoStyle, st);
    return st;
}
static uint32_t g_AskSoftStyle(M68K *c) { (void)c; return 7; }
static uint32_t g_ClearEOL(M68K *c) {
    RP r;
    uint32_t rp = c->a[1];
    if (!rp_get(rp, &r)) return 0;
    int x = (int16_t)rd16(rp + RastPort_cp_x), y = (int16_t)rd16(rp + RastPort_cp_y) - rd16(rp + RastPort_TxBaseline);
    bm_fill(&r.bm, x, y, r.bm.bpr * 8 - 1, y + rd16(rp + RastPort_TxHeight) - 1, r.bg, JAM1, r.mask);
    gfx_touch();
    return 0;
}
static uint32_t g_ClearScreen(M68K *c) {
    RP r;
    uint32_t rp = c->a[1];
    if (!rp_get(rp, &r)) return 0;
    int y = (int16_t)rd16(rp + RastPort_cp_y) - rd16(rp + RastPort_TxBaseline);
    g_ClearEOL(c);
    bm_fill(&r.bm, 0, y + rd16(rp + RastPort_TxHeight), r.bm.bpr * 8 - 1, r.bm.rows - 1, r.bg, JAM1, r.mask);
    gfx_touch();
    return 0;
}
static uint32_t g_AddFont(M68K *c) { list_enqueue(GfxBase + GfxBase_TextFonts, c->a[1]); return 0; }
static uint32_t g_RemFont(M68K *c) { list_remove(c->a[1]); return 0; }

/* ---- colours -------------------------------------------------------------- */

static uint32_t cm_of_vp(uint32_t vp) { return vp ? rd32(vp + ViewPort_ColorMap) : 0; }

/* every colour change goes to the ColorMap (12-bit table) and to the screen
   palette (24-bit) used by the display */
static void set_colour(uint32_t vp, uint32_t cm, int n, uint32_t rgb24) {
    if (cm && n < (int)rd16(cm + ColorMap_Count)) {
        uint32_t ct = rd32(cm + ColorMap_ColorTable);
        uint32_t r = (rgb24 >> 20) & 15, g = (rgb24 >> 12) & 15, b = (rgb24 >> 4) & 15;
        if (ct) wr16(ct + 2 * (uint32_t)n, r << 8 | g << 4 | b);
    }
    uint32_t *pal = vp ? gfx_palette_of_vp(vp) : 0;
    if (pal && n < 256) pal[n] = rgb24;
    gfx_touch();
}

static uint32_t rgb4_to24(uint32_t r, uint32_t g, uint32_t b) { return (r & 15) * 0x110000 + (g & 15) * 0x1100 + (b & 15) * 0x11; }

static uint32_t g_SetRGB4(M68K *c) {
    uint32_t vp = c->a[0];
    set_colour(vp, cm_of_vp(vp), (int)(c->d[0] & 0xFF), rgb4_to24(c->d[1], c->d[2], c->d[3]));
    return 0;
}
static uint32_t g_LoadRGB4(M68K *c) {
    uint32_t vp = c->a[0], tab = c->a[1];
    for (uint32_t i = 0; i < (c->d[0] & 0xFFFF); i++) {
        uint32_t w = rd16(tab + 2 * i);
        set_colour(vp, cm_of_vp(vp), (int)i, rgb4_to24(w >> 8, w >> 4, w));
    }
    return 0;
}
static uint32_t g_SetRGB32(M68K *c) {
    uint32_t vp = c->a[0];
    set_colour(vp, cm_of_vp(vp), (int)(c->d[0] & 0xFF), (c->d[1] >> 24) << 16 | (c->d[2] >> 24) << 8 | (c->d[3] >> 24));
    return 0;
}
static uint32_t g_LoadRGB32(M68K *c) {
    /* table: (count << 16 | first), then count * 3 longs, ..., 0 */
    uint32_t vp = c->a[0], t = c->a[1];
    while (t) {
        uint32_t hdr = rd32(t);
        uint32_t count = hdr >> 16, first = hdr & 0xFFFF;
        if (!count) break;
        t += 4;
        for (uint32_t i = 0; i < count; i++, t += 12)
            set_colour(vp, cm_of_vp(vp), (int)(first + i), (rd32(t) >> 24) << 16 | (rd32(t + 4) >> 24) << 8 | (rd32(t + 8) >> 24));
    }
    return 0;
}
static uint32_t g_SetRGB4CM(M68K *c) { set_colour(0, c->a[0], (int)(c->d[0] & 0xFF), rgb4_to24(c->d[1], c->d[2], c->d[3])); return 0; }
static uint32_t g_SetRGB32CM(M68K *c) {
    set_colour(0, c->a[0], (int)(c->d[0] & 0xFF), (c->d[1] >> 24) << 16 | (c->d[2] >> 24) << 8 | (c->d[3] >> 24));
    return 0;
}
static uint32_t g_GetRGB4(M68K *c) {
    uint32_t cm = c->a[0], n = c->d[0];
    if (!cm || n >= rd16(cm + ColorMap_Count)) return 0xFFFFFFFF;
    return rd16(rd32(cm + ColorMap_ColorTable) + 2 * n);
}
static uint32_t g_GetRGB32(M68K *c) {
    uint32_t cm = c->a[0], first = c->d[0], n = c->d[1], tab = c->a[1];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = rd16(rd32(cm + ColorMap_ColorTable) + 2 * (first + i));
        wr32(tab + 12 * i, ((w >> 8) & 15) * 0x11111111u);
        wr32(tab + 12 * i + 4, ((w >> 4) & 15) * 0x11111111u);
        wr32(tab + 12 * i + 8, (w & 15) * 0x11111111u);
    }
    return 0;
}
static uint32_t g_GetColorMap(M68K *c) {
    uint32_t n = c->d[0] ? c->d[0] : 32;
    uint32_t cm = exec_allocmem(ColorMap_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(cm + ColorMap_Type, 1);                              /* COLORMAP_TYPE_V36 */
    wr16(cm + ColorMap_Count, n);
    wr32(cm + ColorMap_ColorTable, exec_allocmem(2 * n, MEMF_PUBLIC | MEMF_CLEAR));
    return cm;
}
static uint32_t g_FreeColorMap(M68K *c) {
    uint32_t cm = c->a[0];
    if (!cm) return 0;
    exec_freemem(rd32(cm + ColorMap_ColorTable), 2u * rd16(cm + ColorMap_Count));
    exec_freemem(cm, ColorMap_SIZEOF);
    return 0;
}
static uint32_t nearest(uint32_t cm, uint32_t r, uint32_t g, uint32_t b, int maxpen) {
    uint32_t ct = rd32(cm + ColorMap_ColorTable), n = rd16(cm + ColorMap_Count);
    if (maxpen >= 0 && (uint32_t)maxpen < n) n = (uint32_t)maxpen + 1;
    uint32_t best = 0;
    long bd = 1L << 40;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = rd16(ct + 2 * i);
        long dr = (long)((w >> 8) & 15) * 17 - (long)(r >> 24), dg = (long)((w >> 4) & 15) * 17 - (long)(g >> 24), db = (long)(w & 15) * 17 - (long)(b >> 24);
        long d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static uint32_t g_ObtainBestPenA(M68K *c) { return c->a[0] ? nearest(c->a[0], c->d[1], c->d[2], c->d[3], -1) : 0xFFFFFFFF; }
static uint32_t g_FindColor(M68K *c) { return nearest(c->a[3], c->d[1], c->d[2], c->d[3], (int)c->d[4]); }
static uint32_t g_ObtainPen(M68K *c) {
    /* cm A0, n D0 (-1 = any), r D1, g D2, b D3, flags D4 */
    uint32_t cm = c->a[0];
    int n = (int32_t)c->d[0];
    if (n < 0) n = (int)rd16(cm + ColorMap_Count) - 1;   /* hand out from the top */
    set_colour(0, cm, n, (c->d[1] >> 24) << 16 | (c->d[2] >> 24) << 8 | (c->d[3] >> 24));
    return (uint32_t)n;
}
static uint32_t g_ReleasePen(M68K *c) { (void)c; return 0; }
static uint32_t g_SetMaxPen(M68K *c) { (void)c; return 0; }
static uint32_t g_GetVPModeID(M68K *c) { return c->a[0] ? 0x8000 : 0xFFFFFFFF; }   /* HIRES_KEY */

/* ---- chunky pixels (V40) ---------------------------------------------------- */

static uint32_t g_WriteChunkyPixels(M68K *c) {
    RP r;
    if (!rp_get(c->a[0], &r)) return 0;
    int x0 = (int16_t)c->d[0], y0 = (int16_t)c->d[1], x1 = (int16_t)c->d[2], y1 = (int16_t)c->d[3];
    uint32_t arr = c->a[2], bpr = c->d[4];
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            bm_pixel(&r.bm, x, y, rd8(arr + (uint32_t)(y - y0) * bpr + (uint32_t)(x - x0)), JAM1, r.mask);
    gfx_touch();
    return 0;
}
static uint32_t g_WritePixelArray8(M68K *c) {
    /* rp A0, xstart D0, ystart D1, xstop D2, ystop D3, array A2, temprp A1 */
    RP r;
    if (!rp_get(c->a[0], &r)) return 0;
    int x0 = (int16_t)c->d[0], y0 = (int16_t)c->d[1], x1 = (int16_t)c->d[2], y1 = (int16_t)c->d[3];
    int w = ((x1 - x0 + 1) + 15) & ~15;
    uint32_t arr = c->a[2], n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++, n++) bm_pixel(&r.bm, x, y, rd8(arr + (uint32_t)((y - y0) * w + (x - x0))), JAM1, r.mask);
    gfx_touch();
    return n;
}
static uint32_t g_ReadPixelArray8(M68K *c) {
    RP r;
    if (!rp_get(c->a[0], &r)) return 0;
    int x0 = (int16_t)c->d[0], y0 = (int16_t)c->d[1], x1 = (int16_t)c->d[2], y1 = (int16_t)c->d[3];
    int w = ((x1 - x0 + 1) + 15) & ~15;
    uint32_t arr = c->a[2], n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++, n++) wr8(arr + (uint32_t)((y - y0) * w + (x - x0)), (uint32_t)bm_read(&r.bm, x, y));
    return n;
}
static uint32_t g_WritePixelLine8(M68K *c) {
    RP r;
    if (!rp_get(c->a[0], &r)) return 0;
    int x0 = (int16_t)c->d[0], y = (int16_t)c->d[1], w = (int16_t)c->d[2];
    for (int i = 0; i < w; i++) bm_pixel(&r.bm, x0 + i, y, rd8(c->a[2] + (uint32_t)i), JAM1, r.mask);
    gfx_touch();
    return (uint32_t)w;
}
static uint32_t g_ReadPixelLine8(M68K *c) {
    RP r;
    if (!rp_get(c->a[0], &r)) return 0;
    int x0 = (int16_t)c->d[0], y = (int16_t)c->d[1], w = (int16_t)c->d[2];
    for (int i = 0; i < w; i++) wr8(c->a[2] + (uint32_t)i, (uint32_t)bm_read(&r.bm, x0 + i, y));
    return (uint32_t)w;
}

/* ---- display timing and the no-op hardware layer ------------------------------ */

static uint32_t g_WaitTOF(M68K *c) {
    (void)c;
    uint32_t t = this_task();
    wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) & ~SIGF_SINGLE);
    uint64_t now = now_us(), frame = 20000;
    sched_timer_add(now + frame - now % frame, TIMER_SIGNAL, t, SIGF_SINGLE);   /* next 50 Hz tick */
    exec_wait(SIGF_SINGLE);
    return 0;
}
static uint32_t g_VBeamPos(M68K *c) { (void)c; return (uint32_t)((now_us() % 20000) * 312 / 20000); }
static uint32_t g_nop(M68K *c) { (void)c; return 0; }
static uint32_t g_OwnBlitter(M68K *c) { (void)c; return 0; }

/* ---- regions: kept as rectangle lists (layers' clip regions) ------------------- */

static uint32_t g_NewRegion(M68K *c) { (void)c; return exec_allocmem(12, MEMF_PUBLIC | MEMF_CLEAR); }
static uint32_t g_DisposeRegion(M68K *c) {
    uint32_t r = c->a[0];
    if (!r) return 0;
    uint32_t rr = rd32(r + 8);
    while (rr) { uint32_t n = rd32(rr); exec_freemem(rr, 16); rr = n; }
    exec_freemem(r, 12);
    return 0;
}
static uint32_t g_ClearRegion(M68K *c) {
    uint32_t r = c->a[0], rr = rd32(r + 8);
    while (rr) { uint32_t n = rd32(rr); exec_freemem(rr, 16); rr = n; }
    for (int i = 0; i < 12; i++) wr8(r + (uint32_t)i, 0);
    return 0;
}
static uint32_t g_OrRectRegion(M68K *c) {
    /* region A0, rectangle A1: appended (no merging), bounds grown */
    uint32_t r = c->a[0], rect = c->a[1];
    uint32_t rr = exec_allocmem(16, MEMF_PUBLIC | MEMF_CLEAR);
    for (int i = 0; i < 8; i++) wr8(rr + 8 + (uint32_t)i, rd8(rect + (uint32_t)i));
    wr32(rr, rd32(r + 8));
    if (rd32(r + 8)) wr32(rd32(r + 8) + 4, rr);
    wr32(r + 8, rr);
    int16_t x0 = (int16_t)rd16(rect), y0 = (int16_t)rd16(rect + 2), x1 = (int16_t)rd16(rect + 4), y1 = (int16_t)rd16(rect + 6);
    if (!rd32(rr)) { wr16(r, (uint16_t)x0); wr16(r + 2, (uint16_t)y0); wr16(r + 4, (uint16_t)x1); wr16(r + 6, (uint16_t)y1); }
    else {
        if (x0 < (int16_t)rd16(r)) wr16(r, (uint16_t)x0);
        if (y0 < (int16_t)rd16(r + 2)) wr16(r + 2, (uint16_t)y0);
        if (x1 > (int16_t)rd16(r + 4)) wr16(r + 4, (uint16_t)x1);
        if (y1 > (int16_t)rd16(r + 6)) wr16(r + 6, (uint16_t)y1);
    }
    return DOSTRUE;
}
static uint32_t g_true(M68K *c) { (void)c; return DOSTRUE; }

static const NativeEntry gfx_natives[] = {
    { "InitRastPort", g_InitRastPort }, { "InitBitMap", g_InitBitMap },
    { "AllocBitMap", g_AllocBitMap }, { "FreeBitMap", g_FreeBitMap }, { "GetBitMapAttr", g_GetBitMapAttr },
    { "AllocRaster", g_AllocRaster }, { "FreeRaster", g_FreeRaster },
    { "SetAPen", g_SetAPen }, { "SetBPen", g_SetBPen }, { "SetDrMd", g_SetDrMd },
    { "SetOutlinePen", g_SetOutlinePen }, { "SetWriteMask", g_SetWriteMask },
    { "GetAPen", g_GetAPen }, { "GetBPen", g_GetBPen }, { "GetDrMd", g_GetDrMd }, { "GetOutlinePen", g_GetOutlinePen },
    { "Move", g_Move }, { "Draw", g_Draw }, { "PolyDraw", g_PolyDraw },
    { "RectFill", g_RectFill }, { "EraseRect", g_EraseRect }, { "WritePixel", g_WritePixel },
    { "ReadPixel", g_ReadPixel }, { "SetRast", g_SetRast }, { "ScrollRaster", g_ScrollRaster },
    { "BltBitMap", g_BltBitMap }, { "BltBitMapRastPort", g_BltBitMapRastPort }, { "ClipBlit", g_ClipBlit },
    { "BltTemplate", g_BltTemplate }, { "BltClear", g_BltClear }, { "BltPattern", g_BltPattern },
    { "DrawEllipse", g_DrawEllipse },
    { "InitArea", g_InitArea }, { "AreaMove", g_AreaMove }, { "AreaDraw", g_AreaDraw },
    { "AreaEllipse", g_AreaEllipse }, { "AreaEnd", g_AreaEnd }, { "InitTmpRas", g_InitTmpRas }, { "Flood", g_Flood },
    { "Text", g_Text }, { "TextLength", g_TextLength }, { "TextExtent", g_TextExtent }, { "TextFit", g_TextFit },
    { "SetFont", g_SetFont }, { "OpenFont", g_OpenFont }, { "CloseFont", g_CloseFont }, { "AskFont", g_AskFont },
    { "SetSoftStyle", g_SetSoftStyle }, { "AskSoftStyle", g_AskSoftStyle },
    { "ClearEOL", g_ClearEOL }, { "ClearScreen", g_ClearScreen }, { "AddFont", g_AddFont }, { "RemFont", g_RemFont },
    { "SetRGB4", g_SetRGB4 }, { "LoadRGB4", g_LoadRGB4 }, { "SetRGB32", g_SetRGB32 }, { "LoadRGB32", g_LoadRGB32 },
    { "SetRGB4CM", g_SetRGB4CM }, { "SetRGB32CM", g_SetRGB32CM }, { "GetRGB4", g_GetRGB4 }, { "GetRGB32", g_GetRGB32 },
    { "GetColorMap", g_GetColorMap }, { "FreeColorMap", g_FreeColorMap },
    { "ObtainBestPenA", g_ObtainBestPenA }, { "FindColor", g_FindColor }, { "ObtainPen", g_ObtainPen },
    { "ReleasePen", g_ReleasePen }, { "SetMaxPen", g_SetMaxPen }, { "GetVPModeID", g_GetVPModeID },
    { "WriteChunkyPixels", g_WriteChunkyPixels }, { "WritePixelArray8", g_WritePixelArray8 },
    { "ReadPixelArray8", g_ReadPixelArray8 }, { "WritePixelLine8", g_WritePixelLine8 },
    { "ReadPixelLine8", g_ReadPixelLine8 },
    { "WaitTOF", g_WaitTOF }, { "WaitBOVP", g_WaitTOF }, { "VBeamPos", g_VBeamPos },
    { "WaitBlit", g_nop }, { "OwnBlitter", g_OwnBlitter }, { "DisownBlitter", g_nop },
    { "LockLayerRom", g_nop }, { "UnlockLayerRom", g_nop }, { "InitVPort", g_nop }, { "InitView", g_nop },
    { "MakeVPort", g_nop }, { "MrgCop", g_nop }, { "LoadView", g_nop }, { "FreeVPortCopLists", g_nop },
    { "FreeCprList", g_nop }, { "FreeCopList", g_nop }, { "VideoControl", g_nop },
    { "NewRegion", g_NewRegion }, { "DisposeRegion", g_DisposeRegion }, { "ClearRegion", g_ClearRegion },
    { "OrRectRegion", g_OrRectRegion }, { "AndRectRegion", g_true }, { "ClearRectRegion", g_true },
    { "OrRegionRegion", g_true }, { "AndRegionRegion", g_true }, { "XorRectRegion", g_true },
    { "XorRegionRegion", g_true },
    { 0, 0 }
};

void gfx_init(void) {
    GfxBase = lib_create("graphics.library", "graphics", GfxBase_SIZEOF, gfx_natives, 40, 1, "graphics 40.1 (AmigaMac)");
    list_new(GfxBase + GfxBase_TextFonts);
    topaz8 = make_font("topaz.font", 8);
    topaz9 = make_font("topaz.font", 9);
    list_addtail(GfxBase + GfxBase_TextFonts, topaz8);
    list_addtail(GfxBase + GfxBase_TextFonts, topaz9);
    wr32(GfxBase + GfxBase_DefaultFont, topaz8);
    wr16(GfxBase + GfxBase_DisplayFlags, 0x0004);              /* PAL */
    wr16(GfxBase + GfxBase_MaxDisplayRow, 312);
    wr16(GfxBase + GfxBase_NormalDisplayRows, 256);
    wr16(GfxBase + GfxBase_NormalDisplayColumns, 640);
    wr8(GfxBase + GfxBase_ChipRevBits0, 0x0F);                  /* AGA: Alice, Lisa, ECS Agnus/Denise */
    wr8(GfxBase + GfxBase_MemType, 0);
    lib_add(GfxBase);
}
