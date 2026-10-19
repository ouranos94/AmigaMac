/* icon.library, written from the icon.library autodocs and the documented
 * layout of .info files (workbench/workbench.h):
 *   DiskObject (78 bytes, pointer fields only say "present")
 *   [OldDrawerData 56]  if do_DrawerData
 *   Image header 20 + planes   (gadget render)
 *   [Image header 20 + planes] if the gadget has a SelectRender
 *   [ULONG len + DefaultTool]  if do_DefaultTool
 *   [ULONG (n+1)*4, then n x (ULONG len + string)] if do_ToolTypes
 *   [ULONG len + ToolWindow]   if do_ToolWindow
 *   [ULONG dd_Flags, UWORD dd_ViewModes] if drawer data and revision >= 1
 * Then, from AmigaOS 3.5 on, colour images: an IFF FORM ICON with a FACE chunk
 * (size, flags) and one or two IMAG chunks (normal, selected: pens, RLE-packed,
 * and a palette). Those are kept natively, next to the DiskObject, raw too so
 * that PutDiskObject writes them back. NewIcons, the colour images in tool
 * types from before 3.5, are read too, as 3.5's icon.library does.
 * A loaded DiskObject is one AllocVec block, so FreeDiskObject is FreeVec. */
#include "wb.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include "icons_default.h"
#include "intui.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint32_t IconBase;

#define GADGET_OFF DiskObject_do_Gadget

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* ---- parsing a .info image ---------------------------------------------------------- */

typedef struct { const uint8_t *p; size_t len, pos; int bad; } Rd;
static const uint8_t *take(Rd *r, size_t n) {
    if (r->bad || r->pos + n > r->len) { r->bad = 1; return 0; }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}

static int image_bytes(const uint8_t *hdr) {
    int w = (int16_t)be16(hdr + 4), h = (int16_t)be16(hdr + 6), d = (int16_t)be16(hdr + 8);
    if (w <= 0 || h <= 0 || d <= 0 || d > 8 || w > 1024 || h > 1024) return -1;
    return ((w + 15) / 16) * 2 * h * d;
}

/* copy n bytes into 68k memory at *at (word aligned) and return the address */
static uint32_t put_bytes(uint32_t *at, const uint8_t *src, size_t n) {
    uint32_t a = *at;
    for (size_t i = 0; i < n; i++) wr8(a + (uint32_t)i, src[i]);
    *at = (a + (uint32_t)n + 1) & ~1u;
    return a;
}

static uint32_t put_image(uint32_t *at, Rd *r) {
    const uint8_t *hdr = take(r, Image_SIZEOF);
    if (!hdr) return 0;
    int n = image_bytes(hdr);
    if (n < 0) { r->bad = 1; return 0; }
    uint32_t img = put_bytes(at, hdr, Image_SIZEOF);
    wr32(img + Image_NextImage, 0);
    if (be32(hdr + 10)) {
        const uint8_t *data = take(r, (size_t)n);
        if (!data) return 0;
        wr32(img + Image_ImageData, put_bytes(at, data, (size_t)n));
    }
    return img;
}

static uint32_t put_string(uint32_t *at, Rd *r) {
    const uint8_t *l = take(r, 4);
    if (!l) return 0;
    uint32_t n = be32(l);
    if (n > 65536) { r->bad = 1; return 0; }
    const uint8_t *s = take(r, n);
    if (!s) return 0;
    uint32_t a = put_bytes(at, s, n);
    wr8(a + (n ? n - 1 : 0), 0);                                     /* make sure it ends */
    if (!n) { wr8(a, 0); *at += 2; }
    return a;
}

/* ---- colour icons (FORM ICON) -------------------------------------------------------- */

typedef struct ColorIcon {
    uint32_t dobj;
    int w, h, frameless;
    int nimg;
    uint8_t *pix[2];                /* pens of each image, w x h */
    uint32_t pal[2][256];           /* their palettes, 0x00RRGGBB */
    int ncol[2], trans[2];          /* colours, transparent pen (-1 none) */
    uint8_t *raw;                   /* the FORM as read */
    size_t rawlen;
    int map_screen;                 /* pens mapped for this screen (id) */
    int16_t map[2][256];
    int aspect;                     /* FACE's aspect byte, 0 unknown */
    int newicon;                    /* read from NewIcons tool types */
    int dirty;                      /* changed by IconControlA: written anew */
    uint32_t data68[2], pal68[2], mask68[2];   /* copies handed to programs (68k memory) */
    struct ColorIcon *next;
} ColorIcon;
static ColorIcon *colour_icons;

static ColorIcon *colour_of(uint32_t dobj) {
    for (ColorIcon *c = colour_icons; c; c = c->next) if (c->dobj == dobj) return c;
    return 0;
}
static void colour_free(uint32_t dobj) {
    for (ColorIcon **pp = &colour_icons; *pp; pp = &(*pp)->next)
        if ((*pp)->dobj == dobj) {
            ColorIcon *c = *pp;
            *pp = c->next;
            for (int k = 0; k < 2; k++) {
                if (c->data68[k]) exec_freevec(c->data68[k]);
                if (c->pal68[k]) exec_freevec(c->pal68[k]);
                if (c->mask68[k]) exec_freevec(c->mask68[k]);
            }
            free(c->pix[0]); free(c->pix[1]); free(c->raw); free(c);
            return;
        }
}

/* the packed form: a bit stream; a control byte n < 128 copies n+1 values,
   n > 128 repeats the next value 257-n times; values are `bits` wide */
