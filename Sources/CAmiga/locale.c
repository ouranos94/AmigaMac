/* locale.library, written from the locale.library autodocs.
 *
 * The default Locale follows the Mac's preferred language (English and
 * French texts are built in, other languages get English texts with their
 * own catalog directory). Catalogs are the real IFF CTLG files, looked for
 * in PROGDIR:Catalogs/<language>/ then LOCALE:Catalogs/<language>/.
 * FormatString/FormatDate send each character to the caller's hook. */
#include "os.h"
#include "gfx.h"
#include "ndk_offsets.h"
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t LocaleBase;
static uint32_t def_locale;
static char language[32] = "english";                 /* Latin-1 directory name */
static int french;

/* ---- texts --------------------------------------------------------------------- */

static const char *en_str[71] = {
    0, "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December",
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    "Yes", "No", "AM", "PM", "-", "-", "\"", "\"", "Yesterday", "Today", "Tomorrow", "Future", 0,
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December",
};
static const char *fr_str[71] = {
    0, "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi",
    "dim", "lun", "mar", "mer", "jeu", "ven", "sam",
    "janvier", "f\xe9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xfbt", "septembre", "octobre", "novembre", "d\xe9" "cembre",
    "jan", "f\xe9v", "mar", "avr", "mai", "juin", "juil", "ao\xfbt", "sep", "oct", "nov", "d\xe9" "c",
    "Oui", "Non", "AM", "PM", "-", "-", "\xab", "\xbb", "Hier", "Aujourd'hui", "Demain", "Futur", 0,
    "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi",
    "janvier", "f\xe9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xfbt", "septembre", "octobre", "novembre", "d\xe9" "cembre",
};
static uint32_t strs[71];                              /* the texts in 68k memory */

static const char *str_of(int id) {
    if (id < 1 || id > 70) return "";
    const char *s = french ? fr_str[id] : en_str[id];
    return s ? s : "";
}

/* ---- the default locale ------------------------------------------------------------ */

static void pick_language(void) {
    static const struct { const char *code, *name; } map[] = {
        { "fr", "fran\xe7" "ais" }, { "de", "deutsch" }, { "it", "italiano" }, { "es", "espa\xf1ol" },
        { "nl", "nederlands" }, { "da", "dansk" }, { "nb", "norsk" }, { "no", "norsk" }, { "sv", "svenska" },
        { "fi", "suomi" }, { "pt", "portugu\xeas" }, { "pl", "polski" }, { "hu", "magyar" }, { "el", "greek" },
        { "ru", "russian" }, { "cs", "czech" }, { "en", "english" },
    };
    char code[16] = "en";
    const char *env = getenv("AMIGAMAC_LANG");
    if (env) snprintf(code, sizeof code, "%s", env);
    else {
        CFArrayRef langs = CFLocaleCopyPreferredLanguages();
        if (langs && CFArrayGetCount(langs) > 0) {
            CFStringRef l = CFArrayGetValueAtIndex(langs, 0);
            CFStringGetCString(l, code, sizeof code, kCFStringEncodingUTF8);
        }
        if (langs) CFRelease(langs);
    }
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (!strncasecmp(code, map[i].code, strlen(map[i].code))) { snprintf(language, sizeof language, "%s", map[i].name); break; }
    french = !strncasecmp(code, "fr", 2);
}

