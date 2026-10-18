/* cybergraphics.library: the graphics-card interface of AmigaOS (CyberGraphX v41),
 * written from its public description. AmigaMac's "AmigaMac:" display modes are
 * its modes; their screens are true colour, 32 bits a pixel (PIXFMT_ARGB32).
 * Pixel arrays go to and from RastPorts in RGB, RGBA, ARGB, palette or grey;
 * LockBitMapTagList hands out the pixels themselves.
 */
#include "os.h"
#include "gfx.h"
#include <stdlib.h>
#include <string.h>

uint32_t CyberGfxBase;

#define PIXFMT_LUT8 0
#define PIXFMT_ARGB32 11
#define RECTFMT_RGB 0
#define RECTFMT_RGBA 1
#define RECTFMT_ARGB 2
#define RECTFMT_LUT8 3
#define RECTFMT_GREY8 4
#define CYBRMATTR_XMOD 0x80000001
#define CYBRMATTR_BPPIX 0x80000002
#define CYBRMATTR_DISPADR 0x80000003
#define CYBRMATTR_PIXFMT 0x80000004
#define CYBRMATTR_WIDTH 0x80000005
#define CYBRMATTR_HEIGHT 0x80000006
#define CYBRMATTR_DEPTH 0x80000007
#define CYBRMATTR_ISCYBERGFX 0x80000008
#define CYBRMATTR_ISLINEARMEM 0x80000009
#define CYBRIDATTR_PIXFMT 0x80000001
#define CYBRIDATTR_WIDTH 0x80000002
#define CYBRIDATTR_HEIGHT 0x80000003
#define CYBRIDATTR_DEPTH 0x80000004
#define CYBRIDATTR_BPPIX 0x80000005
#define CYBRBIDTG_DEPTH 0x80050001
#define CYBRBIDTG_NOMINALWIDTH 0x80050002
#define CYBRBIDTG_NOMINALHEIGHT 0x80050003
#define LBMI_WIDTH 0x84001001
#define LBMI_HEIGHT 0x84001002
#define LBMI_DEPTH 0x84001003
#define LBMI_PIXFMT 0x84001004
#define LBMI_BYTESPERPIX 0x84001005
#define LBMI_BYTESPERROW 0x84001006
#define LBMI_BASEADDRESS 0x84001007

extern int gfx_mode_info(uint32_t id, int *w, int *h, int *res, int *lace);
extern uint32_t gfx_mode_id(int i, char *name, int max);

static int rp_bm(uint32_t rp, BM *b) { return rp && bm_load(rd32(rp + RastPort_BitMap), b); }

/* ---- display modes -------------------------------------------------------------------- */

static int is_cyber(uint32_t id) { int w, h, res, lace; return gfx_mode_info(id, &w, &h, &res, &lace) && res == 3; }

static uint32_t x_IsCyberModeID(M68K *c) { return is_cyber(c->d[0]) ? DOSTRUE : 0; }

static uint32_t x_GetCyberIDAttr(M68K *c) {
    int w, h, res, lace;
    if (!gfx_mode_info(c->d[1], &w, &h, &res, &lace)) return 0;
    int deep = res == 3;
    switch (c->d[0]) {
    case CYBRIDATTR_PIXFMT: return deep ? PIXFMT_ARGB32 : PIXFMT_LUT8;
    case CYBRIDATTR_WIDTH: return (uint32_t)w;
    case CYBRIDATTR_HEIGHT: return (uint32_t)h;
    case CYBRIDATTR_DEPTH: return deep ? 24 : 8;
    case CYBRIDATTR_BPPIX: return deep ? 4 : 1;
    }
    return 0;
}

/* the smallest AmigaMac mode holding the wanted size */
static uint32_t x_BestCModeIDTagList(M68K *c) {
    int ww = (int)tag_get(c->a[0], CYBRBIDTG_NOMINALWIDTH, 640), hh = (int)tag_get(c->a[0], CYBRBIDTG_NOMINALHEIGHT, 480);
    uint32_t best = 0xFFFFFFFF, id;
    long area = 1L << 40;
    for (int i = 0; (id = gfx_mode_id(i, 0, 0)) != 0xFFFFFFFF; i++) {
        int w, h, res, lace;
        if (!gfx_mode_info(id, &w, &h, &res, &lace) || res != 3 || w < ww || h < hh) continue;
        if ((long)w * h < area) { area = (long)w * h; best = id; }
    }
    if (best == 0xFFFFFFFF)                                     /* bigger than all: the largest */
        for (int i = 0; (id = gfx_mode_id(i, 0, 0)) != 0xFFFFFFFF; i++) if (is_cyber(id)) best = id;
    return best;
}