typedef struct { const uint8_t *p; size_t len, bit; } Bits;
static int get_bits(Bits *b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (b->bit >= b->len * 8) return -1;
        v = v << 1 | ((b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
        b->bit++;
    }
    return v;
}
static int unpack(const uint8_t *src, size_t len, int packed, int bits, uint8_t *out, int count) {
    if (!packed) {
        if ((size_t)count > len) return 0;
        memcpy(out, src, (size_t)count);
        return 1;
    }
    Bits b = { src, len, 0 };
    int k = 0;
    while (k < count) {
        int ctl = get_bits(&b, 8);
        if (ctl < 0) return 0;
        if (ctl < 128) {
            for (int i = 0; i <= ctl && k < count; i++) { int v = get_bits(&b, bits); if (v < 0) return 0; out[k++] = (uint8_t)v; }
        } else if (ctl > 128) {
            int v = get_bits(&b, bits);
            if (v < 0) return 0;
            for (int i = 0; i < 257 - ctl && k < count; i++) out[k++] = (uint8_t)v;
        }
    }
    return 1;
}

static void parse_colour(uint32_t dobj, const uint8_t *p, size_t len) {
    /* the FORM may follow at any even offset of the tail */
    size_t at = 0;
    while (at + 12 <= len && !(memcmp(p + at, "FORM", 4) == 0 && memcmp(p + at + 8, "ICON", 4) == 0)) at++;
    if (at + 12 > len) return;
    size_t flen = be32(p + at + 4) + 8;
    if (at + flen > len) flen = len - at;
    ColorIcon *c = calloc(1, sizeof *c);
    c->dobj = dobj;
    c->trans[0] = c->trans[1] = -1;
    c->map_screen = -1;
    for (size_t q = at + 12; q + 8 <= at + flen;) {
        size_t sz = be32(p + q + 4);
        const uint8_t *d = p + q + 8;
        if (q + 8 + sz > at + flen) break;
        if (!memcmp(p + q, "FACE", 4) && sz >= 6) {
            c->w = d[0] + 1; c->h = d[1] + 1; c->frameless = d[2] & 1; c->aspect = d[3];
        } else if (!memcmp(p + q, "IMAG", 4) && sz >= 10 && c->w && c->nimg < 2) {
            int k = c->nimg, flags = d[2], depth = d[5];
            int isize = (int)be16(d + 6) + 1, psize = (int)be16(d + 8) + 1;
            if (depth < 1 || depth > 8 || 10 + (size_t)isize > sz) break;
            c->pix[k] = calloc((size_t)(c->w * c->h), 1);
            if (!unpack(d + 10, (size_t)isize, d[3] == 1, depth, c->pix[k], c->w * c->h)) { free(c->pix[k]); c->pix[k] = 0; break; }
            if (flags & 1) c->trans[k] = d[0];
            if (flags & 2) {
                uint8_t rgb[768];
                int n = d[1] + 1;
                if (10 + (size_t)isize + (size_t)psize <= sz && unpack(d + 10 + isize, (size_t)psize, d[4] == 1, 8, rgb, n * 3)) {
                    c->ncol[k] = n;
                    for (int i = 0; i < n; i++) c->pal[k][i] = (uint32_t)rgb[i * 3] << 16 | (uint32_t)rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2];
                }
            } else if (k == 1) {                                     /* the selected image shares the palette */
                c->ncol[1] = c->ncol[0];
                memcpy(c->pal[1], c->pal[0], sizeof c->pal[0]);
            }
            c->nimg++;
        }
        q += 8 + ((sz + 1) & ~(size_t)1);
    }
    if (!c->nimg || !c->ncol[0]) { free(c->pix[0]); free(c->pix[1]); free(c); return; }
    c->raw = malloc(flen);
    memcpy(c->raw, p + at, flen);
    c->rawlen = flen;
    c->next = colour_icons;
    colour_icons = c;
}

/* NewIcons (from before AmigaOS 3.5, read by its icon.library too): the
   images are tool types, "IM1=" lines for the normal one, "IM2=" for the
   selected one. Each line is a stream of 7-bit values, one a character
   (0x20-0x6F: 0-0x4F, 0xA1-0xD0: 0x50-0x7F, 0xD1-0xFF: that many minus 0xD0
   zero values). The first line starts with a header: 'B' transparent pen 0
   or 'C' none, width + 0x21, height + 0x21, the colour count in two 6-bit
   characters (+ 0x21). Then the palette (8-bit R, G, B), then the pens, as
   many bits each as the colour count needs. Palette and pens each start on a
   line of their own; a line's left-over bits are dropped. */
static int newicon_image(uint32_t tt, const char *key, ColorIcon *c, int k) {
    int w = 0, h = 0, n = 0, depth = 1, got_pal = 0, npix = 0, header = 0;
    uint8_t pal[256 * 3];
    for (uint32_t i = 0; tt && rd32(tt + 4 * i) && i < 1000; i++) {
        uint32_t s = rd32(tt + 4 * i);
        char pre[5];
        for (int j = 0; j < 4; j++) pre[j] = (char)rd8(s + (uint32_t)j);
        pre[4] = 0;
        if (strcmp(pre, key)) continue;
        s += 4;
        if (!header) {
            int t = (int)rd8(s);
            w = (int)rd8(s + 1) - 0x21; h = (int)rd8(s + 2) - 0x21;
            n = (((int)rd8(s + 3) - 0x21) << 6) + ((int)rd8(s + 4) - 0x21);
            if (w < 1 || h < 1 || w > 256 || h > 256 || n < 1 || n > 256) return 0;
            if (c->w && (c->w != w || c->h != h)) return 0;     /* both images are one size */
            c->w = w; c->h = h;
            while ((1 << depth) < n) depth++;
            c->trans[k] = t == 'B' ? 0 : -1;
            c->pix[k] = calloc((size_t)(w * h), 1);
            header = 1;
            s += 5;
        }
        uint32_t buf = 0;
        int nb = 0, pal_line = got_pal < n * 3;
        for (uint32_t ch; (ch = rd8(s)) != 0; s++) {
            int reps = 1, v;
            if (ch >= 0xD1) { reps = (int)ch - 0xD0; v = 0; }
            else if (ch >= 0xA0) v = (int)ch - 0x51;
            else v = (int)ch - 0x20;
            for (int r = 0; r < reps; r++) {
                if (pal_line && got_pal == n * 3) break;
                buf = (buf << 7 | (uint32_t)(v & 0x7F)) & 0x3FFF;
                nb += 7;
                if (pal_line) {
                    if (nb >= 8) { nb -= 8; pal[got_pal++] = (uint8_t)(buf >> nb); }
                } else
                    while (nb >= depth && npix < w * h) { nb -= depth; c->pix[k][npix++] = (uint8_t)((buf >> nb) & ((1u << depth) - 1)); }
            }
            if (pal_line && got_pal == n * 3) break;            /* the rest of the palette's line: dropped */
        }
    }
    if (!header) return 0;
    if (got_pal < n * 3 || npix < w * h) { free(c->pix[k]); c->pix[k] = 0; return 0; }
    c->ncol[k] = n;
    for (int i = 0; i < n; i++) c->pal[k][i] = (uint32_t)pal[i * 3] << 16 | (uint32_t)pal[i * 3 + 1] << 8 | pal[i * 3 + 2];
    return 1;
}

static void parse_newicons(uint32_t dobj) {
    uint32_t tt = rd32(dobj + DiskObject_do_ToolTypes);
    if (!tt || !icon_find_tooltype(tt, "IM1")) return;
    ColorIcon *c = calloc(1, sizeof *c);
    c->dobj = dobj;
    c->trans[0] = c->trans[1] = -1;
    c->map_screen = -1;
    if (!newicon_image(tt, "IM1=", c, 0)) { free(c->pix[0]); free(c); return; }
    c->nimg = 1 + newicon_image(tt, "IM2=", c, 1);
    c->newicon = 1;
    c->next = colour_icons;                                     /* raw stays empty: the tool types carry them */
    colour_icons = c;
}