static uint32_t make_locale(void) {
    uint32_t L = exec_allocmem(Locale_SIZEOF + 8, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(L + Locale_loc_LocaleName, str_alloc(french ? "fran\xe7" "ais.prefs" : "english.prefs"));
    wr32(L + Locale_loc_LanguageName, str_alloc(french ? "fran\xe7" "ais.language" : "english.language"));
    wr32(L + Locale_loc_PrefLanguages, str_alloc(language));
    if (strcmp(language, "english")) wr32(L + Locale_loc_PrefLanguages + 4, str_alloc("english"));
    wr32(L + Locale_loc_CountryCode, french ? 0x46524100u /* "FRA" */ : 0x55534100u);
    wr32(L + Locale_loc_TelephoneCode, french ? 33 : 1);
    time_t t = time(0);
    struct tm lt;
    localtime_r(&t, &lt);
    wr32(L + Locale_loc_GMTOffset, (uint32_t)(int32_t)(-lt.tm_gmtoff / 60));   /* minutes west of GMT */
    wr8(L + Locale_loc_MeasuringSystem, french ? 0 : 1);
    wr8(L + Locale_loc_CalendarType, french ? 1 : 0);
    wr32(L + Locale_loc_DateTimeFormat, str_alloc(french ? "%A %e %B %Y %H:%M:%S" : "%A %B %e %Y %Q:%M:%S %p"));
    wr32(L + Locale_loc_DateFormat, str_alloc(french ? "%A %e %B %Y" : "%A %B %e %Y"));
    wr32(L + Locale_loc_TimeFormat, str_alloc(french ? "%H:%M:%S" : "%Q:%M:%S %p"));
    wr32(L + Locale_loc_ShortDateTimeFormat, str_alloc(french ? "%d/%m/%y %H:%M" : "%m/%d/%y %Q:%M %p"));
    wr32(L + Locale_loc_ShortDateFormat, str_alloc(french ? "%d/%m/%y" : "%m/%d/%y"));
    wr32(L + Locale_loc_ShortTimeFormat, str_alloc(french ? "%H:%M" : "%Q:%M %p"));
    wr32(L + Locale_loc_DecimalPoint, str_alloc(french ? "," : "."));
    wr32(L + Locale_loc_GroupSeparator, str_alloc(french ? " " : ","));
    wr32(L + Locale_loc_FracGroupSeparator, str_alloc(french ? " " : ","));
    uint32_t grp = exec_allocmem(4, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(grp, 3);
    wr32(L + Locale_loc_Grouping, grp);
    wr32(L + Locale_loc_FracGrouping, grp);
    wr32(L + Locale_loc_MonDecimalPoint, str_alloc(french ? "," : "."));
    wr32(L + Locale_loc_MonGroupSeparator, str_alloc(french ? " " : ","));
    wr32(L + Locale_loc_MonFracGroupSeparator, str_alloc(french ? " " : ","));
    wr32(L + Locale_loc_MonGrouping, grp);
    wr32(L + Locale_loc_MonFracGrouping, grp);
    wr8(L + Locale_loc_MonFracDigits, 2);
    wr8(L + Locale_loc_MonIntFracDigits, 2);
    wr32(L + Locale_loc_MonCS, str_alloc(french ? "\x80" : "$"));
    wr32(L + Locale_loc_MonSmallCS, str_alloc(french ? "c" : "c"));
    wr32(L + Locale_loc_MonIntCS, str_alloc(french ? "EUR" : "USD"));
    wr32(L + Locale_loc_MonPositiveSign, str_alloc(""));
    wr32(L + Locale_loc_MonNegativeSign, str_alloc("-"));
    wr8(L + Locale_loc_MonPositiveCSPos, french ? 1 : 0);
    wr8(L + Locale_loc_MonNegativeCSPos, french ? 1 : 0);
    return L;
}

static uint32_t loc_or_default(uint32_t L) { return L ? L : def_locale; }

/* ---- catalogs ------------------------------------------------------------------------ */

typedef struct Cat { uint32_t cat; int n; uint32_t *ids, *strs, *lens; struct Cat *next; } Cat;
static Cat *cats;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t load_catalog(const char *path, int minversion) {
    char hp[2048];
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 12 || size > (8 << 20)) { fclose(f); return 0; }
    uint8_t *b = malloc((size_t)size);
    if (fread(b, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(b); return 0; }
    fclose(f);
    if (memcmp(b, "FORM", 4) || memcmp(b + 8, "CTLG", 4)) { free(b); return 0; }
    long end = 8 + (long)be32(b + 4);
    if (end > size) end = size;
    Cat *c = calloc(1, sizeof *c);
    int version = 0, revision = 0, cap = 0;
    char lang[64] = "";
    for (long pos = 12; pos + 8 <= end;) {
        uint32_t len = be32(b + pos + 4);
        const uint8_t *d = b + pos + 8;
        if (pos + 8 + (long)len > end) break;
        if (!memcmp(b + pos, "FVER", 4)) {
            /* "$VER: name.catalog 44.2 (date)" */
            char v[128];
            snprintf(v, sizeof v, "%.*s", (int)len, d);
            char *p = strstr(v, ".catalog");
            if (p) { p += 8; while (*p == ' ') p++; version = atoi(p); char *dot = strchr(p, '.'); if (dot) revision = atoi(dot + 1); }
        } else if (!memcmp(b + pos, "LANG", 4)) {
            snprintf(lang, sizeof lang, "%.*s", (int)len, d);
        } else if (!memcmp(b + pos, "STRS", 4)) {
            for (uint32_t q = 0; q + 8 <= len;) {
                uint32_t id = be32(d + q), sl = be32(d + q + 4);
                if (q + 8 + sl > len) break;
                if (c->n == cap) { cap = cap * 2 + 64; c->ids = realloc(c->ids, sizeof(uint32_t) * (size_t)cap); c->strs = realloc(c->strs, sizeof(uint32_t) * (size_t)cap); c->lens = realloc(c->lens, sizeof(uint32_t) * (size_t)cap); }
                uint32_t s = exec_allocmem(sl + 1, MEMF_PUBLIC | MEMF_CLEAR);
                for (uint32_t i = 0; i < sl; i++) wr8(s + i, d[q + 8 + i]);
                c->ids[c->n] = id;
                c->strs[c->n] = s;
                c->lens[c->n] = sl + 1;
                c->n++;
                q += 8 + ((sl + 3) & ~3u);
            }
        }
        pos += 8 + ((len + 1) & ~1u);
    }
    free(b);
    if (minversion && version != minversion) {
        for (int i = 0; i < c->n; i++) exec_freemem(c->strs[i], c->lens[i]);
        free(c->ids); free(c->strs); free(c->lens); free(c);
        return 0;
    }
    uint32_t cat = exec_allocmem(Catalog_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(cat + Catalog_cat_Language, str_alloc(lang[0] ? lang : language));
    wr16(cat + Catalog_cat_Version, (uint32_t)version);
    wr16(cat + Catalog_cat_Revision, (uint32_t)revision);
    c->cat = cat;
    c->next = cats;
    cats = c;
    return cat;
}

/* ---- formatting ------------------------------------------------------------------------ */

typedef struct { M68K *c; uint32_t hook, locale; char *buf; int len, cap; } Out;

static void emit(Out *o, char ch) {
    if (o->len < o->cap) o->buf[o->len++] = ch;
}

static void flush_hook(Out *o) {
    /* the hook gets each character: A0 hook, A2 locale, A1 character */
    for (int i = 0; i < o->len; i++) call_hook(o->c, o->hook, o->locale, (uint8_t)o->buf[i]);
    call_hook(o->c, o->hook, o->locale, 0);
}

/* RawDoFmt-style format with locale numbers and %n$ positions */
static int format_string(uint32_t L, uint32_t fmt, uint32_t args, char *out, int cap) {
    Out o = { 0, 0, 0, out, 0, cap };
    char dp[8] = ".", gs[8] = "";
    str_get(rd32(L + Locale_loc_DecimalPoint), dp, sizeof dp);
    (void)gs;
    uint32_t argp = args;
    for (uint32_t p = fmt;;) {
        int ch = rd8(p++);
        if (!ch) break;
        if (ch != '%') { emit(&o, (char)ch); continue; }
        /* %[n$][flags][width][.limit][l]type */
        uint32_t save = p;
        int pos = 0;
        while (isdigit(rd8(p))) pos = pos * 10 + (rd8(p++) - '0');
        if (rd8(p) == '$' && pos > 0) { p++; } else { p = save; pos = 0; }
        int left = 0, zero = 0, width = 0, limit = -1, lng = 0, group = 0;
        for (;;) {
            int f = rd8(p);
            if (f == '-') left = 1; else if (f == '0') zero = 1; else if (f == '\'') group = 1; else break;
            p++;
        }
        while (isdigit(rd8(p))) width = width * 10 + (rd8(p++) - '0');
        if (rd8(p) == '.') { p++; limit = 0; while (isdigit(rd8(p))) limit = limit * 10 + (rd8(p++) - '0'); }
        if (rd8(p) == 'l') { lng = 1; p++; }
        int type = rd8(p++);
        if (!type) break;
        if (type == '%') { emit(&o, '%'); continue; }
        /* arguments are WORDs unless 'l' (strings, %b and %c too per RawDoFmt) */
        int size = (lng || type == 's' || type == 'b') ? 4 : 2;
        uint32_t ap = argp;
        if (pos) {
            /* positional: walk the earlier arguments of this format... simplified: all LONG */
            ap = args + 4 * (uint32_t)(pos - 1);
            size = 4;
        } else argp += (uint32_t)size;
        uint32_t v = size == 4 ? rd32(ap) : rd16(ap);
        char tmp[300];
        int n = 0;
        switch (type) {
        case 'd': case 'D': case 'u': case 'U': {
            int neg = 0;
            uint64_t u;
            if (type == 'd' || type == 'D') { int32_t sv = size == 4 ? (int32_t)v : (int16_t)v; neg = sv < 0; u = (uint64_t)(neg ? -(int64_t)sv : sv); }
            else u = v;
            char digits[32];
            int nd = 0;
            do { digits[nd++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (neg) tmp[n++] = '-';
            for (int i = nd - 1; i >= 0; i--) {
                tmp[n++] = digits[i];
                if (group && i && i % 3 == 0) {
                    char g[8] = "";
                    str_get(rd32(L + Locale_loc_GroupSeparator), g, sizeof g);
                    for (char *q = g; *q; q++) tmp[n++] = *q;
                }
            }
            break;
        }
        case 'x': case 'X': n = snprintf(tmp, sizeof tmp, type == 'x' ? "%x" : "%X", size == 4 ? v : (v & 0xFFFF)); break;
        case 'c': tmp[n++] = (char)(v & 0xFF); break;
        case 's': n = v ? str_get(v, tmp, sizeof tmp) : 0; break;
        case 'b': n = v ? bstr_get(v, tmp, sizeof tmp) : 0; break;
        default: tmp[n++] = (char)type; break;
        }
        if (limit >= 0 && (type == 's' || type == 'b') && n > limit) n = limit;
        int pad = width > n ? width - n : 0;
        if (!left) for (int i = 0; i < pad; i++) emit(&o, zero ? '0' : ' ');
        for (int i = 0; i < n; i++) emit(&o, tmp[i]);
        if (left) for (int i = 0; i < pad; i++) emit(&o, ' ');
    }
    return o.len;
}

/* DateStamp (days since 1978, minutes, ticks) to broken-down time */
static void ds_to_tm(uint32_t ds, struct tm *tm) {
    int32_t days = (int32_t)rd32(ds), mins = (int32_t)rd32(ds + 4), ticks = (int32_t)rd32(ds + 8);
    time_t t = (time_t)252460800 + (time_t)days * 86400 + mins * 60 + ticks / 50;
    gmtime_r(&t, tm);
}

static void put_str(Out *o, const char *s) { for (; *s; s++) emit(o, *s); }
static void put_num(Out *o, int v, int w, char pad) { char b[16]; snprintf(b, sizeof b, "%*d", w, v); for (char *p = b; *p; p++) emit(o, *p == ' ' ? pad : *p); }

static int format_date(uint32_t L, uint32_t fmt, uint32_t ds, char *out, int cap) {
    Out o = { 0, 0, 0, out, 0, cap };
    struct tm tm;
    ds_to_tm(ds, &tm);
    char f[256];
    str_get(fmt, f, sizeof f);
    (void)L;
    for (char *p = f; *p; p++) {
        if (*p != '%' || !p[1]) { emit(&o, *p); continue; }
        switch (*++p) {
        case 'a': put_str(&o, str_of(8 + tm.tm_wday)); break;
        case 'A': put_str(&o, str_of(1 + tm.tm_wday)); break;
        case 'b': case 'h': put_str(&o, str_of(27 + tm.tm_mon)); break;
        case 'B': put_str(&o, str_of(15 + tm.tm_mon)); break;
        case 'c': { char b[128]; int n = format_date(L, rd32(L + Locale_loc_DateTimeFormat), ds, b, sizeof b); for (int i = 0; i < n; i++) emit(&o, b[i]); break; }
        case 'C': { char b[128]; int n = format_date(L, rd32(L + Locale_loc_DateTimeFormat), ds, b, sizeof b); for (int i = 0; i < n; i++) emit(&o, b[i]); break; }
        case 'd': put_num(&o, tm.tm_mday, 2, '0'); break;
        case 'D': put_num(&o, tm.tm_mon + 1, 2, '0'); emit(&o, '/'); put_num(&o, tm.tm_mday, 2, '0'); emit(&o, '/'); put_num(&o, tm.tm_year % 100, 2, '0'); break;
        case 'e': put_num(&o, tm.tm_mday, 2, ' '); break;
        case 'H': put_num(&o, tm.tm_hour, 2, '0'); break;
        case 'I': put_num(&o, tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, 2, '0'); break;
        case 'j': put_num(&o, tm.tm_yday + 1, 3, '0'); break;
        case 'm': put_num(&o, tm.tm_mon + 1, 2, '0'); break;
        case 'M': put_num(&o, tm.tm_min, 2, '0'); break;
        case 'n': emit(&o, '\n'); break;
        case 'p': put_str(&o, str_of(tm.tm_hour < 12 ? 41 : 42)); break;
        case 'q': put_num(&o, tm.tm_hour, 0, ' '); break;
        case 'Q': put_num(&o, tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, 0, ' '); break;
        case 'r': put_num(&o, tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, 2, '0'); emit(&o, ':'); put_num(&o, tm.tm_min, 2, '0'); emit(&o, ':'); put_num(&o, tm.tm_sec, 2, '0'); emit(&o, ' '); put_str(&o, str_of(tm.tm_hour < 12 ? 41 : 42)); break;
        case 'R': put_num(&o, tm.tm_hour, 2, '0'); emit(&o, ':'); put_num(&o, tm.tm_min, 2, '0'); break;
        case 'S': put_num(&o, tm.tm_sec, 2, '0'); break;
        case 't': emit(&o, '\t'); break;
        case 'T': put_num(&o, tm.tm_hour, 2, '0'); emit(&o, ':'); put_num(&o, tm.tm_min, 2, '0'); emit(&o, ':'); put_num(&o, tm.tm_sec, 2, '0'); break;
        case 'U': put_num(&o, (tm.tm_yday + 7 - tm.tm_wday) / 7, 2, '0'); break;
        case 'w': put_num(&o, tm.tm_wday, 0, ' '); break;
        case 'W': put_num(&o, (tm.tm_yday + 7 - (tm.tm_wday + 6) % 7) / 7, 2, '0'); break;
        case 'x': { char b[128]; int n = format_date(L, rd32(L + Locale_loc_ShortDateFormat), ds, b, sizeof b); for (int i = 0; i < n; i++) emit(&o, b[i]); break; }
        case 'X': { char b[128]; int n = format_date(L, rd32(L + Locale_loc_TimeFormat), ds, b, sizeof b); for (int i = 0; i < n; i++) emit(&o, b[i]); break; }
        case 'y': put_num(&o, tm.tm_year % 100, 2, '0'); break;
        case 'Y': put_num(&o, tm.tm_year + 1900, 4, '0'); break;
        case '%': emit(&o, '%'); break;
        default: emit(&o, '%'); emit(&o, *p);
        }
    }
    return o.len;
}

/* ---- Latin-1 character classes ------------------------------------------------------------ */

static int l1_upper(int c) { return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 32 : c; }
static int l1_lower(int c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c; }
static int l1_isupper(int c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7); }
static int l1_islower(int c) { return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7); }
static int l1_isalpha(int c) { return l1_isupper(c) || l1_islower(c) || c == 0xAA || c == 0xBA; }
static int l1_isdigit(int c) { return c >= '0' && c <= '9'; }
static int l1_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0; }
static int l1_iscntrl(int c) { return c < 32 || (c >= 0x7F && c < 0xA0); }
static int l1_isprint(int c) { return !l1_iscntrl(c); }
static int l1_isgraph(int c) { return l1_isprint(c) && c != ' ' && c != 0xA0; }
static int l1_ispunct(int c) { return l1_isgraph(c) && !l1_isalpha(c) && !l1_isdigit(c); }
static int l1_isxdigit(int c) { return isxdigit(c) != 0 && c < 128; }

/* base letter of an accented Latin-1 letter (for the primary collation) */
static int l1_base(int c) {
    static const char map[64] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xC0) return (uint8_t)map[c - 0xC0];
    return c;
}

/* ---- library vectors -------------------------------------------------------------------- */

static uint32_t l_OpenLocale(M68K *c) {
    if (!c->a[0]) return def_locale;
    return def_locale;                                       /* named locales: the default one */
}
static uint32_t l_CloseLocale(M68K *c) { (void)c; return 0; }

static uint32_t l_OpenCatalogA(M68K *c) {
    char name[256], want[64] = "", builtin[64] = "english";
    str_get(c->a[1], name, sizeof name);
    uint32_t tags = c->a[2], ti;
    if ((ti = tag_find(tags, 0x80090001u)) && rd32(ti + 4)) str_get(rd32(ti + 4), builtin, sizeof builtin);
    if ((ti = tag_find(tags, 0x80090004u)) && rd32(ti + 4)) str_get(rd32(ti + 4), want, sizeof want);
    int version = (int)tag_get(tags, 0x80090003u, 0);
    uint32_t L = loc_or_default(c->a[0]);
    /* the preferred languages in order, stopping at the program's own */
    char langs[11][64];
    int nl = 0;
    if (want[0]) snprintf(langs[nl++], 64, "%s", want);
    for (int i = 0; i < 10 && nl < 11; i++) {
        uint32_t s = rd32(L + Locale_loc_PrefLanguages + 4 * (uint32_t)i);
        if (s) str_get(s, langs[nl++], 64);
    }
    for (int i = 0; i < nl; i++) {
        if (!strcasecmp(langs[i], builtin)) return 0;        /* the built-in strings are wanted */
        const char *dirs[] = { "PROGDIR:Catalogs", "LOCALE:Catalogs" };
        for (int d = 0; d < 2; d++) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s/%s", dirs[d], langs[i], name);
            uint32_t cat = load_catalog(p, version);
            if (cat) return cat;
        }
    }
    return 0;
}

