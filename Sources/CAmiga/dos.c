/* dos.library, native, on top of the macOS file system.
 *
 * Amiga volumes and assigns map to host directories (SYS: is the AmigaMac
 * system folder, Mac: the whole host disk). Locks and file handles are real
 * Amiga structures whose fl_Key / fh_Args index native tables.
 * Register conventions follow dos_lib.fd.
 */
#include "os.h"
#include "console.h"
#include "sched.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

uint32_t DOSBase;
static int next_tasknum = 1;

/* names the Amiga does not see: ".", "..", the Mac's hidden files (and the RAM disk's
   folder); the few Amiga files whose name starts with a dot stay */
int dos_hidden_name(const char *n) {
    return n[0] == '.' && strcmp(n, ".info") && strcmp(n, ".fastdir") && strcmp(n, ".backdrop");
}

/* ---- assigns ------------------------------------------------------------ */

typedef struct { char name[32]; char path[1024]; } Assign;
static Assign assigns[64];
static int nassigns;
static int dol_dirty = 1;                  /* the DosList must follow the assigns */
static char sys_dir[1024], ram_dir[1024];

static void assign_set(const char *name, const char *path) {
    dol_dirty = 1;
    for (int i = 0; i < nassigns; i++)
        if (!strcasecmp(assigns[i].name, name)) { snprintf(assigns[i].path, sizeof assigns[i].path, "%s", path); return; }
    if (nassigns == 64) return;
    snprintf(assigns[nassigns].name, sizeof assigns[nassigns].name, "%s", name);
    snprintf(assigns[nassigns].path, sizeof assigns[nassigns].path, "%s", path);
    nassigns++;
}

/* volumes mounted from disk images: their device and their name */
typedef struct { char device[8], volume[64], path[1024]; } Mount;
static Mount mounts[4];

static void assign_del(const char *name) {
    for (int i = 0; i < nassigns; i++)
        if (!strcasecmp(assigns[i].name, name)) { assigns[i] = assigns[--nassigns]; dol_dirty = 1; return; }
}

static const char *assign_get(const char *name) {
    for (int i = 0; i < nassigns; i++)
        if (!strcasecmp(assigns[i].name, name)) return assigns[i].path;
    return 0;
}

/* ---- errors ------------------------------------------------------------- */

int dos_set_ioerr(int err) {
    uint32_t t = this_task();
    if (t) wr32(t + PR_RESULT2, (uint32_t)err);
    return err;
}

static int host_err(void) {
    switch (errno) {
    case ENOENT: return ERROR_OBJECT_NOT_FOUND;
    case EEXIST: return ERROR_OBJECT_EXISTS;
    case EACCES: case EPERM: return ERROR_READ_PROTECTED;
    case ENOTEMPTY: return ERROR_DIRECTORY_NOT_EMPTY;
    case ENOSPC: return ERROR_DISK_FULL;
    case ENOTDIR: return ERROR_DIR_NOT_FOUND;
    case EISDIR: return ERROR_OBJECT_WRONG_TYPE;
    default: return ERROR_ACTION_NOT_KNOWN;
    }
}

const char *dos_fault_text(int err) {
    switch (err) {
    case 0: return "";
    case ERROR_NO_FREE_STORE: return "not enough memory available";
    case ERROR_BAD_TEMPLATE: return "bad template";
    case ERROR_BAD_NUMBER: return "bad number";
    case ERROR_REQUIRED_ARG_MISSING: return "required argument missing";
    case ERROR_TOO_MANY_ARGS: return "wrong number of arguments";
    case ERROR_LINE_TOO_LONG: return "argument line invalid or too long";
    case ERROR_FILE_NOT_OBJECT: return "file is not executable";
    case ERROR_OBJECT_IN_USE: return "object is in use";
    case ERROR_OBJECT_EXISTS: return "object already exists";
    case ERROR_DIR_NOT_FOUND: return "directory not found";
    case ERROR_OBJECT_NOT_FOUND: return "object not found";
    case ERROR_ACTION_NOT_KNOWN: return "packet request type unknown";
    case ERROR_INVALID_COMPONENT_NAME: return "invalid filename";
    case ERROR_OBJECT_WRONG_TYPE: return "object is of wrong type";
    case ERROR_DIRECTORY_NOT_EMPTY: return "directory not empty";
    case ERROR_DEVICE_NOT_MOUNTED: return "device (or volume) is not mounted";
    case ERROR_SEEK_ERROR: return "seek error";
    case ERROR_DISK_FULL: return "disk is full";
    case ERROR_DELETE_PROTECTED: return "file is protected from deletion";
    case ERROR_READ_PROTECTED: return "file is read protected";
    case ERROR_NO_MORE_ENTRIES: return "no more entries in directory";
    case ERROR_NOT_IMPLEMENTED: return "function not implemented";
    default: return "unknown error";
    }
}

/* ---- locks -------------------------------------------------------------- */

typedef struct { int used; char path[1024]; } HostLock;
static HostLock locks[1024];

static uint32_t lock_new(const char *path, int access) {
    int i;
    for (i = 1; i < 1024 && locks[i].used; i++) ;
    if (i == 1024) { dos_set_ioerr(ERROR_NO_FREE_STORE); return 0; }
    locks[i].used = 1;
    snprintf(locks[i].path, sizeof locks[i].path, "%s", path);
    uint32_t fl = exec_allocmem(FL_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(fl + FL_KEY, i);
    wr32(fl + FL_ACCESS, (uint32_t)access);
    wr32(fl + FL_TASK, dos_handler_port);
    return MKBADDR(fl);
}

static const char *lock_path(uint32_t bl) {
    if (!bl) return 0;
    uint32_t k = rd32(BADDR(bl) + FL_KEY);
    return k > 0 && k < 1024 && locks[k].used ? locks[k].path : 0;
}

static void lock_free(uint32_t bl) {
    if (!bl) return;
    uint32_t fl = BADDR(bl), k = rd32(fl + FL_KEY);
    if (k > 0 && k < 1024) locks[k].used = 0;
    exec_freemem(fl, FL_SIZE);
}

/* ---- path resolution ---------------------------------------------------- */

static void current_dir(char *out, int max) {
    uint32_t t = this_task();
    const char *p = t ? lock_path(rd32(t + PR_CURRENTDIR)) : 0;
    snprintf(out, max, "%s", p ? p : sys_dir);
}

/* ---- names: the Amiga side speaks ISO-8859-1, macOS file names are UTF-8 ---- */

static int valid_utf8_multibyte(const char *s) {
    int multi = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x80) continue;
        int n = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return 0;
        for (int i = 1; i <= n; i++) if ((p[i] & 0xC0) != 0x80) return 0;
        p += n;
        multi = 1;
    }
    return multi;
}

/* an Amiga (Latin-1) name to UTF-8; names that already are UTF-8 (native callers) pass */
static void name_to_host(const char *in, char *out, int max) {
    if (valid_utf8_multibyte(in)) { snprintf(out, (size_t)max, "%s", in); return; }
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k < max - 3; p++) {
        if (*p < 0x80) out[k++] = (char)*p;
        else { out[k++] = (char)(0xC0 | (*p >> 6)); out[k++] = (char)(0x80 | (*p & 0x3F)); }
    }
    out[k] = 0;
}

/* a host (UTF-8, possibly decomposed) name to Latin-1 */
static void name_to_amiga(const char *in, char *out, int max) {
    static const struct { char base; unsigned mark; unsigned char res; } comp[] = {
        { 'a', 0x300, 0xE0 }, { 'a', 0x301, 0xE1 }, { 'a', 0x302, 0xE2 }, { 'a', 0x303, 0xE3 }, { 'a', 0x308, 0xE4 },
        { 'c', 0x327, 0xE7 }, { 'e', 0x300, 0xE8 }, { 'e', 0x301, 0xE9 }, { 'e', 0x302, 0xEA }, { 'e', 0x308, 0xEB },
        { 'i', 0x300, 0xEC }, { 'i', 0x301, 0xED }, { 'i', 0x302, 0xEE }, { 'i', 0x308, 0xEF }, { 'n', 0x303, 0xF1 },
        { 'o', 0x300, 0xF2 }, { 'o', 0x301, 0xF3 }, { 'o', 0x302, 0xF4 }, { 'o', 0x303, 0xF5 }, { 'o', 0x308, 0xF6 },
        { 'u', 0x300, 0xF9 }, { 'u', 0x301, 0xFA }, { 'u', 0x302, 0xFB }, { 'u', 0x308, 0xFC }, { 'y', 0x301, 0xFD },
        { 'y', 0x308, 0xFF },
        { 'A', 0x300, 0xC0 }, { 'A', 0x301, 0xC1 }, { 'A', 0x302, 0xC2 }, { 'A', 0x303, 0xC3 }, { 'A', 0x308, 0xC4 },
        { 'C', 0x327, 0xC7 }, { 'E', 0x300, 0xC8 }, { 'E', 0x301, 0xC9 }, { 'E', 0x302, 0xCA }, { 'E', 0x308, 0xCB },
        { 'I', 0x301, 0xCD }, { 'I', 0x302, 0xCE }, { 'I', 0x308, 0xCF }, { 'N', 0x303, 0xD1 }, { 'O', 0x301, 0xD3 },
        { 'O', 0x302, 0xD4 }, { 'O', 0x308, 0xD6 }, { 'U', 0x300, 0xD9 }, { 'U', 0x301, 0xDA }, { 'U', 0x302, 0xDB },
        { 'U', 0x308, 0xDC },
    };
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k < max - 1; p++) {
        unsigned cp = *p;
        int n = 0;
        if (cp >= 0xC0 && cp < 0xE0) { cp &= 0x1F; n = 1; }
        else if (cp >= 0xE0 && cp < 0xF0) { cp &= 0x0F; n = 2; }
        else if (cp >= 0xF0) { cp &= 0x07; n = 3; }
        else if (cp >= 0x80) { out[k++] = '?'; continue; }
        for (int i = 0; i < n && (p[1] & 0xC0) == 0x80; i++) cp = cp << 6 | (*++p & 0x3F);
        if (cp >= 0x300 && cp <= 0x36F) {                  /* combining mark: compose with the base */
            for (unsigned i = 0; i < sizeof comp / sizeof *comp; i++)
                if (k && out[k - 1] == comp[i].base && comp[i].mark == cp) { out[k - 1] = (char)comp[i].res; break; }
            continue;
        }
        out[k++] = cp < 256 ? (char)cp : '?';
    }
    out[k] = 0;
}

/* host path of an Amiga path, relative to the current directory */
int dos_resolve(const char *apath_in, char *out, int max) {
    char apath[2048];
    name_to_host(apath_in, apath, sizeof apath);
    char base[1024], rest[1024];
    const char *colon = strchr(apath, ':');
    if (colon) {
        char vol[64];
        int n = (int)(colon - apath);
        if (n >= (int)sizeof vol) return dos_set_ioerr(ERROR_INVALID_COMPONENT_NAME), 0;
        memcpy(vol, apath, n);
        vol[n] = 0;
        if (n == 0) {                                   /* ":" = root of the current volume */
            snprintf(base, sizeof base, "%s", sys_dir);
        } else if (!strcasecmp(vol, "PROGDIR")) {        /* the running program's directory */
            uint32_t t = this_task();
            const char *hd = t && rd8(t + LN_TYPE) == NT_PROCESS ? lock_path(rd32(t + PR_HOMEDIR)) : 0;
            if (hd) snprintf(base, sizeof base, "%s", hd); else current_dir(base, sizeof base);
        } else {
            const char *p = assign_get(vol);
            if (!p) return dos_set_ioerr(ERROR_DEVICE_NOT_MOUNTED), 0;
            snprintf(base, sizeof base, "%s", p);
        }
        snprintf(rest, sizeof rest, "%s", colon + 1);
    } else {
        current_dir(base, sizeof base);
        snprintf(rest, sizeof rest, "%s", apath);
    }
    /* walk components: an empty component (leading or doubled '/') is the parent */
    char path[2048];
    snprintf(path, sizeof path, "%s", base);
    char *s = rest;
    while (*s) {
        char *slash = strchr(s, '/');
        size_t len = slash ? (size_t)(slash - s) : strlen(s);
        if (len == 0) {
            char *last = strrchr(path, '/');
            if (last && last != path) *last = 0;
        } else {
            size_t pl = strlen(path);
            if (pl + 1 + len + 1 >= sizeof path) return dos_set_ioerr(ERROR_LINE_TOO_LONG), 0;
            if (pl && path[pl - 1] != '/') path[pl++] = '/';
            memcpy(path + pl, s, len);
            path[pl + len] = 0;
        }
        if (!slash) break;
        s = slash + 1;
    }
    snprintf(out, max, "%s", path);
    return 1;
}

/* Amiga name of a host path (for NameFromLock): prefer the longest assign */
static void amiga_name(const char *hpath, char *out, int max) {
    int best = -1;
    size_t bestlen = 0;
    static const char *prefer[] = { "SYS", "RAM", "Mac" };
    for (int k = 0; k < 3; k++) {
        const char *p = assign_get(prefer[k]);
        size_t l = p ? strlen(p) : 0;
        if (p && !strncmp(hpath, p, l) && (hpath[l] == 0 || hpath[l] == '/') && l > bestlen) {
            for (int i = 0; i < nassigns; i++) if (!strcasecmp(assigns[i].name, prefer[k])) best = i;
            bestlen = l;
        }
    }
    for (int i = 0; i < 4; i++) {                               /* a disk: its name */
        size_t l = strlen(mounts[i].path);
        if (mounts[i].device[0] && !strncmp(hpath, mounts[i].path, l) && (hpath[l] == 0 || hpath[l] == '/')) {
            snprintf(out, max, "%s:%s", mounts[i].volume, hpath[l] ? hpath + l + 1 : "");
            return;
        }
    }
    if (best < 0) { snprintf(out, max, "Mac:%s", hpath + 1); return; }
    const char *tail = hpath + bestlen;
    if (*tail == '/') tail++;
    snprintf(out, max, "%s:%s", assigns[best].name, tail);
}

/* ---- file handles ------------------------------------------------------- */

extern uint32_t dos_handler_port;
typedef struct { int used; FILE *f; int console; int ungot; Con *con; char *path; int written; } HostFile;   /* console: 1 host Shell, 2 window */
static HostFile files[256];

static uint32_t fh_new(FILE *f, int console) {
    int i;
    for (i = 1; i < 256 && files[i].used; i++) ;
    if (i == 256) return 0;
    files[i].used = 1;
    files[i].f = f;
    files[i].console = console;
    files[i].ungot = -1;
    files[i].con = 0;
    files[i].written = 0;
    free(files[i].path);
    files[i].path = 0;
    uint32_t fh = exec_allocmem(FH_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(fh + FH_ARGS, i);
    wr32(fh + FH_TYPE, dos_handler_port);                 /* packets go to the native handler */
    wr32(fh + FH_PORT, console ? 0xFFFFFFFF : 0);   /* non-zero: interactive */
    return MKBADDR(fh);
}

static HostFile *fh_get(uint32_t bfh) {
    if (!bfh) return 0;
    uint32_t k = rd32(BADDR(bfh) + FH_ARGS);
    return k > 0 && k < 256 && files[k].used ? &files[k] : 0;
}

static uint32_t fh_new_con(Con *k) {
    uint32_t fh = fh_new(0, 2);
    if (fh) fh_get(fh)->con = k;
    return fh;
}

static void fh_close(uint32_t bfh) {
    HostFile *h = fh_get(bfh);
    if (!h) return;
    /* IPrefs' job: new screen mode preferences take effect when written */
    int prefs = 0;
    if (h->path && h->f && !h->console && h->written) {
        char env[1100];
        snprintf(env, sizeof env, "%s/Env/Sys/screenmode.prefs", ram_dir);
        prefs = !strcasecmp(h->path, env);
    }
    if (h->con) { NTask *me = sched_current(); if (me) con_close(&me->cpu, h->con); h->con = 0; }
    if (h->f && !h->console) fclose(h->f);
    h->used = 0;
    exec_freemem(BADDR(bfh), FH_SIZE);
    if (prefs) { extern void intuition_screenmode_changed(void); intuition_screenmode_changed(); }
}

/* console input comes from the host as an event queue; a reader only
   blocks its own task */
static int host_con_read(char *buf, int n) {
    NTask *me = sched_current();
    if (me) sched_set_console_owner(me->task);
    return sched_console_read(buf, n);
}

static int32_t hf_read(HostFile *h, uint32_t buf, int32_t len);
static int32_t fh_read(uint32_t bfh, uint32_t buf, int32_t len) { return hf_read(fh_get(bfh), buf, len); }
static int32_t hf_read(HostFile *h, uint32_t buf, int32_t len) {
    if (!h) return -1;
    if (len <= 0) return 0;
    char *dst = mem_ptr(buf, len);
    if (!dst) return -1;
    int32_t got = 0;
    if (h->ungot >= 0) { dst[got++] = (char)h->ungot; h->ungot = -1; }
    if (got == len) return got;
    if (h->con) return got + con_read(h->con, dst + got, len - got);
    if (h->console) return got + host_con_read(dst + got, len - got);
    return got + (int32_t)fread(dst + got, 1, len - got, h->f);
}

static int32_t hf_write(HostFile *h, uint32_t buf, int32_t len);
static int32_t fh_write(uint32_t bfh, uint32_t buf, int32_t len) { return hf_write(fh_get(bfh), buf, len); }
static int32_t hf_write(HostFile *h, uint32_t buf, int32_t len) {
    if (!h) return -1;
    if (len <= 0) return 0;
    const char *src = mem_ptr(buf, len);
    if (!src) return -1;
    if (h->con) { con_write(h->con, src, len); return len; }
    if (h->console) { if (host.console_write) host.console_write(src, len); return len; }
    h->written = 1;
    return (int32_t)fwrite(src, 1, len, h->f);
}

static void fh_write_str(uint32_t bfh, const char *s, int len) {
    HostFile *h = fh_get(bfh);
    if (!h) return;
    if (h->con) con_write(h->con, s, len);
    else if (h->console) { if (host.console_write) host.console_write(s, len); }
    else { h->written = 1; fwrite(s, 1, len, h->f); }
}

static uint32_t cur_out(void) { uint32_t t = this_task(); return t ? rd32(t + PR_COS) : 0; }
static uint32_t cur_in(void) { uint32_t t = this_task(); return t ? rd32(t + PR_CIS) : 0; }

/* ---- dos functions: files ----------------------------------------------- */

static uint32_t f_Open(M68K *c) {
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    int mode = (int)c->d[2];
    if (!strcasecmp(name, "*") || !strcasecmp(name, "CONSOLE:")) {
        /* the process's own console: a window if its output is one */
        HostFile *o = fh_get(cur_out());
        if (o && o->con) { con_ref(o->con); return fh_new_con(o->con); }
        return fh_new(0, 1);
    }
    if (!strncasecmp(name, "NIL:", 4)) {
        FILE *f = fopen("/dev/null", mode == MODE_OLDFILE ? "r" : "w");
        return fh_new(f, 0);
    }
    if (!strncasecmp(name, "CON:", 4) || !strncasecmp(name, "RAW:", 4)) {
        Con *k = con_open(c, name + 4, toupper((unsigned char)name[0]) == 'R');
        if (!k) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
        return fh_new_con(k);
    }
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    const char *fm = mode == MODE_NEWFILE ? "w+b" : "r+b";
    FILE *f = fopen(hp, fm);
    if (!f && mode == MODE_OLDFILE) f = fopen(hp, "rb");
    if (!f && mode == MODE_READWRITE) f = fopen(hp, "w+b");
    if (!f) { dos_set_ioerr(host_err()); return 0; }
    uint32_t fh = fh_new(f, 0);
    if (!fh) { fclose(f); dos_set_ioerr(ERROR_NO_FREE_STORE); }
    else fh_get(fh)->path = strdup(hp);
    return fh;
}

static uint32_t f_Close(M68K *c) { fh_close(c->d[1]); return DOSTRUE; }
static uint32_t f_Read(M68K *c) { return (uint32_t)fh_read(c->d[1], c->d[2], (int32_t)c->d[3]); }
static uint32_t f_Write(M68K *c) { return (uint32_t)fh_write(c->d[1], c->d[2], (int32_t)c->d[3]); }

static uint32_t f_Seek(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h || h->console) { dos_set_ioerr(ERROR_SEEK_ERROR); return 0xFFFFFFFF; }
    long old = ftell(h->f);
    int whence = (int32_t)c->d[3] == OFFSET_BEGINNING ? SEEK_SET : (int32_t)c->d[3] == OFFSET_END ? SEEK_END : SEEK_CUR;
    if (fseek(h->f, (int32_t)c->d[2], whence)) { dos_set_ioerr(ERROR_SEEK_ERROR); return 0xFFFFFFFF; }
    h->ungot = -1;
    return (uint32_t)old;
}

