/* clipboard.device, written from the clipboard.device autodocs.
 * Each unit keeps its clip natively. Unit 0 is shared with the Mac: text
 * written as IFF FTXT goes to the Mac pasteboard on CMD_UPDATE, and a read
 * from the start gets what the Mac copied since, as FTXT. */
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_UNIT 24
#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define IO_ACTUAL 32
#define IO_LENGTH 36
#define IO_DATA 40
#define IO_OFFSET 44
#define IO_CLIPID 48
#define IOF_QUICK 1
#define CMD_RESET 1
#define CMD_READ 2
#define CMD_WRITE 3
#define CMD_UPDATE 4
#define CBD_POST 9
#define CBD_CURRENTREADID 10
#define CBD_CURRENTWRITEID 11
#define CBD_CHANGEHOOK 12
#define CBERR_OBSOLETEID 1

typedef struct { uint8_t *data; uint32_t len, cap; int32_t id; uint8_t *wdata; uint32_t wlen, wcap; } Clip;
static Clip clips[256];
static int32_t ids;
static char host_last[65536];                     /* the Mac text we last saw or set */
static uint32_t ClipBase;

static void complete(uint32_t ior) { if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) exec_reply_msg(ior); }
static Clip *clip_of(uint32_t ior) { return &clips[rd32(ior + IO_UNIT) & 0xFF]; }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* text -> FORM FTXT CHRS */
static void set_ftxt(Clip *k, const char *latin1, uint32_t n) {
    uint32_t chrs = n + (n & 1), form = 4 + 8 + chrs;
    free(k->data);
    k->len = 8 + form;
    k->data = malloc(k->len);
    memcpy(k->data, "FORM", 4); put32(k->data + 4, form);
    memcpy(k->data + 8, "FTXT", 4);
    memcpy(k->data + 12, "CHRS", 4); put32(k->data + 16, n);
    memcpy(k->data + 20, latin1, n);
    if (n & 1) k->data[20 + n] = 0;
    k->id = ++ids;
}

/* the text of an FTXT clip (CHRS chunks), or -1 */
static int ftxt_text(const uint8_t *d, uint32_t len, char *out, int max) {
    if (len < 12 || memcmp(d, "FORM", 4) || memcmp(d + 8, "FTXT", 4)) return -1;
    int k = 0;
    for (uint32_t p = 12; p + 8 <= len;) {
        uint32_t sz = get32(d + p + 4);
        if (p + 8 + sz > len) break;
        if (!memcmp(d + p, "CHRS", 4)) for (uint32_t i = 0; i < sz && k < max - 1; i++) out[k++] = (char)d[p + 8 + i];
        p += 8 + ((sz + 1) & ~1u);
    }
    out[k] = 0;
    return k;
}

/* unit 0: has the Mac pasteboard changed? */
static void pull_host(Clip *k) {
    if (!host.clipboard_get) return;
    static char buf[65536];
    int n = host.clipboard_get(buf, sizeof buf);
    if (n < 0 || !strcmp(buf, host_last)) return;
    snprintf(host_last, sizeof host_last, "%s", buf);
    char *l1 = malloc((size_t)n + 1);
    dos_name_to_amiga(buf, l1, n + 1);
    set_ftxt(k, l1, (uint32_t)strlen(l1));
    free(l1);
}

static void push_host(Clip *k) {
    if (!host.clipboard_set) return;
    static char l1[65536], utf8[3 * 65536];
    int n = ftxt_text(k->data, k->len, l1, sizeof l1);
    if (n < 0) return;
    int m = 0;
    for (int i = 0; i < n && m < (int)sizeof utf8 - 3; i++) {
        uint8_t c = (uint8_t)l1[i];
        if (c < 0x80) utf8[m++] = (char)c;
        else { utf8[m++] = (char)(0xC0 | (c >> 6)); utf8[m++] = (char)(0x80 | (c & 0x3F)); }
    }
    utf8[m] = 0;
    snprintf(host_last, sizeof host_last, "%s", utf8);
    host.clipboard_set(utf8, m);
}

