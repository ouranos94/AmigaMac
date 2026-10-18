/* The Shell, native: built-in commands plus 68k programs from disk.
 *
 * AmigaOS ships its commands as programs in C:. Here the common ones are
 * built into the shell (written from scratch); anything else is searched in
 * the current directory then C: and run as a 68k executable.
 */
#include "os.h"
#include "wb.h"
#include "console.h"
#include "sched.h"
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char prompt_fmt[128] = "%N.%S> ";
static char prompt_buf[1200];
static int last_rc, last_err;

/* local variables (SET) and the command path (PATH), per AmigaMac session */
typedef struct { char name[64], value[256]; } LVar;
static LVar lvars[64];
static int nlvars;
static char cmd_path[16][256];
static int ncmd_path;

static const char *lvar_get(const char *n) {
    for (int i = 0; i < nlvars; i++) if (!strcasecmp(lvars[i].name, n)) return lvars[i].value;
    return 0;
}

/* $name and ${name}: local variables, RC, Result2, then ENV: */
static void expand_vars(const char *in, char *out, int max) {
    int k = 0;
    for (const char *p = in; *p && k < max - 1;) {
        if (*p == '*' && p[1] == '$') { out[k++] = '$'; p += 2; continue; }   /* escaped */
        if (*p != '$' || !(isalpha((unsigned char)p[1]) || p[1] == '{' || p[1] == '_')) { out[k++] = *p++; continue; }
        p++;
        int brace = *p == '{';
        if (brace) p++;
        char name[64];
        int n = 0;
        while ((isalnum((unsigned char)*p) || *p == '_' || *p == '.') && n < 63) name[n++] = *p++;
        name[n] = 0;
        if (brace && *p == '}') p++;
        char val[256] = "";
        const char *l = lvar_get(name);
        if (l) snprintf(val, sizeof val, "%s", l);
        else if (!strcasecmp(name, "RC")) snprintf(val, sizeof val, "%d", last_rc);
        else if (!strcasecmp(name, "Result2")) snprintf(val, sizeof val, "%d", last_err);
        else {
            char ap[128], hp[1024];
            snprintf(ap, sizeof ap, "ENV:%s", name);
            FILE *f = dos_resolve(ap, hp, sizeof hp) ? fopen(hp, "rb") : 0;
            if (f) { size_t r = fread(val, 1, sizeof val - 1, f); val[r] = 0; fclose(f); val[strcspn(val, "\n")] = 0; }
            else snprintf(val, sizeof val, "$%s", name);
        }
        for (const char *v = val; *v && k < max - 1; v++) out[k++] = *v;
    }
    out[k] = 0;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    /* the shell's own messages are UTF-8 in this source; the Amiga side
       speaks ISO-8859-1 */
    int o = 0;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if ((ch == 0xC2 || ch == 0xC3) && i + 1 < n) { buf[o++] = (char)(((ch & 0x03) << 6) | (buf[i + 1] & 0x3F)); i++; }
        else buf[o++] = (char)ch;
    }
    n = o;
    /* through the current output handle, so redirection works */
    dos_write_handle(rd32(this_task() + PR_COS), buf, n);
}

static void cwd_amiga(char *buf, int max) {
    const char *hp = dos_lock_host_path(rd32(this_task() + PR_CURRENTDIR));
    if (hp) dos_amiga_name(hp, buf, max);
    else snprintf(buf, max, "SYS:");
}

const char *shell_prompt(void) {
    char cwd[1024];
    cwd_amiga(cwd, sizeof cwd);
    char *o = prompt_buf;
    for (const char *p = prompt_fmt; *p && o < prompt_buf + sizeof prompt_buf - 64; p++) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == 'N' || *p == 'n') o += sprintf(o, "%u", rd32(this_task() + PR_TASKNUM));
            else if (*p == 'S' || *p == 's') o += sprintf(o, "%s", cwd);
            else if (*p == 'R' || *p == 'r') o += sprintf(o, "%d", last_rc);
            else *o++ = *p;
        } else *o++ = *p;
    }
    *o = 0;
    return prompt_buf;
}

/* ---- tokenizer ---------------------------------------------------------- */

static int tokenize(const char *line, char tok[][512], int max) {
    int n = 0;
    const char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == ';') break;                     /* ';' starts a comment */
        int l = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '*' && p[1]) { p++; tok[n][l++] = (*p == 'N' || *p == 'n') ? '\n' : *p; p++; continue; }
                if (l < 511) tok[n][l++] = *p;
                p++;
            }
            if (*p) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') { if (l < 511) tok[n][l++] = *p; p++; }
        }
        tok[n][l] = 0;
        n++;
    }
    return n;
}

/* ---- built-in commands -------------------------------------------------- */

typedef int (*Builtin)(int argc, char argv[][512], const char *rest);

static int c_echo(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", rest);
    /* strip surrounding quotes like Echo does */
    int n = (int)strlen(buf);
    if (n >= 2 && buf[0] == '"' && buf[n - 1] == '"') { buf[n - 1] = 0; memmove(buf, buf + 1, n - 1); }
    out("%s\n", buf);
    return 0;
}

static int c_cd(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) {
        char cwd[1024];
        cwd_amiga(cwd, sizeof cwd);
        out("%s\n", cwd);
        return 0;
    }
    uint32_t l = dos_lock(argv[1]);
    if (!l) { out("Can't find %s\n", argv[1]); return 20; }
    struct stat st;
    if (stat(dos_lock_host_path(l), &st) || !S_ISDIR(st.st_mode)) {
        dos_unlock(l);
        out("%s is not a directory\n", argv[1]);
        return 20;
    }
    uint32_t old = rd32(this_task() + PR_CURRENTDIR);
    wr32(this_task() + PR_CURRENTDIR, l);
    dos_unlock(old);
    return 0;
}

typedef struct { char name[256]; int dir; long size; time_t mtime; } Entry;

static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    return strcasecmp(x->name, y->name);
}