static Cat *cat_of(uint32_t cat) { for (Cat *x = cats; x; x = x->next) if (x->cat == cat) return x; return 0; }

static uint32_t l_CloseCatalog(M68K *c) {
    Cat **pp = &cats;
    while (*pp && (*pp)->cat != c->a[0]) pp = &(*pp)->next;
    if (!*pp) return 0;
    Cat *x = *pp;
    *pp = x->next;
    for (int i = 0; i < x->n; i++) exec_freemem(x->strs[i], x->lens[i]);
    exec_freemem(x->cat, Catalog_SIZEOF);
    free(x->ids);
    free(x->strs);
    free(x->lens);
    free(x);
    return 0;
}

static uint32_t l_GetCatalogStr(M68K *c) {
    Cat *x = c->a[0] ? cat_of(c->a[0]) : 0;
    if (x) for (int i = 0; i < x->n; i++) if (x->ids[i] == c->d[0]) return x->strs[i];
    return c->a[1];
}

static uint32_t l_GetLocaleStr(M68K *c) {
    int id = (int)c->d[0];
    if (id < 1 || id > 70 || !str_of(id)[0]) return 0;
    if (!strs[id]) strs[id] = str_alloc(str_of(id));
    return strs[id];
}

static uint32_t l_FormatString(M68K *c) {
    uint32_t L = loc_or_default(c->a[0]);
    char *buf = malloc(65536);
    Out o = { c, c->a[3], L, buf, 0, 65536 };
    o.len = format_string(L, c->a[1], c->a[2], buf, 65536);
    if (c->a[3]) flush_hook(&o);
    /* returns the argument pointer past the used arguments: approximated by a WORD/LONG walk */
    free(buf);
    return c->a[2];
}

