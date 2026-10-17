/* utility.library, native: tag lists, hooks, dates, 32/64-bit arithmetic,
 * case-insensitive strings. Registers: utility_lib.fd. */
#include "os.h"
#include "gfx.h"
#include <ctype.h>
#include <string.h>
#include <time.h>

uint32_t UtilityBase;

/* ---- tag lists (shared with BOOPSI, gadtools, asl...) ------------------------- */

#define TAG_DONE 0
#define TAG_IGNORE 1
#define TAG_MORE 2
#define TAG_SKIP 3

/* next real TagItem of a list, following TAG_MORE/TAG_SKIP; *state advances */
uint32_t tag_next(uint32_t *state) {
    uint32_t t = *state;
    while (t) {
        uint32_t tag = rd32(t), data = rd32(t + 4);
        switch (tag) {
        case TAG_DONE: *state = 0; return 0;
        case TAG_IGNORE: t += 8; continue;
        case TAG_MORE: t = data; continue;
        case TAG_SKIP: t += 8 * (data + 1); continue;
        default: *state = t + 8; return t;
        }
    }
    *state = 0;
    return 0;
}

uint32_t tag_find(uint32_t list, uint32_t tag) {
    uint32_t st = list, ti;
    while ((ti = tag_next(&st))) if (rd32(ti) == tag) return ti;
    return 0;
}

uint32_t tag_get(uint32_t list, uint32_t tag, uint32_t def) {
    uint32_t ti = tag_find(list, tag);
    return ti ? rd32(ti + 4) : def;
}