static int list_dir(const char *apath_in, Entry **res) {
    char hp[1024], apath[512], pat[256] = "";
    snprintf(apath, sizeof apath, "%s", apath_in);
    char *last = strrchr(apath, '/') ? strrchr(apath, '/') + 1 : strchr(apath, ':') ? strchr(apath, ':') + 1 : apath;
    if (dos_has_wild(last)) {                                    /* DIR RAM:#?.txt */
        snprintf(pat, sizeof pat, "%s", last);
        *last = 0;
        if (last > apath && last[-1] == '/') last[-1] = 0;
    }
    if (!dos_resolve(apath, hp, sizeof hp)) return -1;
    DIR *d = opendir(hp);
    if (!d) return -1;
    int n = 0, cap = 64;
    Entry *e = malloc(sizeof *e * cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (pat[0]) {
            char l1[256];
            dos_name_to_amiga(de->d_name, l1, sizeof l1);
            if (!dos_match_pattern(pat, l1, 1)) continue;
        }
        if (n == cap) { cap *= 2; e = realloc(e, sizeof *e * cap); }
        char p[2048];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", hp, de->d_name);
        if (stat(p, &st)) continue;
        snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
        e[n].dir = S_ISDIR(st.st_mode);
        e[n].size = (long)st.st_size;
        e[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(e, n, sizeof *e, cmp_entry);
    *res = e;
    return n;
}

static int c_dir(int argc, char argv[][512], const char *rest) {
    (void)rest;
    const char *path = argc > 1 ? argv[1] : "";
    Entry *e;
    int n = list_dir(path, &e);
    if (n < 0) { out("Can't examine \"%s\": object not found\n", path); return 20; }
    for (int i = 0; i < n; i++) if (e[i].dir) out("     %s (dir)\n", e[i].name);
    int col = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].dir) continue;
        out(col ? "  %-32s\n" : "  %-32s", e[i].name);
        col = !col;
    }
    if (col) out("\n");
    free(e);
    return 0;
}

static int c_list(int argc, char argv[][512], const char *rest) {
    (void)rest;
    const char *path = argc > 1 ? argv[1] : "";
    Entry *e;
    int n = list_dir(path, &e);
    if (n < 0) {
        char hp[1024];
        struct stat st;
        if (!dos_resolve(path, hp, sizeof hp) || stat(hp, &st)) { out("Can't examine \"%s\": object not found\n", path); return 20; }
        e = malloc(sizeof *e);                                  /* a single file */
        const char *b = strrchr(path, '/') ? strrchr(path, '/') + 1 : strchr(path, ':') ? strchr(path, ':') + 1 : path;
        snprintf(e[0].name, sizeof e[0].name, "%s", b);
        e[0].dir = 0;
        e[0].size = (long)st.st_size;
        e[0].mtime = st.st_mtime;
        n = 1;
    }
    int files = 0, dirs = 0;
    long blocks = 0;
    for (int i = 0; i < n; i++) {
        struct tm t;
        localtime_r(&e[i].mtime, &t);
        char date[32];
        strftime(date, sizeof date, "%d-%b-%y %H:%M:%S", &t);
        if (e[i].dir) { out("%-24s       Dir ----rwed %s\n", e[i].name, date); dirs++; }
        else { out("%-24s %9ld ----rwed %s\n", e[i].name, e[i].size, date); files++; blocks += (e[i].size + 511) / 512; }
    }
    out("%d file%s - %d director%s - %ld blocks used\n", files, files == 1 ? "" : "s",
        dirs, dirs == 1 ? "y" : "ies", blocks);
    free(e);
    return 0;
}

static int c_type(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("TYPE: required argument missing\n"); return 20; }
    char hp[1024];
    if (!dos_resolve(argv[1], hp, sizeof hp)) { out("Can't open %s\n", argv[1]); return 20; }
    FILE *f = fopen(hp, "rb");
    if (!f) { out("Can't open %s\n", argv[1]); return 20; }
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) { buf[n] = 0; out("%s", buf); }
    fclose(f);
    return 0;
}

static int c_makedir(int argc, char argv[][512], const char *rest) {
    (void)rest;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char hp[1024];
        if (!dos_resolve(argv[i], hp, sizeof hp) || mkdir(hp, 0755)) { out("Can't create directory %s\n", argv[i]); rc = 20; }
    }
    return rc;
}

static int c_delete(int argc, char argv[][512], const char *rest) {
    (void)rest;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char hp[1024];
        if (!dos_resolve(argv[i], hp, sizeof hp) || remove(hp)) { out("%s  Not Deleted - object not found\n", argv[i]); rc = 20; }
        else out("%s  Deleted\n", argv[i]);
    }
    return rc;
}

static int c_rename(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 3) { out("RENAME: required argument missing\n"); return 20; }
    char a[1024], b[1024];
    int to = argc > 3 && !strcasecmp(argv[2], "TO") ? 3 : 2;
    if (!dos_resolve(argv[1], a, sizeof a) || !dos_resolve(argv[to], b, sizeof b) || rename(a, b)) {
        out("Can't rename %s as %s\n", argv[1], argv[to]);
        return 20;
    }
    return 0;
}

static int c_copy(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 3) { out("COPY: required argument missing\n"); return 20; }
    int to = argc > 3 && !strcasecmp(argv[2], "TO") ? 3 : 2;
    char a[1024], b[1024];
    if (!dos_resolve(argv[1], a, sizeof a) || !dos_resolve(argv[to], b, sizeof b)) { out("Can't copy\n"); return 20; }
    struct stat st;
    if (!stat(b, &st) && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(a, '/');
        strncat(b, base ? base : "/", sizeof b - strlen(b) - 1);
    }
    FILE *fi = fopen(a, "rb"), *fo = fi ? fopen(b, "wb") : 0;
    if (!fi || !fo) { if (fi) fclose(fi); out("Can't copy %s to %s\n", argv[1], argv[to]); return 20; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, n, fo);
    fclose(fi);
    fclose(fo);
    return 0;
}

static int c_assign(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) {
        out("Volumes and assigns:\n");
        for (int i = 0; i < dos_assign_count(); i++) out("%-12s %s\n", dos_assign_name(i), dos_assign_target(i));
        return 0;
    }
    char name[64];
    snprintf(name, sizeof name, "%s", argv[1]);
    size_t l = strlen(name);
    if (l && name[l - 1] == ':') name[l - 1] = 0;
    if (argc < 3) { out("ASSIGN: target missing\n"); return 20; }
    char hp[1024];
    if (!dos_resolve(argv[2], hp, sizeof hp)) { out("Can't find %s\n", argv[2]); return 20; }
    dos_assign(name, hp);
    return 0;
}

static int c_avail(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv; (void)rest;
    uint32_t total = heap_avail(MEMF_TOTAL), avail = heap_avail(0), largest = heap_avail(MEMF_LARGEST);
    out("Type   Available    In-Use   Maximum   Largest\n");
    out("any    %9u %9u %9u %9u\n", avail, total - avail, total, largest);
    return 0;
}