static uint32_t f_Input(M68K *c) { (void)c; return cur_in(); }
static uint32_t f_Output(M68K *c) { (void)c; return cur_out(); }
static uint32_t f_SelectInput(M68K *c) { uint32_t o = cur_in(); wr32(this_task() + PR_CIS, c->d[1]); return o; }
static uint32_t f_SelectOutput(M68K *c) { uint32_t o = cur_out(); wr32(this_task() + PR_COS, c->d[1]); return o; }
static uint32_t f_IsInteractive(M68K *c) { HostFile *h = fh_get(c->d[1]); return h && h->console ? DOSTRUE : 0; }
static uint32_t f_Flush(M68K *c) { HostFile *h = fh_get(c->d[1]); if (h && h->f) fflush(h->f); return DOSTRUE; }

static uint32_t f_FGetC(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h) return 0xFFFFFFFF;
    if (h->ungot >= 0) { int ch = h->ungot; h->ungot = -1; return (uint32_t)ch; }
    char ch;
    int n = h->con ? con_read(h->con, &ch, 1) : h->console ? host_con_read(&ch, 1) : (int)fread(&ch, 1, 1, h->f);
    return n == 1 ? (uint8_t)ch : 0xFFFFFFFF;
}
static uint32_t f_UnGetC(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h) return 0;
    h->ungot = (int32_t)c->d[2] == -1 ? -1 : (int)(c->d[2] & 0xFF);
    return DOSTRUE;
}
static uint32_t f_FPutC(M68K *c) {
    char ch = (char)c->d[2];
    fh_write_str(c->d[1], &ch, 1);
    return c->d[2] & 0xFF;
}
static uint32_t f_FPuts(M68K *c) {
    char buf[4096];
    int n = str_get(c->d[2], buf, sizeof buf);
    fh_write_str(c->d[1], buf, n);
    return 0;
}
static uint32_t f_PutStr(M68K *c) {
    char buf[4096];
    int n = str_get(c->d[1], buf, sizeof buf);
    fh_write_str(cur_out(), buf, n);
    return 0;
}
static uint32_t f_WriteChars(M68K *c) { return (uint32_t)fh_write(cur_out(), c->d[1], (int32_t)c->d[2]); }
static uint32_t f_FGets(M68K *c) {
    uint32_t buf = c->d[2];
    int32_t max = (int32_t)c->d[3], i = 0;
    if (max <= 0) return 0;
    while (i < max - 1) {
        c->d[1] = c->d[1];
        uint32_t ch = f_FGetC(c);
        if (ch == 0xFFFFFFFF) break;
        wr8(buf + i++, ch);
        if (ch == '\n') break;
    }
    wr8(buf + i, 0);
    return i ? buf : 0;
}
static uint32_t f_FRead(M68K *c) {
    int32_t n = fh_read(c->d[1], c->d[2], (int32_t)(c->d[3] * c->d[4]));
    return n < 0 || !c->d[3] ? 0 : (uint32_t)n / c->d[3];
}
static uint32_t f_FWrite(M68K *c) {
    int32_t n = fh_write(c->d[1], c->d[2], (int32_t)(c->d[3] * c->d[4]));
    return n < 0 || !c->d[3] ? 0 : (uint32_t)n / c->d[3];
}

/* formatted output goes through exec's RawDoFmt into a host buffer */
static int dos_format(M68K *c, uint32_t fmt, uint32_t argv, char *out, int max) {
    /* putch: move.b d0,(a3)+ ; rts  in a scratch buffer */
    static uint32_t scratch, putch_code;
    if (!scratch) {
        scratch = exec_allocmem(8192, MEMF_PUBLIC);
        putch_code = exec_allocmem(4, MEMF_PUBLIC);
        wr16(putch_code, 0x16C0);
        wr16(putch_code + 2, 0x4E75);
    }
    uint32_t sa[8], sd[8];
    memcpy(sa, c->a, sizeof sa);
    memcpy(sd, c->d, sizeof sd);
    c->a[0] = fmt; c->a[1] = argv; c->a[2] = putch_code; c->a[3] = scratch;
    uint32_t rawdofmt = SysBase - 522;
    cpu_call(c, rawdofmt);
    memcpy(c->a, sa, sizeof sa);
    memcpy(c->d, sd, sizeof sd);
    return str_get(scratch, out, max);
}

static uint32_t f_VPrintf(M68K *c) {
    char buf[8192];
    int n = dos_format(c, c->d[1], c->d[2], buf, sizeof buf);
    fh_write_str(cur_out(), buf, n);
    return (uint32_t)n;
}
static uint32_t f_VFPrintf(M68K *c) {
    char buf[8192];
    uint32_t fh = c->d[1];
    int n = dos_format(c, c->d[2], c->d[3], buf, sizeof buf);
    fh_write_str(fh, buf, n);
    return (uint32_t)n;
}
/* VFWritef(fh, format, argv): the BCPL formats %S %Tx %C %Ox %Xx %Ix %N %Ux %$,
   x a field width 0-9 or A-Z, each argument a longword */
static uint32_t f_VFWritef(M68K *c) {
    char fmt[1024], out[8192];
    str_get(c->d[2], fmt, sizeof fmt);
    uint32_t argv = c->d[3];
    int k = 0;
    #define PUT(ch) do { if (k < (int)sizeof out - 1) out[k++] = (char)(ch); } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) { PUT(*p); continue; }
        char f = (char)toupper((unsigned char)*++p);
        int w = -1;
        if (strchr("TOXIU", f) && p[1]) {
            char x = (char)toupper((unsigned char)*++p);
            w = isdigit((unsigned char)x) ? x - '0' : (x >= 'A' && x <= 'Z') ? x - 'A' + 10 : 0;
        }
        char tmp[300];
        uint32_t v = 0;
        if (f != '%') { v = rd32(argv); argv += 4; }
        switch (f) {
        case 'S': if (v) { str_get(v, tmp, sizeof tmp); for (char *q = tmp; *q; q++) PUT(*q); } break;
        case 'T': {
            int n = v ? str_get(v, tmp, sizeof tmp) : 0;
            for (int i = 0; i < n; i++) PUT(tmp[i]);
            for (int i = n; i < w; i++) PUT(' ');
            break;
        }
        case 'C': PUT(v & 0xFF); break;
        case 'O': snprintf(tmp, sizeof tmp, "%*o", w, v); for (char *q = tmp; *q; q++) PUT(*q); break;
        case 'X': snprintf(tmp, sizeof tmp, "%0*X", w, v); for (char *q = tmp; *q; q++) PUT(*q); break;
        case 'I': snprintf(tmp, sizeof tmp, "%*d", w, (int32_t)v); for (char *q = tmp; *q; q++) PUT(*q); break;
        case 'U': snprintf(tmp, sizeof tmp, "%*u", w, v); for (char *q = tmp; *q; q++) PUT(*q); break;
        case 'N': snprintf(tmp, sizeof tmp, "%d", (int32_t)v); for (char *q = tmp; *q; q++) PUT(*q); break;
        case '$': break;
        case '%': PUT('%'); break;
        default: PUT('%'); PUT(f); break;
        }
    }
    #undef PUT
    fh_write_str(c->d[1], out, k);
    return (uint32_t)k;
}

/* ---- dos functions: locks and directories --------------------------------- */

static uint32_t lock_by_name(const char *name, int access) {
    char hp[1024];
    struct stat st;
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    if (stat(hp, &st)) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
    return lock_new(hp, access);
}

static uint32_t f_Lock(M68K *c) {
    char name[512];
    str_get(c->d[1], name, sizeof name);
    return lock_by_name(name, (int)c->d[2]);
}
static uint32_t f_UnLock(M68K *c) { lock_free(c->d[1]); return 0; }
static uint32_t f_DupLock(M68K *c) {
    const char *p = lock_path(c->d[1]);
    return p ? lock_new(p, SHARED_LOCK) : 0;            /* DupLock(NULL) is NULL */
}
static uint32_t f_CurrentDir(M68K *c) {
    uint32_t t = this_task(), old = rd32(t + PR_CURRENTDIR);
    wr32(t + PR_CURRENTDIR, c->d[1]);
    return old;
}
static uint32_t f_ParentDir(M68K *c) {
    const char *p = lock_path(c->d[1]);
    char buf[1024];
    if (!p) return 0;
    snprintf(buf, sizeof buf, "%s", p);
    /* the root of a volume has no parent */
    for (int i = 0; i < nassigns; i++) if (!strcmp(assigns[i].path, buf)) return 0;
    char *last = strrchr(buf, '/');
    if (!last || last == buf) return 0;
    *last = 0;
    return lock_new(buf, SHARED_LOCK);
}

/* DOS date: days since 1 Jan 1978, minutes, ticks */
static void unix_to_datestamp(time_t t, uint32_t ds) {
    struct tm lt;
    localtime_r(&t, &lt);
    time_t local = t + lt.tm_gmtoff;
    long secs = (long)local - 252460800L;              /* 1978-01-01 */
    if (secs < 0) secs = 0;
    wr32(ds + DS_DAYS, (uint32_t)(secs / 86400));
    wr32(ds + DS_MINUTE, (uint32_t)((secs % 86400) / 60));
    wr32(ds + DS_TICK, (uint32_t)((secs % 60) * 50));
}

static int fill_fib(const char *hp, const char *name, uint32_t fib) {
    struct stat st;
    if (stat(hp, &st)) return 0;
    int isdir = S_ISDIR(st.st_mode);
    for (int i = 0; i < FIB_SIZE; i++) wr8(fib + i, 0);
    wr32(fib + FIB_DIRENTRYTYPE, isdir ? ST_USERDIR : (uint32_t)ST_FILE);
    wr32(fib + FIB_ENTRYTYPE, isdir ? ST_USERDIR : (uint32_t)ST_FILE);
    char l1[256];
    name_to_amiga(name, l1, sizeof l1);
    int n = (int)strlen(l1);
    if (n > 106) n = 106;
    for (int i = 0; i < n; i++) wr8(fib + FIB_FILENAME + i, (uint8_t)l1[i]);
    uint32_t prot = 0;                                  /* bits set = forbidden (RWED inverted) */
    if (!(st.st_mode & S_IRUSR)) prot |= 8;
    if (!(st.st_mode & S_IWUSR)) prot |= 4 | 1;
    uint8_t xb[4];
    if (getxattr(hp, "com.amigamac.protection", xb, 4, 0, 0) == 4)
        prot = (uint32_t)xb[0] << 24 | (uint32_t)xb[1] << 16 | (uint32_t)xb[2] << 8 | xb[3];
    wr32(fib + FIB_PROTECTION, prot);
    char com[80];
    ssize_t cl = getxattr(hp, "com.amigamac.comment", com, sizeof com - 1, 0, 0);
    if (cl > 0) for (ssize_t i = 0; i < cl; i++) wr8(fib + FIB_COMMENT + i, (uint8_t)com[i]);
    wr32(fib + FIB_SIZE_, isdir ? 0 : (uint32_t)st.st_size);
    wr32(fib + FIB_NUMBLOCKS, (uint32_t)((st.st_size + 511) / 512));
    unix_to_datestamp(st.st_mtime, fib + FIB_DATE);
    return 1;
}

/* directory scanning state lives natively, keyed by lock */
typedef struct { uint32_t lock; DIR *d; } Scan;
static Scan scans[64];

static uint32_t f_Examine(M68K *c) {
    const char *p = lock_path(c->d[1]);
    if (!p) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
    const char *name = strrchr(p, '/');
    name = name ? name + 1 : p;
    for (int i = 0; i < nassigns; i++)                   /* volume root: its name */
        if (!strcmp(assigns[i].path, p)) name = assigns[i].name;
    if (!fill_fib(p, name, c->d[2])) { dos_set_ioerr(host_err()); return 0; }
    for (int i = 0; i < 64; i++)
        if (scans[i].lock == c->d[1] && scans[i].d) { closedir(scans[i].d); scans[i].d = 0; scans[i].lock = 0; }
    return DOSTRUE;
}

static uint32_t f_ExNext(M68K *c) {
    const char *p = lock_path(c->d[1]);
    if (!p) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
    Scan *s = 0;
    for (int i = 0; i < 64; i++) if (scans[i].lock == c->d[1] && scans[i].d) s = &scans[i];
    if (!s) {
        for (int i = 0; i < 64; i++) if (!scans[i].d) { s = &scans[i]; break; }
        if (!s) { dos_set_ioerr(ERROR_NO_FREE_STORE); return 0; }
        s->d = opendir(p);
        s->lock = c->d[1];
        if (!s->d) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    }
    struct dirent *e;
    while ((e = readdir(s->d))) {
        if (dos_hidden_name(e->d_name)) continue;       /* ., .., the Mac's hidden files */
        char hp[2048];
        snprintf(hp, sizeof hp, "%s/%s", p, e->d_name);
        if (fill_fib(hp, e->d_name, c->d[2])) return DOSTRUE;
    }
    closedir(s->d);
    s->d = 0;
    s->lock = 0;
    dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
    return 0;
}

static uint32_t f_ExamineFH(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h || h->console) return 0;
    struct stat st;
    if (fstat(fileno(h->f), &st)) return 0;
    for (int i = 0; i < FIB_SIZE; i++) wr8(c->d[2] + i, 0);
    wr32(c->d[2] + FIB_DIRENTRYTYPE, (uint32_t)ST_FILE);
    wr32(c->d[2] + FIB_ENTRYTYPE, (uint32_t)ST_FILE);
    wr32(c->d[2] + FIB_SIZE_, (uint32_t)st.st_size);
    unix_to_datestamp(st.st_mtime, c->d[2] + FIB_DATE);
    return DOSTRUE;
}

static uint32_t f_NameFromLock(M68K *c) {
    const char *p = lock_path(c->d[1]);
    char an[1024];
    if (!c->d[1]) snprintf(an, sizeof an, "SYS:");
    else if (!p) return 0;
    else dos_amiga_name(p, an, sizeof an);
    uint32_t len = c->d[3];
    if (strlen(an) + 1 > len) { dos_set_ioerr(ERROR_LINE_TOO_LONG); return 0; }
    for (size_t i = 0; i <= strlen(an); i++) wr8(c->d[2] + i, (uint8_t)an[i]);
    return DOSTRUE;
}

static uint32_t f_CreateDir(M68K *c) {
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    if (mkdir(hp, 0755)) { dos_set_ioerr(host_err()); return 0; }
    return lock_new(hp, EXCLUSIVE_LOCK);
}
static uint32_t f_DeleteFile(M68K *c) {
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    if (remove(hp)) { dos_set_ioerr(host_err()); return 0; }
    return DOSTRUE;
}
static uint32_t f_Rename(M68K *c) {
    char a[512], b[512], ha[1024], hb[1024];
    str_get(c->d[1], a, sizeof a);
    str_get(c->d[2], b, sizeof b);
    if (!dos_resolve(a, ha, sizeof ha) || !dos_resolve(b, hb, sizeof hb)) return 0;
    if (rename(ha, hb)) { dos_set_ioerr(host_err()); return 0; }
    return DOSTRUE;
}

