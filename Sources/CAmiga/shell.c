/* The Shell, native: built-in commands plus 68k programs from disk.
 *
 * AmigaOS ships its commands as programs in C:. Here the common ones are
 * built into the shell (written from scratch); anything else is searched in
 * the current directory then C: and run as a 68k executable.
 */
#include "os.h"
#include "wb.h"
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

static int list_dir(const char *apath, Entry **res) {
    char hp[1024];
    if (!dos_resolve(apath, hp, sizeof hp)) return -1;
    DIR *d = opendir(hp);
    if (!d) return -1;
    int n = 0, cap = 64;
    Entry *e = malloc(sizeof *e * cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
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
    if (n < 0) { out("Can't examine \"%s\": object not found\n", path); return 20; }
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

static int c_execute(int argc, char argv[][512], const char *rest) {
    (void)rest;
    if (argc < 2) { out("EXECUTE: required argument missing\n"); return 20; }
    char hp[1024];
    if (!dos_resolve(argv[1], hp, sizeof hp)) return 20;
    FILE *f = fopen(hp, "r");
    if (!f) { out("Can't open %s\n", argv[1]); return 20; }
    char line[2048];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '.') continue;                    /* script directives (.KEY ...) */
        rc = shell_execute(line);
        if (rc == -2) break;
        if (rc >= 20) break;                             /* default FAILAT */
    }
    fclose(f);
    return rc < 0 ? 0 : rc;
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
    if (argc < 2) { out("INPUT MOVE|CLICK|DOWN|UP|RCLICK|RDOWN|RUP x y, KEY texte, RAW code\n"); return 20; }
    int id = intui_active_screen_id();
    int x = argc > 2 ? atoi(argv[2]) : 0, y = argc > 3 ? atoi(argv[3]) : 0;
    InputEv e = { IEV_MOUSE, id, x, y, 0, 0, 0 };
    const char *op = argv[1];
    if (!strcasecmp(op, "KEY")) {
        for (int i = 2; i < argc; i++)
            for (const char *p = argv[i]; *p || (i < argc - 1 && p == argv[i] + strlen(argv[i])); p++) {
                InputEv k = { IEV_KEY, id, 0, 0, 0x40, 0, *p ? (uint8_t)*p : ' ' };
                amiga_input_event(&k);
                if (!*p) break;
            }
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
    { "GETENV", c_getenv, "lit une variable" },
    { "EXECUTE", c_execute, "exécute un script" },
    { "RUN", c_run, "lance une commande en arrière-plan" },
    { "LOADWB", c_loadwb, "démarre le Workbench" },
    { "STATUS", c_status, "liste les processus et tâches" },
    { "BREAK", c_break, "envoie Ctrl-C/D/E/F à un processus" },
    { "WAIT", c_wait, "attend n secondes" },
    { "SNAPSHOT", c_snapshot, "image de l'écran actif (tests)" },
    { "INPUT", c_input, "injecte souris/clavier (tests)" },
    { "ENDCLI", c_endcli, "ferme le Shell" },
    { "ENDSHELL", c_endcli, "ferme le Shell" },
    { "HELP", c_help, "cette liste" },
};

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

int shell_execute(const char *line) {
    static char tok[64][512];
    char clean[2048];
    snprintf(clean, sizeof clean, "%s", line);
    /* redirections: >file, >>file, <file */
    uint32_t in_fh = 0, out_fh = 0;
    int ntok = tokenize(clean, tok, 64);
    if (!ntok) return 0;
    char args[2048] = "";
    int argc = 0;
    static char argv[64][512];
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

typedef struct { char **cmds; int n; int interactive; } ShellBoot;

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
            if (n == 0) continue;
            line[strcspn(line, "\n")] = 0;
            rc = shell_execute(line);
            if (rc == -2) break;
        }
    }
    if (host.shutdown) host.shutdown(rc < 0 ? 0 : rc);
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