static int c_version(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) {
        out("AmigaMac Kickstart 40.1, Workbench 40.1 (natif macOS)\n");
        return 0;
    }
    char name[256];
    snprintf(name, sizeof name, "%s", argv[1]);
    uint32_t lib = exec_openlibrary(name, 0);
    if (!lib) { out("Could not find version information for '%s'\n", name); return 5; }
    char id[256];
    str_get(rd32(lib + LIB_IDSTRING), id, sizeof id);
    out("%s\n", id);
    return 0;
}

static int c_prompt(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv;
    snprintf(prompt_fmt, sizeof prompt_fmt, "%s", *rest ? rest : "%N.%S> ");
    size_t n = strlen(prompt_fmt);
    if (n >= 2 && prompt_fmt[0] == '"' && prompt_fmt[n - 1] == '"') { prompt_fmt[n - 1] = 0; memmove(prompt_fmt, prompt_fmt + 1, n - 1); }
    return 0;
}

static int c_why(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv; (void)rest;
    if (!last_err) out("The last command did not set a return code\n");
    else out("Last command failed because %s\n", dos_fault_text(last_err));
    return 0;
}

static int c_date(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv; (void)rest;
    time_t now = time(0);
    struct tm t;
    localtime_r(&now, &t);
    char buf[64];
    strftime(buf, sizeof buf, "%A %d-%b-%y %H:%M:%S", &t);
    out("%s\n", buf);
    return 0;
}

static int c_setenv(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) {
        Entry *e;
        int n = list_dir("ENV:", &e);
        for (int i = 0; i < n; i++) out("%s\n", e[i].name);
        if (n > 0) free(e);
        return 0;
    }
    char ap[600], hp[1024];
    snprintf(ap, sizeof ap, "ENV:%s", argv[1]);
    if (!dos_resolve(ap, hp, sizeof hp)) return 20;
    FILE *f = fopen(hp, "wb");
    if (!f) return 20;
    for (int i = 2; i < argc; i++) fprintf(f, i > 2 ? " %s" : "%s", argv[i]);
    fclose(f);
    return 0;
}

static int c_path(int argc, char argv[][512], const char *rest) {
    (void)rest;
    int add = 0, reset = 0, rem = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "ADD")) add = 1;
        else if (!strcasecmp(argv[i], "RESET")) reset = 1;
        else if (!strcasecmp(argv[i], "REMOVE")) rem = 1;
    }
    if (reset) ncmd_path = 0;
    if (argc == 1 || (argc == 2 && (add || reset) && !reset)) {
        out("Répertoire courant\n");
        for (int i = 0; i < ncmd_path; i++) out("%s\n", cmd_path[i]);
        out("C:\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "ADD") || !strcasecmp(argv[i], "RESET") || !strcasecmp(argv[i], "REMOVE") || !strcasecmp(argv[i], "QUIET")) continue;
        if (rem) {
            for (int k = 0; k < ncmd_path; k++)
                if (!strcasecmp(cmd_path[k], argv[i])) { memmove(cmd_path[k], cmd_path[k + 1], sizeof cmd_path[0] * (size_t)(ncmd_path - k - 1)); ncmd_path--; break; }
            continue;
        }
        uint32_t l = dos_lock(argv[i]);
        if (!l) { out("PATH: %s introuvable\n", argv[i]); continue; }
        dos_unlock(l);
        if (ncmd_path < 16) snprintf(cmd_path[ncmd_path++], sizeof cmd_path[0], "%s", argv[i]);
    }
    return 0;
}

static int c_set(int argc, char argv[][512], const char *rest) {
    if (argc < 2) { for (int i = 0; i < nlvars; i++) out("%-16s %s\n", lvars[i].name, lvars[i].value); return 0; }
    const char *val = rest + strlen(argv[1]);
    while (*val == ' ') val++;
    for (int i = 0; i < nlvars; i++)
        if (!strcasecmp(lvars[i].name, argv[1])) { snprintf(lvars[i].value, sizeof lvars[i].value, "%s", val); return 0; }
    if (nlvars < 64) { snprintf(lvars[nlvars].name, 64, "%s", argv[1]); snprintf(lvars[nlvars].value, 256, "%s", val); nlvars++; }
    return 0;
}

static int c_unset(int argc, char argv[][512], const char *rest) {
    (void)rest;
    for (int i = 0; argc > 1 && i < nlvars; i++)
        if (!strcasecmp(lvars[i].name, argv[1])) { lvars[i] = lvars[--nlvars]; break; }
    return 0;
}

static int c_getenv(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) return 20;
    char ap[600], hp[1024], buf[1024];
    snprintf(ap, sizeof ap, "ENV:%s", argv[1]);
    if (!dos_resolve(ap, hp, sizeof hp)) return 20;
    FILE *f = fopen(hp, "rb");
    if (!f) { out("Can't get %s\n", argv[1]); return 5; }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    out("%s\n", buf);
    return 0;
}

static const char *builtin_name(const char *name);
/* ---- scripts: EXECUTE with .KEY arguments and IF/ELSE/ENDIF, SKIP/LAB, QUIT, FAILAT ---- */

typedef struct { char name[32]; char val[512]; int sw, kw, multi; } KeyArg;

/* bind the arguments of a script to its .KEY template */
static int bind_keys(const char *tpl, int argc, char argv[][512], KeyArg *k, int max) {
    int n = 0;
    char t[512];
    snprintf(t, sizeof t, "%s", tpl);
    for (char *p = strtok(t, ","); p && n < max; p = strtok(0, ",")) {
        while (*p == ' ') p++;
        memset(&k[n], 0, sizeof k[n]);
        char *sl = strchr(p, '/');
        snprintf(k[n].name, sizeof k[n].name, "%.*s", sl ? (int)(sl - p) : (int)strlen(p), p);
        char *eq = strchr(k[n].name, '=');                 /* aliases: keep the first name */
        if (eq) *eq = 0;
        for (char *f = sl; f && *f; f++) {
            char m = (char)toupper((unsigned char)f[1]);
            if (*f == '/') { if (m == 'S') k[n].sw = 1; else if (m == 'K') k[n].kw = 1; else if (m == 'M') k[n].multi = 1; }
        }
        n++;
    }
    int used[64] = { 0 };
    for (int i = 0; i < argc && i < 64; i++) {
        for (int j = 0; j < n; j++) {
            size_t l = strlen(k[j].name);
            if (strncasecmp(argv[i], k[j].name, l) || (argv[i][l] && argv[i][l] != '=')) continue;
            used[i] = 1;
            if (k[j].sw) snprintf(k[j].val, sizeof k[j].val, "%s", k[j].name);
            else if (argv[i][l] == '=') snprintf(k[j].val, sizeof k[j].val, "%s", argv[i] + l + 1);
            else if (i + 1 < argc) { snprintf(k[j].val, sizeof k[j].val, "%s", argv[i + 1]); used[++i] = 1; }
            k[j].kw = 2;                                     /* filled */
            break;
        }
    }
    int j = 0;
    for (int i = 0; i < argc && i < 64; i++) {
        if (used[i]) continue;
        while (j < n && (k[j].sw || k[j].kw)) j++;
        if (j >= n) return 0;
        if (k[j].multi) {
            size_t l = strlen(k[j].val);
            snprintf(k[j].val + l, sizeof k[j].val - l, "%s%s", l ? " " : "", argv[i]);
            continue;
        }
        snprintf(k[j].val, sizeof k[j].val, "%s", argv[i]);
        j++;
    }
    return n;
}