static uint32_t c_Open(M68K *c) {
    uint32_t ior = c->a[1];
    if (c->d[0] > 255) { wr8(ior + IO_ERROR, (uint8_t)-1); return 0; }
    wr32(ior + IO_UNIT, c->d[0]);
    wr8(ior + IO_ERROR, 0);
    return 0;
}
static uint32_t c_Close(M68K *c) { (void)c; return 0; }
static uint32_t c_Expunge(M68K *c) { (void)c; return 0; }

static uint32_t c_BeginIO(M68K *c) {
    (void)c;
    uint32_t ior = c->a[1];
    Clip *k = clip_of(ior);
    int unit = (int)(rd32(ior + IO_UNIT) & 0xFF);
    wr8(ior + IO_ERROR, 0);
    switch (rd16(ior + IO_COMMAND)) {
    case CMD_READ: {
        uint32_t off = rd32(ior + IO_OFFSET), len = rd32(ior + IO_LENGTH), dst = rd32(ior + IO_DATA);
        if (off == 0 && !rd32(ior + IO_CLIPID)) {             /* a new read */
            if (unit == 0) pull_host(k);
            wr32(ior + IO_CLIPID, (uint32_t)k->id);
        }
        uint32_t n = off < k->len ? k->len - off : 0;
        if (n > len) n = len;
        for (uint32_t i = 0; i < n; i++) wr8(dst + i, k->data[off + i]);
        wr32(ior + IO_ACTUAL, n);
        wr32(ior + IO_OFFSET, off + n);
        break;
    }
    case CMD_WRITE: {
        uint32_t off = rd32(ior + IO_OFFSET), len = rd32(ior + IO_LENGTH), src = rd32(ior + IO_DATA);
        if (!rd32(ior + IO_CLIPID)) { wr32(ior + IO_CLIPID, (uint32_t)++ids); k->wlen = 0; }
        if (off + len > k->wcap) { k->wcap = (off + len) * 2 + 256; k->wdata = realloc(k->wdata, k->wcap); }
        for (uint32_t i = 0; i < len; i++) k->wdata[off + i] = (uint8_t)rd8(src + i);
        if (off + len > k->wlen) k->wlen = off + len;
        wr32(ior + IO_ACTUAL, len);
        wr32(ior + IO_OFFSET, off + len);
        break;
    }
    case CMD_UPDATE:
        free(k->data);
        k->data = k->wdata;
        k->len = k->wlen;
        k->id = (int32_t)rd32(ior + IO_CLIPID);
        k->wdata = 0;
        k->wlen = k->wcap = 0;
        if (unit == 0) push_host(k);
        break;
    case CBD_CURRENTREADID:
        if (unit == 0) pull_host(k);
        wr32(ior + IO_CLIPID, (uint32_t)k->id);
        break;
    case CBD_CURRENTWRITEID: wr32(ior + IO_CLIPID, (uint32_t)ids); break;
    case CBD_POST: case CBD_CHANGEHOOK: case CMD_RESET: break;
    default: wr8(ior + IO_ERROR, (uint8_t)-3);
    }
    complete(ior);
    return 0;
}
static uint32_t c_AbortIO(M68K *c) { (void)c; return 0; }

static const NativeEntry clip_natives[] = {
    { "Open", c_Open }, { "Close", c_Close }, { "Expunge", c_Expunge },
    { "BeginIO", c_BeginIO }, { "AbortIO", c_AbortIO },
    { 0, 0 }
};

void clipboard_init(void) {
    ClipBase = lib_create("clipboard.device", 0, LIB_SIZE, clip_natives, 40, 1, "clipboard 40.1 (AmigaMac)");
    wr8(ClipBase + LN_TYPE, NT_DEVICE);
    list_enqueue(SysBase + EB_DEVICELIST, ClipBase);
}
