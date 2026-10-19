/* The font preferences (ENV:Sys/font.prefs), as IPrefs applies them: the
 * Workbench's icon font, the system's default font (fixed width: Shells and
 * text), the screens' font (title bars, menus, requesters).
 *
 * The file is an IFF FORM PREF: a PRHD, then one FONT chunk a font, a
 * struct FontPrefs (prefs/font.h): fp_Type at 14 (FP_WBFONT, FP_SYSFONT,
 * FP_SCREENFONT), fp_TextAttr at 20 (ta_YSize at 24), fp_Name at 28.
 * It is read at boot, and again when a program writes it (Font "Use" or
 * "Save"): the Workbench screen then opens again with the new fonts. */
#include "os.h"
#include "gfx.h"
#include "ndk_offsets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern uint32_t diskfont_open(const char *name, int ysize);

typedef struct { char name[128]; int size; uint32_t tf; } FontPref;
static FontPref prefs[3];                       /* FP_WBFONT, FP_SYSFONT, FP_SCREENFONT */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int read_prefs(const char *apath, FontPref out[3]) {
    char hp[2048];
    if (!dos_resolve(apath, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, "rb");
    if (!f) return 0;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 12 || memcmp(buf, "FORM", 4) || memcmp(buf + 8, "PREF", 4)) return 0;
    int got = 0;
    for (size_t at = 12; at + 8 <= n;) {
        size_t sz = be32(buf + at + 4);
        const uint8_t *d = buf + at + 8;
        if (at + 8 + sz > n) break;
        if (!memcmp(buf + at, "FONT", 4) && sz >= 156) {
            int type = d[14] << 8 | d[15], size = d[24] << 8 | d[25];
            if (type >= 0 && type <= 2 && d[28] && size > 0) {
                snprintf(out[type].name, sizeof out[type].name, "%.*s", 127, (const char *)d + 28);
                out[type].size = size;
                got++;
            }
        }
        at += 8 + ((sz + 1) & ~(size_t)1);
    }
    return got;
}

static uint32_t open_or_topaz(FontPref *p, int fixed_only) {
    uint32_t tf = diskfont_open(p->name, p->size);
    if (tf && fixed_only && (rd8(tf + TextFont_tf_Flags) & 0x20)) {   /* FPF_PROPORTIONAL: not for the system text */
        os_log("Polices : %s %d est proportionnelle, le texte système reste en topaz", p->name, p->size);
        tf = 0;
    }
    if (!tf) {
        if (strcasecmp(p->name, "topaz.font")) os_log("Polices : %s %d introuvable, topaz 8 à la place", p->name, p->size);
        snprintf(p->name, sizeof p->name, "topaz.font");
        p->size = 8;
        tf = gfx_topaz8();
    }
    return tf;
}

/* reads the preferences and opens their fonts; the system font becomes GfxBase's default */
void fontprefs_load(void) {
    FontPref p[3];
    for (int i = 0; i < 3; i++) { snprintf(p[i].name, sizeof p[i].name, "topaz.font"); p[i].size = 8; p[i].tf = 0; }
    if (!read_prefs("ENV:Sys/font.prefs", p)) read_prefs("ENVARC:Sys/font.prefs", p);
    for (int i = 0; i < 3; i++) p[i].tf = open_or_topaz(&p[i], i == 1);
    memcpy(prefs, p, sizeof prefs);
    wr32(GfxBase + GfxBase_DefaultFont, prefs[1].tf);
}

/* the font a TextAttr names: in memory or from FONTS:, the nearest size */
uint32_t font_from_attr(uint32_t ta) {
    if (!ta || !rd32(ta + TextAttr_ta_Name)) return 0;
    char name[128];
    str_get(rd32(ta + TextAttr_ta_Name), name, sizeof name);
    return diskfont_open(name, rd16(ta + TextAttr_ta_YSize));
}

uint32_t font_workbench(void) { return prefs[0].tf ? prefs[0].tf : gfx_topaz8(); }
uint32_t font_system(void) { return prefs[1].tf ? prefs[1].tf : gfx_topaz8(); }
uint32_t font_screen(void) { return prefs[2].tf ? prefs[2].tf : gfx_topaz8(); }

/* ENV:Sys/font.prefs was written: new fonts, the Workbench screen anew */
void fontprefs_changed(void) {
    fontprefs_load();
    extern void intuition_screenmode_changed(void);
    intuition_screenmode_changed();
}