static uint32_t parse_info(const uint8_t *buf, size_t len) {
    Rd r = { buf, len, 0, 0 };
    const uint8_t *hdr = take(&r, DiskObject_SIZEOF);
    if (!hdr || be16(hdr) != WB_DISKMAGIC) return 0;
    /* count the tool types first to size the pointer array */
    uint32_t blk = exec_allocvec((uint32_t)len + 1024, MEMF_PUBLIC | MEMF_CLEAR);
    if (!blk) return 0;
    uint32_t dobj = blk, at = blk + 80;
    for (int i = 0; i < DiskObject_SIZEOF; i++) wr8(dobj + (uint32_t)i, hdr[i]);
    uint32_t g = dobj + GADGET_OFF;
    int has_dd = be32(hdr + DiskObject_do_DrawerData) != 0;
    int has_sel = be32(hdr + GADGET_OFF + Gadget_SelectRender) != 0;
    int rev = (int)(be32(hdr + GADGET_OFF + Gadget_UserData) & 0xFF);
    wr32(g + Gadget_NextGadget, 0);
    wr32(g + Gadget_GadgetText, 0);
    wr32(g + Gadget_SpecialInfo, 0);
    wr32(g + Gadget_MutualExclude, 0);
    uint32_t dd = 0;
    if (has_dd) {
        const uint8_t *d = take(&r, OldDrawerData_SIZEOF);
        if (d) { dd = at; at += 64; for (int i = 0; i < OldDrawerData_SIZEOF; i++) wr8(dd + (uint32_t)i, d[i]); }
    }
    wr32(dobj + DiskObject_do_DrawerData, dd);
    uint32_t img = put_image(&at, &r);
    wr32(g + Gadget_GadgetRender, img);
    wr32(g + Gadget_SelectRender, has_sel ? put_image(&at, &r) : 0);
    if (r.bad || !img) { exec_freevec(blk); return 0; }
    wr32(dobj + DiskObject_do_DefaultTool, be32(hdr + DiskObject_do_DefaultTool) ? put_string(&at, &r) : 0);
    uint32_t tt = 0;
    if (be32(hdr + DiskObject_do_ToolTypes)) {
        const uint8_t *l = take(&r, 4);
        uint32_t n = l ? be32(l) / 4 : 0;
        if (n > 0) n--;
        if (n > 1000) r.bad = 1;
        if (!r.bad) {
            tt = at;
            at += 4 * (n + 1);
            for (uint32_t i = 0; i < n && !r.bad; i++) wr32(tt + 4 * i, put_string(&at, &r));
            wr32(tt + 4 * n, 0);
        }
    }
    wr32(dobj + DiskObject_do_ToolTypes, tt);
    wr32(dobj + DiskObject_do_ToolWindow, be32(hdr + DiskObject_do_ToolWindow) ? put_string(&at, &r) : 0);
    if (dd && rev >= WB_DISKREVISION) {
        const uint8_t *x = take(&r, 6);
        if (x) { wr32(dd + DrawerData_dd_Flags, be32(x)); wr16(dd + DrawerData_dd_ViewModes, be16(x + 4)); }
    }
    if (r.bad) {
        /* a truncated tail: keep what was read, drop the broken parts */
        if (!rd32(dobj + DiskObject_do_ToolWindow)) wr32(dobj + DiskObject_do_ToolWindow, 0);
    } else if (r.pos < len) parse_colour(dobj, buf + r.pos, len - r.pos);
    if (!colour_of(dobj)) parse_newicons(dobj);
    return dobj;
}

/* ---- writing ------------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t len, cap; } Wr;
static void w_bytes(Wr *w, const void *src, size_t n) {
    if (w->len + n > w->cap) { w->cap = (w->len + n) * 2 + 256; w->p = realloc(w->p, w->cap); }
    memcpy(w->p + w->len, src, n);
    w->len += n;
}
static void w32(Wr *w, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; w_bytes(w, b, 4); }
static void w16(Wr *w, uint32_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; w_bytes(w, b, 2); }

/* the colour images as a FORM ICON: FACE, then each IMAG unpacked, one byte a pixel */
static void w_colour(Wr *w, ColorIcon *c) {
    if (!c->w || !c->h || c->w > 256 || c->h > 256 || !c->pix[0]) return;
    Wr f = { 0 };
    w_bytes(&f, "ICON", 4);
    int maxpal = c->ncol[0] > c->ncol[1] ? c->ncol[0] : c->ncol[1];
    w_bytes(&f, "FACE", 4); w32(&f, 6);
    uint8_t face[6] = { (uint8_t)(c->w - 1), (uint8_t)(c->h - 1), (uint8_t)(c->frameless ? 1 : 0), (uint8_t)c->aspect,
                        (uint8_t)((maxpal * 3 - 1) >> 8), (uint8_t)(maxpal * 3 - 1) };
    w_bytes(&f, face, 6);
    for (int k = 0; k < c->nimg && k < 2 && c->pix[k]; k++) {
        int n = c->ncol[k] ? c->ncol[k] : 1, own = k == 0 || memcmp(c->pal[0], c->pal[1], sizeof c->pal[0]) || c->ncol[0] != c->ncol[1];
        int isize = c->w * c->h, psize = own ? n * 3 : 0;
        size_t sz = 10 + (size_t)isize + (size_t)psize;
        w_bytes(&f, "IMAG", 4); w32(&f, (uint32_t)sz);
        uint8_t h[10] = { (uint8_t)(c->trans[k] < 0 ? 0 : c->trans[k]), (uint8_t)(n - 1),
                          (uint8_t)((c->trans[k] >= 0 ? 1 : 0) | (own ? 2 : 0)), 0, 0, 8,
                          (uint8_t)((isize - 1) >> 8), (uint8_t)(isize - 1), (uint8_t)((psize ? psize - 1 : 0) >> 8), (uint8_t)(psize ? psize - 1 : 0) };
        w_bytes(&f, h, 10);
        w_bytes(&f, c->pix[k], (size_t)isize);
        for (int i = 0; own && i < n; i++) { uint8_t rgb[3] = { (uint8_t)(c->pal[k][i] >> 16), (uint8_t)(c->pal[k][i] >> 8), (uint8_t)c->pal[k][i] }; w_bytes(&f, rgb, 3); }
        if (sz & 1) w_bytes(&f, "", 1);
    }
    w_bytes(w, "FORM", 4);
    w32(w, (uint32_t)f.len);
    w_bytes(w, f.p, f.len);
    free(f.p);
}
static void w_mem(Wr *w, uint32_t a, size_t n) { for (size_t i = 0; i < n; i++) { uint8_t b = (uint8_t)rd8(a + (uint32_t)i); w_bytes(w, &b, 1); } }
static void w_string(Wr *w, uint32_t s) {
    uint32_t n = 0;
    while (rd8(s + n) && n < 65535) n++;
    w32(w, n + 1);
    w_mem(w, s, n);
    uint8_t z = 0;
    w_bytes(w, &z, 1);
}
static void w_image(Wr *w, uint32_t img) {
    uint8_t hdr[Image_SIZEOF];
    for (int i = 0; i < Image_SIZEOF; i++) hdr[i] = (uint8_t)rd8(img + (uint32_t)i);
    uint32_t data = rd32(img + Image_ImageData);
    hdr[10] = hdr[11] = hdr[12] = 0; hdr[13] = data ? 1 : 0;
    hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
    w_bytes(w, hdr, Image_SIZEOF);
    int n = image_bytes(hdr);
    if (data && n > 0) w_mem(w, data, (size_t)n);
}