/* <name> and <name$default> */
static void subst_keys(const char *in, char *outp, int max, KeyArg *k, int nk, char bra, char ket, char dollar,
                       char defs[][2][256], int ndefs) {
    int o = 0;
    for (const char *p = in; *p && o < max - 1;) {
        const char *e = *p == bra ? strchr(p + 1, ket) : 0;
        if (!e) { outp[o++] = *p++; continue; }
        char name[256], def[256] = "";
        snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
        char *d = strchr(name, dollar);
        if (d) { snprintf(def, sizeof def, "%s", d + 1); *d = 0; }
        int found = 0;
        for (int i = 0; i < nk; i++)
            if (!strcasecmp(k[i].name, name)) {
                found = 1;
                const char *v = k[i].val;
                if (!*v) {
                    v = def;
                    for (int q = 0; !*v && q < ndefs; q++) if (!strcasecmp(defs[q][0], name)) v = defs[q][1];
                }
                for (; *v && o < max - 1; v++) outp[o++] = *v;
            }
        if (!found) { outp[o++] = *p++; continue; }
        p = e + 1;
    }
    outp[o] = 0;
}

static int script_cond(int argc, char argv[][512], int rc) {
    int i = 1, not = 0, val = 0;
    if (i < argc && !strcasecmp(argv[i], "NOT")) { not = 1; i++; }
    int r = 0;
    if (i < argc && !strcasecmp(argv[i], "WARN")) r = rc >= 5;
    else if (i < argc && !strcasecmp(argv[i], "ERROR")) r = rc >= 10;
    else if (i < argc && !strcasecmp(argv[i], "FAIL")) r = rc >= 20;
    else if (i < argc && !strcasecmp(argv[i], "EXISTS")) {
        char hp[1024];
        struct stat st;
        r = i + 1 < argc && dos_resolve(argv[i + 1], hp, sizeof hp) && !stat(hp, &st);
    } else {
        if (i < argc && !strcasecmp(argv[i], "VAL")) { val = 1; i++; }
        if (i + 2 < argc && !strcasecmp(argv[i + 2], "VAL")) val = 1;
        const char *a = i < argc ? argv[i] : "", *op = i + 1 < argc ? argv[i + 1] : "", *b = i + 2 < argc ? argv[i + 2] : "";
        if (!strcasecmp(b, "VAL")) b = i + 3 < argc ? argv[i + 3] : "";
        long cmp = val ? (atol(a) > atol(b)) - (atol(a) < atol(b)) : strcasecmp(a, b);
        if (!strcasecmp(op, "EQ")) r = cmp == 0;
        else if (!strcasecmp(op, "GT")) r = cmp > 0;
        else if (!strcasecmp(op, "GE")) r = cmp >= 0;
        else if (!strcasecmp(op, "LT")) r = cmp < 0;
        else if (!strcasecmp(op, "LE")) r = cmp <= 0;
    }
    return not ? !r : r;
}

int shell_run_script(const char *hp, int argc, char argv[][512]) {
    FILE *f = fopen(hp, "r");
    if (!f) return -1;
    int cap = 256, n = 0;
    char **lines = malloc(sizeof *lines * cap);
    char buf[2048];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (n == cap) { cap *= 2; lines = realloc(lines, sizeof *lines * cap); }
        lines[n++] = strdup(buf);
    }
    fclose(f);
    KeyArg *keys = calloc(32, sizeof *keys);
    char (*defs)[2][256] = calloc(16, sizeof *defs);
    int nk = 0, ndefs = 0, failat = 10, rc = 0, last = 0;
    char bra = '<', ket = '>', dollar = '$';
    for (int i = 0; i < n; i++) {                            /* directives */
        const char *l = lines[i];
        if (l[0] != '.') continue;
        char d[16];
        sscanf(l + 1, "%15s", d);
        const char *v = l + 1 + strlen(d);
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(d, "KEY") || !strcasecmp(d, "K")) nk = bind_keys(v, argc, argv, keys, 32);
        else if (!strcasecmp(d, "BRA")) bra = *v;
        else if (!strcasecmp(d, "KET")) ket = *v;
        else if (!strcasecmp(d, "DOLLAR") || !strcasecmp(d, "DOL")) dollar = *v;
        else if ((!strcasecmp(d, "DEF") || !strcasecmp(d, "DEFAULT")) && ndefs < 16) {
            sscanf(v, "%255s", defs[ndefs][0]);
            const char *w = v + strlen(defs[ndefs][0]);
            while (*w == ' ') w++;
            char tk[1][512];
            snprintf(defs[ndefs][1], 256, "%s", tokenize(w, tk, 1) ? tk[0] : "");
            ndefs++;
        }
    }
    int stack[32], sp = 0, active = 1;                      /* IF nesting */
    char (*tok)[512] = malloc(16 * 512);
    for (int pc = 0; pc < n; pc++) {
        char line[2048], exp[2048];
        if (lines[pc][0] == '.') continue;
        subst_keys(lines[pc], line, sizeof line, keys, nk, bra, ket, dollar, defs, ndefs);
        expand_vars(line, exp, sizeof exp);
        int nt = tokenize(exp, tok, 16);
        if (!nt) continue;
        const char *w = tok[0];
        if (!strcasecmp(w, "IF")) {
            if (sp < 32) stack[sp++] = active;
            active = active && script_cond(nt, tok, last);
            continue;
        }
        if (!strcasecmp(w, "ELSE")) { if (sp && stack[sp - 1]) active = !active; continue; }
        if (!strcasecmp(w, "ENDIF")) { if (sp) active = stack[--sp]; continue; }
        if (!active) continue;
        if (!strcasecmp(w, "LAB")) continue;
        if (!strcasecmp(w, "QUIT")) { rc = nt > 1 ? atoi(tok[1]) : 0; break; }
        if (!strcasecmp(w, "FAILAT")) { if (nt > 1) failat = atoi(tok[1]); else out("Fail limit: %d\n", failat); continue; }
        if (!strcasecmp(w, "SKIP")) {
            int back = nt > 2 && !strcasecmp(tok[2], "BACK"), target = -1;
            for (int j = back ? 0 : pc + 1; j < n && target < 0; j++) {
                char lt[2][512];
                if (tokenize(lines[j], lt, 2) >= 1 && !strcasecmp(lt[0], "LAB") &&
                    (nt < 2 || !strcasecmp(nt > 1 ? tok[1] : "", tokenize(lines[j], lt, 2) > 1 ? lt[1] : ""))) target = j;
            }
            if (target < 0) { out("SKIP: label %s not found\n", nt > 1 ? tok[1] : ""); rc = 10; break; }
            pc = target;
            sp = 0;
            active = 1;
            continue;
        }
        last = shell_execute(line);
        if (last == -2) { last = 0; break; }
        if (last < 0) last = 0;
        last_rc = last;
        rc = last;
        if (last >= failat) break;
    }
    free(tok);
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
    free(keys);
    free(defs);
    return rc;
}