/* call a Hook: h_Entry with A0 = hook, A2 = object, A1 = message */
uint32_t call_hook(M68K *c, uint32_t hook, uint32_t obj, uint32_t msg) {
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    c->a[0] = hook; c->a[2] = obj; c->a[1] = msg;
    cpu_call(c, rd32(hook + Hook_h_Entry));
    uint32_t r = c->d[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    return r;
}

static uint32_t u_FindTagItem(M68K *c) { return tag_find(c->a[0], c->d[0]); }
static uint32_t u_GetTagData(M68K *c) { return tag_get(c->a[0], c->d[0], c->d[1]); }
static uint32_t u_NextTagItem(M68K *c) {
    uint32_t pp = c->a[0], st = rd32(pp);
    uint32_t ti = tag_next(&st);
    wr32(pp, st);
    return ti;
}
static uint32_t u_TagInArray(M68K *c) {
    for (uint32_t a = c->a[0]; rd32(a) != TAG_DONE; a += 4) if (rd32(a) == c->d[0]) return DOSTRUE;
    return 0;
}
static uint32_t u_PackBoolTags(M68K *c) {
    /* boolMap: {tag, flag} pairs; each TRUE/FALSE tag sets/clears its flag */
    uint32_t flags = c->d[0], st = c->a[0], ti;
    while ((ti = tag_next(&st)))
        for (uint32_t m = c->a[1]; rd32(m) != TAG_DONE; m += 8)
            if (rd32(m) == rd32(ti)) flags = rd32(ti + 4) ? flags | rd32(m + 4) : flags & ~rd32(m + 4);
    return flags;
}
static uint32_t count_tags(uint32_t list) {
    uint32_t st = list, n = 0;
    while (tag_next(&st)) n++;
    return n;
}
static uint32_t u_AllocateTagItems(M68K *c) {
    uint32_t n = c->d[0];
    uint32_t a = exec_allocvec(8 * (n + 1), MEMF_PUBLIC | MEMF_CLEAR);
    return a;
}
static uint32_t u_CloneTagItems(M68K *c) {
    uint32_t n = count_tags(c->a[0]), a = exec_allocvec(8 * (n + 1), MEMF_PUBLIC | MEMF_CLEAR), st = c->a[0], ti, i = 0;
    while ((ti = tag_next(&st))) { wr32(a + 8 * i, rd32(ti)); wr32(a + 8 * i + 4, rd32(ti + 4)); i++; }
    return a;
}
static uint32_t u_FreeTagItems(M68K *c) { exec_freevec(c->a[0]); return 0; }
static uint32_t u_RefreshTagItemClones(M68K *c) {
    uint32_t st = c->a[1], ti, i = 0;
    while ((ti = tag_next(&st))) { wr32(c->a[0] + 8 * i, rd32(ti)); wr32(c->a[0] + 8 * i + 4, rd32(ti + 4)); i++; }
    return 0;
}
static uint32_t u_FilterTagItems(M68K *c) {
    /* logic D0: TAGFILTER_AND 0 keeps tags in the array, TAGFILTER_NOT 1 removes them */
    uint32_t st = c->a[0], ti, kept = 0;
    while ((ti = tag_next(&st))) {
        int in = 0;
        for (uint32_t a = c->a[1]; rd32(a) != TAG_DONE; a += 4) if (rd32(a) == rd32(ti)) in = 1;
        if (in == (c->d[0] == 0)) kept++; else wr32(ti, TAG_IGNORE);
    }
    return kept;
}
static uint32_t u_MapTags(M68K *c) {
    /* mapType D0: MAP_REMOVE_NOT_FOUND 0, MAP_KEEP_NOT_FOUND 1 */
    uint32_t st = c->a[0], ti;
    while ((ti = tag_next(&st))) {
        int found = 0;
        for (uint32_t m = c->a[1]; m && rd32(m) != TAG_DONE; m += 8)
            if (rd32(m) == rd32(ti)) { wr32(ti, rd32(m + 4)); found = 1; break; }
        if (!found && c->d[0] == 0) wr32(ti, TAG_IGNORE);
    }
    return 0;
}
static uint32_t u_ApplyTagChanges(M68K *c) {
    uint32_t st = c->a[1], ti;
    while ((ti = tag_next(&st))) {
        uint32_t t = tag_find(c->a[0], rd32(ti));
        if (t) wr32(t + 4, rd32(ti + 4));
    }
    return 0;
}
static uint32_t u_FilterTagChanges(M68K *c) {
    /* drop from changeList the tags whose value equals the original; apply if D0 */
    uint32_t st = c->a[0], ti;
    while ((ti = tag_next(&st))) {
        uint32_t o = tag_find(c->a[1], rd32(ti));
        if (o && rd32(o + 4) == rd32(ti + 4)) wr32(ti, TAG_IGNORE);
        else if (o && c->d[0]) wr32(o + 4, rd32(ti + 4));
    }
    return 0;
}
static uint32_t u_CallHookPkt(M68K *c) { return call_hook(c, c->a[0], c->a[2], c->a[1]); }

/* ---- dates: seconds since 1 Jan 1978 <-> ClockData ------------------------------ */

static uint32_t u_Amiga2Date(M68K *c) {
    time_t t = (time_t)c->d[0] + 252460800;
    struct tm tm;
    gmtime_r(&t, &tm);
    uint32_t cd = c->a[0];
    wr16(cd + ClockData_sec, (uint32_t)tm.tm_sec);
    wr16(cd + ClockData_min, (uint32_t)tm.tm_min);
    wr16(cd + ClockData_hour, (uint32_t)tm.tm_hour);
    wr16(cd + ClockData_mday, (uint32_t)tm.tm_mday);
    wr16(cd + ClockData_month, (uint32_t)(tm.tm_mon + 1));
    wr16(cd + ClockData_year, (uint32_t)(tm.tm_year + 1900));
    wr16(cd + ClockData_wday, (uint32_t)tm.tm_wday);
    return 0;
}
static uint32_t u_Date2Amiga(M68K *c) {
    uint32_t cd = c->a[0];
    struct tm tm = { 0 };
    tm.tm_sec = (int)rd16(cd + ClockData_sec);
    tm.tm_min = (int)rd16(cd + ClockData_min);
    tm.tm_hour = (int)rd16(cd + ClockData_hour);
    tm.tm_mday = (int)rd16(cd + ClockData_mday);
    tm.tm_mon = (int)rd16(cd + ClockData_month) - 1;
    tm.tm_year = (int)rd16(cd + ClockData_year) - 1900;
    time_t t = timegm(&tm);
    return (uint32_t)(t - 252460800);
}
static uint32_t u_CheckDate(M68K *c) {
    uint32_t cd = c->a[0];
    int mon = (int)rd16(cd + ClockData_month), day = (int)rd16(cd + ClockData_mday), year = (int)rd16(cd + ClockData_year);
    static const int dim[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon < 1 || mon > 12 || day < 1 || day > dim[mon - 1] || year < 1978 || rd16(cd + ClockData_hour) > 23 ||
        rd16(cd + ClockData_min) > 59 || rd16(cd + ClockData_sec) > 59) return 0;
    return u_Date2Amiga(c);
}

/* ---- arithmetic -------------------------------------------------------------- */

static uint32_t u_SMult32(M68K *c) { return (uint32_t)((int32_t)c->d[0] * (int32_t)c->d[1]); }
static uint32_t u_UMult32(M68K *c) { return c->d[0] * c->d[1]; }
static uint32_t u_SDivMod32(M68K *c) {
    int32_t a = (int32_t)c->d[0], b = (int32_t)c->d[1];
    if (!b) { c->d[1] = 0; return 0; }
    c->d[1] = (uint32_t)(a % b);
    return (uint32_t)(a / b);
}
static uint32_t u_UDivMod32(M68K *c) {
    uint32_t a = c->d[0], b = c->d[1];
    if (!b) { c->d[1] = 0; return 0; }
    c->d[1] = a % b;
    return a / b;
}
static uint32_t u_SMult64(M68K *c) {
    int64_t r = (int64_t)(int32_t)c->d[0] * (int32_t)c->d[1];
    c->d[1] = (uint32_t)r;                         /* D0:D1 = high:low */
    return (uint32_t)((uint64_t)r >> 32);
}
static uint32_t u_UMult64(M68K *c) {
    uint64_t r = (uint64_t)c->d[0] * c->d[1];
    c->d[1] = (uint32_t)r;
    return (uint32_t)(r >> 32);
}

/* ---- strings ------------------------------------------------------------------ */

static int lat1_upper(int ch) {
    ch &= 0xFF;
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) return ch - 32;      /* Latin-1 accented letters */
    return ch;
}
static int lat1_lower(int ch) {
    ch &= 0xFF;
    if (ch >= 'A' && ch <= 'Z') return ch + 32;
    if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return ch + 32;
    return ch;
}
static uint32_t u_Stricmp(M68K *c) {
    for (uint32_t i = 0;; i++) {
        int a = lat1_upper((int)rd8(c->a[0] + i)), b = lat1_upper((int)rd8(c->a[1] + i));
        if (a != b) return (uint32_t)(a < b ? -1 : 1);
        if (!a) return 0;
    }
}
static uint32_t u_Strnicmp(M68K *c) {
    for (uint32_t i = 0; i < c->d[0]; i++) {
        int a = lat1_upper((int)rd8(c->a[0] + i)), b = lat1_upper((int)rd8(c->a[1] + i));
        if (a != b) return (uint32_t)(a < b ? -1 : 1);
        if (!a) return 0;
    }
    return 0;
}
static uint32_t u_ToUpper(M68K *c) { return (uint32_t)lat1_upper((int)c->d[0]); }
static uint32_t u_ToLower(M68K *c) { return (uint32_t)lat1_lower((int)c->d[0]); }
static uint32_t u_GetUniqueID(M68K *c) { (void)c; static uint32_t id = 0x10000; return id++; }