/* AllocCModeListTagList: a List of CyberModeNode {Node, ModeText[24], DisplayID, Width, Height, Depth, DisplayTagList} */
#define CMN_SIZE 52
static uint32_t x_AllocCModeListTagList(M68K *c) {
    (void)c;
    uint32_t list = exec_allocmem(14, MEMF_PUBLIC | MEMF_CLEAR), id;
    list_new(list);
    char name[64];
    for (int i = 0; (id = gfx_mode_id(i, name, sizeof name)) != 0xFFFFFFFF; i++) {
        int w, h, res, lace;
        if (!gfx_mode_info(id, &w, &h, &res, &lace) || res != 3) continue;
        uint32_t n = exec_allocmem(CMN_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        for (int k = 0; name[k] && k < 23; k++) wr8(n + 14 + (uint32_t)k, (uint8_t)name[k]);
        wr32(n + LN_NAME, n + 14);
        wr32(n + 38, id);
        wr16(n + 42, (uint32_t)w); wr16(n + 44, (uint32_t)h); wr16(n + 46, 24);
        list_addtail(list, n);
    }
    return list;
}
static uint32_t x_FreeCModeList(M68K *c) {
    uint32_t list = c->a[0], n;
    if (!list) return 0;
    while ((n = list_remhead(list))) exec_freemem(n, CMN_SIZE);
    exec_freemem(list, 14);
    return 0;
}
static uint32_t x_CModeRequestTagList(M68K *c) { (void)c; return 0; }    /* no requester: "cancelled" */

/* ---- BitMaps ---------------------------------------------------------------------------- */

static uint32_t x_GetCyberMapAttr(M68K *c) {
    BM b;
    if (!bm_load(c->a[0], &b)) return 0;
    if (!b.chunky) return c->d[0] == CYBRMATTR_DEPTH ? (uint32_t)b.depth : c->d[0] == CYBRMATTR_WIDTH ? (uint32_t)b.width :
                          c->d[0] == CYBRMATTR_HEIGHT ? (uint32_t)b.rows : 0;
    switch (c->d[0]) {
    case CYBRMATTR_XMOD: return (uint32_t)b.width * 4;
    case CYBRMATTR_BPPIX: return 4;
    case CYBRMATTR_DISPADR: return b.data;
    case CYBRMATTR_PIXFMT: return PIXFMT_ARGB32;
    case CYBRMATTR_WIDTH: return (uint32_t)b.width;
    case CYBRMATTR_HEIGHT: return (uint32_t)b.rows;
    case CYBRMATTR_DEPTH: return 24;
    case CYBRMATTR_ISCYBERGFX: case CYBRMATTR_ISLINEARMEM: return DOSTRUE;
    }
    return 0;
}

/* LockBitMapTagList(bitmap, tags): the pixels' address and layout, for true-colour BitMaps */
static uint32_t x_LockBitMapTagList(M68K *c) {
    BM b;
    if (!bm_load(c->a[0], &b) || !b.chunky) return 0;
    for (uint32_t t = c->a[1]; t && rd32(t); t += 8) {
        uint32_t tag = rd32(t), dst = rd32(t + 4), v;
        switch (tag) {
        case LBMI_WIDTH: v = (uint32_t)b.width; break;
        case LBMI_HEIGHT: v = (uint32_t)b.rows; break;
        case LBMI_DEPTH: v = 24; break;
        case LBMI_PIXFMT: v = PIXFMT_ARGB32; break;
        case LBMI_BYTESPERPIX: v = 4; break;
        case LBMI_BYTESPERROW: v = (uint32_t)b.width * 4; break;
        case LBMI_BASEADDRESS: v = b.data; break;
        default: continue;
        }
        if (dst) wr32(dst, v);
    }
    return c->a[0];                                             /* the handle */
}
static uint32_t x_UnLockBitMap(M68K *c) { (void)c; gfx_touch(); return 0; }

/* ---- pixels ------------------------------------------------------------------------------ */

static uint32_t x_ReadRGBPixel(M68K *c) {
    BM b;
    return rp_bm(c->a[1], &b) ? 0xFF000000u | bm_read_rgb(&b, (int16_t)c->d[0], (int16_t)c->d[1]) : 0;
}
static uint32_t x_WriteRGBPixel(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return (uint32_t)-1;
    bm_put_rgb(&b, (int16_t)c->d[0], (int16_t)c->d[1], c->d[2] & 0xFFFFFF);
    gfx_touch();
    return 0;
}

/* one pixel of a rectangle in a given format */
static int bytes_of(int fmt) { return fmt == RECTFMT_RGB ? 3 : fmt == RECTFMT_RGBA || fmt == RECTFMT_ARGB ? 4 : 1; }
static uint32_t fetch(uint32_t a, int fmt, const uint32_t *pal) {
    switch (fmt) {
    case RECTFMT_RGB: return rd8(a) << 16 | rd8(a + 1) << 8 | rd8(a + 2);
    case RECTFMT_RGBA: return rd8(a) << 16 | rd8(a + 1) << 8 | rd8(a + 2);
    case RECTFMT_ARGB: return rd8(a + 1) << 16 | rd8(a + 2) << 8 | rd8(a + 3);
    case RECTFMT_GREY8: return rd8(a) * 0x010101u;
    case RECTFMT_LUT8: return pal ? pal[rd8(a)] : rd8(a) * 0x010101u;
    }
    return 0;
}
static void store(uint32_t a, int fmt, uint32_t rgb, int pen) {
    uint32_t r = rgb >> 16 & 0xFF, g = rgb >> 8 & 0xFF, bl = rgb & 0xFF;
    switch (fmt) {
    case RECTFMT_RGB: wr8(a, r); wr8(a + 1, g); wr8(a + 2, bl); break;
    case RECTFMT_RGBA: wr8(a, r); wr8(a + 1, g); wr8(a + 2, bl); wr8(a + 3, 0xFF); break;
    case RECTFMT_ARGB: wr8(a, 0xFF); wr8(a + 1, r); wr8(a + 2, g); wr8(a + 3, bl); break;
    case RECTFMT_GREY8: wr8(a, (r * 77 + g * 151 + bl * 28) >> 8); break;
    case RECTFMT_LUT8: wr8(a, (uint32_t)pen); break;
    }
}

/* WritePixelArray(src, SrcX, SrcY, SrcMod, rp, DestX, DestY, SizeX, SizeY, SrcFormat) */
static uint32_t x_WritePixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return 0;
    int fmt = (int)(c->d[7] & 0xFF), bpp = bytes_of(fmt), mod = (int)(c->d[2] & 0xFFFF);
    int sx = (int)(c->d[0] & 0xFFFF), sy = (int)(c->d[1] & 0xFFFF), dx = (int16_t)c->d[3], dy = (int16_t)c->d[4];
    int w = (int)(c->d[5] & 0xFFFF), h = (int)(c->d[6] & 0xFFFF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t a = c->a[0] + (uint32_t)((sy + y) * mod + (sx + x) * bpp);
            if (fmt == RECTFMT_LUT8 && !b.chunky) bm_pixel(&b, dx + x, dy + y, (int)rd8(a), JAM1, 0xFF);
            else bm_put_rgb(&b, dx + x, dy + y, fetch(a, fmt, b.pal));
        }
    gfx_touch();
    return (uint32_t)(w * h);
}

