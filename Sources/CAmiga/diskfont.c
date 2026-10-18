/* diskfont.library, written from the diskfont autodocs and diskfont/diskfont.h.
 *
 * Bitmap fonts are the real Amiga files: FONTS:name.font lists the sizes
 * (FontContentsHeader), FONTS:name/size is a loadable hunk file whose data
 * starts with MOVEQ #0,D0 / RTS followed by a DiskFontHeader holding the
 * TextFont. LoadSeg relocates it; the font is then added to the system list
 * so graphics OpenFont() finds it too. Outline (scalable) fonts are not
 * handled: their sizes are offered only when a bitmap exists. */
#include "os.h"
#include "gfx.h"
#include "ntk.h"
#include "ndk_offsets.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

uint32_t DiskfontBase;

#define FCH_ID 0x0f00
#define TFCH_ID 0x0f02
#define OFCH_ID 0x0f03
#define AFF_MEMORY 1
#define AFF_DISK 2
#define AFF_TAGGED 0x10000
#define FPF_DISKFONT 0x02

/* "FONTS:helvetica.font" or "helvetica.font" -> "helvetica.font" */
static const char *font_file(const char *name) {
    const char *p = strrchr(name, ':'), *q = strrchr(name, '/');
    if (q && (!p || q > p)) return q + 1;
    return p ? p + 1 : name;
}

static int name_eq(uint32_t tf, const char *name) {
    char n[64];
    if (!rd32(tf + LN_NAME)) return 0;
    str_get(rd32(tf + LN_NAME), n, sizeof n);
    return !strcasecmp(font_file(n), font_file(name));
}

/* a font already in memory: exact size, or the nearest when not exact */
uint32_t font_in_memory(const char *name, int ysize, int exact) {
    uint32_t best = 0;
    int bestd = 1 << 30;
    for (uint32_t n = rd32(GfxBase + GfxBase_TextFonts); n && rd32(n); n = rd32(n)) {
        if (!name_eq(n, name)) continue;
        int d = abs((int)rd16(n + TextFont_tf_YSize) - ysize);
        if (d < bestd) { bestd = d; best = n; }
    }
    if (exact && bestd) return 0;
    return best;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

typedef struct { char file[256]; int ysize, style, flags; } FC;

/* the sizes listed in a .font file, or found in its directory */
static int read_contents(const char *fontfile, FC *out, int max) {
    char hp[2048], path[600];
    snprintf(path, sizeof path, strchr(fontfile, ':') ? "%s" : "FONTS:%s", fontfile);
    int n = 0;
    if (dos_resolve(path, hp, sizeof hp)) {
        FILE *f = fopen(hp, "rb");
        if (f) {
            uint8_t h[4];
            if (fread(h, 1, 4, f) == 4 && (be16(h) == FCH_ID || be16(h) == TFCH_ID || be16(h) == OFCH_ID)) {
                int cnt = (int)be16(h + 2);
                uint8_t e[FontContents_SIZEOF];
                for (int i = 0; i < cnt && n < max && fread(e, 1, sizeof e, f) == sizeof e; i++) {
                    snprintf(out[n].file, sizeof out[n].file, "%.*s", 255, (char *)e);
                    out[n].ysize = (int)be16(e + FontContents_fc_YSize);
                    out[n].style = e[FontContents_fc_Style];
                    out[n].flags = e[FontContents_fc_Flags];
                    if (out[n].file[0]) n++;
                }
            }
            fclose(f);
        }
    }
    if (n) return n;
    /* no contents file: the numbered files of FONTS:name/ */
    char base[256];
    snprintf(base, sizeof base, "%s", font_file(fontfile));
    char *dot = strrchr(base, '.');
    if (dot && !strcasecmp(dot, ".font")) *dot = 0;
    snprintf(path, sizeof path, "FONTS:%s", base);
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    DIR *d = opendir(hp);
    struct dirent *de;
    while (d && (de = readdir(d)) && n < max) {
        int sz = atoi(de->d_name);
        if (sz <= 0) continue;
        snprintf(out[n].file, sizeof out[n].file, "%s/%s", base, de->d_name);
        out[n].ysize = sz;
        out[n].style = out[n].flags = 0;
        n++;
    }
    if (d) closedir(d);
    return n;
}

static uint32_t load_font(const char *name, int ysize) {
    FC fc[64];
    int n = read_contents(font_file(name), fc, 64);
    if (!n) return 0;
    int best = 0;
    for (int i = 1; i < n; i++) if (abs(fc[i].ysize - ysize) < abs(fc[best].ysize - ysize)) best = i;
    char path[600], hp[2048];
    snprintf(path, sizeof path, "FONTS:%s", fc[best].file);
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    uint32_t seg = dos_loadseg(hp);
    if (!seg) return 0;
    uint32_t dfh = BADDR(seg) + 8;
    if (rd16(dfh + DiskFontHeader_dfh_FileID) != 0x0f80) {
        os_log("diskfont : %s n'est pas une police", path);
        dos_unloadseg(seg);
        return 0;
    }
    uint32_t tf = dfh + DiskFontHeader_dfh_TF;
    wr32(dfh + DiskFontHeader_dfh_Segment, seg);
    /* the font's name is the .font name, as programs ask for it */
    char nm[64];
    snprintf(nm, sizeof nm, "%s", font_file(name));
    wr32(tf + LN_NAME, str_alloc(nm));
    wr8(tf + LN_TYPE, NT_FONT);
    wr8(tf + TextFont_tf_Flags, rd8(tf + TextFont_tf_Flags) | FPF_DISKFONT);
    wr16(tf + TextFont_tf_Accessors, 0);
    list_enqueue(GfxBase + GfxBase_TextFonts, tf);
    return tf;
}

static uint32_t df_OpenDiskFont(M68K *c) {
    uint32_t ta = c->a[0];
    if (!ta || !rd32(ta + TextAttr_ta_Name)) return 0;
    char name[256];
    str_get(rd32(ta + TextAttr_ta_Name), name, sizeof name);
    int ysize = rd16(ta + TextAttr_ta_YSize);
    uint32_t tf = font_in_memory(name, ysize, 1);
    if (!tf) tf = load_font(name, ysize);
    if (!tf) tf = font_in_memory(name, ysize, 0);
    if (tf) wr16(tf + TextFont_tf_Accessors, rd16(tf + TextFont_tf_Accessors) + 1);
    return tf;
}

/* AvailFonts(buffer, bufBytes, flags): 0, or how many more bytes are needed */
static uint32_t df_AvailFonts(M68K *c) {
    uint32_t buf = c->a[0], size = c->d[0], flags = c->d[1];
    int tagged = (flags & AFF_TAGGED) != 0;
    int esz = tagged ? TAvailFonts_SIZEOF : AvailFonts_SIZEOF;
    struct { int type, ysize, style, flags; char name[64]; } e[256];
    int n = 0;
    if (flags & AFF_MEMORY)
        for (uint32_t f = rd32(GfxBase + GfxBase_TextFonts); f && rd32(f) && n < 256; f = rd32(f)) {
            e[n].type = AFF_MEMORY;
            e[n].ysize = rd16(f + TextFont_tf_YSize);
            e[n].style = rd8(f + TextFont_tf_Style);
            e[n].flags = rd8(f + TextFont_tf_Flags);
            str_get(rd32(f + LN_NAME), e[n].name, sizeof e[n].name);
            n++;
        }
    if (flags & AFF_DISK) {
        char hp[2048];
        DIR *d = dos_resolve("FONTS:", hp, sizeof hp) ? opendir(hp) : 0;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 6 || strcasecmp(de->d_name + l - 5, ".font")) continue;
            FC fc[64];
            int k = read_contents(de->d_name, fc, 64);
            for (int i = 0; i < k && n < 256; i++) {
                e[n].type = AFF_DISK;
                e[n].ysize = fc[i].ysize;
                e[n].style = fc[i].style;
                e[n].flags = fc[i].flags;
                snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
                n++;
            }
        }
        if (d) closedir(d);
    }
    uint32_t need = 2 + (uint32_t)(n * esz);
    for (int i = 0; i < n; i++) need += (uint32_t)strlen(e[i].name) + 1;
    need = (need + 1) & ~1u;
    if (need > size) return need - size;
    wr16(buf, (uint32_t)n);
    uint32_t names = buf + 2 + (uint32_t)(n * esz);
    for (int i = 0; i < n; i++) {
        uint32_t a = buf + 2 + (uint32_t)(i * esz);
        wr16(a, (uint32_t)e[i].type);
        put_cstr(names, e[i].name, 64);
        wr32(a + 2, names);
        wr16(a + 6, (uint32_t)e[i].ysize);
        wr8(a + 8, (uint32_t)e[i].style);
        wr8(a + 9, (uint32_t)e[i].flags);
        if (tagged) wr32(a + 10, 0);
        names += (uint32_t)strlen(e[i].name) + 1;
    }
    return 0;
}