static uint8_t *serialize(uint32_t dobj, size_t *len) {
    Wr w = { 0, 0, 0 };
    uint32_t g = dobj + GADGET_OFF;
    uint32_t img = rd32(g + Gadget_GadgetRender), sel = rd32(g + Gadget_SelectRender);
    uint32_t dd = rd32(dobj + DiskObject_do_DrawerData), tool = rd32(dobj + DiskObject_do_DefaultTool);
    uint32_t tt = rd32(dobj + DiskObject_do_ToolTypes), twin = rd32(dobj + DiskObject_do_ToolWindow);
    uint8_t hdr[DiskObject_SIZEOF];
    for (int i = 0; i < DiskObject_SIZEOF; i++) hdr[i] = (uint8_t)rd8(dobj + (uint32_t)i);
    hdr[0] = WB_DISKMAGIC >> 8; hdr[1] = WB_DISKMAGIC & 0xFF;
    hdr[2] = 0; hdr[3] = WB_DISKVERSION;
    memset(hdr + GADGET_OFF, 0, 4);                                   /* NextGadget */
    memset(hdr + GADGET_OFF + Gadget_GadgetText, 0, 4);
    memset(hdr + GADGET_OFF + Gadget_MutualExclude, 0, 8);           /* + SpecialInfo */
    if (dd) hdr[GADGET_OFF + Gadget_UserData + 3] = WB_DISKREVISION;
    w_bytes(&w, hdr, DiskObject_SIZEOF);
    if (dd) w_mem(&w, dd, OldDrawerData_SIZEOF);
    if (img) w_image(&w, img);
    if (img && sel) w_image(&w, sel);
    if (tool) w_string(&w, tool);
    if (tt) {
        uint32_t n = 0;
        while (rd32(tt + 4 * n) && n < 1000) n++;
        w32(&w, 4 * (n + 1));
        for (uint32_t i = 0; i < n; i++) w_string(&w, rd32(tt + 4 * i));
    }
    if (twin) w_string(&w, twin);
    if (dd) { w32(&w, rd32(dd + DrawerData_dd_Flags)); w16(&w, rd16(dd + DrawerData_dd_ViewModes)); }
    ColorIcon *ci = colour_of(dobj);
    if (ci && ci->dirty) w_colour(&w, ci);                          /* changed: written anew */
    else if (ci && ci->raw) w_bytes(&w, ci->raw, ci->rawlen);       /* the colour images, as they were */
    *len = w.len;
    return w.p;
}

/* ---- the library functions' native side ---------------------------------------------- */

static int info_path(const char *name, char *hp, int max) {
    char n[1024];
    size_t l = strlen(name);
    if (l && name[l - 1] == ':') snprintf(n, sizeof n, "%sDisk.info", name);   /* a volume */
    else snprintf(n, sizeof n, "%s.info", name);
    return dos_resolve(n, hp, max);
}

uint32_t icon_get(const char *name) {
    char hp[2048];
    if (!info_path(name, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, "rb");
    if (!f) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
    uint8_t *buf = malloc(1 << 20);
    size_t n = fread(buf, 1, 1 << 20, f);
    fclose(f);
    uint32_t d = parse_info(buf, n);
    free(buf);
    if (!d) dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE);
    return d;
}

int icon_put(const char *name, uint32_t dobj) {
    char hp[2048];
    if (!dobj || !info_path(name, hp, sizeof hp)) return 0;
    size_t len;
    uint8_t *buf = serialize(dobj, &len);
    FILE *f = fopen(hp, "wb");
    if (!f) { free(buf); dos_set_ioerr(214 /* ERROR_DISK_WRITE_PROTECTED */); return 0; }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return 1;
}

void icon_free(uint32_t dobj) { if (dobj) { colour_free(dobj); exec_freevec(dobj); } }

static uint32_t dup_icon(uint32_t dobj) {
    size_t len;
    uint8_t *buf = serialize(dobj, &len);
    uint32_t d = parse_info(buf, len);
    free(buf);
    return d;
}

/* a default icon from ENV:Sys/def_<name>.info, as icon.library does */
uint32_t icon_env_default(const char *name) {
    char n[64];
    snprintf(n, sizeof n, "ENV:Sys/def_%s", name);
    return icon_get(n);
}