/* ReadPixelArray(dst, DestX, DestY, DestMod, rp, SrcX, SrcY, SizeX, SizeY, DestFormat) */
static uint32_t x_ReadPixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return 0;
    int fmt = (int)(c->d[7] & 0xFF), bpp = bytes_of(fmt), mod = (int)(c->d[2] & 0xFFFF);
    int dx = (int)(c->d[0] & 0xFFFF), dy = (int)(c->d[1] & 0xFFFF), sx = (int16_t)c->d[3], sy = (int16_t)c->d[4];
    int w = (int)(c->d[5] & 0xFFFF), h = (int)(c->d[6] & 0xFFFF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t a = c->a[0] + (uint32_t)((dy + y) * mod + (dx + x) * bpp);
            store(a, fmt, bm_read_rgb(&b, sx + x, sy + y), bm_read(&b, sx + x, sy + y));
        }
    return (uint32_t)(w * h);
}

/* WriteLUTPixelArray(src, SrcX, SrcY, SrcMod, rp, ColorTab, DestX, DestY, SizeX, SizeY, CTFormat):
   bytes indexing a table of 0x00RRGGBB */
static uint32_t x_WriteLUTPixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b) || !c->a[2]) return 0;
    int mod = (int)(c->d[2] & 0xFFFF), sx = (int)(c->d[0] & 0xFFFF), sy = (int)(c->d[1] & 0xFFFF);
    int dx = (int16_t)c->d[3], dy = (int16_t)c->d[4], w = (int)(c->d[5] & 0xFFFF), h = (int)(c->d[6] & 0xFFFF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = (int)rd8(c->a[0] + (uint32_t)((sy + y) * mod + sx + x));
            bm_put_rgb(&b, dx + x, dy + y, rd32(c->a[2] + 4 * (uint32_t)i) & 0xFFFFFF);
        }
    gfx_touch();
    return (uint32_t)(w * h);
}