static uint32_t f_Info(M68K *c) {
    /* InfoData: id_NumSoftErrors, id_UnitNumber, id_DiskState, id_NumBlocks,
       id_NumBlocksUsed, id_BytesPerBlock, id_DiskType, id_VolumeNode, id_InUse */
    uint32_t id = c->d[2];
    for (int i = 0; i < 36; i++) wr8(id + i, 0);
    wr32(id + 8, 82);                                    /* ID_VALIDATED */
    wr32(id + 12, 1 << 22);
    wr32(id + 16, 1 << 20);
    wr32(id + 20, 512);
    wr32(id + 24, 0x444F5301);                           /* 'DOS\1' */
    extern int adf_unit_of_path(const char *hp);
    extern int adf_unit_info(int unit, char *label, int max, int *used, int *total);
    const char *hp = c->d[1] ? lock_path(c->d[1]) : 0;
    int unit = hp ? adf_unit_of_path(hp) : -1, used, total;
    if (unit >= 0 && adf_unit_info(unit, 0, 0, &used, &total)) {   /* a disk in a drive: its own blocks */
        wr32(id + 4, (uint32_t)unit);
        wr32(id + 12, (uint32_t)total);
        wr32(id + 16, (uint32_t)used);
        wr32(id + 24, 0x444F5300);                       /* 'DOS\0' */
    }
    return DOSTRUE;
}

static uint32_t f_IoErr(M68K *c) { (void)c; return rd32(this_task() + PR_RESULT2); }
static uint32_t f_SetIoErr(M68K *c) {
    uint32_t old = rd32(this_task() + PR_RESULT2);
    wr32(this_task() + PR_RESULT2, c->d[1]);
    return old;
}

static uint32_t f_Fault(M68K *c) {
    char hdr[256], msg[512];
    str_get(c->d[2], hdr, sizeof hdr);
    const char *t = dos_fault_text((int)c->d[1]);
    int n = hdr[0] ? snprintf(msg, sizeof msg, "%s: %s", hdr, t) : snprintf(msg, sizeof msg, "%s", t);
    int32_t len = (int32_t)c->d[4];
    if (n >= len) n = len - 1;
    for (int i = 0; i < n; i++) wr8(c->d[3] + i, (uint8_t)msg[i]);
    if (len > 0) wr8(c->d[3] + n, 0);
    return (uint32_t)n;
}
static uint32_t f_PrintFault(M68K *c) {
    char hdr[256], msg[512];
    str_get(c->d[2], hdr, sizeof hdr);
    if (!c->d[1]) return 0;
    int n = hdr[0] ? snprintf(msg, sizeof msg, "%s: %s\n", hdr, dos_fault_text((int)c->d[1]))
                   : snprintf(msg, sizeof msg, "%s\n", dos_fault_text((int)c->d[1]));
    fh_write_str(cur_out(), msg, n);
    return DOSTRUE;
}

/* Delay(ticks): 50 ticks a second, sleeping only this task */
static void dos_delay_us(uint64_t us) {
    uint32_t t = this_task();
    wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) & ~SIGF_SINGLE);
    sched_timer_add(now_us() + us, TIMER_SIGNAL, t, SIGF_SINGLE);
    exec_wait(SIGF_SINGLE);
}
static uint32_t f_Delay(M68K *c) { if ((int32_t)c->d[1] > 0) dos_delay_us((uint64_t)c->d[1] * 20000u); return 0; }
static uint32_t f_DateStamp(M68K *c) { unix_to_datestamp(time(0), c->d[1]); return c->d[1]; }

static uint32_t f_CheckSignal(M68K *c) {
    uint32_t t = this_task(), got = rd32(t + TC_SIGRECVD) & c->d[1];
    wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) & ~got);
    return got;
}
static uint32_t f_WaitForChar(M68K *c) {
    /* D2: timeout in microseconds */
    HostFile *h = fh_get(c->d[1]);
    if (!h || !h->console) return DOSTRUE;
    uint64_t end = now_us() + c->d[2];
    while (h->con ? !con_ready(h->con) : !sched_console_ready()) {
        if (now_us() >= end) return 0;
        dos_delay_us(20000);
    }
    return DOSTRUE;
}
static uint32_t f_SetMode(M68K *c) { HostFile *h = fh_get(c->d[1]); if (h && h->con) con_set_raw(h->con, c->d[2] != 0); return DOSTRUE; }

/* ---- path helpers -------------------------------------------------------- */

static uint32_t f_FilePart(M68K *c) {
    uint32_t p = c->d[1], last = p;
    if (!p) return 0;
    for (uint32_t a = p; rd8(a); a++) if (rd8(a) == '/' || rd8(a) == ':') last = a + 1;
    return last;
}
static uint32_t f_PathPart(M68K *c) {
    uint32_t p = c->d[1], last = p;
    for (uint32_t a = p; rd8(a); a++) {
        if (rd8(a) == ':') last = a + 1;
        else if (rd8(a) == '/') last = a;
    }
    return last;
}
static uint32_t f_AddPart(M68K *c) {
    char dir[1024], file[512];
    str_get(c->d[1], dir, sizeof dir);
    str_get(c->d[2], file, sizeof file);
    char out[1600];
    if (strchr(file, ':')) snprintf(out, sizeof out, "%s", file);
    else {
        size_t l = strlen(dir);
        if (l && dir[l - 1] != ':' && dir[l - 1] != '/') snprintf(out, sizeof out, "%s/%s", dir, file);
        else snprintf(out, sizeof out, "%s%s", dir, file);
    }
    if (strlen(out) + 1 > c->d[3]) { dos_set_ioerr(ERROR_LINE_TOO_LONG); return 0; }
    for (size_t i = 0; i <= strlen(out); i++) wr8(c->d[1] + i, (uint8_t)out[i]);
    return DOSTRUE;
}

static uint32_t f_StrToLong(M68K *c) {
    char buf[64];
    str_get(c->d[1], buf, sizeof buf);
    char *s = buf;
    while (*s == ' ' || *s == '\t') s++;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) { wr32(c->d[2], 0); return 0xFFFFFFFF; }
    wr32(c->d[2], (uint32_t)v);
    return (uint32_t)(end - buf);
}

/* ---- environment variables (ENV: files) ---------------------------------- */

static uint32_t f_GetVar(M68K *c) {
    char name[256], hp[1024], path[300];
    str_get(c->d[1], name, sizeof name);
    snprintf(path, sizeof path, "ENV:%s", name);
    if (!dos_resolve(path, hp, sizeof hp)) return 0xFFFFFFFF;
    FILE *f = fopen(hp, "rb");
    if (!f) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0xFFFFFFFF; }
    int32_t max = (int32_t)c->d[3];
    char buf[4096];
    int n = (int)fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    while (n && (buf[n - 1] == '\n')) n--;
    if (max <= 0) return 0xFFFFFFFF;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; i++) wr8(c->d[2] + i, (uint8_t)buf[i]);
    wr8(c->d[2] + n, 0);
    return (uint32_t)n;
}
static uint32_t f_SetVar(M68K *c) {
    char name[256], hp[1024], path[300];
    str_get(c->d[1], name, sizeof name);
    snprintf(path, sizeof path, "ENV:%s", name);
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, "wb");
    if (!f) return 0;
    int32_t n = (int32_t)c->d[3];
    if (n < 0) { char v[4096]; n = str_get(c->d[2], v, sizeof v); fwrite(v, 1, n, f); }
    else for (int32_t i = 0; i < n; i++) fputc((int)rd8(c->d[2] + i), f);
    fclose(f);
    return DOSTRUE;
}
static uint32_t f_DeleteVar(M68K *c) {
    char name[256], hp[1024], path[300];
    str_get(c->d[1], name, sizeof name);
    snprintf(path, sizeof path, "ENV:%s", name);
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    return remove(hp) ? 0 : DOSTRUE;
}
static uint32_t f_FindVar(M68K *c) { (void)c; return 0; }

/* ---- LoadSeg: hunk executables -------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t dos_loadseg(const char *hp) {
    FILE *f = fopen(hp, "rb");
    if (!f) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size + 4);
    fread(buf, 1, size, f);
    fclose(f);
    long pos = 0;
#define NEXT() (pos + 4 <= size ? be32(buf + (pos += 4) - 4) : (bad = 1, 0))
    int bad = 0;
    if (NEXT() != HUNK_HEADER) { free(buf); dos_set_ioerr(ERROR_FILE_NOT_OBJECT); return 0; }
    while (NEXT()) {                                     /* resident library names: skip */
        uint32_t n = be32(buf + pos - 4);
        pos += n * 4;
    }
    uint32_t table = NEXT(), first = NEXT(), last = NEXT();
    int nh = (int)(last - first + 1);
    if (bad || nh <= 0 || nh > 4096) { free(buf); dos_set_ioerr(ERROR_FILE_NOT_OBJECT); return 0; }
    (void)table;
    uint32_t *segs = calloc(nh, sizeof *segs), *sizes = calloc(nh, sizeof *sizes);
    for (int i = 0; i < nh; i++) {
        uint32_t s = NEXT();
        uint32_t flags = s & 0xC0000000;
        if (flags == 0xC0000000) NEXT();                 /* extended memory attributes */
        sizes[i] = (s & 0x3FFFFFFF) * 4;
        uint32_t a = exec_allocmem(sizes[i] + 8, MEMF_PUBLIC | MEMF_CLEAR);
        if (!a) { bad = 1; break; }
        wr32(a, sizes[i] + 8);
        segs[i] = a + 8;
    }
    /* chain: each segment's next-BPTR sits 4 bytes before its data */
    for (int i = 0; i < nh && !bad; i++) wr32(segs[i] - 4, i + 1 < nh ? MKBADDR(segs[i + 1] - 4) : 0);
    if (getenv("AMIGAMAC_TRACE")) for (int i = 0; i < nh; i++) os_log("LoadSeg %s : hunk %d à $%06X (%u octets)", hp, i, segs[i], sizes[i]);

    int h = -1;
    while (!bad && pos < size) {
        uint32_t type = NEXT() & 0x3FFFFFFF;
        switch (type) {
        case HUNK_CODE: case HUNK_DATA: {
            uint32_t n = NEXT() & 0x3FFFFFFF;
            h++;
            if (h >= nh || n * 4 > sizes[h] || pos + n * 4 > size) { bad = 1; break; }
            memcpy(mem + segs[h], buf + pos, n * 4);
            pos += n * 4;
            break;
        }
        case HUNK_BSS: NEXT(); h++; break;
        case HUNK_RELOC32: case HUNK_RELRELOC32:
            for (;;) {
                uint32_t n = NEXT();
                if (!n || bad) break;
                uint32_t target = NEXT();
                for (uint32_t i = 0; i < n && !bad; i++) {
                    uint32_t off = NEXT();
                    if (h < 0 || target >= (uint32_t)nh || off + 4 > sizes[h]) { bad = 1; break; }
                    uint32_t a = segs[h] + off;
                    wr32(a, rd32(a) + (type == HUNK_RELOC32 ? segs[target] : segs[target] - a));
                }
            }
            break;
        case HUNK_RELOC32SHORT: case HUNK_DREL32: {
            long start = pos;
            for (;;) {
                if (pos + 2 > size) { bad = 1; break; }
                uint32_t n = (uint32_t)buf[pos] << 8 | buf[pos + 1]; pos += 2;
                if (!n) break;
                uint32_t target = (uint32_t)buf[pos] << 8 | buf[pos + 1]; pos += 2;
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t off = (uint32_t)buf[pos] << 8 | buf[pos + 1]; pos += 2;
                    if (h < 0 || target >= (uint32_t)nh || off + 4 > sizes[h]) { bad = 1; break; }
                    uint32_t a = segs[h] + off;
                    wr32(a, rd32(a) + segs[target]);
                }
            }
            if ((pos - start) & 2) pos += 2;                 /* long-align */
            break;
        }
        case HUNK_SYMBOL:
            for (;;) { uint32_t n = NEXT(); if (!n || bad) break; pos += n * 4 + 4; }
            break;
        case HUNK_DEBUG: case HUNK_NAME: { uint32_t n = NEXT(); pos += n * 4; break; }
        case HUNK_END: break;
        case HUNK_BREAK: pos = size; break;
        default:
            os_log("LoadSeg : type de hunk $%X non géré", type);
            bad = 1;
        }
    }
#undef NEXT
    uint32_t result = 0;
    if (bad) {
        for (int i = 0; i < nh; i++) if (segs[i]) exec_freemem(segs[i] - 8, sizes[i] + 8);
        dos_set_ioerr(ERROR_FILE_NOT_OBJECT);
    } else {
        result = MKBADDR(segs[0] - 4);
    }
    free(segs);
    free(sizes);
    free(buf);
    return result;
}

void dos_unloadseg(uint32_t seg) {
    while (seg) {
        uint32_t a = BADDR(seg), next = rd32(a);
        exec_freemem(a - 4, rd32(a - 4));
        seg = next;
    }
}

static uint32_t f_LoadSeg(M68K *c) {
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    return dos_loadseg(hp);
}
static uint32_t f_UnLoadSeg(M68K *c) { dos_unloadseg(c->d[1]); return DOSTRUE; }

/* ---- ReadArgs ------------------------------------------------------------- */

/* RDArgs: CSource {Buffer, Length, CurChr}, DAList, Buffer, BufSiz, ExtHelp, Flags */
#define RDA_BUFFER 0
#define RDA_LENGTH 4
#define RDA_CURCHR 8
#define RDA_DALIST 12
#define RDA_FLAGS 28
#define RDA_SIZE 32
#define RDAF_ALLOCATED_BY_READARGS 0x80000000

typedef struct { char name[32][16]; int nnames; int a, k, s, n, m, f, t; } TItem;

static int parse_template(const char *tpl, TItem *items, int max) {
    int n = 0;
    const char *p = tpl;
    while (*p && n < max) {
        TItem *it = &items[n];
        memset(it, 0, sizeof *it);
        char word[64];
        int wl = 0;
        for (;;) {
            char ch = *p;
            if (ch == '=' || ch == '/' || ch == ',' || !ch) {
                word[wl] = 0;
                if (wl && it->nnames < 16) snprintf(it->name[it->nnames++], 32, "%s", word);
                wl = 0;
                if (ch == '=') { p++; continue; }
                break;
            }
            if (wl < 63) word[wl++] = ch;
            p++;
        }
        while (*p == '/') {
            switch (toupper((unsigned char)p[1])) {
            case 'A': it->a = 1; break;
            case 'K': it->k = 1; break;
            case 'S': it->s = 1; break;
            case 'N': it->n = 1; break;
            case 'M': it->m = 1; break;
            case 'F': it->f = 1; break;
            case 'T': it->t = 1; break;
            }
            p += 2;
        }
        if (*p == ',') p++;
        n++;
    }
    return n;
}

static int match_item(TItem *items, int n, const char *word) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < items[i].nnames; j++)
            if (!strcasecmp(items[i].name[j], word)) return i;
    return -1;
}

/* split the argument line into words; quotes and *" *N escapes as in AmigaDOS */
static int split_args(const char *line, char words[][256], int max) {
    int n = 0;
    const char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        int l = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '*' && p[1]) {
                    p++;
                    char e = *p == 'N' || *p == 'n' ? '\n' : *p == 'E' || *p == 'e' ? 27 : *p;
                    if (l < 255) words[n][l++] = e;
                    p++;
                    continue;
                }
                if (l < 255) words[n][l++] = *p;
                p++;
            }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') { if (l < 255) words[n][l++] = *p; p++; }
        }
        words[n][l] = 0;
        n++;
    }
    return n;
}

