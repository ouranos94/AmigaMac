/* dos.library, native, on top of the macOS file system.
 *
 * Amiga volumes and assigns map to host directories (SYS: is the AmigaMac
 * system folder, Mac: the whole host disk). Locks and file handles are real
 * Amiga structures whose fl_Key / fh_Args index native tables.
 * Register conventions follow dos_lib.fd.
 */
#include "os.h"
#include "sched.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint32_t DOSBase;
static int next_tasknum = 1;

/* ---- assigns ------------------------------------------------------------ */

typedef struct { char name[32]; char path[1024]; } Assign;
static Assign assigns[64];
static int nassigns;
static char sys_dir[1024], ram_dir[1024];

static void assign_set(const char *name, const char *path) {
    for (int i = 0; i < nassigns; i++)
        if (!strcasecmp(assigns[i].name, name)) { snprintf(assigns[i].path, sizeof assigns[i].path, "%s", path); return; }
    if (nassigns == 64) return;
    snprintf(assigns[nassigns].name, sizeof assigns[nassigns].name, "%s", name);
    snprintf(assigns[nassigns].path, sizeof assigns[nassigns].path, "%s", path);
    nassigns++;
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

/* host path of an Amiga path, relative to the current directory */
int dos_resolve(const char *apath, char *out, int max) {
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
    if (best < 0) { snprintf(out, max, "Mac:%s", hpath + 1); return; }
    const char *tail = hpath + bestlen;
    if (*tail == '/') tail++;
    snprintf(out, max, "%s:%s", assigns[best].name, tail);
}

/* ---- file handles ------------------------------------------------------- */

typedef struct { int used; FILE *f; int console; int ungot; } HostFile;
static HostFile files[256];

static uint32_t fh_new(FILE *f, int console) {
    int i;
    for (i = 1; i < 256 && files[i].used; i++) ;
    if (i == 256) return 0;
    files[i].used = 1;
    files[i].f = f;
    files[i].console = console;
    files[i].ungot = -1;
    uint32_t fh = exec_allocmem(FH_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(fh + FH_ARGS, i);
    wr32(fh + FH_TYPE, console ? 1 : 0);
    wr32(fh + FH_PORT, console ? 0xFFFFFFFF : 0);   /* non-zero: interactive */
    return MKBADDR(fh);
}

static HostFile *fh_get(uint32_t bfh) {
    if (!bfh) return 0;
    uint32_t k = rd32(BADDR(bfh) + FH_ARGS);
    return k > 0 && k < 256 && files[k].used ? &files[k] : 0;
}

static void fh_close(uint32_t bfh) {
    HostFile *h = fh_get(bfh);
    if (!h) return;
    if (h->f && !h->console) fclose(h->f);
    h->used = 0;
    exec_freemem(BADDR(bfh), FH_SIZE);
}

/* console input comes from the host as an event queue; a reader only
   blocks its own task */
static int con_read(char *buf, int n) {
    NTask *me = sched_current();
    if (me) sched_set_console_owner(me->task);
    return sched_console_read(buf, n);
}

static int32_t fh_read(uint32_t bfh, uint32_t buf, int32_t len) {
    HostFile *h = fh_get(bfh);
    if (!h) return -1;
    if (len <= 0) return 0;
    char *dst = mem_ptr(buf, len);
    if (!dst) return -1;
    int32_t got = 0;
    if (h->ungot >= 0) { dst[got++] = (char)h->ungot; h->ungot = -1; }
    if (got == len) return got;
    if (h->console) return got + con_read(dst + got, len - got);
    return got + (int32_t)fread(dst + got, 1, len - got, h->f);
}

static int32_t fh_write(uint32_t bfh, uint32_t buf, int32_t len) {
    HostFile *h = fh_get(bfh);
    if (!h) return -1;
    if (len <= 0) return 0;
    const char *src = mem_ptr(buf, len);
    if (!src) return -1;
    if (h->console) { if (host.console_write) host.console_write(src, len); return len; }
    return (int32_t)fwrite(src, 1, len, h->f);
}

static void fh_write_str(uint32_t bfh, const char *s, int len) {
    HostFile *h = fh_get(bfh);
    if (!h) return;
    if (h->console) { if (host.console_write) host.console_write(s, len); }
    else fwrite(s, 1, len, h->f);
}

static uint32_t cur_out(void) { uint32_t t = this_task(); return t ? rd32(t + PR_COS) : 0; }
static uint32_t cur_in(void) { uint32_t t = this_task(); return t ? rd32(t + PR_CIS) : 0; }

/* ---- dos functions: files ----------------------------------------------- */

static uint32_t f_Open(M68K *c) {
    char name[512], hp[1024];
    str_get(c->d[1], name, sizeof name);
    int mode = (int)c->d[2];
    if (!strcasecmp(name, "*") || !strcasecmp(name, "CONSOLE:")) return fh_new(0, 1);
    if (!strncasecmp(name, "NIL:", 4)) {
        FILE *f = fopen("/dev/null", mode == MODE_OLDFILE ? "r" : "w");
        return fh_new(f, 0);
    }
    if (!strncasecmp(name, "CON:", 4) || !strncasecmp(name, "RAW:", 4)) return fh_new(0, 1);
    if (!dos_resolve(name, hp, sizeof hp)) return 0;
    const char *fm = mode == MODE_NEWFILE ? "w+b" : "r+b";
    FILE *f = fopen(hp, fm);
    if (!f && mode == MODE_OLDFILE) f = fopen(hp, "rb");
    if (!f && mode == MODE_READWRITE) f = fopen(hp, "w+b");
    if (!f) { dos_set_ioerr(host_err()); return 0; }
    uint32_t fh = fh_new(f, 0);
    if (!fh) { fclose(f); dos_set_ioerr(ERROR_NO_FREE_STORE); }
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
    int n = h->console ? con_read(&ch, 1) : (int)fread(&ch, 1, 1, h->f);
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
static uint32_t f_VFWritef(M68K *c) {
    /* BCPL-style %S %N %X... formatting, approximated with the same engine */
    return f_VFPrintf(c);
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
    int n = (int)strlen(name);
    if (n > 106) n = 106;
    for (int i = 0; i < n; i++) wr8(fib + FIB_FILENAME + i, (uint8_t)name[i]);
    uint32_t prot = 0;                                  /* bits set = forbidden (RWED inverted) */
    if (!(st.st_mode & S_IRUSR)) prot |= 8;
    if (!(st.st_mode & S_IWUSR)) prot |= 4 | 1;
    wr32(fib + FIB_PROTECTION, prot);
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
        if (e->d_name[0] == '.') continue;               /* ., .., hidden host files */
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
    else amiga_name(p, an, sizeof an);
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
    while (!sched_console_ready()) {
        if (now_us() >= end) return 0;
        dos_delay_us(20000);
    }
    return DOSTRUE;
}
static uint32_t f_SetMode(M68K *c) { (void)c; return DOSTRUE; }

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
        int got = con_read(line, sizeof line - 1);
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
    /* positional: remaining words go to unfilled non-keyword items in order */
    int multi = -1;
    for (int i = 0; i < ni; i++) if (items[i].m && !filled[i]) multi = i;
    uint32_t mlist[128];
    int nm = 0;
    for (int w = 0; w < nw && !err; w++) {
        if (used[w]) continue;
        int it = -1;
        for (int i = 0; i < ni; i++)
            if (!filled[i] && !items[i].k && !items[i].s && !items[i].t && !items[i].m) { it = i; break; }
        if (it < 0) {
            if (multi >= 0) { mlist[nm++] = rda_str(rda, words[w]); used[w] = 1; continue; }
            err = ERROR_TOO_MANY_ARGS;
            break;
        }
        TItem *ti = &items[it];
        if (ti->f) {
            const char *p = line;
            for (int k = 0; k < w; k++) { const char *q = strstr(p, words[k]); if (q) p = q + strlen(words[k]); }
            while (*p == ' ') p++;
            wr32(array + it * 4, rda_str(rda, p));
            filled[it] = 1;
            break;
        }
        if (ti->n) {
            char *end;
            long num = strtol(words[w], &end, 10);
            if (*end) { err = ERROR_BAD_NUMBER; break; }
            uint32_t v = rda_alloc(rda, 4);
            wr32(v, (uint32_t)num);
            wr32(array + it * 4, v);
        } else wr32(array + it * 4, rda_str(rda, words[w]));
        filled[it] = 1;
        used[w] = 1;
    }
    if (!err && multi >= 0 && nm) {
        uint32_t v = rda_alloc(rda, (nm + 1) * 4);
        for (int i = 0; i < nm; i++) wr32(v + i * 4, mlist[i]);
        wr32(v + nm * 4, 0);
        wr32(array + multi * 4, v);
        filled[multi] = 1;
    }
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
    amiga_name(cd, an, sizeof an);
    if (strlen(an) + 1 > c->d[2]) return 0;
    for (size_t i = 0; i <= strlen(an); i++) wr8(c->d[1] + i, (uint8_t)an[i]);
    return DOSTRUE;
}
static uint32_t f_SystemTagList(M68K *c) {
    char cmd[1024];
    str_get(c->d[1], cmd, sizeof cmd);
    os_log("SystemTagList(\"%s\") : pas encore géré", cmd);
    return 0xFFFFFFFF;
}

static uint32_t f_CreateNewProc(M68K *c);
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
    { "GetCurrentDirName", f_GetCurrentDirName }, { "SystemTagList", f_SystemTagList },
    { "CreateNewProc", f_CreateNewProc }, { "CreateProc", f_CreateProc },
    { "ParsePattern", f_ParsePattern }, { "ParsePatternNoCase", f_ParsePatternNoCase },
    { "MatchPattern", f_MatchPattern }, { "MatchPatternNoCase", f_MatchPatternNoCase },
    { 0, 0 }
};

/* ---- boot ------------------------------------------------------------------ */

void dos_init(const char *sysdir) {
    snprintf(sys_dir, sizeof sys_dir, "%s", sysdir);
    snprintf(ram_dir, sizeof ram_dir, "%s/.ram", sysdir);
    mkdir(ram_dir, 0755);
    char p[1100];
    nassigns = 0;
    assign_set("SYS", sys_dir);
    assign_set("RAM", ram_dir);
    assign_set("Mac", "/");
    static const char *dirs[] = { "C", "Libs", "Devs", "S", "L", "Fonts", "Prefs", "Utilities", "Tools" };
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(p, sizeof p, "%s/%s", sys_dir, dirs[i]);
        mkdir(p, 0755);
        assign_set(dirs[i], p);
    }
    snprintf(p, sizeof p, "%s/T", ram_dir); mkdir(p, 0755); assign_set("T", p);
    snprintf(p, sizeof p, "%s/Env", ram_dir); mkdir(p, 0755); assign_set("ENV", p);
    snprintf(p, sizeof p, "%s/Prefs/Env-Archive", sys_dir); mkdir(p, 0755); assign_set("ENVARC", p);

    DOSBase = lib_create("dos.library", "dos", DL_SIZE, dos_natives, 40, 1, "dos 40.1 (AmigaMac)");
    lib_add(DOSBase);
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
    sched_preempt_point();
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
        int n = con_read(buf, max - 1);
        buf[n > 0 ? n : 0] = 0;
        return n;
    }
    if (!fgets(buf, max, h->f)) return -1;
    return (int)strlen(buf);
}
const char *dos_assign_path(const char *name) { return assign_get(name); }
void dos_assign(const char *name, const char *path) { assign_set(name, path); }
int dos_assign_count(void) { return nassigns; }
const char *dos_assign_name(int i) { return assigns[i].name; }
const char *dos_assign_target(int i) { return assigns[i].path; }
uint32_t dos_lock(const char *apath) { return lock_by_name(apath, SHARED_LOCK); }
void dos_unlock(uint32_t bl) { lock_free(bl); }
const char *dos_lock_host_path(uint32_t bl) { return lock_path(bl); }
void dos_amiga_name(const char *hp, char *out, int max) { amiga_name(hp, out, max); }

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