/* a script with the S bit, run by its name */
int shell_run_script_args(const char *hp, const char *args) {
    char (*tok)[512] = malloc(64 * 512);
    int n = tokenize(args ? args : "", tok, 64);
    int rc = shell_run_script(hp, n, tok);
    free(tok);
    return rc;
}

static int c_execute(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("EXECUTE: required argument missing\n"); return 20; }
    char hp[1024];
    if (!dos_resolve(argv[1], hp, sizeof hp)) { out("Can't open %s\n", argv[1]); return 20; }
    int rc = shell_run_script(hp, argc - 2, argv + 2);
    if (rc < 0) { out("Can't open %s\n", argv[1]); return 20; }
    return rc;
}

/* control words outside a script */
static int c_scriptonly(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)rest;
    if (!strcasecmp(argv[0], "LAB") || !strcasecmp(argv[0], "ENDIF") || !strcasecmp(argv[0], "ELSE")) return 0;
    if (!strcasecmp(argv[0], "QUIT")) return argc > 1 ? atoi(argv[1]) : 0;
    out("%s: only valid in a script\n", argv[0]);
    return 10;
}
static int c_if(int argc, char argv[][512], const char *rest) {
    (void)rest;
    return script_cond(argc, argv, last_rc) ? 0 : 5;
}

/* ASK prompt: WARN (5) for yes */
static int c_ask(int argc, char argv[][512], const char *rest) {
    (void)rest;
    for (;;) {
        out("%s ", argc > 1 ? argv[1] : "");
        uint32_t in = rd32(this_task() + PR_CIS);
        char line[256];
        if (dos_read_line(in, line, sizeof line) < 0) return 0;
        char ch = (char)toupper((unsigned char)line[0]);
        if (ch == 'Y' || ch == 'O') return 5;
        if (ch == 'N' || ch == 0) return 0;
    }
}

/* WHICH command: where it would be found; WARN when nowhere */
static int c_which(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) return 10;
    if (builtin_name(argv[1])) { out("INTERNAL %s\n", builtin_name(argv[1])); return 0; }
    char cand[20][300];
    int nc = 0;
    snprintf(cand[nc++], 300, "%s", argv[1]);
    if (!strchr(argv[1], ':') && !strchr(argv[1], '/')) {
        for (int k = 0; k < ncmd_path && nc < 19; k++) {
            size_t l = strlen(cmd_path[k]);
            snprintf(cand[nc++], 300, "%s%s%s", cmd_path[k], l && (cmd_path[k][l - 1] == ':' || cmd_path[k][l - 1] == '/') ? "" : "/", argv[1]);
        }
        snprintf(cand[nc++], 300, "C:%s", argv[1]);
    }
    for (int i = 0; i < nc; i++) {
        char hp[1024];
        struct stat st;
        if (dos_resolve(cand[i], hp, sizeof hp) && !stat(hp, &st) && !S_ISDIR(st.st_mode)) {
            char an[1024];
            dos_amiga_name(hp, an, sizeof an);
            out("%s\n", an);
            return 0;
        }
    }
    return 5;
}

/* PROTECT file [+|-]flags (HSPARWED) */
static int c_protect(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("PROTECT: required argument missing\n"); return 20; }
    uint32_t l = dos_lock(argv[1]);
    if (!l) { out("%s: object not found\n", argv[1]); return 20; }
    const char *hp = dos_lock_host_path(l);
    uint32_t prot = dos_protection(hp);
    static const char bits[] = "HSPARWED";                 /* bit 7 .. bit 0 */
    if (argc > 2) {
        const char *f = argv[2];
        int mode = *f == '+' ? 1 : *f == '-' ? -1 : 0;
        if (mode) f++;
        /* RWED are "forbidden" when set: the flags name what is allowed */
        uint32_t named = 0;
        for (; *f; f++) { const char *b = strchr(bits, toupper((unsigned char)*f)); if (b) named |= 1u << (7 - (b - bits)); }
        uint32_t allow = (prot ^ 0x0F) & 0xFF;
        if (mode > 0) allow |= named; else if (mode < 0) allow &= ~named; else allow = named;
        prot = (prot & ~0xFFu) | ((allow ^ 0x0F) & 0xFF);
        dos_set_protection(hp, prot);
    } else {
        char s[9];
        uint32_t allow = (prot ^ 0x0F) & 0xFF;
        for (int i = 0; i < 8; i++) s[i] = allow & (1u << (7 - i)) ? (char)tolower(bits[i]) : '-';
        s[8] = 0;
        out("%s\n", s);
    }
    dos_unlock(l);
    return 0;
}

static int c_endcli(int argc, char argv[][512], const char *rest) { (void)argc; (void)argv; (void)rest; return -2; }

extern int intui_active_screen_id(void);

/* WAIT n [SEC|MIN]: sleep this process only */
static int c_wait(int argc, char argv[][512], const char *rest) {
    (void)rest;
    long n = argc > 1 ? atol(argv[1]) : 1;
    if (argc > 2 && !strncasecmp(argv[2], "MIN", 3)) n *= 60;
    uint32_t t = this_task();
    wr32(t + TC_SIGRECVD, rd32(t + TC_SIGRECVD) & ~(SIGF_SINGLE | SIGBREAKF_CTRL_C));
    sched_timer_add(now_us() + (uint64_t)n * 1000000u, TIMER_SIGNAL, t, SIGF_SINGLE);
    uint32_t got = exec_wait(SIGF_SINGLE | SIGBREAKF_CTRL_C);
    if (got & SIGBREAKF_CTRL_C) { sched_timer_remove(TIMER_SIGNAL, t); out("***Break\n"); return 5; }
    return 0;
}