static const NativeEntry util_natives[] = {
    { "FindTagItem", u_FindTagItem }, { "GetTagData", u_GetTagData }, { "NextTagItem", u_NextTagItem },
    { "TagInArray", u_TagInArray }, { "PackBoolTags", u_PackBoolTags }, { "AllocateTagItems", u_AllocateTagItems },
    { "CloneTagItems", u_CloneTagItems }, { "FreeTagItems", u_FreeTagItems },
    { "RefreshTagItemClones", u_RefreshTagItemClones }, { "FilterTagItems", u_FilterTagItems },
    { "MapTags", u_MapTags }, { "ApplyTagChanges", u_ApplyTagChanges }, { "FilterTagChanges", u_FilterTagChanges },
    { "CallHookPkt", u_CallHookPkt },
    { "Amiga2Date", u_Amiga2Date }, { "Date2Amiga", u_Date2Amiga }, { "CheckDate", u_CheckDate },
    { "SMult32", u_SMult32 }, { "UMult32", u_UMult32 }, { "SDivMod32", u_SDivMod32 }, { "UDivMod32", u_UDivMod32 },
    { "SMult64", u_SMult64 }, { "UMult64", u_UMult64 },
    { "Stricmp", u_Stricmp }, { "Strnicmp", u_Strnicmp }, { "ToUpper", u_ToUpper }, { "ToLower", u_ToLower },
    { "GetUniqueID", u_GetUniqueID },
    { 0, 0 }
};

void utility_init(void) {
    UtilityBase = lib_create("utility.library", "utility", 64, util_natives, 40, 1, "utility 40.1 (AmigaMac)");
    lib_add(UtilityBase);
}