static uint32_t l_FormatDate(M68K *c) {
    uint32_t L = loc_or_default(c->a[0]);
    char buf[1024];
    Out o = { c, c->a[3], L, buf, 0, sizeof buf };
    o.len = format_date(L, c->a[1], c->a[2], buf, sizeof buf);
    if (c->a[3]) flush_hook(&o);
    return 0;
}

/* ParseDate(locale, date, template, getCharHook): %d %e %m %y %Y %H %M %S %b %B %a %A %p */
static uint32_t l_ParseDate(M68K *c) {
    uint32_t L = loc_or_default(c->a[0]), ds = c->a[1];
    char fmt[128], in[256];
    str_get(c->a[2], fmt, sizeof fmt);
    int n = 0;
    if (c->a[3]) {
        for (; n < (int)sizeof in - 1; n++) {
            uint32_t ch = call_hook(c, c->a[3], L, 0) & 0xFF;
            if (!ch) break;
            in[n] = (char)ch;
        }
    }
    in[n] = 0;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 78;
    tm.tm_mday = 1;
    const char *s = in;
    for (const char *f = fmt; *f; f++) {
        while (*s == ' ') s++;
        if (*f != '%') { if (*f == ' ') continue; if (*s != *f) return 0; s++; continue; }
        char *e;
        switch (*++f) {
        case 'd': case 'e': tm.tm_mday = (int)strtol(s, &e, 10); if (e == s) return 0; s = e; break;
        case 'm': tm.tm_mon = (int)strtol(s, &e, 10) - 1; if (e == s) return 0; s = e; break;
        case 'y': { int y = (int)strtol(s, &e, 10); if (e == s) return 0; tm.tm_year = y < 78 ? y + 100 : y; s = e; break; }
        case 'Y': tm.tm_year = (int)strtol(s, &e, 10) - 1900; if (e == s) return 0; s = e; break;
        case 'H': tm.tm_hour = (int)strtol(s, &e, 10); s = e; break;
        case 'M': tm.tm_min = (int)strtol(s, &e, 10); s = e; break;
        case 'S': tm.tm_sec = (int)strtol(s, &e, 10); s = e; break;
        case 'b': case 'B': case 'h': {
            int found = 0;
            for (int m = 0; m < 12 && !found; m++)
                for (int k = 0; k < 2 && !found; k++) {
                    const char *nm = str_of(k ? 15 + m : 27 + m);
                    size_t l = strlen(nm);
                    if (l && !strncasecmp(s, nm, l)) { tm.tm_mon = m; s += l; found = 1; }
                }
            if (!found) return 0;
            break;
        }
        case 'a': case 'A':
            for (int d = 1; d <= 14; d++) { const char *nm = str_of(d); size_t l = strlen(nm); if (l && !strncasecmp(s, nm, l)) { s += l; break; } }
            break;
        case 'p': if (!strncasecmp(s, "PM", 2)) { if (tm.tm_hour < 12) tm.tm_hour += 12; s += 2; } else if (!strncasecmp(s, "AM", 2)) s += 2; break;
        default: break;
        }
    }
    if (ds) {
        time_t t = timegm(&tm) - 252460800;
        wr32(ds, (uint32_t)(t / 86400));
        wr32(ds + 4, (uint32_t)(t % 86400 / 60));
        wr32(ds + 8, (uint32_t)(t % 60 * 50));
    }
    return DOSTRUE;
}