/* INPUT: inject mouse/keyboard events into the active screen (testing aid)
   INPUT MOVE x y | CLICK x y | DOWN x y | UP x y | RCLICK x y | RDOWN x y | RUP x y
   INPUT KEY text | RAW code | MS n (pause in milliseconds) */
static int c_input(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("INPUT MOVE|CLICK|DOWN|UP|RCLICK|RDOWN|RUP x y, KEY texte, RAW code, CLOSE\n"); return 20; }
    int id = intui_active_screen_id();
    int x = argc > 2 ? atoi(argv[2]) : 0, y = argc > 3 ? atoi(argv[3]) : 0;
    InputEv e = { IEV_MOUSE, id, x, y, 0, 0, 0 };
    const char *op = argv[1];
    if (!strcasecmp(op, "KEY")) {
        for (int i = 2; i < argc; i++)
            for (const char *p = argv[i]; *p || (i < argc - 1 && p == argv[i] + strlen(argv[i])); p++) {
                int ch = *p ? (uint8_t)*p : ' ', qual = 0, raw = keymap_raw_for(ch, &qual);
                InputEv k = { IEV_KEY, id, 0, 0, raw, qual, ch };
                amiga_input_event(&k);
                InputEv u = { IEV_KEY, id, 0, 0, raw | 0x80, qual, 0 };
                amiga_input_event(&u);
                if (!*p) break;
            }
        return 0;
    }
    if (!strcasecmp(op, "CLOSE")) {                          /* the Mac window's close box */
        InputEv k = { IEV_CLOSE_SCREEN, id, 0, 0, 0, 0, 0 };
        amiga_input_event(&k);
        return 0;
    }
    if (!strcasecmp(op, "RAW")) {
        InputEv k = { IEV_KEY, id, 0, 0, (int)strtol(argv[2], 0, 0), 0, 0 };
        amiga_input_event(&k);
        return 0;
    }
    amiga_input_event(&e);                                   /* move there first */
    InputEv b = { IEV_BUTTON, id, x, y, 0, 0, 0 };
    if (!strcasecmp(op, "CLICK")) { b.code = 1; amiga_input_event(&b); b.code = -1; amiga_input_event(&b); }
    else if (!strcasecmp(op, "DOWN")) { b.code = 1; amiga_input_event(&b); }
    else if (!strcasecmp(op, "UP")) { b.code = -1; amiga_input_event(&b); }
    else if (!strcasecmp(op, "RCLICK")) { b.code = 2; amiga_input_event(&b); b.code = -2; amiga_input_event(&b); }
    else if (!strcasecmp(op, "RDOWN")) { b.code = 2; amiga_input_event(&b); }
    else if (!strcasecmp(op, "RUP")) { b.code = -2; amiga_input_event(&b); }
    /* let input.device (priority 20) run now */
    sched_preempt_point();
    return 0;
}

/* RUN: the command line in a new background CLI process */
static void run_body(void *arg) {
    char *line = arg;
    shell_execute(line);
    free(line);
}

static int c_run(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv;
    if (!*rest) { out("RUN: required argument missing\n"); return 20; }
    uint32_t pr = dos_create_cli("Background CLI", run_body, strdup(rest));
    out("[CLI %u]\n", rd32(pr + PR_TASKNUM));
    sched_preempt_point();
    return 0;
}

void shell_run_background(const char *line) {
    dos_create_cli("Background CLI", run_body, strdup(line));
    sched_preempt_point();
}

static void shell_body(void *arg);
static int c_newshell(int argc, char argv[][512], const char *rest);

static int c_loadwb(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv; (void)rest;
    if (workbench_running()) { out("Le Workbench est déjà lancé\n"); return 5; }
    workbench_start();
    sched_preempt_point();
    return 0;
}

static int c_status(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv; (void)rest;
    static const char *states[] = { "invalide", "ajoutée", "active", "prête", "en attente", "exception", "retirée" };
    for (NTask *n = sched_tasks(); n; n = n->next) {
        if (n->dead) continue;
        uint32_t t = n->task;
        char name[128];
        str_get(rd32(t + LN_NAME), name, sizeof name);
        int st = rd8(t + TC_STATE);
        const char *sname = st < 7 ? states[st] : "?";
        uint32_t cli = rd8(t + LN_TYPE) == NT_PROCESS && rd32(t + PR_CLI) ? BADDR(rd32(t + PR_CLI)) : 0;
        if (cli) {
            char cmd[256];
            bstr_get(rd32(cli + CLI_COMMANDNAME), cmd, sizeof cmd);
            out("Process %2u: %-18s pri %3d  %-10s %s\n", rd32(t + PR_TASKNUM), name,
                (int8_t)rd8(t + LN_PRI), sname, rd32(cli + CLI_MODULE) ? cmd : "(pas de commande)");
        } else {
            out("Tâche     : %-18s pri %3d  %s\n", name, (int8_t)rd8(t + LN_PRI), sname);
        }
    }
    return 0;
}

static int c_break(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("BREAK: required argument missing\n"); return 20; }
    uint32_t num = (uint32_t)atoi(argv[1]), sig = 0;
    for (int i = 2; i < argc; i++) {
        char f = (char)toupper((unsigned char)argv[i][0]);
        if (!strcasecmp(argv[i], "ALL")) sig |= SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F;
        else if (f >= 'C' && f <= 'F') sig |= SIGBREAKF_CTRL_C << (f - 'C');
    }
    if (!sig) sig = SIGBREAKF_CTRL_C;
    for (NTask *n = sched_tasks(); n; n = n->next) {
        if (n->dead || rd8(n->task + LN_TYPE) != NT_PROCESS || rd32(n->task + PR_TASKNUM) != num) continue;
        sched_signal(n->task, sig);
        sched_preempt_point();
        return 0;
    }
    out("BREAK: process %u does not exist\n", num);
    return 20;
}

static int c_help(int argc, char argv[][512], const char *rest);

