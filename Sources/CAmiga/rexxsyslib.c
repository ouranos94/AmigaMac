/* rexxsyslib.library, written from the rexxsyslib autodocs and rexx/storage.h:
 * argument strings and REXX messages, as programs with an ARexx port use
 * them. There is no ARexx interpreter (no "REXX" port), so scripts are not
 * run; hosts and their messages work. */
#include "os.h"
#include <stdio.h>
#include <string.h>

uint32_t RexxSysBase;

#define RM_SIZE 128
#define RM_ACTION 28
#define RM_RESULT1 32
#define RM_RESULT2 36
#define RM_ARGS 40
#define RM_COMMADDR 108
#define RM_FILEEXT 112
#define RM_STDIN 116
#define RM_STDOUT 120
#define RXFF_RESULT 0x00020000

static uint32_t rexx_name;                    /* "REXX": the ln_Name of every RexxMsg */

/* RexxArg: ra_Size, ra_Length, ra_Flags, ra_Hash, then the string; the
   argstring points at the string */
static uint32_t make_argstring(uint32_t src, uint32_t len, const char *native) {
    uint32_t size = (8 + len + 1 + 7) & ~7u;
    uint32_t a = exec_allocmem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!a) return 0;
    wr32(a, size);
    wr16(a + 4, len);
    uint8_t hash = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t ch = native ? (uint8_t)native[i] : (uint8_t)rd8(src + i);
        wr8(a + 8 + i, ch);
        hash = (uint8_t)(hash + ch);
    }
    wr8(a + 7, hash);
    return a + 8;
}

static uint32_t r_CreateArgstring(M68K *c) { return make_argstring(c->a[0], c->d[0] & 0xFFFF, 0); }
static uint32_t r_DeleteArgstring(M68K *c) { if (c->a[0]) exec_freemem(c->a[0] - 8, rd32(c->a[0] - 8)); return 0; }
static uint32_t r_LengthArgstring(M68K *c) { return c->a[0] ? rd16(c->a[0] - 4) : 0; }

static uint32_t r_CreateRexxMsg(M68K *c) {
    uint32_t m = exec_allocmem(RM_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(m + LN_TYPE, NT_MESSAGE);
    wr32(m + LN_NAME, rexx_name);
    wr32(m + MN_REPLYPORT, c->a[0]);
    wr16(m + MN_LENGTH, RM_SIZE);
    wr32(m + RM_FILEEXT, c->a[1]);
    wr32(m + RM_COMMADDR, c->d[0]);
    return m;
}
static uint32_t r_DeleteRexxMsg(M68K *c) { if (c->a[0]) exec_freemem(c->a[0], RM_SIZE); return 0; }

static uint32_t r_ClearRexxMsg(M68K *c) {
    uint32_t m = c->a[0], n = c->d[0] > 16 ? 16 : c->d[0];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = rd32(m + RM_ARGS + 4 * i);
        if (a) exec_freemem(a - 8, rd32(a - 8));
        wr32(m + RM_ARGS + 4 * i, 0);
    }
    return 0;
}

/* FillRexxMsg(msg, count, mask): each argument a string, or a number when its mask bit is set */
static uint32_t r_FillRexxMsg(M68K *c) {
    uint32_t m = c->a[0], n = c->d[0] > 16 ? 16 : c->d[0], mask = c->d[1];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = rd32(m + RM_ARGS + 4 * i), a;
        if (mask & (1u << i)) {
            char b[16];
            snprintf(b, sizeof b, "%d", (int32_t)v);
            a = make_argstring(0, (uint32_t)strlen(b), b);
        } else {
            uint32_t len = 0;
            if (v) while (rd8(v + len) && len < 65535) len++;
            a = make_argstring(v, len, 0);
        }
        if (!a) return 0;
        wr32(m + RM_ARGS + 4 * i, a);
    }
    return DOSTRUE;
}

static uint32_t r_IsRexxMsg(M68K *c) {
    uint32_t m = c->a[0];
    if (!m || !rd32(m + LN_NAME)) return 0;
    char n[8];
    str_get(rd32(m + LN_NAME), n, sizeof n);
    return strcmp(n, "REXX") ? 0 : DOSTRUE;
}
static uint32_t r_Lock(M68K *c) { (void)c; return 0; }

/* CreateRexxHostPort(name): a public port */
static uint32_t r_CreateRexxHostPort(M68K *c) {
    uint32_t p = lib_call(c, SysBase, "exec", "CreateMsgPort");
    if (!p) return 0;
    char n[64] = "";
    if (c->a[0]) str_get(c->a[0], n, sizeof n);
    wr32(p + LN_NAME, str_alloc(n));
    wr8(p + LN_PRI, 0);
    lib_call(c, SysBase, "exec", "AddPort", p);
    return p;
}
static uint32_t r_DeleteRexxHostPort(M68K *c) {
    if (!c->a[0]) return 0;
    lib_call(c, SysBase, "exec", "RemPort", c->a[0]);
    lib_call(c, SysBase, "exec", "DeleteMsgPort", c->a[0]);
    return 0;
}
static uint32_t r_FreeRexxMsg(M68K *c) { c->d[0] = 16; r_ClearRexxMsg(c); return r_DeleteRexxMsg(c); }
static uint32_t r_NoVar(M68K *c) { (void)c; return 3; }       /* ERR10_003: no symbol table */
static uint32_t r_Launch(M68K *c) { (void)c; return 0; }

static const NativeEntry rexx_natives[] = {
    { "CreateArgstring", r_CreateArgstring }, { "DeleteArgstring", r_DeleteArgstring },
    { "LengthArgstring", r_LengthArgstring }, { "CreateRexxMsg", r_CreateRexxMsg },
    { "DeleteRexxMsg", r_DeleteRexxMsg }, { "ClearRexxMsg", r_ClearRexxMsg }, { "FillRexxMsg", r_FillRexxMsg },
    { "IsRexxMsg", r_IsRexxMsg }, { "LockRexxBase", r_Lock }, { "UnlockRexxBase", r_Lock },
    { "CreateRexxHostPort", r_CreateRexxHostPort }, { "DeleteRexxHostPort", r_DeleteRexxHostPort },
    { "GetRexxVarFromMsg", r_NoVar }, { "SetRexxVarFromMsg", r_NoVar }, { "LaunchRexxScript", r_Launch },
    { "FreeRexxMsg", r_FreeRexxMsg }, { "GetRexxBufferFromMsg", r_NoVar },
    { 0, 0 }
};

void rexxsyslib_init(void) {
    rexx_name = str_alloc("REXX");
    RexxSysBase = lib_create("rexxsyslib.library", "rexxsyslib", 128, rexx_natives, 45, 1, "rexxsyslib 45.1 (AmigaMac)");
    lib_add(RexxSysBase);
}