/* NewFontContents(fontsLock, fontName) */
static uint32_t df_NewFontContents(M68K *c) {
    char name[256];
    str_get(c->a[1], name, sizeof name);
    FC fc[64];
    int n = read_contents(name, fc, 64);
    uint32_t h = exec_allocvec(4 + (uint32_t)n * FontContents_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr16(h, FCH_ID);
    wr16(h + 2, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        uint32_t e = h + 4 + (uint32_t)i * FontContents_SIZEOF;
        put_cstr(e, fc[i].file, 256);
        wr16(e + FontContents_fc_YSize, (uint32_t)fc[i].ysize);
        wr8(e + FontContents_fc_Style, (uint32_t)fc[i].style);
        wr8(e + FontContents_fc_Flags, (uint32_t)fc[i].flags);
    }
    return h;
}
static uint32_t df_DisposeFontContents(M68K *c) { if (c->a[1]) exec_freevec(c->a[1]); return 0; }
static uint32_t df_Zero(M68K *c) { (void)c; return 0; }

static const NativeEntry diskfont_natives[] = {
    { "OpenDiskFont", df_OpenDiskFont }, { "AvailFonts", df_AvailFonts },
    { "NewFontContents", df_NewFontContents }, { "DisposeFontContents", df_DisposeFontContents },
    { "NewScaledDiskFont", df_Zero }, { "GetDiskFontCtrl", df_Zero }, { "SetDiskFontCtrlA", df_Zero },
    { "EOpenEngine", df_Zero }, { "ECloseEngine", df_Zero }, { "ESetInfoA", df_Zero }, { "EObtainInfoA", df_Zero },
    { "EReleaseInfoA", df_Zero }, { "OpenOutlineFont", df_Zero }, { "CloseOutlineFont", df_Zero },
    { "WriteFontContents", df_Zero }, { "WriteDiskFontHeaderA", df_Zero }, { "ObtainCharsetInfo", df_Zero },
    { 0, 0 }
};

void diskfont_init(void) {
    DiskfontBase = lib_create("diskfont.library", "diskfont", 64, diskfont_natives, 45, 1, "diskfont 45.1 (AmigaMac)");
    lib_add(DiskfontBase);
}
