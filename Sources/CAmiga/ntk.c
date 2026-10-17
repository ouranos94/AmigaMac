/* Small toolkit for native code that builds Amiga user interfaces through the
 * public library calls (ASL requesters, the Workbench): 68k memory arenas for
 * strings, tag lists and structures, and IntuiMessage helpers. */
#include "ntk.h"
#include "ndk_offsets.h"
#include "amiga.h"
#include <stdarg.h>
#include <string.h>

uint32_t ar_alloc(Arena *a, uint32_t size) {
    uint32_t m = exec_allocvec(size ? size : 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (a->n == a->cap) { a->cap = a->cap * 2 + 32; a->blk = realloc(a->blk, sizeof(uint32_t) * (size_t)a->cap); }
    a->blk[a->n++] = m;
    return m;
}

uint32_t ar_str(Arena *a, const char *s) {
    size_t n = strlen(s);
    uint32_t m = ar_alloc(a, (uint32_t)n + 1);
    for (size_t i = 0; i <= n; i++) wr8(m + (uint32_t)i, (uint8_t)s[i]);
    return m;
}

uint32_t ar_tags(Arena *a, ...) {
    uint32_t v[128];
    int n = 0;
    va_list ap;
    va_start(ap, a);
    for (;;) {
        uint32_t t = va_arg(ap, uint32_t);
        if (t == 0 || n >= 126) break;
        v[n++] = t;
        v[n++] = va_arg(ap, uint32_t);
    }
    va_end(ap);
    uint32_t m = ar_alloc(a, 4 * (uint32_t)n + 8);
    for (int i = 0; i < n; i++) wr32(m + 4 * (uint32_t)i, v[i]);
    return m;
}

void ar_free(Arena *a) {
    for (int i = 0; i < a->n; i++) exec_freevec(a->blk[i]);
    free(a->blk);
    a->blk = 0;
    a->n = a->cap = 0;
}

void put_cstr(uint32_t dst, const char *s, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) wr8(dst + (uint32_t)i, (uint8_t)s[i]);
    wr8(dst + (uint32_t)i, 0);
}

/* next IntuiMessage of a window, copied and replied; 0 when none */
int ntk_getmsg(uint32_t win, NMsg *m) {
    uint32_t port = rd32(win + Window_UserPort);
    if (!port) return 0;
    uint32_t msg = list_remhead(port + MP_MSGLIST);
    if (!msg) return 0;
    m->cls = rd32(msg + IntuiMessage_Class);
    m->code = rd16(msg + IntuiMessage_Code);
    m->qual = rd16(msg + IntuiMessage_Qualifier);
    m->iaddr = rd32(msg + IntuiMessage_IAddress);
    m->mx = (int16_t)rd16(msg + IntuiMessage_MouseX);
    m->my = (int16_t)rd16(msg + IntuiMessage_MouseY);
    m->secs = rd32(msg + IntuiMessage_Seconds);
    m->micros = rd32(msg + IntuiMessage_Micros);
    exec_reply_msg(msg);
    return 1;
}

uint32_t ntk_sigmask(uint32_t win) {
    uint32_t port = rd32(win + Window_UserPort);
    return port ? 1u << rd8(port + MP_SIGBIT) : 0;
}

int ntk_double_click(uint32_t s1, uint32_t m1, uint32_t s2, uint32_t m2) {
    int64_t a = (int64_t)s1 * 1000000 + m1, b = (int64_t)s2 * 1000000 + m2;
    return b - a >= 0 && b - a < 500000;
}

/* contents of a string gadget */
int ntk_gadget_string(uint32_t g, char *buf, int max) {
    uint32_t si = rd32(g + Gadget_SpecialInfo);
    if (!si) { buf[0] = 0; return 0; }
    return str_get(rd32(si + StringInfo_Buffer), buf, max);
}

uint32_t ntk_new_gadget(M68K *c, Arena *A, int kind, uint32_t prev, uint32_t vi, int x, int y, int w, int h,
                        const char *label, int id, int flags, uint32_t tags) {
    uint32_t ng = ar_alloc(A, NewGadget_SIZEOF + 2);
    wr16(ng + NewGadget_ng_LeftEdge, (uint32_t)x);
    wr16(ng + NewGadget_ng_TopEdge, (uint32_t)y);
    wr16(ng + NewGadget_ng_Width, (uint32_t)w);
    wr16(ng + NewGadget_ng_Height, (uint32_t)h);
    wr32(ng + NewGadget_ng_GadgetText, label ? ar_str(A, label) : 0);
    wr16(ng + NewGadget_ng_GadgetID, (uint32_t)id);
    wr32(ng + NewGadget_ng_Flags, (uint32_t)flags);
    wr32(ng + NewGadget_ng_VisualInfo, vi);
    return lib_call(c, GadToolsBase, "gadtools", "CreateGadgetA", (uint32_t)kind, prev, ng, tags);
}

void ntk_set_gattr(M68K *c, Arena *A, uint32_t g, uint32_t win, uint32_t tag, uint32_t data) {
    uint32_t tags = ar_alloc(A, 16);
    wr32(tags, tag);
    wr32(tags + 4, data);
    lib_call(c, GadToolsBase, "gadtools", "GT_SetGadgetAttrsA", g, win, 0, tags);
}

/* UTF-8 (host names, C literals) to the Latin-1 the Amiga side uses */
void to_latin1(const char *in, char *out, int max) {
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k < max - 1; p++) {
        if ((*p == 0xC2 || *p == 0xC3) && (p[1] & 0xC0) == 0x80) { out[k++] = (char)((*p == 0xC3 ? 0x40 : 0) + p[1]); p++; }
        else if (*p >= 0xC4 && *p < 0xF8) {                     /* outside Latin-1 */
            out[k++] = '?';
            while ((p[1] & 0xC0) == 0x80) p++;
        } else out[k++] = (char)*p;
    }
    out[k] = 0;
}

/* a string in 68k memory, converted from UTF-8 */
uint32_t ar_text(Arena *a, const char *utf8) {
    char buf[1024];
    to_latin1(utf8, buf, sizeof buf);
    return ar_str(a, buf);
}