/* SNAPSHOT file: the active screen as composed for display, in PPM (testing aid) */
static int c_snapshot(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("SNAPSHOT: required argument missing\n"); return 20; }
    int id = intui_active_screen_id(), w, h, hi, la;
    if (!amiga_display_size(id, &w, &h, &hi, &la)) { out("SNAPSHOT: aucun écran\n"); return 20; }
    uint32_t *px = malloc(sizeof *px * (size_t)w * h);
    amiga_display_render(id, px, w, h);
    char hp[1024];
    if (!dos_resolve(argv[1], hp, sizeof hp)) { free(px); return 20; }
    FILE *f = fopen(hp, "wb");
    if (!f) { free(px); out("SNAPSHOT: écriture impossible\n"); return 20; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(rgb, 1, 3, f); }
    fclose(f);
    free(px);
    return 0;
}

static const struct { const char *name; Builtin fn; const char *help; } builtins[] = {
    { "ECHO", c_echo, "affiche un texte" },
    { "CD", c_cd, "change de répertoire" },
    { "DIR", c_dir, "liste un répertoire" },
    { "LIST", c_list, "liste détaillée" },
    { "TYPE", c_type, "affiche un fichier" },
    { "MAKEDIR", c_makedir, "crée un répertoire" },
    { "DELETE", c_delete, "supprime des fichiers" },
    { "RENAME", c_rename, "renomme" },
    { "COPY", c_copy, "copie un fichier" },
    { "ASSIGN", c_assign, "volumes et assignations" },
    { "AVAIL", c_avail, "mémoire disponible" },
    { "VERSION", c_version, "version du système ou d'une bibliothèque" },
    { "PROMPT", c_prompt, "change l'invite" },
    { "WHY", c_why, "explique la dernière erreur" },
    { "DATE", c_date, "date et heure" },
    { "SETENV", c_setenv, "variable d'environnement" },
    { "SET", c_set, "variable locale du Shell" },
    { "UNSET", c_unset, "supprime une variable locale" },
    { "PATH", c_path, "chemin de recherche des commandes" },
    { "GETENV", c_getenv, "lit une variable" },
    { "EXECUTE", c_execute, "exécute un script" },
    { "IF", c_if, "condition (dans un script)" },
    { "ELSE", c_scriptonly, "condition (dans un script)" },
    { "ENDIF", c_scriptonly, "condition (dans un script)" },
    { "SKIP", c_scriptonly, "saut à un LAB (dans un script)" },
    { "LAB", c_scriptonly, "étiquette (dans un script)" },
    { "QUIT", c_scriptonly, "quitte un script" },
    { "FAILAT", c_scriptonly, "seuil d'échec (dans un script)" },
    { "ASK", c_ask, "pose une question oui/non" },
    { "WHICH", c_which, "trouve une commande" },
    { "PROTECT", c_protect, "bits de protection" },
    { "RUN", c_run, "lance une commande en arrière-plan" },
    { "LOADWB", c_loadwb, "démarre le Workbench" },
    { "NEWSHELL", c_newshell, "ouvre un Shell dans une fenêtre" },
    { "STATUS", c_status, "liste les processus et tâches" },
    { "BREAK", c_break, "envoie Ctrl-C/D/E/F à un processus" },
    { "WAIT", c_wait, "attend n secondes" },
    { "SNAPSHOT", c_snapshot, "image de l'écran actif (tests)" },
    { "INPUT", c_input, "injecte souris/clavier (tests)" },
    { "ENDCLI", c_endcli, "ferme le Shell" },
    { "ENDSHELL", c_endcli, "ferme le Shell" },
    { "HELP", c_help, "cette liste" },
};

static const char *builtin_name(const char *name) {
    for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (!strcasecmp(name, builtins[i].name)) return builtins[i].name;
    return 0;
}

static int c_help(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv; (void)rest;
    out("Commandes intégrées :\n");
    for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
        out("  %-10s %s\n", builtins[i].name, builtins[i].help);
    out("Les autres commandes sont cherchées dans le répertoire courant puis dans C:.\n");
    return 0;
}

/* ---- dispatcher --------------------------------------------------------- */

void shell_init(void) { last_rc = 0; last_err = 0; }

static int shell_execute_tok(char *clean, char tok[][512], char argv[][512]);

/* `command`: replaced by what the command prints, lines joined by spaces */
static void expand_backquotes(const char *in, char *out, int max) {
    int k = 0, quoted = 0;
    for (const char *p = in; *p && k < max - 1;) {
        if (*p == '"') quoted = !quoted;
        if (*p == '*' && p[1] == '`') { out[k++] = '`'; p += 2; continue; }
        const char *e = *p == '`' && !quoted ? strchr(p + 1, '`') : 0;
        if (!e) { out[k++] = *p++; continue; }
        char cmd[1024];
        int n = (int)(e - p - 1) < (int)sizeof cmd - 1 ? (int)(e - p - 1) : (int)sizeof cmd - 1;
        memcpy(cmd, p + 1, n);
        cmd[n] = 0;
        p = e + 1;
        char tmp[64];
        static int seq;
        snprintf(tmp, sizeof tmp, "T:.bq%d", ++seq);
        uint32_t fh = dos_open_host(tmp, 1);
        if (!fh) continue;
        uint32_t pr = this_task(), old = rd32(pr + PR_COS);
        wr32(pr + PR_COS, fh);
        shell_execute(cmd);
        wr32(pr + PR_COS, old);
        dos_close_handle(fh);
        char hp[1024];
        if (!dos_resolve(tmp, hp, sizeof hp)) continue;
        FILE *f = fopen(hp, "rb");
        int start = k;
        if (f) {
            int ch;
            while ((ch = fgetc(f)) != EOF && k < max - 1) out[k++] = ch == '\n' ? ' ' : (char)ch;
            fclose(f);
        }
        while (k > start && out[k - 1] == ' ') k--;
        remove(hp);
    }
    out[k] = 0;
}

int shell_execute(const char *line) {
    char (*tok)[512] = malloc(64 * 512);                     /* reentrant: SystemTagList may nest */
    char (*argv)[512] = malloc(64 * 512);
    char clean[2048], bq[2048];
    expand_vars(line, bq, sizeof bq);
    if (strchr(bq, '`')) expand_backquotes(bq, clean, sizeof clean);
    else memcpy(clean, bq, sizeof clean);
    int rc = shell_execute_tok(clean, tok, argv);
    free(tok);
    free(argv);
    return rc;
}