/* default icons: ENV:Sys/def_*.info, else built-in .info images parsed like any other */
static uint32_t make_default(int type) {
    const char *nm = type == WBDISK || type == WBDEVICE || type == WBKICK ? "disk" : type == WBDRAWER ? "drawer" :
                     type == WBTOOL ? "tool" : type == WBGARBAGE ? "trashcan" : "project";
    uint32_t env = icon_env_default(nm);
    if (env) {
        wr32(env + DiskObject_do_CurrentX, NO_ICON_POSITION);
        wr32(env + DiskObject_do_CurrentY, NO_ICON_POSITION);
        return env;
    }
    const DefIcon *di = default_icons;
    while (di->name && strcmp(di->name, nm)) di++;
    if (!di->name) return 0;
    Wr w = { 0, 0, 0 };
    uint8_t hdr[DiskObject_SIZEOF];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = WB_DISKMAGIC >> 8; hdr[1] = WB_DISKMAGIC & 0xFF; hdr[3] = WB_DISKVERSION;
    uint8_t *g = hdr + GADGET_OFF;
    g[Gadget_Width] = (uint8_t)(di->width >> 8); g[Gadget_Width + 1] = (uint8_t)di->width;
    g[Gadget_Width + 3] = (uint8_t)(di->height + 1);
    g[Gadget_Flags + 1] = 0x04 | 0x00;                                 /* GADGIMAGE | GADGHCOMP */
    g[Gadget_Activation + 1] = 0x03;                                   /* RELVERIFY | IMMEDIATE */
    g[Gadget_GadgetType + 1] = 1;                                      /* BOOLGADGET */
    g[Gadget_GadgetRender + 3] = 1;
    hdr[DiskObject_do_Type] = (uint8_t)type;
    int drawer = type == WBDISK || type == WBDRAWER || type == WBGARBAGE || type == WBDEVICE;
    if (drawer) hdr[DiskObject_do_DrawerData + 3] = 1;
    uint32_t nopos = NO_ICON_POSITION;
    for (int i = 0; i < 4; i++) { hdr[DiskObject_do_CurrentX + i] = (uint8_t)(nopos >> (24 - 8 * i)); hdr[DiskObject_do_CurrentY + i] = (uint8_t)(nopos >> (24 - 8 * i)); }
    if (type == WBTOOL) hdr[DiskObject_do_StackSize + 2] = 0x10;      /* 4096 */
    if (drawer) g[Gadget_UserData + 3] = WB_DISKREVISION;
    w_bytes(&w, hdr, sizeof hdr);
    if (drawer) {
        uint8_t dd[OldDrawerData_SIZEOF];
        memset(dd, 0, sizeof dd);
        /* NewWindow: no box (the Workbench places the drawer itself), pens -1 */
        dd[8] = 0xFF; dd[9] = 0xFF;
        w_bytes(&w, dd, sizeof dd);
    }
    uint8_t ih[Image_SIZEOF];
    memset(ih, 0, sizeof ih);
    ih[5] = (uint8_t)di->width; ih[4] = (uint8_t)(di->width >> 8);
    ih[7] = (uint8_t)di->height;
    ih[9] = 2;
    ih[13] = 1;
    ih[14] = 3;
    w_bytes(&w, ih, sizeof ih);
    w_bytes(&w, di->planes, (size_t)(((di->width + 15) / 16) * 2 * di->height * 2));
    if (drawer) { w32(&w, 0); w16(&w, 0); }
    uint32_t d = parse_info(w.p, w.len);
    free(w.p);
    return d;
}

uint32_t icon_default(int type) { return make_default(type); }