int run_command(const char *path, const char *args, int *found) {
    char hp[1024];
    *found = 0;
    if (!dos_resolve(path, hp, sizeof hp)) return -1;
    struct stat st;
    if (stat(hp, &st) || S_ISDIR(st.st_mode)) return -1;
    uint32_t seg = dos_loadseg(hp);
    if (!seg) { *found = 1; return -1; }
    *found = 1;

    NTask *me = sched_current();
    uint32_t pr = me->task, cli = cur_cli();
    uint32_t stacksize = cli ? rd32(cli + CLI_DEFAULTSTACK) * 4 : 65536;
    uint32_t stack = exec_allocmem(stacksize, MEMF_PUBLIC);
    /* argument line ends with a newline, as on AmigaOS */
    char line[2048];
    snprintf(line, sizeof line, "%s\n", args ? args : "");
    uint32_t argstr = str_alloc(line);
    wr32(pr + PR_ARGUMENTS, argstr);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : (strchr(path, ':') ? strchr(path, ':') + 1 : path);
    if (cli) {
        uint32_t bname = BADDR(rd32(cli + CLI_COMMANDNAME));
        int nl = (int)strlen(name) > 254 ? 254 : (int)strlen(name);
        wr8(bname, nl);
        for (int i = 0; i < nl; i++) wr8(bname + 1 + i, (uint8_t)name[i]);
        wr32(cli + CLI_MODULE, seg);
    }
    uint32_t old_lo = rd32(pr + TC_SPLOWER), old_hi = rd32(pr + TC_SPUPPER);
    wr32(pr + TC_SPLOWER, stack);
    wr32(pr + TC_SPUPPER, stack + stacksize);
    wr32(pr + PR_STACKSIZE, stacksize);
    wr32(pr + PR_RESULT2, 0);
    wr32(pr + TC_SIGRECVD, rd32(pr + TC_SIGRECVD) & ~(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F));
    sched_set_console_owner(pr);

    M68K *c = &me->cpu;
    M68K saved = *c;                                     /* the native shell's own context */
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
    *c = saved;
    if (cli) {
        wr32(cli + CLI_RETURNCODE, (uint32_t)rc);
        wr32(cli + CLI_RESULT2, rd32(pr + PR_RESULT2));
        wr32(cli + CLI_MODULE, 0);
    }
    wr32(pr + TC_SPLOWER, old_lo);
    wr32(pr + TC_SPUPPER, old_hi);
    dos_unloadseg(seg);
    exec_freemem(stack, stacksize);
    heap_free(argstr, (uint32_t)strlen(line) + 1);
    return rc;
}