static uint32_t l_ConvToLower(M68K *c) { return (uint32_t)l1_lower((int)(c->d[0] & 0xFF)); }
static uint32_t l_ConvToUpper(M68K *c) { return (uint32_t)l1_upper((int)(c->d[0] & 0xFF)); }
static uint32_t l_IsAlNum2(M68K *c) { int x = (int)(c->d[0] & 0xFF); return l1_isalpha(x) || l1_isdigit(x) ? DOSTRUE : 0; }
static uint32_t l_IsAlpha(M68K *c) { return l1_isalpha((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsCntrl(M68K *c) { return l1_iscntrl((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsDigit(M68K *c) { return l1_isdigit((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsGraph(M68K *c) { return l1_isgraph((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsLower(M68K *c) { return l1_islower((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsPrint(M68K *c) { return l1_isprint((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsPunct(M68K *c) { return l1_ispunct((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsSpace(M68K *c) { return l1_isspace((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsUpper(M68K *c) { return l1_isupper((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }
static uint32_t l_IsXDigit(M68K *c) { return l1_isxdigit((int)(c->d[0] & 0xFF)) ? DOSTRUE : 0; }

/* StrnCmp(locale, s1, s2, len, type) */
static uint32_t l_StrnCmp(M68K *c) {
    uint32_t a = c->a[1], b = c->a[2];
    int32_t len = (int32_t)c->d[0];
    int type = (int)c->d[1];
    int secondary = 0;
    for (int32_t i = 0; len < 0 || i < len; i++) {
        int x = rd8(a + (uint32_t)i), y = rd8(b + (uint32_t)i);
        if (!x && !y) break;
        int px = x, py = y;
        if (type != 0) { px = l1_upper(l1_base(x)); py = l1_upper(l1_base(y)); }
        if (px != py) return (uint32_t)(px < py ? -1 : 1);
        if (type == 2 && !secondary && x != y) secondary = x < y ? -1 : 1;
        if (!x || !y) break;
    }
    return (uint32_t)secondary;
}

/* StrConvert(locale, string, buffer, size, type): a key that sorts with strcmp */
static uint32_t l_StrConvert(M68K *c) {
    uint32_t s = c->a[1], buf = c->a[2], size = c->d[0];
    int type = (int)c->d[1];
    uint32_t i = 0;
    for (; size && i < size - 1 && rd8(s + i); i++) {
        int x = rd8(s + i);
        wr8(buf + i, (uint32_t)(type ? l1_upper(l1_base(x)) : x));
    }
    if (size) wr8(buf + i, 0);
    return i;
}

static const NativeEntry locale_natives[] = {
    { "CloseCatalog", l_CloseCatalog }, { "CloseLocale", l_CloseLocale }, { "ConvToLower", l_ConvToLower },
    { "ConvToUpper", l_ConvToUpper }, { "FormatDate", l_FormatDate }, { "FormatString", l_FormatString },
    { "GetCatalogStr", l_GetCatalogStr }, { "GetLocaleStr", l_GetLocaleStr }, { "IsAlNum", l_IsAlNum2 },
    { "IsAlpha", l_IsAlpha }, { "IsCntrl", l_IsCntrl }, { "IsDigit", l_IsDigit }, { "IsGraph", l_IsGraph },
    { "IsLower", l_IsLower }, { "IsPrint", l_IsPrint }, { "IsPunct", l_IsPunct }, { "IsSpace", l_IsSpace },
    { "IsUpper", l_IsUpper }, { "IsXDigit", l_IsXDigit }, { "OpenCatalogA", l_OpenCatalogA },
    { "OpenLocale", l_OpenLocale }, { "ParseDate", l_ParseDate }, { "StrConvert", l_StrConvert },
    { "StrnCmp", l_StrnCmp },
    { 0, 0 }
};

void locale_init(void) {
    pick_language();
    def_locale = make_locale();
    LocaleBase = lib_create("locale.library", "locale", 64, locale_natives, 44, 1, "locale 44.1 (AmigaMac)");
    lib_add(LocaleBase);
}