/* ScalePixelArray(src, SrcW, SrcH, SrcMod, rp, DestX, DestY, DestW, DestH, SrcFormat) */
static uint32_t x_ScalePixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return 0;
    int sw = (int)(c->d[0] & 0xFFFF), sh = (int)(c->d[1] & 0xFFFF), mod = (int)(c->d[2] & 0xFFFF);
    int dx = (int16_t)c->d[3], dy = (int16_t)c->d[4], dw = (int)(c->d[5] & 0xFFFF), dh = (int)(c->d[6] & 0xFFFF);
    int fmt = (int)(c->d[7] & 0xFF), bpp = bytes_of(fmt);
    if (!sw || !sh) return 0;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            uint32_t a = c->a[0] + (uint32_t)((y * sh / dh) * mod + (x * sw / dw) * bpp);
            bm_put_rgb(&b, dx + x, dy + y, fetch(a, fmt, b.pal));
        }
    gfx_touch();
    return (uint32_t)(dw * dh);
}

static uint32_t x_FillPixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return 0;
    int x0 = (int16_t)c->d[0], y0 = (int16_t)c->d[1], w = (int)(c->d[2] & 0xFFFF), h = (int)(c->d[3] & 0xFFFF);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) bm_put_rgb(&b, x0 + x, y0 + y, c->d[4] & 0xFFFFFF);
    gfx_touch();
    return (uint32_t)(w * h);
}
static uint32_t x_InvertPixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return 0;
    int x0 = (int16_t)c->d[0], y0 = (int16_t)c->d[1], w = (int)(c->d[2] & 0xFFFF), h = (int)(c->d[3] & 0xFFFF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) bm_put_rgb(&b, x0 + x, y0 + y, bm_read_rgb(&b, x0 + x, y0 + y) ^ 0xFFFFFF);
    gfx_touch();
    return (uint32_t)(w * h);
}
/* MovePixelArray(SrcX, SrcY, rp, DestX, DestY, SizeX, SizeY): within the RastPort */
static uint32_t x_MovePixelArray(M68K *c) {
    BM b;
    if (!rp_bm(c->a[1], &b)) return 0;
    int w = (int)(c->d[4] & 0xFFFF), h = (int)(c->d[5] & 0xFFFF);
    bm_copy(&b, (int16_t)c->d[0], (int16_t)c->d[1], &b, (int16_t)c->d[2], (int16_t)c->d[3], w, h, 0xC0, 0xFF);
    gfx_touch();
    return (uint32_t)(w * h);
}

/* DoCDrawMethodTagList(hook, rp, tags): the hook draws into the pixels itself, getting
   a CDrawMsg {MemPtr, offx, offy, xsize, ysize, BytesPerRow, BytesPerPix, ColorModel} */
static uint32_t x_DoCDrawMethodTagList(M68K *c) {
    BM b;
    if (!c->a[0] || !rp_bm(c->a[1], &b) || !b.chunky) return 0;
    uint32_t msg = exec_allocmem(28, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(msg, b.data);
    wr32(msg + 4, 0); wr32(msg + 8, 0);
    wr32(msg + 12, (uint32_t)b.width); wr32(msg + 16, (uint32_t)b.rows);
    wr16(msg + 20, (uint32_t)b.width * 4); wr16(msg + 22, 4);
    wr32(msg + 24, PIXFMT_ARGB32);
    call_hook(c, c->a[0], c->a[1], msg);
    exec_freemem(msg, 28);
    gfx_touch();
    return 0;
}
static uint32_t x_Nop(M68K *c) { (void)c; return 0; }

static const NativeEntry cgx_natives[] = {
    { "IsCyberModeID", x_IsCyberModeID }, { "BestCModeIDTagList", x_BestCModeIDTagList },
    { "CModeRequestTagList", x_CModeRequestTagList }, { "AllocCModeListTagList", x_AllocCModeListTagList },
    { "FreeCModeList", x_FreeCModeList }, { "ScalePixelArray", x_ScalePixelArray },
    { "GetCyberMapAttr", x_GetCyberMapAttr }, { "GetCyberIDAttr", x_GetCyberIDAttr },
    { "ReadRGBPixel", x_ReadRGBPixel }, { "WriteRGBPixel", x_WriteRGBPixel },
    { "ReadPixelArray", x_ReadPixelArray }, { "WritePixelArray", x_WritePixelArray },
    { "MovePixelArray", x_MovePixelArray }, { "InvertPixelArray", x_InvertPixelArray },
    { "FillPixelArray", x_FillPixelArray }, { "DoCDrawMethodTagList", x_DoCDrawMethodTagList },
    { "CVideoCtrlTagList", x_Nop }, { "LockBitMapTagList", x_LockBitMapTagList },
    { "UnLockBitMap", x_UnLockBitMap }, { "UnLockBitMapTagList", x_UnLockBitMap },
    { "ExtractColor", x_Nop }, { "WriteLUTPixelArray", x_WriteLUTPixelArray },
    { 0, 0 }
};

void cybergfx_init(void) {
    CyberGfxBase = lib_create("cybergraphics.library", "cybergraphics", LIB_SIZE, cgx_natives, 41, 1, "cybergraphics 41.1 (AmigaMac)");
    lib_add(CyberGfxBase);
}