/* allocation chain for FreeArgs: each block's first long links the next */
static uint32_t rda_alloc(uint32_t rda, uint32_t size) {
    uint32_t a = exec_allocvec(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(a, rd32(rda + RDA_DALIST));
    wr32(rda + RDA_DALIST, a);
    return a + 4;
}

static uint32_t rda_str(uint32_t rda, const char *s) {
    uint32_t a = rda_alloc(rda, (uint32_t)strlen(s) + 1);
    for (size_t i = 0; i <= strlen(s); i++) wr8(a + i, (uint8_t)s[i]);
    return a;
}

static uint32_t f_ReadArgs(M68K *c) {
    char tpl[512], line[2048];
    str_get(c->d[1], tpl, sizeof tpl);
    uint32_t array = c->d[2], rda = c->d[3];
    if (!rda) {
        rda = exec_allocvec(RDA_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        wr32(rda + RDA_FLAGS, RDAF_ALLOCATED_BY_READARGS);
    }
    if (rd32(rda + RDA_BUFFER)) {
        uint32_t b = rd32(rda + RDA_BUFFER), len = rd32(rda + RDA_LENGTH);
        int i;
        for (i = 0; i < (int)len && i < (int)sizeof line - 1; i++) line[i] = (char)rd8(b + i);
        line[i] = 0;
    } else {
        uint32_t t = this_task();
        str_get(rd32(t + PR_ARGUMENTS), line, sizeof line);
    }
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;

    TItem items[32];
    int ni = parse_template(tpl, items, 32);
    static char words[128][256];
    int nw = split_args(line, words, 128);

    if (nw == 1 && !strcmp(words[0], "?")) {             /* template help, then a fresh line */
        char prompt[600];
        int n = snprintf(prompt, sizeof prompt, "%s: ", tpl);
        fh_write_str(cur_out(), prompt, n);
        HostFile *ih = fh_get(cur_in());
        int got = ih && ih->con ? con_read(ih->con, line, sizeof line - 1) : host_con_read(line, sizeof line - 1);
        line[got > 0 ? got : 0] = 0;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        nw = split_args(line, words, 128);
    }

    int used[128] = { 0 }, filled[32] = { 0 };
    for (int i = 0; i < ni; i++) wr32(array + i * 4, rd32(array + i * 4));   /* keep defaults */
    int err = 0;

    /* keywords first: NAME=value, NAME value, switches */
    for (int w = 0; w < nw && !err; w++) {
        char key[256];
        snprintf(key, sizeof key, "%s", words[w]);
        char *eq = strchr(key, '=');
        if (eq) *eq = 0;
        int it = match_item(items, ni, key);
        if (it < 0) continue;
        TItem *ti = &items[it];
        if (ti->s || ti->t) {
            if (eq) continue;
            wr32(array + it * 4, DOSTRUE);
            used[w] = 1; filled[it] = 1;
            continue;
        }
        const char *val;
        if (eq) val = words[w] + (eq - key) + 1;
        else if (w + 1 < nw) { val = words[w + 1]; used[w + 1] = 1; }
        else { err = ERROR_REQUIRED_ARG_MISSING; break; }
        used[w] = 1;
        if (ti->f) {
            /* rest of the line, verbatim */
            const char *p = strstr(line, words[w]);
            val = p ? p + strlen(words[w]) : val;
            while (*val == ' ' || *val == '=') val++;
            for (int k = w + 1; k < nw; k++) used[k] = 1;
        }
        if (ti->n) {
            uint32_t v = rda_alloc(rda, 4);
            char *end;
            long num = strtol(val, &end, 10);
            if (*end) { err = ERROR_BAD_NUMBER; break; }
            wr32(v, (uint32_t)num);
            wr32(array + it * 4, v);
        } else if (ti->m) {
            uint32_t v = rda_alloc(rda, 8);
            wr32(v, rda_str(rda, val));
            wr32(array + it * 4, v);
        } else wr32(array + it * 4, rda_str(rda, val));
        filled[it] = 1;
    }
    /* positional: remaining words go to unfilled non-keyword items in template
       order; a /M item takes all but what the required items after it need */
    int pw[128], np = 0, pi = 0;
    for (int w = 0; w < nw; w++) if (!used[w]) pw[np++] = w;
    for (int it = 0; it < ni && !err && pi < np; it++) {
        TItem *ti = &items[it];
        if (filled[it] || ti->k || ti->s || ti->t) continue;
        if (ti->f) {
            int w = pw[pi];
            const char *p = line;
            for (int k = 0; k < w; k++) { const char *q = strstr(p, words[k]); if (q) p = q + strlen(words[k]); }
            while (*p == ' ') p++;
            wr32(array + it * 4, rda_str(rda, p));
            filled[it] = 1;
            pi = np;
            break;
        }
        if (ti->m) {
            int need = 0;
            for (int j = it + 1; j < ni; j++)
                if (!filled[j] && items[j].a && !items[j].k && !items[j].s && !items[j].t) need++;
            int take = np - pi - need;
            if (take < 0) take = 0;
            if (!take) continue;
            uint32_t v = rda_alloc(rda, (uint32_t)(take + 1) * 4);
            for (int i = 0; i < take; i++) wr32(v + i * 4, rda_str(rda, words[pw[pi + i]]));
            wr32(v + take * 4, 0);
            wr32(array + it * 4, v);
            pi += take;
            filled[it] = 1;
            continue;
        }
        const char *word = words[pw[pi++]];
        if (ti->n) {
            char *end;
            long num = strtol(word, &end, 10);
            if (*end || !*word) { err = ERROR_BAD_NUMBER; break; }
            uint32_t v = rda_alloc(rda, 4);
            wr32(v, (uint32_t)num);
            wr32(array + it * 4, v);
        } else wr32(array + it * 4, rda_str(rda, word));
        filled[it] = 1;
    }
    if (!err && pi < np) err = ERROR_TOO_MANY_ARGS;
    for (int i = 0; i < ni && !err; i++) if (items[i].a && !filled[i]) err = ERROR_REQUIRED_ARG_MISSING;
    if (err) {
        uint32_t b = rd32(rda + RDA_DALIST);
        while (b) { uint32_t nx = rd32(b); exec_freevec(b); b = nx; }
        wr32(rda + RDA_DALIST, 0);
        if (rd32(rda + RDA_FLAGS) & RDAF_ALLOCATED_BY_READARGS) exec_freevec(rda);
        dos_set_ioerr(err);
        return 0;
    }
    return rda;
}

static uint32_t f_FreeArgs(M68K *c) {
    uint32_t rda = c->d[1];
    if (!rda) return 0;
    uint32_t b = rd32(rda + RDA_DALIST);
    while (b) { uint32_t nx = rd32(b); exec_freevec(b); b = nx; }
    wr32(rda + RDA_DALIST, 0);
    if (rd32(rda + RDA_FLAGS) & RDAF_ALLOCATED_BY_READARGS) exec_freevec(rda);
    return 0;
}

static uint32_t f_AllocDosObject(M68K *c) {
    switch (c->d[1]) {
    case 0: return 0;                                    /* DOS_FILEHANDLE: not yet */
    case 2: return exec_allocvec(FIB_SIZE, MEMF_PUBLIC | MEMF_CLEAR);   /* DOS_FIB */
    case 5: {                                            /* DOS_RDARGS */
        uint32_t r = exec_allocvec(RDA_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        return r;
    }
    default: return exec_allocvec(256, MEMF_PUBLIC | MEMF_CLEAR);
    }
}
static uint32_t f_FreeDosObject(M68K *c) { exec_freevec(c->d[2]); return 0; }

/* ---- process / CLI ---------------------------------------------------------- */

static uint32_t cur_cli(void) { uint32_t t = this_task(); return t ? BADDR(rd32(t + PR_CLI)) : 0; }

static uint32_t f_Exit(M68K *c) { c->d[0] = c->d[1]; c->stop = 2; return c->d[1]; }
static uint32_t f_GetArgStr(M68K *c) { (void)c; return rd32(this_task() + PR_ARGUMENTS); }
static uint32_t f_SetArgStr(M68K *c) {
    uint32_t old = rd32(this_task() + PR_ARGUMENTS);
    wr32(this_task() + PR_ARGUMENTS, c->d[1]);
    return old;
}
static uint32_t f_Cli(M68K *c) { (void)c; return rd32(this_task() + PR_CLI) ? BADDR(rd32(this_task() + PR_CLI)) : 0; }
static uint32_t f_GetProgramName(M68K *c) {
    uint32_t cli = cur_cli();
    char name[256];
    if (!cli) return 0;
    int n = bstr_get(rd32(cli + CLI_COMMANDNAME), name, sizeof name);
    if ((int32_t)c->d[2] <= n) return 0;
    for (int i = 0; i <= n; i++) wr8(c->d[1] + i, (uint8_t)name[i]);
    return DOSTRUE;
}
static uint32_t f_GetCurrentDirName(M68K *c) {
    char cd[1024], an[1024];
    current_dir(cd, sizeof cd);
    dos_amiga_name(cd, an, sizeof an);
    if (strlen(an) + 1 > c->d[2]) return 0;
    for (size_t i = 0; i <= strlen(an); i++) wr8(c->d[1] + i, (uint8_t)an[i]);
    return DOSTRUE;
}

static uint32_t f_CreateNewProc(M68K *c);
static int run_seglist(uint32_t seg, const char *name, const char *args, uint32_t stacksize, const char *homedir);
static uint32_t f_CreateProc(M68K *c);

/* ---- AmigaDOS patterns: ? # * ( | ) ~ [ ] % ' ---------------------------- */

static const char *pat_item_end(const char *p, const char *pe) {
    if (p >= pe) return pe;
    switch (*p) {
    case '#': case '~': return pat_item_end(p + 1, pe);
    case '\'': return p + 2 <= pe ? p + 2 : pe;
    case '(': {
        int depth = 0;
        for (const char *q = p; q < pe; q++) {
            if (*q == '\'') { q++; continue; }
            if (*q == '(') depth++;
            else if (*q == ')' && --depth == 0) return q + 1;
        }
        return pe;
    }
    case '[': {
        const char *q = memchr(p, ']', (size_t)(pe - p));
        return q ? q + 1 : pe;
    }
    }
    return p + 1;
}

static int pat_eq(int a, int b, int nocase) {
    if (!nocase) return a == b;
    return toupper(a) == toupper(b) ||
           (a >= 0xE0 && a <= 0xFE && a != 0xF7 && a - 32 == b) || (b >= 0xE0 && b <= 0xFE && b != 0xF7 && b - 32 == a);
}

static int pat_match(const char *p, const char *pe, const char *s, const char *se, int nocase);

static int pat_item(const char *p, const char *q, const char *s, const char *se, int nocase) {
    if (p >= q) return s == se;
    switch (*p) {
    case '?': return se == s + 1;
    case '*': return 1;
    case '%': return s == se;
    case '\'': return se == s + 1 && p + 1 < q && pat_eq((unsigned char)*s, (unsigned char)p[1], nocase);
    case '#': case '~': return pat_match(p, q, s, se, nocase);
    case '(': {
        const char *a = p + 1, *end = q - 1;
        int depth = 0;
        for (const char *r = a; r <= end; r++) {
            if (r < end && *r == '\'') { r++; continue; }
            if (r < end && *r == '(') depth++;
            else if (r < end && *r == ')') depth--;
            else if (r == end || (*r == '|' && depth == 0)) {
                if (pat_match(a, r, s, se, nocase)) return 1;
                a = r + 1;
            }
        }
        return 0;
    }
    case '[': {
        if (se != s + 1) return 0;
        int c = (unsigned char)*s, neg = 0, hit = 0;
        const char *r = p + 1, *end = q - 1;
        if (r < end && *r == '~') { neg = 1; r++; }
        for (; r < end; r++) {
            int lo = (unsigned char)*r, hi = lo;
            if (r + 2 < end && r[1] == '-') { hi = (unsigned char)r[2]; r += 2; }
            if (nocase) { int u = toupper(c); if ((u >= toupper(lo) && u <= toupper(hi)) || (c >= lo && c <= hi)) hit = 1; }
            else if (c >= lo && c <= hi) hit = 1;
        }
        return hit != neg;
    }
    }
    return se == s + 1 && pat_eq((unsigned char)*s, (unsigned char)*p, nocase);
}

static int pat_match(const char *p, const char *pe, const char *s, const char *se, int nocase) {
    if (p >= pe) return s == se;
    if (*p == '#') {
        const char *q = pat_item_end(p + 1, pe);
        if (pat_match(q, pe, s, se, nocase)) return 1;
        for (const char *k = s + 1; k <= se; k++)
            if (pat_item(p + 1, q, s, k, nocase) && pat_match(p, pe, k, se, nocase)) return 1;
        return 0;
    }
    if (*p == '~') {
        const char *q = pat_item_end(p + 1, pe);
        for (const char *k = s; k <= se; k++)
            if (!pat_item(p + 1, q, s, k, nocase) && pat_match(q, pe, k, se, nocase)) return 1;
        return 0;
    }
    const char *q = pat_item_end(p, pe);
    if (*p == '*') {
        for (const char *k = s; k <= se; k++) if (pat_match(q, pe, k, se, nocase)) return 1;
        return 0;
    }
    if (*p != '(' && *p != '#' && *p != '~') {
        /* single-character items: no need to try every split */
        if (s >= se || !pat_item(p, q, s, s + 1, nocase)) return 0;
        return pat_match(q, pe, s + 1, se, nocase);
    }
    for (const char *k = s; k <= se; k++)
        if (pat_item(p, q, s, k, nocase) && pat_match(q, pe, k, se, nocase)) return 1;
    return 0;
}

int dos_match_pattern(const char *pat, const char *str, int nocase) {
    return pat_match(pat, pat + strlen(pat), str, str + strlen(str), nocase);
}

int dos_has_wild(const char *pat) {
    for (const char *p = pat; *p; p++) {
        if (*p == '\'') { if (p[1]) p++; continue; }
        if (strchr("?#*(|)~[]%", *p)) return 1;
    }
    return 0;
}

/* the "parsed" form is the pattern itself behind a marker byte */
static uint32_t pattern_parse(M68K *c, int nocase) {
    char pat[512];
    str_get(c->d[1], pat, sizeof pat);
    uint32_t len = c->d[3];
    if (len < strlen(pat) + 2) return (uint32_t)-1;
    wr8(c->d[2], nocase ? 2 : 1);
    for (uint32_t i = 0; i <= strlen(pat); i++) wr8(c->d[2] + 1 + i, (uint8_t)pat[i]);
    return dos_has_wild(pat) ? 1 : 0;
}
static uint32_t pattern_match(M68K *c, int nocase) {
    char pat[512], str[512];
    str_get(c->d[1] + 1, pat, sizeof pat);
    str_get(c->d[2], str, sizeof str);
    return dos_match_pattern(pat, str, nocase) ? DOSTRUE : 0;
}
static uint32_t f_ParsePattern(M68K *c) { return pattern_parse(c, 0); }
static uint32_t f_ParsePatternNoCase(M68K *c) { return pattern_parse(c, 1); }
static uint32_t f_MatchPattern(M68K *c) { return pattern_match(c, 0); }
static uint32_t f_MatchPatternNoCase(M68K *c) { return pattern_match(c, 1); }


/* ---- MatchFirst / MatchNext: patterns in the last path component ------------------- */

#define AP_FLAGS 16
#define AP_STRLEN 18
#define AP_INFO 20
#define AP_BUF (20 + FIB_SIZE)
#define APF_ITSWILD 0x04

typedef struct Match { uint32_t anchor; char dir[600], hdir[1024]; char **names; int n, pos; struct Match *next; } Match;
static Match *matches;

static int name_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static uint32_t match_emit(Match *m) {
    while (m->pos < m->n) {
        const char *nm = m->names[m->pos++];
        char hp[2048];
        snprintf(hp, sizeof hp, "%s/%s", m->hdir, nm);
        if (!fill_fib(hp, nm, m->anchor + AP_INFO)) continue;
        int len = (int16_t)rd16(m->anchor + AP_STRLEN);
        if (len > 0) {
            char full[1024], l1[256];
            name_to_amiga(nm, l1, sizeof l1);
            size_t dl = strlen(m->dir);
            if (!dl) snprintf(full, sizeof full, "%s", l1);
            else snprintf(full, sizeof full, "%s%s%s", m->dir, (m->dir[dl - 1] == ':' || m->dir[dl - 1] == '/') ? "" : "/", l1);
            int k = 0;
            for (; full[k] && k < len - 1; k++) wr8(m->anchor + AP_BUF + (uint32_t)k, (uint8_t)full[k]);
            wr8(m->anchor + AP_BUF + (uint32_t)k, 0);
        }
        dos_set_ioerr(0);
        return 0;
    }
    return (uint32_t)dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
}

static void match_free(uint32_t anchor) {
    for (Match **pp = &matches; *pp; pp = &(*pp)->next)
        if ((*pp)->anchor == anchor) {
            Match *m = *pp;
            *pp = m->next;
            for (int i = 0; i < m->n; i++) free(m->names[i]);
            free(m->names);
            free(m);
            return;
        }
}

static uint32_t f_MatchFirst(M68K *c) {
    char pat[600];
    str_get(c->d[1], pat, sizeof pat);
    uint32_t anchor = c->d[2];
    match_free(anchor);
    Match *m = calloc(1, sizeof *m);
    m->anchor = anchor;
    /* split into drawer and the last component */
    char *cut = strrchr(pat, '/');
    if (!cut) cut = strchr(pat, ':');
    const char *last = cut ? cut + 1 : pat;
    if (cut) { snprintf(m->dir, sizeof m->dir, "%.*s", (int)(cut - pat + (*cut == ':' ? 1 : 0)), pat); }
    if (!dos_resolve(m->dir, m->hdir, sizeof m->hdir)) { free(m); return (uint32_t)dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); }
    int wild = dos_has_wild(last);
    wr8(anchor + AP_FLAGS, (rd8(anchor + AP_FLAGS) & ~APF_ITSWILD) | (wild ? APF_ITSWILD : 0));
    int cap = 0;
    if (!wild) {
        char hp[2048], hn[600];
        name_to_host(last, hn, sizeof hn);
        snprintf(hp, sizeof hp, "%s/%s", m->hdir, hn);
        struct stat st;
        if (!last[0] || stat(hp, &st)) { free(m); return (uint32_t)dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); }
        m->names = malloc(sizeof(char *));
        m->names[m->n++] = strdup(hn);
    } else {
        DIR *d = opendir(m->hdir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (dos_hidden_name(e->d_name)) continue;
            char l1[256];
            name_to_amiga(e->d_name, l1, sizeof l1);
            if (!dos_match_pattern(last, l1, 1)) continue;
            if (m->n == cap) { cap = cap * 2 + 32; m->names = realloc(m->names, sizeof(char *) * (size_t)cap); }
            m->names[m->n++] = strdup(e->d_name);
        }
        if (d) closedir(d);
        if (m->n) qsort(m->names, (size_t)m->n, sizeof(char *), name_cmp);
    }
    m->next = matches;
    matches = m;
    uint32_t r = match_emit(m);
    return r == ERROR_NO_MORE_ENTRIES ? (uint32_t)dos_set_ioerr(ERROR_OBJECT_NOT_FOUND) : r;
}
static uint32_t f_MatchNext(M68K *c) {
    for (Match *m = matches; m; m = m->next) if (m->anchor == c->d[1]) return match_emit(m);
    return (uint32_t)dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
}
static uint32_t f_MatchEnd(M68K *c) { match_free(c->d[1]); return 0; }

/* ---- locks from handles ---------------------------------------------------------- */

static uint32_t f_DupLockFromFH(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h || !h->path) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    return lock_new(h->path, SHARED_LOCK);
}
static uint32_t f_ParentOfFH(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h || !h->path) return 0;
    char p[1024];
    snprintf(p, sizeof p, "%s", h->path);
    char *sl = strrchr(p, '/');
    if (sl && sl != p) *sl = 0;
    return lock_new(p, SHARED_LOCK);
}
static uint32_t f_NameFromFH(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h || !h->path) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    char an[1024];
    dos_amiga_name(h->path, an, sizeof an);
    if (strlen(an) + 1 > c->d[3]) { dos_set_ioerr(ERROR_LINE_TOO_LONG); return 0; }
    for (size_t i = 0; i <= strlen(an); i++) wr8(c->d[2] + (uint32_t)i, (uint8_t)an[i]);
    return DOSTRUE;
}

/* ---- dates as text: struct DateTime { DateStamp, Format, Flags, StrDay, StrDate, StrTime } ---- */