static int shell_execute_tok(char *clean, char tok[][512], char argv[][512]) {
    /* redirections: >file, >>file, <file */
    uint32_t in_fh = 0, out_fh = 0;
    int ntok = tokenize(clean, tok, 64);
    if (!ntok) return 0;
    char args[2048] = "";
    int argc = 0;
    for (int i = 0; i < ntok; i++) {
        if (tok[i][0] == '>' && i > 0) {
            int append = tok[i][1] == '>';
            out_fh = dos_open_host(tok[i] + 1 + append, append ? 2 : 1);
            continue;
        }
        if (tok[i][0] == '<' && i > 0) { in_fh = dos_open_host(tok[i] + 1, 0); continue; }
        snprintf(argv[argc++], 512, "%s", tok[i]);
        if (argc > 1) {
            /* keep the original quoting for the program's argument line */
            if (*args) strncat(args, " ", sizeof args - strlen(args) - 1);
            int quote = strchr(tok[i], ' ') != 0;
            if (quote) strncat(args, "\"", sizeof args - strlen(args) - 1);
            strncat(args, tok[i], sizeof args - strlen(args) - 1);
            if (quote) strncat(args, "\"", sizeof args - strlen(args) - 1);
        }
    }
    uint32_t pr = this_task(), old_in = rd32(pr + PR_CIS), old_out = rd32(pr + PR_COS);
    if (in_fh) wr32(pr + PR_CIS, in_fh);
    if (out_fh) wr32(pr + PR_COS, out_fh);

    int rc = -1, handled = 0;
    for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (!strcasecmp(argv[0], builtins[i].name)) { rc = builtins[i].fn(argc, argv, args); handled = 1; break; }

    if (!handled) {
        int found = 0;
        char path[1100];
        if (strchr(argv[0], ':') || strchr(argv[0], '/')) rc = run_command(argv[0], args, &found);
        else {
            rc = run_command(argv[0], args, &found);
            for (int k = 0; !found && k < ncmd_path; k++) {
                size_t l = strlen(cmd_path[k]);
                snprintf(path, sizeof path, "%s%s%s", cmd_path[k], l && (cmd_path[k][l - 1] == ':' || cmd_path[k][l - 1] == '/') ? "" : "/", argv[0]);
                rc = run_command(path, args, &found);
            }
            if (!found) { snprintf(path, sizeof path, "C:%s", argv[0]); rc = run_command(path, args, &found); }
        }
        if (!found) {
            /* a directory name alone means CD, as in the AmigaOS shell */
            uint32_t l = dos_lock(argv[0]);
            struct stat st;
            if (l && !stat(dos_lock_host_path(l), &st) && S_ISDIR(st.st_mode)) {
                dos_unlock(l);
                static char cdargv[2][512];
                snprintf(cdargv[0], 512, "CD");
                snprintf(cdargv[1], 512, "%s", argv[0]);
                rc = c_cd(2, cdargv, argv[0]);
            } else {
                if (l) dos_unlock(l);
                out("%s: Unknown command\n", argv[0]);
                rc = 10;
                last_err = ERROR_OBJECT_NOT_FOUND;
            }
        } else if (rc < 0) {
            out("%s: %s\n", argv[0], rc == -1 && rd32(pr + PR_RESULT2) ? dos_fault_text((int)rd32(pr + PR_RESULT2)) : "Software Failure");
            rc = 20;
        } else {
            last_err = (int)rd32(pr + PR_RESULT2);
        }
    }

    if (in_fh) { wr32(pr + PR_CIS, old_in); dos_close_handle(in_fh); }
    if (out_fh) { wr32(pr + PR_COS, old_out); dos_close_handle(out_fh); }
    if (rc != -2) last_rc = rc;
    return rc;
}

/* ---- the Shell as a native process ----------------------------------------- */

typedef struct { char **cmds; int n; int interactive; int window; } ShellBoot;

static void shell_body(void *arg) {
    ShellBoot *b = arg;
    int rc = 0;
    uint32_t pr = this_task();
    for (int i = 0; i < b->n; i++) {
        if (b->interactive) {                      /* show it as if typed */
            const char *p = shell_prompt();
            dos_write_handle(rd32(pr + PR_COS), p, (int)strlen(p));
            dos_write_handle(rd32(pr + PR_COS), b->cmds[i], (int)strlen(b->cmds[i]));
            dos_write_handle(rd32(pr + PR_COS), "\n", 1);
        }
        rc = shell_execute(b->cmds[i]);
        if (rc == -2) break;
    }
    if (b->interactive && rc != -2) {
        char line[2048];
        for (;;) {
            const char *p = shell_prompt();
            dos_write_handle(rd32(pr + PR_COS), p, (int)strlen(p));
            int n = dos_read_line(rd32(pr + PR_CIS), line, sizeof line);
            if (n < 0) break;
            if (n == 0) {                          /* end of file: Ctrl-\ or the close box */
                if (b->window) { dos_write_handle(rd32(pr + PR_COS), "\n", 1); break; }
                continue;
            }
            line[strcspn(line, "\n")] = 0;
            rc = shell_execute(line);
            if (rc == -2) break;
        }
    }
    if (b->window) {                               /* a NEWSHELL: its window goes with it */
        dos_close_handle(rd32(pr + PR_CIS));
        dos_close_handle(rd32(pr + PR_COS));
        wr32(pr + PR_CIS, 0);
        wr32(pr + PR_COS, 0);
        return;
    }
    if (host.shutdown) host.shutdown(rc < 0 ? 0 : rc);
}

static int c_newshell(int argc, char argv[][512], const char *rest) {
    (void)argc; (void)argv;
    const char *spec = *rest ? rest : "CON:0/12/640/160/AmigaShell/CLOSE";
    if (!strncasecmp(spec, "CON:", 4)) spec += 4;
    uint32_t in, outh;
    if (!dos_open_console(&sched_current()->cpu, spec, &in, &outh)) { out("NEWSHELL: impossible d'ouvrir la fenêtre\n"); return 20; }
    ShellBoot *b = calloc(1, sizeof *b);
    b->interactive = 1;
    b->window = 1;
    uint32_t pr = dos_create_cli_io("Shell Process", shell_body, b, in, outh);
    const char *hello = "AmigaMac Shell\n";
    dos_write_handle(outh, hello, (int)strlen(hello));
    (void)pr;
    sched_preempt_point();
    return 0;
}

void os_start(const char **commands, int ncommands, int interactive) {
    ShellBoot *b = calloc(1, sizeof *b);
    b->cmds = calloc(ncommands + 1, sizeof *b->cmds);
    for (int i = 0; i < ncommands; i++) b->cmds[i] = strdup(commands[i]);
    b->n = ncommands;
    b->interactive = interactive;
    dos_create_cli("Shell Process", shell_body, b);
    sched_start();
}