int icon_type_of(const char *name) {
    size_t l = strlen(name);
    if (l && name[l - 1] == ':') return WBDISK;
    char hp[2048];
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    struct stat st;
    if (stat(hp, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return WBDRAWER;
    FILE *f = fopen(hp, "rb");
    uint8_t m[4] = { 0 };
    if (f) { if (fread(m, 1, 4, f) != 4) memset(m, 0, 4); fclose(f); }
    return be32(m) == 0x3F3 ? WBTOOL : WBPROJECT;
}

uint32_t icon_get_new(const char *name) {
    uint32_t d = icon_get(name);
    if (d) return d;
    int t = icon_type_of(name);
    return make_default(t ? t : WBPROJECT);
}

void icon_size(uint32_t dobj, int *w, int *h) {
    ColorIcon *ci = colour_of(dobj);
    NScreen *ws = workbench_screen(0);
    if (ci && (!ws || ws->depth >= 4)) { *w = ci->w; *h = ci->h; return; }
    uint32_t g = dobj + GADGET_OFF, img = rd32(g + Gadget_GadgetRender);
    *w = (int16_t)rd16(g + Gadget_Width);
    *h = (int16_t)rd16(g + Gadget_Width + 2);
    if (img) {
        int iw = (int16_t)rd16(img + Image_LeftEdge) + (int16_t)rd16(img + Image_Width);
        int ih = (int16_t)rd16(img + Image_LeftEdge + 2) + (int16_t)rd16(img + Image_Width + 2);
        if (iw > *w) *w = iw;
        if (ih > *h) *h = ih;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* a colour icon on a screen with enough pens: each colour gets one (exact if
   free, else the nearest); without a selected image, a darker copy */
static int draw_colour(uint32_t rp, ColorIcon *ci, int x, int y, int selected) {
    NScreen *s = workbench_screen(0);
    BM b;
    if (!s || s->depth < 4 || !bm_load(rd32(rp + RastPort_BitMap), &b)) return 0;
    int k = selected && ci->nimg > 1 ? 1 : 0, darken = selected && ci->nimg < 2;
    int m = k + darken;                                         /* map 2: the darker copy of image 0 */
    if (ci->map_screen != s->id) { ci->map_screen = s->id; memset(ci->map, 0xFF, sizeof ci->map); }
    int16_t *map = m < 2 ? ci->map[m] : 0;
    static int16_t dark[256];
    if (darken && !b.chunky) {                                  /* mapped each time: rare */
        for (int i = 0; i < ci->ncol[0]; i++) {
            uint32_t c = ci->pal[0][i];
            uint32_t r = (c >> 16 & 0xFF) * 3 / 4, g = (c >> 8 & 0xFF) * 3 / 4, bl = (c & 0xFF) * 3 / 4;
            dark[i] = (int16_t)screen_obtain_pen(s, r << 16 | g << 8 | bl);
        }
        map = dark;
    }
    const uint8_t *pix = ci->pix[k];
    if (b.chunky) {                                             /* true colour: the exact colours */
        for (int j = 0; j < ci->h; j++)
            for (int i = 0; i < ci->w; i++) {
                int v = pix[j * ci->w + i];
                if (v == ci->trans[k] || v >= ci->ncol[k]) continue;
                uint32_t c = ci->pal[k][v];
                if (darken) c = ((c >> 16 & 0xFF) * 3 / 4) << 16 | ((c >> 8 & 0xFF) * 3 / 4) << 8 | (c & 0xFF) * 3 / 4;
                bm_put_rgb(&b, x + i, y + j, c);
            }
        return 1;
    }
    for (int j = 0; j < ci->h; j++)
        for (int i = 0; i < ci->w; i++) {
            int v = pix[j * ci->w + i];
            if (v == ci->trans[k] || v >= ci->ncol[k]) continue;
            if (map[v] < 0) map[v] = (int16_t)screen_obtain_pen(s, ci->pal[k][v]);
            bm_pixel(&b, x + i, y + j, map[v], JAM1, 0xFF);
        }
    return 1;
}

/* image at x,y, label centred underneath */
void icon_draw(uint32_t rp, uint32_t dobj, const char *label, int x, int y, int selected, const uint16_t *pens) {
    uint32_t g = dobj + GADGET_OFF;
    uint32_t img = rd32(g + Gadget_GadgetRender), sel = rd32(g + Gadget_SelectRender);
    int hl = rd16(g + Gadget_Flags) & 3;
    int w, h;
    icon_size(dobj, &w, &h);
    ColorIcon *ci = colour_of(dobj);
    if (ci && draw_colour(rp, ci, x, y, selected)) ;
    else if (selected && sel && hl == 2) draw_image_ex(rp, sel, x, y, 1, 1);
    else if (img) {
        draw_image_ex(rp, img, x, y, 0, 1);                     /* pen 0: the background shows through */
        if (selected) {
            BM b;
            if (bm_load(rd32(rp + RastPort_BitMap), &b)) bm_fill(&b, x, y, x + w - 1, y + h - 1, 0, COMPLEMENT, 3);
        }
    }
    if (label && *label) {
        uint32_t tf = rd32(rp + RastPort_Font);
        int n = (int)strlen(label), tw = font_text_width(tf, label, n);
        uint32_t ofg = rd8(rp + RastPort_FgPen), om = rd8(rp + RastPort_DrawMode);
        wr8(rp + RastPort_FgPen, pens ? pens[TEXTPEN] : 1);
        wr8(rp + RastPort_DrawMode, JAM1);
        rp_move(rp, x + (w - tw) / 2, y + h + 1 + rd16((tf ? tf : gfx_default_font()) + TextFont_tf_Baseline));
        rp_text(rp, label, n);
        wr8(rp + RastPort_FgPen, ofg);
        wr8(rp + RastPort_DrawMode, om);
    }
    gfx_touch();
}

/* "NAME=value" or "NAME": the value (possibly empty) or 0 */
uint32_t icon_find_tooltype(uint32_t array, const char *name) {
    if (!array) return 0;
    size_t nl = strlen(name);
    for (uint32_t i = 0; rd32(array + 4 * i) && i < 1000; i++) {
        uint32_t s = rd32(array + 4 * i);
        size_t k = 0;
        while (k < nl && rd8(s + (uint32_t)k) && toupper(rd8(s + (uint32_t)k)) == toupper((unsigned char)name[k])) k++;
        if (k != nl) continue;
        uint32_t c = rd8(s + (uint32_t)k);
        if (c == '=') return s + (uint32_t)k + 1;
        if (c == 0) return s + (uint32_t)k;
    }
    return 0;
}

/* ---- library vectors ------------------------------------------------------------------ */

static uint32_t ic_GetDiskObject(M68K *c) { char n[1024]; str_get(c->a[0], n, sizeof n); return icon_get(n); }
static uint32_t ic_GetDiskObjectNew(M68K *c) { char n[1024]; str_get(c->a[0], n, sizeof n); return icon_get_new(n); }
static uint32_t ic_PutDiskObject(M68K *c) { char n[1024]; str_get(c->a[0], n, sizeof n); return icon_put(n, c->a[1]) ? DOSTRUE : 0; }
static uint32_t ic_FreeDiskObject(M68K *c) { icon_free(c->a[0]); return 0; }
static uint32_t ic_GetDefDiskObject(M68K *c) { return make_default((int)c->d[0]); }
static uint32_t ic_PutDefDiskObject(M68K *c) { (void)c; return DOSTRUE; }   /* defaults are built in */
static uint32_t ic_DeleteDiskObject(M68K *c) {
    char n[1024], hp[2048];
    str_get(c->a[0], n, sizeof n);
    if (!info_path(n, hp, sizeof hp)) return 0;
    return unlink(hp) == 0 ? DOSTRUE : 0;
}
static uint32_t ic_DupDiskObjectA(M68K *c) { return c->a[0] ? dup_icon(c->a[0]) : 0; }
static uint32_t ic_NewDiskObject(M68K *c) {
    uint32_t d = exec_allocvec(DiskObject_SIZEOF + 64 + 80, MEMF_PUBLIC | MEMF_CLEAR);
    wr16(d + DiskObject_do_Magic, WB_DISKMAGIC);
    wr16(d + DiskObject_do_Version, WB_DISKVERSION);
    wr8(d + DiskObject_do_Type, c->d[0]);
    wr32(d + DiskObject_do_CurrentX, NO_ICON_POSITION);
    wr32(d + DiskObject_do_CurrentY, NO_ICON_POSITION);
    if (c->d[0] == WBDISK || c->d[0] == WBDRAWER || c->d[0] == WBGARBAGE) wr32(d + DiskObject_do_DrawerData, d + 80);
    return d;
}
static uint32_t ic_FindToolType(M68K *c) { char n[256]; str_get(c->a[1], n, sizeof n); return icon_find_tooltype(c->a[0], n); }
static uint32_t ic_MatchToolValue(M68K *c) {
    char t[512], v[256];
    str_get(c->a[0], t, sizeof t);
    str_get(c->a[1], v, sizeof v);
    char *s = t;
    for (;;) {
        char *bar = strchr(s, '|');
        if (bar) *bar = 0;
        if (!strcasecmp(s, v)) return DOSTRUE;
        if (!bar) return 0;
        s = bar + 1;
    }
}
static uint32_t ic_BumpRevisionLength(M68K *c) {
    char old[256], nw[300];
    str_get(c->a[1], old, sizeof old);
    int n = 0, max = (int)(c->d[0] ? c->d[0] : 30);
    const char *rest = old;
    if (!strncasecmp(old, "copy of ", 8)) { n = 1; rest = old + 8; }
    else if (!strncasecmp(old, "copy ", 5) && isdigit((unsigned char)old[5])) {
        int k = atoi(old + 5);
        const char *of = strstr(old + 5, " of ");
        if (of) { n = k; rest = of + 4; }
    }
    if (n == 0) snprintf(nw, sizeof nw, "copy of %s", old);
    else snprintf(nw, sizeof nw, "copy %d of %s", n + 1, rest);
    nw[max] = 0;
    for (int i = 0; i <= (int)strlen(nw); i++) wr8(c->a[0] + (uint32_t)i, (uint8_t)nw[i]);
    return c->a[0];
}
static uint32_t ic_BumpRevision(M68K *c) { c->d[0] = 30; return ic_BumpRevisionLength(c); }

/* FreeLists: remember allocations, free them together */
typedef struct FL { uint32_t fl; uint32_t *mem, *len; int n, cap; struct FL *next; } FL;
static FL *freelists;
static FL *fl_of(uint32_t fl, int make) {
    for (FL *f = freelists; f; f = f->next) if (f->fl == fl) return f;
    if (!make) return 0;
    FL *f = calloc(1, sizeof *f);
    f->fl = fl;
    f->next = freelists;
    freelists = f;
    return f;
}
static uint32_t ic_AddFreeList(M68K *c) {
    FL *f = fl_of(c->a[0], 1);
    if (f->n == f->cap) { f->cap = f->cap * 2 + 8; f->mem = realloc(f->mem, sizeof(uint32_t) * (size_t)f->cap); f->len = realloc(f->len, sizeof(uint32_t) * (size_t)f->cap); }
    f->mem[f->n] = c->a[1];
    f->len[f->n] = c->a[2];
    f->n++;
    wr16(c->a[0], (uint32_t)f->n);
    return DOSTRUE;
}
static uint32_t ic_FreeFreeList(M68K *c) {
    FL **pp = &freelists;
    while (*pp && (*pp)->fl != c->a[0]) pp = &(*pp)->next;
    if (!*pp) return 0;
    FL *f = *pp;
    *pp = f->next;
    for (int i = 0; i < f->n; i++) exec_freemem(f->mem[i], f->len[i]);
    free(f->mem); free(f->len); free(f);
    return 0;
}
static uint32_t ic_FreeAlloc(M68K *c) {
    uint32_t m = exec_allocmem(c->a[1], c->a[2] ? c->a[2] : MEMF_PUBLIC);
    if (!m) return 0;
    uint32_t a1 = c->a[1], a2 = c->a[2];
    c->a[1] = m; c->a[2] = a1;
    ic_AddFreeList(c);
    c->a[1] = a1; c->a[2] = a2;
    return m;
}
static uint32_t ic_FreeFree(M68K *c) {
    FL *f = fl_of(c->a[0], 0);
    if (!f) return 0;
    for (int i = 0; i < f->n; i++)
        if (f->mem[i] == c->a[1]) { exec_freemem(f->mem[i], f->len[i]); f->mem[i] = f->mem[--f->n]; f->len[i] = f->len[f->n]; break; }
    return 0;
}

#define ICONA(n) (0x80000000u + 0x9000 + (n))
static uint32_t ic_GetIconTagList(M68K *c) {
    char n[1024];
    uint32_t tags = c->a[1];
    int deftype = (int)tag_get(tags, ICONA(45) /* ICONGETA_GetDefaultType */, 0);
    if (!c->a[0]) return deftype ? make_default(deftype) : 0;
    str_get(c->a[0], n, sizeof n);
    if (deftype) return make_default(deftype);
    uint32_t d = icon_get(n);
    if (d || tag_get(tags, ICONA(47) /* ICONGETA_FailIfUnavailable */, 1)) return d;
    return icon_get_new(n);
}
static uint32_t ic_PutIconTagList(M68K *c) { char n[1024]; str_get(c->a[0], n, sizeof n); return icon_put(n, c->a[1]) ? DOSTRUE : 0; }
/* IconControlA(icon, tags): the colour images and the library's options, as
   AmigaOS 3.5 gives them; Get tags write through their pointer. Returns the
   number of tags handled. */
#define ICC(n) ICONA(n)
static uint32_t g_screen;
static int g_frameless, g_newicons = 1, g_colour = 1, g_precision = 16, g_maxname = 25;
static ColorIcon *colour_make(uint32_t dobj) {
    ColorIcon *c = colour_of(dobj);
    if (c) return c;
    c = calloc(1, sizeof *c);
    c->dobj = dobj;
    c->trans[0] = c->trans[1] = -1;
    c->map_screen = -1;
    c->next = colour_icons;
    colour_icons = c;
    return c;
}
static uint32_t to68(uint32_t *slot, const void *src, size_t n) {
    if (*slot) exec_freevec(*slot);
    *slot = exec_allocvec((uint32_t)n + 1, MEMF_PUBLIC | MEMF_CLEAR);
    for (size_t i = 0; *slot && i < n; i++) wr8(*slot + (uint32_t)i, ((const uint8_t *)src)[i]);
    return *slot;
}
static uint32_t ic_IconControlA(M68K *c) {
    uint32_t dobj = c->a[0], st = c->a[1], ti, done = 0;
    while ((ti = tag_next(&st))) {
        uint32_t t = rd32(ti), d = rd32(ti + 4);
        if (t < ICONA(0) || t > ICONA(100)) continue;
        int n = (int)(t - ICONA(0)), k = 0;
        ColorIcon *ci = dobj ? colour_of(dobj) : 0;
        done++;
        switch (n) {
        /* the library's options */
        case 2: g_screen = d; break;
        case 3: if (d) wr32(d, g_screen ? g_screen : (workbench_screen(0) ? workbench_screen(0)->scr : 0)); break;
        case 4: g_precision = (int)d; break;
        case 5: if (d) wr32(d, (uint32_t)g_precision); break;
        case 6: case 7: break;                                      /* emboss rectangle: kept as is */
        case 8: g_frameless = d != 0; break;
        case 9: if (d) wr32(d, (uint32_t)g_frameless); break;
        case 10: g_newicons = d != 0; break;
        case 11: if (d) wr32(d, (uint32_t)g_newicons); break;
        case 77: g_colour = d != 0; break;
        case 78: if (d) wr32(d, (uint32_t)g_colour); break;
        case 12: break;
        case 13: if (d) wr32(d, 0); break;
        case 67: g_maxname = (int)d; break;
        case 68: if (d) wr32(d, (uint32_t)g_maxname); break;
        /* one icon's images */
        case 15: k = 1; /* fall through */
        case 14:                                                    /* GetImageMask: 1 where opaque */
            if (!d) break;
            if (!ci || !ci->pix[k]) { wr32(d, 0); break; }
            {
                int bpr = (ci->w + 15) / 16 * 2;
                uint8_t *m = calloc((size_t)(bpr * ci->h), 1);
                for (int y = 0; y < ci->h; y++)
                    for (int x = 0; x < ci->w; x++)
                        if (ci->pix[k][y * ci->w + x] != ci->trans[k]) m[y * bpr + x / 8] |= (uint8_t)(0x80 >> (x & 7));
                wr32(d, to68(&ci->mask68[k], m, (size_t)(bpr * ci->h)));
                free(m);
            }
            break;
        case 18: k = 1; /* fall through */
        case 16: if (dobj) { ci = colour_make(dobj); ci->trans[k] = (int32_t)d < 0 ? -1 : (int)(d & 0xFF); ci->dirty = 1; } break;
        case 19: k = 1; /* fall through */
        case 17: if (d) wr32(d, ci ? (uint32_t)ci->trans[k] : 0xFFFFFFFFu); break;
        case 22: k = 1; /* fall through */
        case 20:                                                    /* SetPalette: struct ColorRegister[] */
            if (dobj && d) {
                ci = colour_make(dobj);
                int cnt = ci->ncol[k] ? ci->ncol[k] : 256;
                for (int i = 0; i < cnt; i++) ci->pal[k][i] = rd8(d + 3 * (uint32_t)i) << 16 | rd8(d + 3 * (uint32_t)i + 1) << 8 | rd8(d + 3 * (uint32_t)i + 2);
                ci->dirty = 1;
                ci->map_screen = -1;
            }
            break;
        case 23: k = 1; /* fall through */
        case 21:
            if (!d) break;
            if (!ci || !ci->ncol[k]) { wr32(d, 0); break; }
            {
                uint8_t rgb[768];
                for (int i = 0; i < ci->ncol[k]; i++) { rgb[3 * i] = (uint8_t)(ci->pal[k][i] >> 16); rgb[3 * i + 1] = (uint8_t)(ci->pal[k][i] >> 8); rgb[3 * i + 2] = (uint8_t)ci->pal[k][i]; }
                wr32(d, to68(&ci->pal68[k], rgb, (size_t)ci->ncol[k] * 3));
            }
            break;
        case 26: k = 1; /* fall through */
        case 24: if (dobj) { ci = colour_make(dobj); ci->ncol[k] = (int)(d > 256 ? 256 : d); ci->dirty = 1; } break;
        case 27: k = 1; /* fall through */
        case 25: if (d) wr32(d, ci ? (uint32_t)ci->ncol[k] : 0); break;
        case 30: k = 1; /* fall through */
        case 28:                                                    /* SetImageData: a byte a pixel, Width x Height */
            if (dobj) {
                ci = colour_make(dobj);
                if (!ci->w || !ci->h) break;
                free(ci->pix[k]);
                ci->pix[k] = 0;
                if (d) {
                    ci->pix[k] = malloc((size_t)(ci->w * ci->h));
                    for (int i = 0; i < ci->w * ci->h; i++) ci->pix[k][i] = (uint8_t)rd8(d + (uint32_t)i);
                }
                ci->nimg = ci->pix[1] ? 2 : ci->pix[0] ? 1 : 0;
                ci->dirty = 1;
                ci->map_screen = -1;
            }
            break;
        case 31: k = 1; /* fall through */
        case 29:
            if (!d) break;
            wr32(d, ci && ci->pix[k] ? to68(&ci->data68[k], ci->pix[k], (size_t)(ci->w * ci->h)) : 0);
            break;
        case 32: if (dobj) { ci = colour_make(dobj); ci->frameless = d != 0; ci->dirty = 1; } break;
        case 33: if (d) wr32(d, ci ? (uint32_t)ci->frameless : 0); break;
        case 34: case 35: if (n == 35 && d) wr32(d, (uint32_t)g_newicons); break;
        case 36: if (dobj) { ci = colour_make(dobj); ci->aspect = (int)(d ? rd8(d) : 0); ci->dirty = 1; } break;
        case 37: if (d) wr8(d, ci ? (uint32_t)ci->aspect : 0); break;
        case 38: if (dobj) { ci = colour_make(dobj); ci->w = (int)d; ci->dirty = 1; } break;
        case 39: if (d) wr32(d, ci ? (uint32_t)ci->w : 0); break;
        case 40: if (dobj) { ci = colour_make(dobj); ci->h = (int)d; ci->dirty = 1; } break;
        case 41: if (d) wr32(d, ci ? (uint32_t)ci->h : 0); break;
        case 42: if (d) wr32(d, ci && ci->pix[0] ? DOSTRUE : 0); break;
        case 43: if (d) wr32(d, ci && workbench_screen(0) ? workbench_screen(0)->scr : 0); break;
        case 44: if (d) wr32(d, ci && ci->pix[1] ? DOSTRUE : 0); break;
        case 79: if (d) wr32(d, ci && ci->newicon ? DOSTRUE : 0); break;
        case 80: if (d) wr32(d, DOSTRUE); break;
        default: done--; break;
        }
    }
    return done;
}
static uint32_t ic_DrawIconStateA(M68K *c) {
    char label[256] = "";
    if (c->a[2]) str_get(c->a[2], label, sizeof label);
    NScreen *s = workbench_screen(0);
    icon_draw(c->a[0], c->a[1], label, (int16_t)c->d[0], (int16_t)c->d[1], c->d[2] == 1, s ? s->pens : 0);
    return 0;
}
static uint32_t ic_GetIconRectangleA(M68K *c) {
    int w, h;
    icon_size(c->a[1], &w, &h);
    int x0 = 0, x1 = w - 1, y1 = h - 1;
    if (c->a[2]) {
        char label[256];
        uint32_t tf = c->a[0] ? rd32(c->a[0] + RastPort_Font) : 0;
        if (!tf) tf = gfx_default_font();
        int n = str_get(c->a[2], label, sizeof label), tw = font_text_width(tf, label, n);
        if (tw > w) { x0 = (w - tw) / 2; x1 = x0 + tw - 1; }
        y1 = h + 1 + rd16(tf + TextFont_tf_YSize);
    }
    wr16(c->a[3], (uint32_t)x0); wr16(c->a[3] + 2, 0); wr16(c->a[3] + 4, (uint32_t)x1); wr16(c->a[3] + 6, (uint32_t)y1);
    return DOSTRUE;
}
static uint32_t ic_LayoutIconA(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t ic_ChangeToSelectedIconColor(M68K *c) { (void)c; return 0; }

static const NativeEntry icon_natives[] = {
    { "FreeFreeList", ic_FreeFreeList }, { "AddFreeList", ic_AddFreeList },
    { "GetDiskObject", ic_GetDiskObject }, { "PutDiskObject", ic_PutDiskObject },
    { "FreeDiskObject", ic_FreeDiskObject }, { "FindToolType", ic_FindToolType },
    { "MatchToolValue", ic_MatchToolValue }, { "BumpRevision", ic_BumpRevision },
    { "FreeAlloc", ic_FreeAlloc }, { "GetDefDiskObject", ic_GetDefDiskObject },
    { "PutDefDiskObject", ic_PutDefDiskObject }, { "GetDiskObjectNew", ic_GetDiskObjectNew },
    { "DeleteDiskObject", ic_DeleteDiskObject }, { "FreeFree", ic_FreeFree },
    { "DupDiskObjectA", ic_DupDiskObjectA }, { "IconControlA", ic_IconControlA },
    { "DrawIconStateA", ic_DrawIconStateA }, { "GetIconRectangleA", ic_GetIconRectangleA },
    { "NewDiskObject", ic_NewDiskObject }, { "GetIconTagList", ic_GetIconTagList },
    { "PutIconTagList", ic_PutIconTagList }, { "LayoutIconA", ic_LayoutIconA },
    { "ChangeToSelectedIconColor", ic_ChangeToSelectedIconColor }, { "BumpRevisionLength", ic_BumpRevisionLength },
    { 0, 0 }
};

void icon_init(void) {
    IconBase = lib_create("icon.library", "icon", 64, icon_natives, 44, 1, "icon 44.1 (AmigaMac)");
    lib_add(IconBase);
}