static const char *en_days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *en_mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static uint32_t f_DateToStr(M68K *c) {
    uint32_t dt = c->d[1];
    int32_t days = (int32_t)rd32(dt), mins = (int32_t)rd32(dt + 4), ticks = (int32_t)rd32(dt + 8);
    int fmt = rd8(dt + 12), flags = rd8(dt + 13);
    time_t t = (time_t)252460800 + (time_t)days * 86400 + mins * 60 + ticks / 50;
    struct tm tm;
    gmtime_r(&t, &tm);
    char day[16], date[16], tim[16];
    snprintf(day, sizeof day, "%s", en_days[tm.tm_wday]);
    switch (fmt) {
    case 1: snprintf(date, sizeof date, "%02d-%02d-%02d", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday); break;
    case 2: snprintf(date, sizeof date, "%02d-%02d-%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100); break;
    case 3: snprintf(date, sizeof date, "%02d-%02d-%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100); break;
    default: snprintf(date, sizeof date, "%02d-%s-%02d", tm.tm_mday, en_mon[tm.tm_mon], tm.tm_year % 100); break;
    }
    if (flags & 1) {                                        /* DTF_SUBST: Today, Yesterday... */
        time_t now = time(0);
        struct tm lt;
        localtime_r(&now, &lt);
        long today = ((long)now + lt.tm_gmtoff - 252460800L) / 86400;
        if (days == today) snprintf(date, sizeof date, "Today");
        else if (days == today - 1) snprintf(date, sizeof date, "Yesterday");
        else if (days == today + 1) snprintf(date, sizeof date, "Tomorrow");
    }
    snprintf(tim, sizeof tim, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    const char *parts[3] = { day, date, tim };
    for (int k = 0; k < 3; k++) {
        uint32_t dst = rd32(dt + 14 + 4 * (uint32_t)k);
        if (!dst) continue;
        for (size_t i = 0; i <= strlen(parts[k]); i++) wr8(dst + (uint32_t)i, (uint8_t)parts[k][i]);
    }
    return DOSTRUE;
}

static uint32_t f_StrToDate(M68K *c) {
    uint32_t dt = c->d[1];
    char date[32] = "", tim[32] = "";
    if (rd32(dt + 18)) str_get(rd32(dt + 18), date, sizeof date);
    if (rd32(dt + 22)) str_get(rd32(dt + 22), tim, sizeof tim);
    int fmt = rd8(dt + 12), a = 0, b = 0, y = 0, d = 1, m = 1, hh = 0, mm = 0, ss = 0;
    char mon[8] = "";
    if (date[0]) {
        if (fmt == 0) {
            if (sscanf(date, "%d-%3s-%d", &d, mon, &y) != 3) return 0;
            for (m = 0; m < 12 && strcasecmp(mon, en_mon[m]); m++) ;
            if (m == 12) return 0;
            m++;
        } else {
            if (sscanf(date, "%d-%d-%d", &a, &b, &y) != 3) return 0;
            if (fmt == 1) { int t = a; a = y; y = t; m = b; d = a; }
            else if (fmt == 2) { m = a; d = b; }
            else { d = a; m = b; }
        }
        if (y < 78) y += 100;
        if (y < 1900) y += 1900;
    }
    if (tim[0]) sscanf(tim, "%d:%d:%d", &hh, &mm, &ss);
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
    if (date[0]) {
        time_t t = timegm(&tm) - 252460800;
        wr32(dt, (uint32_t)(t / 86400));
    }
    if (tim[0]) { wr32(dt + 4, (uint32_t)(hh * 60 + mm)); wr32(dt + 8, (uint32_t)(ss * 50)); }
    return DOSTRUE;
}

/* ---- DOS packets: one native handler port answers them --------------------------- */

uint32_t dos_handler_port;

#define DP_LINK 0
#define DP_PORT 4
#define DP_TYPE 8
#define DP_RES1 12
#define DP_RES2 16
#define DP_ARG1 20
#define DP_ARG2 24
#define DP_ARG3 28

void dos_packet(uint32_t dp) {
    uint32_t type = rd32(dp + DP_TYPE), a1 = rd32(dp + DP_ARG1), a2 = rd32(dp + DP_ARG2), a3 = rd32(dp + DP_ARG3);
    HostFile *h = a1 > 0 && a1 < 256 && files[a1].used ? &files[a1] : 0;
    int32_t r1 = 0, r2 = 0;
    switch (type) {
    case 82:                                                    /* ACTION_READ */
        r1 = hf_read(h, a2, (int32_t)a3);
        if (r1 < 0) r2 = ERROR_OBJECT_WRONG_TYPE;
        break;
    case 87:                                                    /* ACTION_WRITE */
        r1 = hf_write(h, a2, (int32_t)a3);
        if (r1 < 0) r2 = ERROR_OBJECT_WRONG_TYPE;
        break;
    case 1008: {                                                /* ACTION_SEEK */
        if (!h || !h->f) { r1 = -1; r2 = ERROR_SEEK_ERROR; break; }
        long old = ftell(h->f);
        int whence = (int32_t)a3 == OFFSET_BEGINNING ? SEEK_SET : (int32_t)a3 == OFFSET_END ? SEEK_END : SEEK_CUR;
        if (fseek(h->f, (int32_t)a2, whence)) { r1 = -1; r2 = ERROR_SEEK_ERROR; } else r1 = (int32_t)old;
        h->ungot = -1;
        break;
    }
    case 1007: r1 = DOSTRUE; break;                             /* ACTION_END: Close() does the rest */
    case 1027: r1 = DOSTRUE; break;                             /* ACTION_IS_FILESYSTEM */
    case 31: r1 = DOSTRUE; break;                               /* ACTION_FLUSH */
    default: r1 = 0; r2 = 209;                                  /* ERROR_ACTION_NOT_KNOWN */
    }
    wr32(dp + DP_RES1, (uint32_t)r1);
    wr32(dp + DP_RES2, (uint32_t)r2);
}

/* SendPkt(packet, port, replyport) */
static uint32_t f_SendPkt(M68K *c) {
    uint32_t dp = c->d[1], port = c->d[2], msg = rd32(dp + DP_LINK);
    wr32(dp + DP_PORT, c->d[3]);
    wr32(msg + LN_NAME, dp);
    lib_call(c, SysBase, "exec", "PutMsg", port, msg);
    return 0;
}
static uint32_t f_WaitPkt(M68K *c) {
    (void)c;
    uint32_t port = this_task() + PR_MSGPORT;
    exec_waitport(port);
    uint32_t msg = list_remhead(port + MP_MSGLIST);
    return msg ? rd32(msg + LN_NAME) : 0;
}
static uint32_t f_ReplyPkt(M68K *c) {
    uint32_t dp = c->d[1], msg = rd32(dp + DP_LINK), port = rd32(dp + DP_PORT);
    wr32(dp + DP_RES1, c->d[2]);
    wr32(dp + DP_RES2, c->d[3]);
    wr32(dp + DP_PORT, this_task() + PR_MSGPORT);
    lib_call(c, SysBase, "exec", "PutMsg", port, msg);
    return 0;
}
/* DoPkt(port, action, arg1..arg5) */
static uint32_t f_DoPkt(M68K *c) {
    uint32_t sp = exec_allocmem(MN_SIZE + 48, MEMF_PUBLIC | MEMF_CLEAR), dp = sp + MN_SIZE;
    wr32(dp + DP_LINK, sp);
    wr32(sp + LN_NAME, dp);
    wr32(dp + DP_TYPE, c->d[2]);
    for (int i = 0; i < 5; i++) wr32(dp + DP_ARG1 + 4 * (uint32_t)i, c->d[3 + i]);
    uint32_t port = c->d[1];
    if (port == dos_handler_port) dos_packet(dp);
    else {
        uint32_t me = this_task() + PR_MSGPORT;
        wr32(dp + DP_PORT, me);
        wr32(sp + MN_REPLYPORT, me);
        lib_call(c, SysBase, "exec", "PutMsg", port, sp);
        exec_waitport(me);
        list_remove(sp);
    }
    uint32_t r1 = rd32(dp + DP_RES1);
    dos_set_ioerr((int)rd32(dp + DP_RES2));
    exec_freemem(sp, MN_SIZE + 48);
    return r1;
}

/* ---- protection bits, comments, dates, sizes -------------------------------- */

/* Amiga bits the host has no place for (comments, the full protection mask)
   live in extended attributes */
#define XA_PROT "com.amigamac.protection"
#define XA_COMMENT "com.amigamac.comment"

static int name_arg(M68K *c, uint32_t a, char *hp, int max) {
    (void)c;
    char name[512];
    str_get(a, name, sizeof name);
    return dos_resolve(name, hp, max);
}

uint32_t dos_protection(const char *hp) {
    uint32_t fib = exec_allocmem(FIB_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t prot = fill_fib(hp, "", fib) ? rd32(fib + FIB_PROTECTION) : 0;
    exec_freemem(fib, FIB_SIZE);
    return prot;
}

void dos_set_protection(const char *hp, uint32_t prot) {
    struct stat st;
    if (stat(hp, &st)) return;
    mode_t m = st.st_mode & 07777 & ~(mode_t)(S_IRUSR | S_IWUSR | S_IXUSR);
    if (!(prot & 8)) m |= S_IRUSR;
    if (!(prot & 4)) m |= S_IWUSR;
    if (!(prot & 2) && !S_ISDIR(st.st_mode) && (st.st_mode & S_IXUSR)) m |= S_IXUSR;
    if (S_ISDIR(st.st_mode)) m |= S_IXUSR;
    chmod(hp, m);
    uint8_t b[4] = { (uint8_t)(prot >> 24), (uint8_t)(prot >> 16), (uint8_t)(prot >> 8), (uint8_t)prot };
    setxattr(hp, XA_PROT, b, 4, 0, 0);
}

static uint32_t f_SetProtection(M68K *c) {
    char hp[1024];
    if (!name_arg(c, c->d[1], hp, sizeof hp)) return 0;
    if (access(hp, F_OK)) { dos_set_ioerr(host_err()); return 0; }
    dos_set_protection(hp, c->d[2]);
    return DOSTRUE;
}

static uint32_t f_SetComment(M68K *c) {
    char hp[1024], com[128];
    if (!name_arg(c, c->d[1], hp, sizeof hp)) return 0;
    str_get(c->d[2], com, sizeof com);
    if (access(hp, F_OK)) { dos_set_ioerr(ERROR_OBJECT_NOT_FOUND); return 0; }
    if (com[0]) setxattr(hp, XA_COMMENT, com, strlen(com), 0, 0);
    else removexattr(hp, XA_COMMENT, 0);
    return DOSTRUE;
}

static time_t datestamp_to_unix(uint32_t ds) {
    long secs = (long)rd32(ds + DS_DAYS) * 86400L + (long)rd32(ds + DS_MINUTE) * 60L + (long)rd32(ds + DS_TICK) / 50;
    time_t local = (time_t)(secs + 252460800L);
    struct tm lt;
    localtime_r(&local, &lt);
    return local - lt.tm_gmtoff;
}

static uint32_t f_SetFileDate(M68K *c) {
    char hp[1024];
    if (!name_arg(c, c->d[1], hp, sizeof hp)) return 0;
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = datestamp_to_unix(c->d[2]);
    tv[0].tv_usec = tv[1].tv_usec = 0;
    if (utimes(hp, tv)) { dos_set_ioerr(host_err()); return 0; }
    return DOSTRUE;
}

/* SetFileSize(fh, pos, mode): new size, or -1 */
static uint32_t f_SetFileSize(M68K *c) {
    HostFile *h = fh_get(c->d[1]);
    if (!h || !h->f) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0xFFFFFFFF; }
    fflush(h->f);
    long cur = ftell(h->f), end;
    fseek(h->f, 0, SEEK_END);
    end = ftell(h->f);
    fseek(h->f, cur, SEEK_SET);
    int32_t mode = (int32_t)c->d[3];
    long size = (int32_t)c->d[2] + (mode == OFFSET_BEGINNING ? 0 : mode == OFFSET_END ? end : cur);
    if (size < 0 || ftruncate(fileno(h->f), size)) { dos_set_ioerr(ERROR_SEEK_ERROR); return 0xFFFFFFFF; }
    if (cur > size) fseek(h->f, size, SEEK_SET);
    return (uint32_t)size;
}

/* CompareDates(d1, d2): < 0 when d1 is later */
static uint32_t f_CompareDates(M68K *c) {
    for (int o = 0; o < 12; o += 4) {
        int32_t a = (int32_t)rd32(c->d[1] + o), b = (int32_t)rd32(c->d[2] + o);
        if (a != b) return (uint32_t)(b - a);
    }
    return 0;
}

static uint32_t f_SetOwner(M68K *c) { (void)c; return DOSTRUE; }

/* ---- locks and handles ------------------------------------------------------- */

static uint32_t f_OpenFromLock(M68K *c) {
    const char *p = lock_path(c->d[1]);
    struct stat st;
    if (!p || stat(p, &st) || S_ISDIR(st.st_mode)) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    FILE *f = fopen(p, "r+b");
    if (!f) f = fopen(p, "rb");
    if (!f) { dos_set_ioerr(host_err()); return 0; }
    uint32_t fh = fh_new(f, 0);
    if (!fh) { fclose(f); return 0; }
    fh_get(fh)->path = strdup(p);
    lock_free(c->d[1]);                                  /* the lock now belongs to the handle */
    return fh;
}

#define LOCK_SAME 0
#define LOCK_SAME_VOLUME 1
#define LOCK_DIFFERENT -1
static uint32_t f_SameLock(M68K *c) {
    const char *a = lock_path(c->d[1]), *b = lock_path(c->d[2]);
    if (!a || !b) return (uint32_t)LOCK_DIFFERENT;
    return !strcmp(a, b) ? LOCK_SAME : LOCK_SAME_VOLUME;
}
static uint32_t f_SameDevice(M68K *c) { return lock_path(c->d[1]) && lock_path(c->d[2]) ? DOSTRUE : 0; }

static uint32_t f_ChangeMode(M68K *c) {
    /* CHANGE_LOCK 0, CHANGE_FH 1: the host does not hold exclusive locks */
    if (c->d[1] == 0 && c->d[2]) wr32(BADDR(c->d[2]) + FL_ACCESS, c->d[3]);
    return DOSTRUE;
}

static uint32_t f_SetVBuf(M68K *c) { (void)c; return 0; }
static uint32_t f_LockRecord(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t f_UnLockRecord(M68K *c) { (void)c; return DOSTRUE; }

static uint32_t f_IsFileSystem(M68K *c) {
    char name[256], hp[1024];
    str_get(c->d[1], name, sizeof name);
    if (!strncasecmp(name, "CON:", 4) || !strncasecmp(name, "RAW:", 4) || !strncasecmp(name, "NIL:", 4)) return 0;
    return dos_resolve(name, hp, sizeof hp) ? DOSTRUE : 0;
}

static uint32_t f_GetCurrentDir(M68K *c) { (void)c; return rd32(this_task() + PR_CURRENTDIR); }

/* ---- ExAll -------------------------------------------------------------------- */

#define ED_NAME 1
#define ED_TYPE 2
#define ED_SIZE 3
#define ED_PROTECTION 4
#define ED_DATE 5
#define ED_COMMENT 6
#define ED_OWNER 7
#define EAC_ENTRIES 0
#define EAC_LASTKEY 4
#define EAC_MATCHSTRING 8
#define EAC_MATCHFUNC 12

static uint32_t f_ExAll(M68K *c) {
    uint32_t lock = c->d[1], buf = c->d[2], size = c->d[3], type = c->d[4], eac = c->d[5];
    const char *p = lock_path(lock);
    if (!p || type < ED_NAME || type > ED_OWNER) { dos_set_ioerr(ERROR_BAD_NUMBER); return 0; }
    static const uint32_t fixed[] = { 0, 8, 12, 16, 20, 32, 36, 40 };
    char pat[512] = "";
    if (rd32(eac + EAC_MATCHSTRING)) str_get(rd32(eac + EAC_MATCHSTRING), pat, sizeof pat);
    DIR *d = opendir(p);
    if (!d) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    uint32_t key = rd32(eac + EAC_LASTKEY), idx = 0, n = 0, prev = 0, at = buf, end = buf + size;
    uint32_t fib = exec_allocmem(FIB_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    struct dirent *e;
    int more = 0;
    while ((e = readdir(d))) {
        if (dos_hidden_name(e->d_name)) continue;
        if (idx++ < key) continue;
        char hp[2048];
        snprintf(hp, sizeof hp, "%s/%s", p, e->d_name);
        if (!fill_fib(hp, e->d_name, fib)) continue;
        char name[108];
        str_get(fib + FIB_FILENAME, name, sizeof name);
        if (pat[0] && !dos_match_pattern(pat, name, 1)) { key = idx; continue; }
        char com[80] = "";
        if (type >= ED_COMMENT) str_get(fib + FIB_COMMENT, com, sizeof com);
        uint32_t need = fixed[type] + (uint32_t)strlen(name) + 1 + (type >= ED_COMMENT ? (uint32_t)strlen(com) + 1 : 0);
        need = (need + 1) & ~1u;
        if (at + need > end) { more = 1; idx--; break; }
        for (uint32_t i = 0; i < fixed[type]; i++) wr8(at + i, 0);
        uint32_t s = at + fixed[type];
        for (size_t i = 0; i <= strlen(name); i++) wr8(s + i, (uint8_t)name[i]);
        wr32(at + 4, s);
        if (type >= ED_TYPE) wr32(at + 8, rd32(fib + FIB_DIRENTRYTYPE));
        if (type >= ED_SIZE) wr32(at + 12, rd32(fib + FIB_SIZE_));
        if (type >= ED_PROTECTION) wr32(at + 16, rd32(fib + FIB_PROTECTION));
        if (type >= ED_DATE) for (int i = 0; i < 12; i++) wr8(at + 20 + i, rd8(fib + FIB_DATE + i));
        if (type >= ED_COMMENT) {
            uint32_t cs = s + (uint32_t)strlen(name) + 1;
            for (size_t i = 0; i <= strlen(com); i++) wr8(cs + i, (uint8_t)com[i]);
            wr32(at + 32, cs);
        }
        if (prev) wr32(prev, at);
        prev = at;
        at += need;
        n++;
        key = idx;
    }
    closedir(d);
    exec_freemem(fib, FIB_SIZE);
    wr32(eac + EAC_ENTRIES, n);
    wr32(eac + EAC_LASTKEY, key);
    if (more && n == 0) { dos_set_ioerr(ERROR_NO_FREE_STORE); return 0; }
    if (more) return DOSTRUE;
    dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
    return 0;
}
static uint32_t f_ExAllEnd(M68K *c) { wr32(c->d[5] + EAC_LASTKEY, 0); return 0; }

/* ---- argument parsing helpers -------------------------------------------------- */

#define ITEM_EQUAL -2
#define ITEM_ERROR -1
#define ITEM_NOTHING 0
#define ITEM_UNQUOTED 1
#define ITEM_QUOTED 2

/* ReadItem(buffer, maxchars, CSource): from the CSource, or Input() when it is NULL */
static uint32_t f_ReadItem(M68K *c) {
    uint32_t buf = c->d[1], max = c->d[2], cs = c->d[3];
    uint32_t sbuf = cs ? rd32(cs + 0) : 0, slen = cs ? rd32(cs + 4) : 0, pos = cs ? rd32(cs + 8) : 0;
    /* without a CSource: the command's argument line, which AmigaOS puts in
       the Input() buffer; each process keeps its own position in it */
    static struct { uint32_t task, args, pos; } cur[16];
    int ci = -1;
    if (!cs) {
        uint32_t t = this_task(), args = rd32(t + PR_ARGUMENTS);
        for (int i = 0; i < 16 && ci < 0; i++) if (cur[i].task == t) ci = i;
        for (int i = 0; i < 16 && ci < 0; i++) if (!cur[i].task) ci = i;
        if (ci < 0) ci = 0;
        if (cur[ci].task != t || cur[ci].args != args) { cur[ci].task = t; cur[ci].args = args; cur[ci].pos = 0; }
        sbuf = args;
        pos = cur[ci].pos;
        slen = 0;
        if (args) while (rd8(args + slen)) slen++;
    }
    #define GETC() (pos < slen ? (int)rd8(sbuf + pos++) : -1)
    #define UNGETC(ch) do { (void)(ch); if (pos) pos--; } while (0)
    if (max) wr8(buf, 0);
    int ch;
    do ch = GETC(); while (ch == ' ' || ch == '\t');
    int32_t r;
    if (ch < 0 || ch == '\n' || ch == ';') { if (ch >= 0) UNGETC(ch); r = ITEM_NOTHING; goto done; }
    if (ch == '=') { r = ITEM_EQUAL; goto done; }
    uint32_t k = 0;
    if (ch == '"') {
        for (;;) {
            ch = GETC();
            if (ch < 0 || ch == '\n') { r = ITEM_ERROR; goto done; }
            if (ch == '"') break;
            if (ch == '*') {
                ch = GETC();
                if (ch == 'n' || ch == 'N') ch = '\n';
                else if (ch == 'e' || ch == 'E') ch = 27;
                else if (ch < 0) { r = ITEM_ERROR; goto done; }
            }
            if (k + 1 >= max) { r = ITEM_ERROR; goto done; }
            wr8(buf + k++, (uint8_t)ch);
        }
        wr8(buf + k, 0);
        r = ITEM_QUOTED;
        goto done;
    }
    for (;;) {
        if (ch < 0) break;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == ';' || ch == '=') { UNGETC(ch); break; }
        if (k + 1 >= max) { r = ITEM_ERROR; goto done; }
        wr8(buf + k++, (uint8_t)ch);
        ch = GETC();
    }
    wr8(buf + k, 0);
    r = ITEM_UNQUOTED;
done:
    if (cs) wr32(cs + 8, pos);
    else cur[ci].pos = pos;
    #undef GETC
    #undef UNGETC
    return (uint32_t)r;
}

/* FindArg(template, keyword): index of the template item naming keyword, or -1 */
static uint32_t f_FindArg(M68K *c) {
    char tpl[512], kw[128];
    str_get(c->d[1], tpl, sizeof tpl);
    str_get(c->d[2], kw, sizeof kw);
    int idx = 0;
    for (char *p = tpl; *p; idx++) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        for (char *n = p; n && *n;) {
            char *eq = strchr(n, '=');
            if (eq) *eq = 0;
            if (!strcasecmp(n, kw)) return (uint32_t)idx;
            n = eq ? eq + 1 : 0;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0xFFFFFFFF;
}

/* SplitName(name, separator, buf, oldpos, size): next component, new position or -1 */
static uint32_t f_SplitName(M68K *c) {
    uint32_t name = c->d[1], buf = c->d[3], pos = c->d[4], size = c->d[5];
    uint8_t sep = (uint8_t)c->d[2];
    uint32_t k = 0;
    if (!size) return 0xFFFFFFFF;
    for (;;) {
        uint8_t ch = rd8(name + pos);
        if (!ch) { wr8(buf + k, 0); return 0xFFFFFFFF; }
        pos++;
        if (ch == sep) { wr8(buf + k, 0); return pos; }
        if (k + 1 < size) wr8(buf + k++, ch);
    }
}

/* ---- CLI names, prompt, consoles -------------------------------------------- */

static int bstr_put(uint32_t bptr, const char *s, int max) {
    if (!bptr) return 0;
    uint32_t a = BADDR(bptr);
    int n = (int)strlen(s);
    if (n > max) n = max;
    wr8(a, (uint8_t)n);
    for (int i = 0; i < n; i++) wr8(a + 1 + i, (uint8_t)s[i]);
    return 1;
}
static int cli_set_bstr(int off, uint32_t src) {
    uint32_t cli = cur_cli();
    char s[256] = "";
    if (!cli) return 0;
    if (src) str_get(src, s, sizeof s);
    return bstr_put(rd32(cli + off), s, 254);
}
static int cli_get_bstr(int off, uint32_t dst, int32_t len) {
    uint32_t cli = cur_cli();
    char s[256] = "";
    if (len <= 0) return 0;
    if (cli) bstr_get(rd32(cli + off), s, sizeof s);
    int n = (int)strlen(s);
    if (n >= len) { n = len - 1; wr8(dst, 0); if (!cli) return 0; }
    for (int i = 0; i < n; i++) wr8(dst + i, (uint8_t)s[i]);
    wr8(dst + n, 0);
    return cli != 0 && (int)strlen(s) < len;
}
static uint32_t f_SetProgramName(M68K *c) { return cli_set_bstr(CLI_COMMANDNAME, c->d[1]) ? DOSTRUE : 0; }
static uint32_t f_SetCurrentDirName(M68K *c) { return cli_set_bstr(CLI_SETNAME, c->d[1]) ? DOSTRUE : 0; }
static uint32_t f_SetPrompt(M68K *c) { return cli_set_bstr(CLI_PROMPT, c->d[1]) ? DOSTRUE : 0; }
static uint32_t f_GetPrompt(M68K *c) { return cli_get_bstr(CLI_PROMPT, c->d[1], (int32_t)c->d[2]) ? DOSTRUE : 0; }

static uint32_t f_GetProgramDir(M68K *c) { (void)c; return rd32(this_task() + PR_HOMEDIR); }
static uint32_t f_SetProgramDir(M68K *c) {
    uint32_t old = rd32(this_task() + PR_HOMEDIR);
    wr32(this_task() + PR_HOMEDIR, c->d[1]);
    return old;
}
static uint32_t f_GetConsoleTask(M68K *c) { (void)c; uint32_t p = rd32(this_task() + PR_CONSOLETASK); return p ? p : dos_handler_port; }
static uint32_t f_SetConsoleTask(M68K *c) {
    uint32_t old = rd32(this_task() + PR_CONSOLETASK);
    wr32(this_task() + PR_CONSOLETASK, c->d[1]);
    return old;
}
static uint32_t f_GetFileSysTask(M68K *c) { (void)c; uint32_t p = rd32(this_task() + PR_FILESYSTEMTASK); return p ? p : dos_handler_port; }
static uint32_t f_SetFileSysTask(M68K *c) {
    uint32_t old = rd32(this_task() + PR_FILESYSTEMTASK);
    wr32(this_task() + PR_FILESYSTEMTASK, c->d[1]);
    return old;
}
static uint32_t f_ErrorOutput(M68K *c) { (void)c; uint32_t e = rd32(this_task() + PR_CES); return e ? e : cur_out(); }
static uint32_t f_SelectError(M68K *c) {
    uint32_t old = rd32(this_task() + PR_CES);
    wr32(this_task() + PR_CES, c->d[1]);
    return old;
}
static uint32_t f_PutErrStr(M68K *c) {
    char s[1024];
    str_get(c->d[1], s, sizeof s);
    uint32_t e = rd32(this_task() + PR_CES);
    fh_write_str(e ? e : cur_out(), s, (int)strlen(s));
    return 0;
}

/* FindCliProc(n): the process of CLI number n */
static uint32_t f_FindCliProc(M68K *c) {
    uint32_t n = c->d[1];
    if (!n) return 0;
    uint32_t lists[] = { SysBase + EB_TASKREADY, SysBase + EB_TASKWAIT };
    for (int l = 0; l < 2; l++)
        for (uint32_t t = rd32(lists[l]); t && rd32(t); t = rd32(t))
            if (rd8(t + LN_TYPE) == NT_PROCESS && rd32(t + PR_CLI) && rd32(t + PR_TASKNUM) == n) return t;
    uint32_t me = this_task();
    return me && rd32(me + PR_TASKNUM) == n && rd32(me + PR_CLI) ? me : 0;
}
static uint32_t f_MaxCli(M68K *c) { (void)c; return (uint32_t)(next_tasknum - 1); }

static uint32_t f_ErrorReport(M68K *c) {
    /* ErrorReport(code, type, arg1, device): print and tell the caller to give up */
    char msg[256];
    int n = snprintf(msg, sizeof msg, "%s\n", dos_fault_text((int)c->d[1]));
    uint32_t e = rd32(this_task() + PR_CES);
    fh_write_str(e ? e : cur_out(), msg, n);
    return DOSTRUE;
}

/* ---- running programs and commands -------------------------------------------- */

/* RunCommand(seglist, stacksize, argptr, argsize): run in this process */
static uint32_t f_RunCommand(M68K *c) {
    char args[2048];
    uint32_t len = c->d[4] < sizeof args - 1 ? c->d[4] : sizeof args - 1;
    for (uint32_t i = 0; i < len; i++) args[i] = (char)rd8(c->d[3] + i);
    args[len] = 0;
    char name[256] = "";
    uint32_t cli = cur_cli();
    if (cli) bstr_get(rd32(cli + CLI_COMMANDNAME), name, sizeof name);
    return (uint32_t)run_seglist(c->d[1], name, args, c->d[2], 0);
}

#define SYS_DUMMY 0x80000020
#define SYS_INPUT (SYS_DUMMY + 1)
#define SYS_OUTPUT (SYS_DUMMY + 2)
#define SYS_ASYNCH (SYS_DUMMY + 3)
#define SYS_USERSHELL (SYS_DUMMY + 4)
#define SYS_CUSTOMSHELL (SYS_DUMMY + 5)

static int shell_with_io(const char *cmd, uint32_t in, uint32_t out) {
    uint32_t pr = this_task(), oi = rd32(pr + PR_CIS), oo = rd32(pr + PR_COS);
    if (in) wr32(pr + PR_CIS, in);
    if (out) wr32(pr + PR_COS, out);
    int rc = 0;
    /* a command string may hold several lines */
    char line[1024];
    for (const char *p = cmd; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (line[0]) rc = shell_execute(line);
        if (rc == -2) { rc = 0; break; }
        if (!nl) break;
        p = nl + 1;
    }
    wr32(pr + PR_CIS, oi);
    wr32(pr + PR_COS, oo);
    return rc;
}

static uint32_t f_SystemTagList(M68K *c) {
    char cmd[2048];
    str_get(c->d[1], cmd, sizeof cmd);
    uint32_t in = 0, out = 0, async = 0;
    for (uint32_t t = c->d[2]; t;) {
        uint32_t tag = rd32(t), data = rd32(t + 4);
        if (tag == 0) break;                                    /* TAG_DONE */
        if (tag == 1) { t += 8; continue; }                     /* TAG_IGNORE */
        if (tag == 2) { t = data; continue; }                   /* TAG_MORE */
        if (tag == 3) { t += 8 + 8 * data; continue; }          /* TAG_SKIP */
        if (tag == SYS_INPUT) in = data;
        else if (tag == SYS_OUTPUT) out = data;
        else if (tag == SYS_ASYNCH) async = data;
        t += 8;
    }
    if (async) {
        shell_run_background(cmd);
        if (in && in != cur_in()) fh_close(in);
        if (out && out != cur_out() && out != in) fh_close(out);
        return 0;
    }
    int rc = shell_with_io(cmd, in, out);
    return (uint32_t)(rc < 0 ? -1 : rc);
}

/* Execute(command, input, output): TRUE once run */
static uint32_t f_Execute(M68K *c) {
    char cmd[2048];
    str_get(c->d[1], cmd, sizeof cmd);
    uint32_t in = c->d[2], out = c->d[3];
    if (cmd[0]) shell_with_io(cmd, 0, out);
    if (in) {                                                   /* more commands from the input */
        uint32_t pr = this_task(), oo = rd32(pr + PR_COS);
        if (out) wr32(pr + PR_COS, out);
        char line[1024];
        while (dos_read_line(in, line, sizeof line) >= 0)
            if (line[0] && shell_execute(line) == -2) break;
        wr32(pr + PR_COS, oo);
    }
    return DOSTRUE;
}

/* ---- segments ------------------------------------------------------------------ */

typedef struct { char name[32]; uint32_t seg; int uc; } Resident;
static Resident residents[32];
static uint32_t res_nodes[32];

static uint32_t f_AddSegment(M68K *c) {
    char name[32];
    str_get(c->d[1], name, sizeof name);
    for (int i = 0; i < 32; i++)
        if (!residents[i].seg) {
            snprintf(residents[i].name, sizeof residents[i].name, "%s", name);
            residents[i].seg = c->d[2];
            residents[i].uc = (int)c->d[3];
            /* Segment: seg_Next, seg_UC, seg_Seg, seg_Name (BSTR) */
            uint32_t s = exec_allocmem(48, MEMF_PUBLIC | MEMF_CLEAR);
            wr32(s + 4, c->d[3]);
            wr32(s + 8, c->d[2]);
            wr8(s + 12, (uint8_t)strlen(name));
            for (size_t k = 0; k < strlen(name); k++) wr8(s + 13 + k, (uint8_t)name[k]);
            res_nodes[i] = s;
            return DOSTRUE;
        }
    return 0;
}
static uint32_t f_FindSegment(M68K *c) {
    char name[32];
    str_get(c->d[1], name, sizeof name);
    int start = 0;
    if (c->d[2]) for (int i = 0; i < 32; i++) if (res_nodes[i] == c->d[2]) start = i + 1;
    for (int i = start; i < 32; i++)
        if (residents[i].seg && !strcasecmp(residents[i].name, name)) return res_nodes[i];
    return 0;
}
static uint32_t f_RemSegment(M68K *c) {
    for (int i = 0; i < 32; i++)
        if (res_nodes[i] && res_nodes[i] == c->d[1]) {
            exec_freemem(res_nodes[i], 48);
            res_nodes[i] = 0;
            residents[i].seg = 0;
            return DOSTRUE;
        }
    return 0;
}

/* NewLoadSeg(file, tags) */
static uint32_t f_NewLoadSeg(M68K *c) {
    char hp[1024];
    if (!name_arg(c, c->d[1], hp, sizeof hp)) return 0;
    return dos_loadseg(hp);
}

/* InternalLoadSeg(fh, table, funcs, stack): the hunks from an open file */
static uint32_t f_InternalLoadSeg(M68K *c) {
    HostFile *h = fh_get(c->d[0]);
    if (!h || !h->path) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    return dos_loadseg(h->path);
}
static uint32_t f_InternalUnLoadSeg(M68K *c) { dos_unloadseg(c->d[1]); return DOSTRUE; }

/* ---- assigns and the DOS list ---------------------------------------------------- */

static uint32_t f_AssignLock(M68K *c) {
    char name[64];
    str_get(c->d[1], name, sizeof name);
    if (!c->d[2]) {                                             /* remove */
        for (int i = 0; i < nassigns; i++)
            if (!strcasecmp(assigns[i].name, name)) { assigns[i] = assigns[--nassigns], dol_dirty = 1; break; }
        return DOSTRUE;
    }
    const char *p = lock_path(c->d[2]);
    if (!p) return 0;
    assign_set(name, p);
    lock_free(c->d[2]);                                         /* the assign owns the lock */
    return DOSTRUE;
}
static uint32_t f_AssignPath(M68K *c) {
    char name[64], path[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    str_get(c->d[2], path, sizeof path);
    if (!dos_resolve(path, hp, sizeof hp)) return 0;
    assign_set(name, hp);
    return DOSTRUE;
}
static uint32_t f_AssignLate(M68K *c) { return f_AssignPath(c); }
static uint32_t f_AssignAdd(M68K *c) {
    /* multi-directory assigns: the first directory stays */
    char name[64];
    str_get(c->d[1], name, sizeof name);
    if (!assign_get(name)) return f_AssignLock(c);
    lock_free(c->d[2]);
    return DOSTRUE;
}
static uint32_t f_RemAssignList(M68K *c) { (void)c; return DOSTRUE; }

/* DosList entries rebuilt from the assigns: dol_Next, dol_Type, dol_Task, dol_Lock, ..., dol_Name */
#define DLT_DEVICE 0
#define DLT_DIRECTORY 1
#define DLT_VOLUME 2
#define LDF_DEVICES 4
#define LDF_VOLUMES 8
#define LDF_ASSIGNS 16
#define DOL_SIZE 48
static uint32_t dol_entries[96];
static int ndol;
static uint32_t dos_info;                  /* DosInfo: the list programs walk directly */

static uint32_t make_dol(const char *name, int type, const char *path) {
    uint32_t d = exec_allocmem(DOL_SIZE + 64, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(d + 4, (uint32_t)type);
    wr32(d + 8, dos_handler_port);
    if (type == DLT_DIRECTORY) wr32(d + 12, lock_new(path, SHARED_LOCK));
    if (type == DLT_VOLUME) { unix_to_datestamp(time(0), d + 16); wr32(d + 32, 0x444F5301); }   /* dol_DiskType 'DOS\1' */
    uint32_t b = d + DOL_SIZE;
    int n = (int)strlen(name);
    if (n > 62) n = 62;
    wr8(b, (uint8_t)n);
    for (int i = 0; i < n; i++) wr8(b + 1 + i, (uint8_t)name[i]);
    wr32(d + 40, MKBADDR(b));
    return d;
}

static int is_mount_name(const char *n) {
    for (int i = 0; i < 4; i++)
        if (mounts[i].device[0] && (!strcasecmp(n, mounts[i].device) || !strcasecmp(n, mounts[i].volume))) return 1;
    return 0;
}

static void dol_rebuild(void) {
    dol_dirty = 0;
    for (int i = 0; i < ndol; i++) {
        uint32_t d = dol_entries[i];
        if (rd32(d + 4) == DLT_DIRECTORY && rd32(d + 12)) lock_free(rd32(d + 12));
        exec_freemem(d, DOL_SIZE + 64);
    }
    ndol = 0;
    dol_entries[ndol++] = make_dol("DH0", DLT_DEVICE, 0);
    dol_entries[ndol++] = make_dol("RAM", DLT_DEVICE, 0);
    dol_entries[ndol++] = make_dol("Mac", DLT_VOLUME, "/");
    dol_entries[ndol++] = make_dol("Ram Disk", DLT_VOLUME, ram_dir);
    dol_entries[ndol++] = make_dol("System", DLT_VOLUME, sys_dir);
    for (int i = 0; i < 4; i++)
        if (mounts[i].device[0]) {
            dol_entries[ndol++] = make_dol(mounts[i].device, DLT_DEVICE, 0);
            dol_entries[ndol++] = make_dol(mounts[i].volume, DLT_VOLUME, mounts[i].path);
        }
    /* in alphabetical order: programs that sort the list themselves (Directory
       Opus 4 swaps only one byte per exchange) then find nothing to move */
    int order[64], n = 0;
    for (int i = 0; i < nassigns; i++)
        if (strcasecmp(assigns[i].name, "RAM") && strcasecmp(assigns[i].name, "Mac") && strcasecmp(assigns[i].name, "DH0") && !is_mount_name(assigns[i].name)) order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcasecmp(assigns[order[j - 1]].name, assigns[order[j]].name) > 0; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (int i = 0; i < n && ndol < 96; i++)
        dol_entries[ndol++] = make_dol(assigns[order[i]].name, DLT_DIRECTORY, assigns[order[i]].path);
    for (int i = 0; i < ndol; i++) wr32(dol_entries[i], i + 1 < ndol ? MKBADDR(dol_entries[i + 1]) : 0);
    if (dos_info) wr32(dos_info + 4, ndol ? MKBADDR(dol_entries[0]) : 0);   /* di_DevInfo */
}

static int dol_wanted(uint32_t d, uint32_t flags) {
    uint32_t t = rd32(d + 4);
    return (t == DLT_DEVICE && (flags & LDF_DEVICES)) || (t == DLT_VOLUME && (flags & LDF_VOLUMES)) ||
           (t == DLT_DIRECTORY && (flags & LDF_ASSIGNS));
}

static uint32_t f_LockDosList(M68K *c) { (void)c; if (dol_dirty) dol_rebuild(); return DOSBase; }   /* any non-zero marker */
static uint32_t f_UnLockDosList(M68K *c) { (void)c; return 0; }
static uint32_t f_AttemptLockDosList(M68K *c) { return f_LockDosList(c); }

static uint32_t f_NextDosEntry(M68K *c) {
    uint32_t flags = c->d[2];
    int i = 0;
    if (c->d[1] != DOSBase) {
        for (i = 0; i < ndol && dol_entries[i] != c->d[1]; i++) ;
        i++;
    }
    for (; i < ndol; i++) if (dol_wanted(dol_entries[i], flags)) return dol_entries[i];
    return 0;
}

static uint32_t f_FindDosEntry(M68K *c) {
    char name[64];
    str_get(c->d[2], name, sizeof name);
    char *colon = strchr(name, ':');
    if (colon) *colon = 0;
    int i = 0;
    if (c->d[1] != DOSBase) {
        for (i = 0; i < ndol && dol_entries[i] != c->d[1]; i++) ;
        i++;
    }
    for (; i < ndol; i++) {
        char n[64];
        bstr_get(rd32(dol_entries[i] + 40), n, sizeof n);
        if (dol_wanted(dol_entries[i], c->d[3]) && !strcasecmp(n, name)) return dol_entries[i];
    }
    return 0;
}

static uint32_t f_MakeDosEntry(M68K *c) {
    char name[64];
    str_get(c->d[1], name, sizeof name);
    return make_dol(name, (int)c->d[2], 0);
}
static uint32_t f_FreeDosEntry(M68K *c) { if (c->d[1]) exec_freemem(c->d[1], DOL_SIZE + 64); return 0; }
static uint32_t f_AddDosEntry(M68K *c) {
    char name[64];
    bstr_get(rd32(c->d[1] + 40), name, sizeof name);
    if (rd32(c->d[1] + 4) == DLT_DIRECTORY && lock_path(rd32(c->d[1] + 12)))
        assign_set(name, lock_path(rd32(c->d[1] + 12)));
    return DOSTRUE;
}
static uint32_t f_RemDosEntry(M68K *c) {
    char name[64];
    bstr_get(rd32(c->d[1] + 40), name, sizeof name);
    for (int i = 0; i < nassigns; i++)
        if (!strcasecmp(assigns[i].name, name)) { assigns[i] = assigns[--nassigns], dol_dirty = 1; return DOSTRUE; }
    return DOSTRUE;
}

/* GetDeviceProc(name, prev): DevProc {dvp_Port, dvp_Lock, dvp_Flags, dvp_DevNode} */
static uint32_t f_GetDeviceProc(M68K *c) {
    if (c->d[2]) { exec_freemem(c->d[2], 16); dos_set_ioerr(ERROR_NO_MORE_ENTRIES); return 0; }
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    uint32_t dp = exec_allocmem(16, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(dp, dos_handler_port);
    return dp;
}
static uint32_t f_FreeDeviceProc(M68K *c) { if (c->d[1]) exec_freemem(c->d[1], 16); return 0; }
static uint32_t f_DeviceProc(M68K *c) {
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    return dos_resolve(name, hp, sizeof hp) ? dos_handler_port : 0;
}

static uint32_t f_Inhibit(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t f_AddBuffers(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t f_Relabel(M68K *c) { (void)c; dos_set_ioerr(ERROR_ACTION_NOT_KNOWN); return 0; }
static uint32_t f_Format(M68K *c) { (void)c; dos_set_ioerr(ERROR_ACTION_NOT_KNOWN); return 0; }
static uint32_t f_StartNotify(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t f_EndNotify(M68K *c) { (void)c; return 0; }
static uint32_t f_AbortPkt(M68K *c) { (void)c; return 0; }

/* links: hard links and soft links on the host */
static uint32_t f_MakeLink(M68K *c) {
    char hp[1024];
    if (!name_arg(c, c->d[1], hp, sizeof hp)) return 0;
    if (c->d[3]) {                                             /* soft: D2 is a path */
        char dest[512];
        str_get(c->d[2], dest, sizeof dest);
        char hd[1024];
        if (!dos_resolve(dest, hd, sizeof hd)) return 0;
        if (symlink(hd, hp)) { dos_set_ioerr(host_err()); return 0; }
    } else {
        const char *p = lock_path(c->d[2]);
        if (!p || link(p, hp)) { dos_set_ioerr(p ? host_err() : ERROR_OBJECT_NOT_FOUND); return 0; }
    }
    return DOSTRUE;
}
static uint32_t f_ReadLink(M68K *c) {
    /* ReadLink(port, lock, path, buffer, size) */
    const char *base = lock_path(c->d[2]);
    char path[512], hp[2048], target[1024], an[1024];
    str_get(c->d[3], path, sizeof path);
    snprintf(hp, sizeof hp, "%s/%s", base ? base : sys_dir, strchr(path, ':') ? strchr(path, ':') + 1 : path);
    ssize_t n = readlink(hp, target, sizeof target - 1);
    if (n < 0) { dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE); return 0; }
    target[n] = 0;
    amiga_name(target, an, sizeof an);
    if (strlen(an) + 1 > c->d[5]) { dos_set_ioerr(ERROR_LINE_TOO_LONG); return 0; }
    for (size_t i = 0; i <= strlen(an); i++) wr8(c->d[4] + i, (uint8_t)an[i]);
    return DOSTRUE;
}

static const NativeEntry dos_natives[] = {
    { "Open", f_Open }, { "Close", f_Close }, { "Read", f_Read }, { "Write", f_Write },
    { "Seek", f_Seek }, { "Input", f_Input }, { "Output", f_Output },
    { "SelectInput", f_SelectInput }, { "SelectOutput", f_SelectOutput },
    { "IsInteractive", f_IsInteractive }, { "Flush", f_Flush },
    { "FGetC", f_FGetC }, { "UnGetC", f_UnGetC }, { "FPutC", f_FPutC }, { "FPuts", f_FPuts },
    { "PutStr", f_PutStr }, { "WriteChars", f_WriteChars }, { "FGets", f_FGets },
    { "FRead", f_FRead }, { "FWrite", f_FWrite },
    { "VPrintf", f_VPrintf }, { "VFPrintf", f_VFPrintf }, { "VFWritef", f_VFWritef },
    { "Lock", f_Lock }, { "UnLock", f_UnLock }, { "DupLock", f_DupLock },
    { "CurrentDir", f_CurrentDir }, { "ParentDir", f_ParentDir },
    { "Examine", f_Examine }, { "ExNext", f_ExNext }, { "ExamineFH", f_ExamineFH },
    { "NameFromLock", f_NameFromLock }, { "CreateDir", f_CreateDir },
    { "DeleteFile", f_DeleteFile }, { "Rename", f_Rename }, { "Info", f_Info },
    { "IoErr", f_IoErr }, { "SetIoErr", f_SetIoErr }, { "Fault", f_Fault },
    { "PrintFault", f_PrintFault }, { "Delay", f_Delay }, { "DateStamp", f_DateStamp },
    { "CheckSignal", f_CheckSignal }, { "WaitForChar", f_WaitForChar }, { "SetMode", f_SetMode },
    { "FilePart", f_FilePart }, { "PathPart", f_PathPart }, { "AddPart", f_AddPart },
    { "StrToLong", f_StrToLong },
    { "GetVar", f_GetVar }, { "SetVar", f_SetVar }, { "DeleteVar", f_DeleteVar }, { "FindVar", f_FindVar },
    { "LoadSeg", f_LoadSeg }, { "UnLoadSeg", f_UnLoadSeg },
    { "ReadArgs", f_ReadArgs }, { "FreeArgs", f_FreeArgs },
    { "AllocDosObject", f_AllocDosObject }, { "FreeDosObject", f_FreeDosObject },
    { "Exit", f_Exit }, { "GetArgStr", f_GetArgStr }, { "SetArgStr", f_SetArgStr },
    { "Cli", f_Cli }, { "GetProgramName", f_GetProgramName },
    { "GetCurrentDirName", f_GetCurrentDirName },
    { "CreateNewProc", f_CreateNewProc }, { "CreateProc", f_CreateProc },
    { "ParsePattern", f_ParsePattern }, { "ParsePatternNoCase", f_ParsePatternNoCase },
    { "MatchPattern", f_MatchPattern }, { "MatchPatternNoCase", f_MatchPatternNoCase },
    { "SendPkt", f_SendPkt }, { "WaitPkt", f_WaitPkt }, { "ReplyPkt", f_ReplyPkt }, { "DoPkt", f_DoPkt },
    { "DupLockFromFH", f_DupLockFromFH }, { "ParentOfFH", f_ParentOfFH }, { "NameFromFH", f_NameFromFH },
    { "DateToStr", f_DateToStr }, { "StrToDate", f_StrToDate },
    { "MatchFirst", f_MatchFirst }, { "MatchNext", f_MatchNext }, { "MatchEnd", f_MatchEnd },
    { "SetProtection", f_SetProtection },
    { "SetComment", f_SetComment },
    { "SetFileDate", f_SetFileDate },
    { "SetFileSize", f_SetFileSize },
    { "CompareDates", f_CompareDates },
    { "SetOwner", f_SetOwner },
    { "OpenFromLock", f_OpenFromLock },
    { "SameLock", f_SameLock },
    { "SameDevice", f_SameDevice },
    { "ChangeMode", f_ChangeMode },
    { "SetVBuf", f_SetVBuf },
    { "LockRecord", f_LockRecord },
    { "UnLockRecord", f_UnLockRecord },
    { "IsFileSystem", f_IsFileSystem },
    { "GetCurrentDir", f_GetCurrentDir },
    { "ExAll", f_ExAll },
    { "ExAllEnd", f_ExAllEnd },
    { "ReadItem", f_ReadItem },
    { "FindArg", f_FindArg },
    { "SplitName", f_SplitName },
    { "SetProgramName", f_SetProgramName },
    { "SetCurrentDirName", f_SetCurrentDirName },
    { "SetPrompt", f_SetPrompt },
    { "GetPrompt", f_GetPrompt },
    { "GetProgramDir", f_GetProgramDir },
    { "SetProgramDir", f_SetProgramDir },
    { "GetConsoleTask", f_GetConsoleTask },
    { "SetConsoleTask", f_SetConsoleTask },
    { "GetFileSysTask", f_GetFileSysTask },
    { "SetFileSysTask", f_SetFileSysTask },
    { "ErrorOutput", f_ErrorOutput },
    { "SelectError", f_SelectError },
    { "PutErrStr", f_PutErrStr },
    { "FindCliProc", f_FindCliProc },
    { "MaxCli", f_MaxCli },
    { "ErrorReport", f_ErrorReport },
    { "RunCommand", f_RunCommand },
    { "SystemTagList", f_SystemTagList },
    { "Execute", f_Execute },
    { "AddSegment", f_AddSegment },
    { "FindSegment", f_FindSegment },
    { "RemSegment", f_RemSegment },
    { "NewLoadSeg", f_NewLoadSeg },
    { "InternalLoadSeg", f_InternalLoadSeg },
    { "InternalUnLoadSeg", f_InternalUnLoadSeg },
    { "AssignLock", f_AssignLock },
    { "AssignPath", f_AssignPath },
    { "AssignLate", f_AssignLate },
    { "AssignAdd", f_AssignAdd },
    { "RemAssignList", f_RemAssignList },
    { "LockDosList", f_LockDosList },
    { "UnLockDosList", f_UnLockDosList },
    { "AttemptLockDosList", f_AttemptLockDosList },
    { "NextDosEntry", f_NextDosEntry },
    { "FindDosEntry", f_FindDosEntry },
    { "MakeDosEntry", f_MakeDosEntry },
    { "FreeDosEntry", f_FreeDosEntry },
    { "AddDosEntry", f_AddDosEntry },
    { "RemDosEntry", f_RemDosEntry },
    { "GetDeviceProc", f_GetDeviceProc },
    { "FreeDeviceProc", f_FreeDeviceProc },
    { "DeviceProc", f_DeviceProc },
    { "Inhibit", f_Inhibit },
    { "AddBuffers", f_AddBuffers },
    { "Relabel", f_Relabel },
    { "Format", f_Format },
    { "StartNotify", f_StartNotify },
    { "EndNotify", f_EndNotify },
    { "AbortPkt", f_AbortPkt },
    { "MakeLink", f_MakeLink },
    { "ReadLink", f_ReadLink },
    { "LockRecords", f_LockRecord }, { "UnLockRecords", f_UnLockRecord },
    { 0, 0 }
};

/* ---- boot ------------------------------------------------------------------ */

void dos_init(const char *sysdir) {
    snprintf(sys_dir, sizeof sys_dir, "%s", sysdir);
    dos_handler_port = exec_allocmem(MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(dos_handler_port + LN_TYPE, NT_MSGPORT);
    wr32(dos_handler_port + LN_NAME, str_alloc("AmigaMac.handler"));
    wr8(dos_handler_port + MP_FLAGS, 2);                          /* PA_IGNORE: answered natively */
    list_new(dos_handler_port + MP_MSGLIST);
    snprintf(ram_dir, sizeof ram_dir, "%s/.ram", sysdir);
    mkdir(ram_dir, 0755);
    char p[1100];
    nassigns = 0;
    assign_set("SYS", sys_dir);
    assign_set("DH0", sys_dir);                                /* the device holding the System disk */
    assign_set("RAM", ram_dir);
    assign_set("Mac", "/");
    static const char *dirs[] = { "C", "Libs", "Devs", "S", "L", "Fonts", "Prefs", "Utilities", "Tools", "Locale" };
    static const char *drawers[] = { "System", "Storage", "WBStartup", "Expansion", "Trashcan" };   /* no assign */
    for (unsigned i = 0; i < sizeof drawers / sizeof *drawers; i++) {
        snprintf(p, sizeof p, "%s/%s", sys_dir, drawers[i]);
        mkdir(p, 0755);
    }
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(p, sizeof p, "%s/%s", sys_dir, dirs[i]);
        mkdir(p, 0755);
        assign_set(dirs[i], p);
    }
    snprintf(p, sizeof p, "%s/T", ram_dir); mkdir(p, 0755); assign_set("T", p);
    snprintf(p, sizeof p, "%s/Env", ram_dir); mkdir(p, 0755); assign_set("ENV", p);
    snprintf(p, sizeof p, "%s/Prefs/Env-Archive", sys_dir); mkdir(p, 0755); assign_set("ENVARC", p);
    /* as the Startup-Sequence's "Copy ENVARC: ENV:": preferences (and default icons) of ENVARC:Sys */
    {
        char es[1100], as[1100], f1[1400], f2[1400];
        snprintf(es, sizeof es, "%s/Env/Sys", ram_dir);
        snprintf(as, sizeof as, "%s/Prefs/Env-Archive/Sys", sys_dir);
        mkdir(es, 0755);
        DIR *d = opendir(es);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (strstr(e->d_name, ".prefs") || strstr(e->d_name, ".info")) { snprintf(f1, sizeof f1, "%s/%s", es, e->d_name); remove(f1); }
        if (d) closedir(d);
        d = opendir(as);
        while (d && (e = readdir(d))) {
            if (!strstr(e->d_name, ".prefs") && !strstr(e->d_name, ".info")) continue;
            snprintf(f1, sizeof f1, "%s/%s", as, e->d_name);
            snprintf(f2, sizeof f2, "%s/%s", es, e->d_name);
            FILE *in = fopen(f1, "rb"), *out = in ? fopen(f2, "wb") : 0;
            char buf[4096];
            size_t n;
            while (out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            if (in) fclose(in);
            if (out) fclose(out);
        }
        if (d) closedir(d);
    }

    DOSBase = lib_create("dos.library", "dos", DL_SIZE, dos_natives, 40, 1, "dos 40.1 (AmigaMac)");
    lib_add(DOSBase);
    /* RootNode {.., rn_Time 8, rn_Info 24 (BPTR DosInfo), rn_CliList 32 ..} and DosInfo {di_DevInfo 4} */
    uint32_t root = exec_allocmem(56, MEMF_PUBLIC | MEMF_CLEAR);
    dos_info = exec_allocmem(40, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(root + 24, MKBADDR(dos_info));
    list_new(root + 32);
    wr32(DOSBase + DL_ROOT, root);
    dol_rebuild();
}

/* ---- processes ------------------------------------------------------------ */

/* a Process with its own CLI structure, sharing the console */
static uint32_t new_process(const char *name, uint32_t stack, int8_t priority, int with_cli,
                            uint32_t in, uint32_t out, uint32_t dir) {
    uint32_t pr = exec_create_process(name, stack);
    wr8(pr + LN_PRI, (uint8_t)priority);
    wr32(pr + PR_CIS, in);
    wr32(pr + PR_COS, out);
    wr32(pr + PR_CES, out);
    wr32(pr + PR_CURRENTDIR, dir);
    if (with_cli) {
        uint32_t cli = exec_allocmem(CLI_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        wr32(cli + CLI_COMMANDNAME, MKBADDR(exec_allocmem(256, MEMF_PUBLIC | MEMF_CLEAR)));
        wr32(cli + CLI_SETNAME, MKBADDR(exec_allocmem(256, MEMF_PUBLIC | MEMF_CLEAR)));
        wr32(cli + CLI_PROMPT, MKBADDR(exec_allocmem(256, MEMF_PUBLIC | MEMF_CLEAR)));
        wr32(cli + CLI_DEFAULTSTACK, 16384);          /* longwords: 64 KB */
        wr32(cli + CLI_FAILLEVEL, 10);
        wr32(cli + CLI_INTERACTIVE, DOSTRUE);
        wr32(cli + CLI_STANDARDINPUT, in);
        wr32(cli + CLI_STANDARDOUTPUT, out);
        wr32(cli + CLI_CURRENTINPUT, in);
        wr32(cli + CLI_CURRENTOUTPUT, out);
        wr32(pr + PR_CLI, MKBADDR(cli));
        wr32(pr + PR_TASKNUM, (uint32_t)next_tasknum++);
    }
    return pr;
}

/* a CLI process running native code (the Shell, RUN) on the shared console */
uint32_t dos_create_cli(const char *name, void (*body)(void *), void *arg) {
    uint32_t parent = this_task();
    const char *dirpath = parent ? lock_path(rd32(parent + PR_CURRENTDIR)) : 0;
    uint32_t dir = lock_new(dirpath ? dirpath : sys_dir, SHARED_LOCK);
    uint32_t pr = new_process(name, 65536, 0, 1, fh_new(0, 1), fh_new(0, 1), dir);
    sched_add(pr, body, arg, 0, 0);
    return pr;
}

/* a CLI process on given input/output handles (NEWSHELL) */
uint32_t dos_create_cli_io(const char *name, void (*body)(void *), void *arg, uint32_t in, uint32_t out) {
    uint32_t parent = this_task();
    const char *dirpath = parent ? lock_path(rd32(parent + PR_CURRENTDIR)) : 0;
    uint32_t dir = lock_new(dirpath ? dirpath : sys_dir, SHARED_LOCK);
    uint32_t pr = new_process(name, 65536, 0, 1, in, out, dir);
    sched_add(pr, body, arg, 0, 0);
    return pr;
}

/* a console window as a pair of handles sharing it */
int dos_open_console(M68K *c, const char *spec, uint32_t *in, uint32_t *out) {
    Con *k = con_open(c, spec, 0);
    if (!k) return 0;
    con_ref(k);
    *in = fh_new_con(k);
    *out = fh_new_con(k);
    return 1;
}

/* entry stub for CreateNewProc'd code: D0/A0 = arguments, return ends it */
typedef struct { uint32_t seglist; int freeseg; uint32_t args; } ProcStart;

static void proc_body(void *arg) {
    ProcStart *ps = arg;
    NTask *me = sched_current();
    M68K *c = &me->cpu;
    uint32_t pr = me->task;
    c->a[7] = rd32(pr + TC_SPUPPER);
    cpu68k_push32(c, rd32(pr + PR_STACKSIZE));
    cpu68k_push32(c, stub_exit);
    char line[1024];
    str_get(ps->args, line, sizeof line);
    c->d[0] = (uint32_t)strlen(line);
    c->a[0] = ps->args;
    c->pc = me->initpc;
    sched_run_cpu(c);
    /* process end: close what dos opened for it, free the code if asked */
    if (ps->freeseg && ps->seglist) dos_unloadseg(ps->seglist);
    free(ps);
}

static uint32_t tag_get(uint32_t tags, uint32_t tag, uint32_t def) {
    /* utility TagItem walk: TAG_DONE 0, TAG_IGNORE 1, TAG_MORE 2, TAG_SKIP 3 */
    while (tags) {
        uint32_t t = rd32(tags), d = rd32(tags + 4);
        if (t == 0) break;
        if (t == 2) { tags = d; continue; }
        if (t == 3) { tags += 8 * (d + 1); continue; }
        if (t == tag) return d;
        tags += 8;
    }
    return def;
}

#define NP_Seglist 0x800003E9
#define NP_FreeSeglist 0x800003EA
#define NP_Entry 0x800003EB
#define NP_Input 0x800003EC
#define NP_Output 0x800003ED
#define NP_CloseInput 0x800003EE
#define NP_CloseOutput 0x800003EF
#define NP_Error 0x800003F0
#define NP_CloseError 0x800003F1
#define NP_CurrentDir 0x800003F2
#define NP_StackSize 0x800003F3
#define NP_Name 0x800003F4
#define NP_Priority 0x800003F5
#define NP_ConsoleTask 0x800003F6
#define NP_WindowPtr 0x800003F7
#define NP_HomeDir 0x800003F8
#define NP_CopyVars 0x800003F9
#define NP_Cli 0x800003FA
#define NP_Path 0x800003FB
#define NP_CommandName 0x800003FC
#define NP_Arguments 0x800003FD

static uint32_t create_proc(uint32_t tags) {
    uint32_t seg = tag_get(tags, NP_Seglist, 0), entry = tag_get(tags, NP_Entry, 0);
    if (!seg && !entry) { dos_set_ioerr(ERROR_REQUIRED_ARG_MISSING); return 0; }
    char name[128] = "New Process";
    uint32_t np = tag_get(tags, NP_Name, 0);
    if (np) str_get(np, name, sizeof name);
    uint32_t stack = tag_get(tags, NP_StackSize, 4000);
    if (stack < 4096) stack = 4096;
    int8_t prio = (int8_t)tag_get(tags, NP_Priority, 0);
    uint32_t in = tag_get(tags, NP_Input, 0xFFFFFFFF), out = tag_get(tags, NP_Output, 0xFFFFFFFF);
    if (in == 0xFFFFFFFF) in = fh_new(fopen("/dev/null", "r"), 0);      /* default: NIL: */
    if (out == 0xFFFFFFFF) out = fh_new(fopen("/dev/null", "w"), 0);
    uint32_t parent = this_task();
    uint32_t dir = tag_get(tags, NP_CurrentDir, 0xFFFFFFFF);
    if (dir == 0xFFFFFFFF) {
        const char *p = parent ? lock_path(rd32(parent + PR_CURRENTDIR)) : 0;
        dir = p ? lock_new(p, SHARED_LOCK) : 0;
    }
    uint32_t pr = new_process(name, stack, prio, (int)tag_get(tags, NP_Cli, 0), in, out, dir);
    wr32(pr + PR_HOMEDIR, tag_get(tags, NP_HomeDir, 0));
    ProcStart *ps = calloc(1, sizeof *ps);
    ps->seglist = seg;
    ps->freeseg = (int)tag_get(tags, NP_FreeSeglist, 1);
    uint32_t args = tag_get(tags, NP_Arguments, 0);
    char line[1024] = "";
    if (args) str_get(args, line, sizeof line);
    ps->args = str_alloc(line);
    wr32(pr + PR_ARGUMENTS, ps->args);
    wr32(pr + PR_SEGLIST, 0);
    sched_add(pr, proc_body, ps, entry ? entry : BADDR(seg) + 4, 0);
    /* on a real Amiga the parent's time slice usually runs out while it sets
       up: programs count on the child having started (made its port...) */
    sched_timeslice();
    return pr;
}

static uint32_t f_CreateNewProc(M68K *c) { return create_proc(c->d[1]); }

/* CreateProc(name, pri, seglist, stacksize): returns the process's MsgPort */
static uint32_t f_CreateProc(M68K *c) {
    uint32_t tags = exec_allocmem(8 * 6, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(tags, NP_Name); wr32(tags + 4, c->d[1]);
    wr32(tags + 8, NP_Priority); wr32(tags + 12, c->d[2]);
    wr32(tags + 16, NP_Seglist); wr32(tags + 20, c->d[3]);
    wr32(tags + 24, NP_StackSize); wr32(tags + 28, c->d[4]);
    wr32(tags + 32, NP_FreeSeglist); wr32(tags + 36, 0);
    uint32_t pr = create_proc(tags);
    exec_freemem(tags, 8 * 6);
    return pr ? pr + PR_MSGPORT : 0;
}

/* shell support: open/close host-backed handles for redirection */
uint32_t dos_open_host(const char *apath, int mode) {
    char hp[1024];
    if (!strncasecmp(apath, "NIL:", 4)) return fh_new(fopen("/dev/null", mode ? "wb" : "rb"), 0);
    if (!strncasecmp(apath, "CON:", 4) || !strncasecmp(apath, "RAW:", 4)) {    /* >CON:x/y/w/h/title */
        NTask *me = sched_current();
        Con *k = me ? con_open(&me->cpu, apath + 4, toupper((unsigned char)apath[0]) == 'R') : 0;
        return k ? fh_new_con(k) : 0;
    }
    if (!dos_resolve(apath, hp, sizeof hp)) return 0;
    FILE *f = fopen(hp, mode == 2 ? "ab" : mode ? "wb" : "rb");
    if (!f) { dos_set_ioerr(host_err()); return 0; }
    return fh_new(f, 0);
}
void dos_close_handle(uint32_t bfh) { fh_close(bfh); }
void dos_write_handle(uint32_t bfh, const char *s, int len) { fh_write_str(bfh, s, len); }
/* one line from a handle, native side (the Shell's input) */
int dos_read_line(uint32_t bfh, char *buf, int max) {
    HostFile *h = fh_get(bfh);
    if (!h) return -1;
    if (h->console) {
        int n = h->con ? con_read(h->con, buf, max - 1) : host_con_read(buf, max - 1);
        buf[n > 0 ? n : 0] = 0;
        return n;
    }
    if (!fgets(buf, max, h->f)) return -1;
    return (int)strlen(buf);
}
const char *dos_assign_path(const char *name) { return assign_get(name); }
void dos_assign(const char *name, const char *path) { assign_set(name, path); if (DOSBase) dol_rebuild(); }

/* a disk image's contents as the device (DF0) and the volume (the disk's name) */
void dos_mount(const char *device, const char *volume, const char *path) {
    int k = 0;
    while (k < 4 && mounts[k].device[0] && strcasecmp(mounts[k].device, device)) k++;
    if (k == 4) return;
    snprintf(mounts[k].device, sizeof mounts[k].device, "%s", device);
    snprintf(mounts[k].volume, sizeof mounts[k].volume, "%s", volume);
    snprintf(mounts[k].path, sizeof mounts[k].path, "%s", path);
    assign_set(device, path);
    assign_set(volume, path);
    dol_dirty = 1;
}
void dos_unmount(const char *device, const char *volume) {
    for (int k = 0; k < 4; k++)
        if (!strcasecmp(mounts[k].device, device)) memset(&mounts[k], 0, sizeof mounts[k]);
    assign_del(device);
    assign_del(volume);
    dol_dirty = 1;
}
const char *dos_system_dir(void) { return sys_dir; }
void dos_name_latin1(const char *host, char *out, int max) { name_to_amiga(host, out, max); }
void dos_name_host(const char *latin1, char *out, int max) { name_to_host(latin1, out, max); }
int dos_assign_count(void) { return nassigns; }
const char *dos_assign_name(int i) { return assigns[i].name; }
const char *dos_assign_target(int i) { return assigns[i].path; }
uint32_t dos_lock(const char *apath) { return lock_by_name(apath, SHARED_LOCK); }
void dos_unlock(uint32_t bl) { lock_free(bl); }
const char *dos_lock_host_path(uint32_t bl) { return lock_path(bl); }
void dos_name_to_amiga(const char *in, char *out, int max) { name_to_amiga(in, out, max); }
void dos_amiga_name(const char *hp, char *out, int max) { char u[2048]; amiga_name(hp, u, sizeof u); name_to_amiga(u, out, max); }

/* ---- running a program in the CLI process ----------------------------------- */

static int guru;

static int on_exception(M68K *c, int vec) {
    /* tc_TrapCode dispatch comes later; until then: guru */
    char dump[300];
    cpu68k_dump(c, dump, sizeof dump);
    os_log("Software Failure : exception %d (#%08X) à $%06X\n%s", vec, 0x80000000u | vec, c->insn_pc, dump);
    guru = vec;
    c->stop = 2;
    return 1;
}

/* run a loaded program in the current process (the shell's commands, RunCommand) */
static int run_seglist(uint32_t seg, const char *name, const char *args, uint32_t stacksize, const char *homedir) {
    NTask *me = sched_current();
    uint32_t pr = me->task, cli = cur_cli();
    if (!stacksize) stacksize = cli ? rd32(cli + CLI_DEFAULTSTACK) * 4 : 65536;
    if (stacksize < 4096) stacksize = 4096;
    uint32_t stack = exec_allocmem(stacksize, MEMF_PUBLIC);
    /* argument line ends with a newline, as on AmigaOS */
    char line[2048];
    snprintf(line, sizeof line, "%s%s", args ? args : "", args && strchr(args, '\n') ? "" : "\n");
    uint32_t argstr = str_alloc(line), old_args = rd32(pr + PR_ARGUMENTS);
    wr32(pr + PR_ARGUMENTS, argstr);
    uint32_t old_module = 0;
    if (cli) {
        uint32_t bname = BADDR(rd32(cli + CLI_COMMANDNAME));
        int nl = (int)strlen(name) > 254 ? 254 : (int)strlen(name);
        wr8(bname, nl);
        for (int i = 0; i < nl; i++) wr8(bname + 1 + i, (uint8_t)name[i]);
        old_module = rd32(cli + CLI_MODULE);
        wr32(cli + CLI_MODULE, seg);
    }
    uint32_t old_lo = rd32(pr + TC_SPLOWER), old_hi = rd32(pr + TC_SPUPPER);
    wr32(pr + TC_SPLOWER, stack);
    wr32(pr + TC_SPUPPER, stack + stacksize);
    wr32(pr + PR_STACKSIZE, stacksize);
    wr32(pr + PR_RESULT2, 0);
    wr32(pr + TC_SIGRECVD, rd32(pr + TC_SIGRECVD) & ~(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F));
    sched_set_console_owner(pr);
    uint32_t old_home = rd32(pr + PR_HOMEDIR), home = homedir ? lock_new(homedir, SHARED_LOCK) : 0;
    if (homedir) wr32(pr + PR_HOMEDIR, home);

    M68K *c = &me->cpu;
    M68K saved = *c;                                     /* the caller's own context */
    memset(c->d, 0, sizeof c->d);
    memset(c->a, 0, sizeof c->a);
    cpu68k_set_sr(c, 0);
    c->a[7] = stack + stacksize;
    cpu68k_push32(c, stacksize);                         /* 4(SP) = stack size */
    cpu68k_push32(c, stub_exit);                         /* RTS ends the program */
    c->d[0] = (uint32_t)strlen(line);
    c->a[0] = argstr;
    c->a[6] = SysBase;
    c->pc = BADDR(seg) + 4;
    c->exception = on_exception;
    guru = 0;
    sched_run_cpu(c);
    int rc = guru ? -1 : (int)c->d[0];
    uint32_t result2 = rd32(pr + PR_RESULT2);
    *c = saved;
    if (cli) {
        wr32(cli + CLI_RETURNCODE, (uint32_t)rc);
        wr32(cli + CLI_RESULT2, result2);
        wr32(cli + CLI_MODULE, old_module);
    }
    wr32(pr + PR_RESULT2, result2);
    wr32(pr + TC_SPLOWER, old_lo);
    wr32(pr + TC_SPUPPER, old_hi);
    if (homedir) { wr32(pr + PR_HOMEDIR, old_home); if (home) lock_free(home); }
    wr32(pr + PR_ARGUMENTS, old_args);
    exec_freemem(stack, stacksize);
    heap_free(argstr, (uint32_t)strlen(line) + 1);
    return rc;
}

int run_command(const char *path, const char *args, int *found) {
    char hp[1024];
    *found = 0;
    if (!dos_resolve(path, hp, sizeof hp)) return -1;
    struct stat st;
    if (stat(hp, &st) || S_ISDIR(st.st_mode)) return -1;
    uint32_t seg = dos_loadseg(hp);
    if (!seg) {
        *found = 1;
        if (dos_protection(hp) & 0x40) return shell_run_script_args(hp, args);   /* FIBF_SCRIPT */
        return -1;
    }
    *found = 1;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : (strchr(path, ':') ? strchr(path, ':') + 1 : path);
    char pdir[1024];
    snprintf(pdir, sizeof pdir, "%s", hp);
    if (strrchr(pdir, '/')) *strrchr(pdir, '/') = 0;
    int rc = run_seglist(seg, name, args, 0, pdir);
    dos_unloadseg(seg);
    return rc;
}
