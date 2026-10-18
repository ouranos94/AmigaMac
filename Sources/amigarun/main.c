/* amigarun: AmigaMac without the GUI, for tests and scripting.
 *   amigarun <system dir>                 interactive Shell on the terminal
 *   amigarun <system dir> "<command>" ... run Shell command lines and exit
 */
#include "os.h"
#include "sched.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t done_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_c = PTHREAD_COND_INITIALIZER;
static int done, exit_rc;

/* Amiga text is ISO-8859-1, the terminal UTF-8 */
static void host_con_write(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch < 0x80) putchar(ch);
        else { putchar(0xC0 | ch >> 6); putchar(0x80 | (ch & 0x3F)); }
    }
    fflush(stdout);
}
static void log_msg(const char *m) { fprintf(stderr, "[AmigaMac] %s\n", m); }
static void open_file(const char *path) { fprintf(stderr, "[AmigaMac] ouvrir avec macOS : %s\n", path); }

static char clip[65536];
static int clip_get(char *buf, int max) { snprintf(buf, (size_t)max, "%s", clip); return (int)strlen(buf); }
static void clip_set(const char *s, int n) { snprintf(clip, sizeof clip, "%.*s", n, s); fprintf(stderr, "[AmigaMac] presse-papiers : %s\n", clip); }

static void on_shutdown(int rc) {
    pthread_mutex_lock(&done_m);
    done = 1;
    exit_rc = rc;
    pthread_cond_signal(&done_c);
    pthread_mutex_unlock(&done_m);
}

static void *stdin_reader(void *arg) {
    (void)arg;
    char line[2048];
    while (fgets(line, sizeof line, stdin)) amiga_console_input(line, (int)strlen(line));
    amiga_console_input("endcli\n", 7);          /* end of input closes the Shell */
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: amigarun <dossier système> [commande...]\n"); return 1; }
    host.console_write = host_con_write;
    host.log = log_msg;
    if (!getenv("AMIGAMAC_SOUND")) setenv("AMIGAMAC_MUTE", "1", 0);   /* tests stay quiet */
    host.shutdown = on_shutdown;
    host.open_file = open_file;
    host.clipboard_get = clip_get;
    host.clipboard_set = clip_set;
    if (getenv("AMIGAMAC_CLIP")) snprintf(clip, sizeof clip, "%s", getenv("AMIGAMAC_CLIP"));
    os_boot(argv[1]);
    int interactive = argc == 2;
    if (interactive) {
        pthread_t t;
        pthread_create(&t, 0, stdin_reader, 0);
        pthread_detach(t);
    }
    os_start((const char **)argv + 2, argc - 2, interactive);
    pthread_mutex_lock(&done_m);
    while (!done) pthread_cond_wait(&done_c, &done_m);
    pthread_mutex_unlock(&done_m);
    return exit_rc;
}
