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
 * Anything after that (newer colour icon formats) is ignored.
 * A loaded DiskObject is one AllocVec block, so FreeDiskObject is FreeVec. */
#include "wb.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include "icons_default.h"
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
    }
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

void icon_free(uint32_t dobj) { if (dobj) exec_freevec(dobj); }

static uint32_t dup_icon(uint32_t dobj) {
    size_t len;
    uint8_t *buf = serialize(dobj, &len);
    uint32_t d = parse_info(buf, len);
    free(buf);
    return d;
}

/* default icons are built as .info images and parsed like any other */
static uint32_t make_default(int type) {
    const char *nm = type == WBDISK || type == WBDEVICE || type == WBKICK ? "disk" : type == WBDRAWER ? "drawer" :
                     type == WBTOOL ? "tool" : type == WBGARBAGE ? "trashcan" : "project";
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
        /* NewWindow: 50,30 400x100, pens -1 */
        dd[0] = 0; dd[1] = 50; dd[2] = 0; dd[3] = 30; dd[4] = 400 >> 8; dd[5] = 400 & 0xFF; dd[7] = 100;
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

/* image at x,y, label centred underneath */
void icon_draw(uint32_t rp, uint32_t dobj, const char *label, int x, int y, int selected, const uint16_t *pens) {
    uint32_t g = dobj + GADGET_OFF;
    uint32_t img = rd32(g + Gadget_GadgetRender), sel = rd32(g + Gadget_SelectRender);
    int hl = rd16(g + Gadget_Flags) & 3;
    int w, h;
    icon_size(dobj, &w, &h);
    if (selected && sel && hl == 2) draw_image(rp, sel, x, y, 1);
    else if (img) {
        draw_image(rp, img, x, y, 0);
        if (selected) {
            BM b;
            if (bm_load(rd32(rp + RastPort_BitMap), &b)) bm_fill(&b, x, y, x + w - 1, y + h - 1, 0, COMPLEMENT, 3);
        }
    }
    if (label && *label) {
        int n = (int)strlen(label), tw = n * 8;
        uint32_t ofg = rd8(rp + RastPort_FgPen), om = rd8(rp + RastPort_DrawMode);
        wr8(rp + RastPort_FgPen, pens ? pens[TEXTPEN] : 1);
        wr8(rp + RastPort_DrawMode, JAM1);
        rp_move(rp, x + (w - tw) / 2, y + h + 1 + 6);
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
static uint32_t ic_IconControlA(M68K *c) { (void)c; return 0; }
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
        int tw = str_get(c->a[2], label, sizeof label) * 8;
        if (tw > w) { x0 = (w - tw) / 2; x1 = x0 + tw - 1; }
        y1 = h + 1 + 8;
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
