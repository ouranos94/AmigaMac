/* Native AmigaOS: shared declarations between the library implementations. */
#ifndef AMIGA_OS_H
#define AMIGA_OS_H

#include <stdint.h>
#include "cpu68k.h"
#include "mem.h"
#include "amiga.h"

/* ---- host interface: how the OS talks to the macOS side ----------------- */
typedef struct {
    void (*console_write)(const char *buf, int len);
    void (*log)(const char *msg);
    void (*shutdown)(int rc);                     /* the last Shell has ended */
    void (*screen_opened)(int id, const char *title);   /* open a macOS window for it */
    void (*screen_closed)(int id);
    void (*beep)(void);
    void (*open_file)(const char *host_path);   /* open with the macOS application */
    int (*clipboard_get)(char *utf8, int max);  /* the Mac's text clipboard, -1 if none */
    void (*clipboard_set)(const char *utf8, int len);
    /* the pointer over a screen: kind 0 the Mac arrow, 1 the image (0xAARRGGBB, lores pixels), 2 hidden */
    void (*pointer)(int screen, int kind, const uint32_t *argb, int w, int h, int hotx, int hoty);
} AmigaHost;
/* input goes the other way: amiga_console_input() / amiga_console_break()
   (sched.h), callable from any host thread */

extern AmigaHost host;
void os_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- display, for the host (any thread) ------------------------------------ */
int amiga_display_size(int id, int *w, int *h, int *hires, int *lace);
int amiga_display_render(int id, uint32_t *rgba, int w, int h);   /* 0xFFRRGGBB */
uint32_t amiga_display_serial(void);                              /* changes when anything is drawn */
/* the display chips' picture (chipset.c), shown while a program drives the copper itself */
#define AMIGA_CHIP_SCREEN 0x4000
void chip_init(void);
void chip_write(uint32_t addr, int size, uint32_t v);
void chip_frame(void);
int chip_active(void);
void chip_task_gone(uint32_t task);
uint32_t chip_frames(void);
uint32_t chip_dmaconr_bits(void);
int chip_display_size(int *w, int *h);
int chip_display_render(uint32_t *dst, int w, int h);
void chip_mouse_moved(int dx, int dy);
int chip_pointer(int *x, int *y);
int chip_pointer_now(int *x, int *y);
int chip_screen_point(int cx, int cy, int *id, int *sx, int *sy);

/* ---- 68k heap (native bookkeeping, Amiga addresses) --------------------- */
void heap_init(uint32_t start, uint32_t end);
uint32_t heap_alloc(uint32_t size, uint32_t flags);     /* 0 on failure */
void heap_free(uint32_t addr, uint32_t size);
uint32_t heap_avail(uint32_t flags);

/* ---- native functions behind library vectors ---------------------------- */
typedef uint32_t (*NativeFn)(M68K *cpu);
typedef struct { const char *name; NativeFn fn; } NativeEntry;

/* one library function as described by the .fd files (generated table) */
typedef struct { uint16_t lvo; const char *name; const char *regs; } FdFunc;
typedef struct { const char *lib; const FdFunc *funcs; int count; } FdLib;
extern const FdLib fd_libs[];
extern const int fd_lib_count;

/* build a library in 68k memory: negative jump table of traps, positive
 * part of possize bytes; names resolved against the native entries */
uint32_t lib_create(const char *name, const char *fdname, uint32_t possize,
                    const NativeEntry *natives, int version, int revision, const char *idstring);
void lib_add(uint32_t base);                       /* add to ExecBase LibList */
int lvo_of(const char *lib, const char *fn);       /* positive offset, from the .fd */

/* traps */
void trap_init(void);
uint32_t trap_new(const char *name, NativeFn fn);  /* returns trap id */
void aline_handler(M68K *cpu, uint16_t op);
/* call 68k code at addr from native code; registers as set in cpu */
void cpu_call(M68K *cpu, uint32_t addr);
uint32_t lib_call(M68K *c, uint32_t base, const char *lib, const char *fn, ...);

/* fixed low-memory stubs */
extern uint32_t stub_exit;     /* return address of a launched program */
extern uint32_t stub_taskexit; /* default finalPC of a task */
extern uint32_t stub_return;   /* return address used by cpu_call */

/* ---- strings and lists in 68k memory ------------------------------------ */
uint32_t str_alloc(const char *s);                 /* NUL-terminated copy, heap */
int str_get(uint32_t addr, char *buf, int max);    /* copy C string out */
int bstr_get(uint32_t bptr, char *buf, int max);   /* copy BSTR (BPTR) out */
void list_new(uint32_t list);
void list_addhead(uint32_t list, uint32_t node);
void list_addtail(uint32_t list, uint32_t node);
uint32_t list_remhead(uint32_t list);
uint32_t list_remtail(uint32_t list);
void list_remove(uint32_t node);
void list_insert(uint32_t list, uint32_t node, uint32_t pred);
void list_enqueue(uint32_t list, uint32_t node);
uint32_t list_findname(uint32_t start, const char *name);

/* ---- system state ------------------------------------------------------- */
extern uint32_t SysBase, DOSBase;
uint32_t this_task(void);
int os_boot(const char *sysdir);                   /* build ExecBase & co */
void os_start(const char **commands, int ncommands, int interactive);  /* first Shell, scheduler */
void exec_init(void);
uint32_t exec_create_process(const char *name, uint32_t stacksize);
void dos_init(const char *sysdir);
extern uint32_t dos_handler_port;
void dos_packet(uint32_t dp);

/* run a 68k program as the CLI process; returns its D0 */
int run_command(const char *path, const char *args, int *found);

/* dos internals used by the shell */
uint32_t dos_loadseg(const char *host_path);
void dos_unloadseg(uint32_t seglist);
int dos_set_ioerr(int err);
int dos_resolve(const char *amiga_path, char *host_path, int max);
const char *dos_fault_text(int err);
int dos_match_pattern(const char *pat, const char *str, int nocase);
int dos_has_wild(const char *pat);

/* dos services for the shell */
uint32_t dos_open_host(const char *apath, int mode);   /* 0 read, 1 write, 2 append */
void dos_write_handle(uint32_t bfh, const char *s, int len);
int dos_read_line(uint32_t bfh, char *buf, int max);       /* -1 at end of file */
void dos_close_handle(uint32_t bfh);
uint32_t dos_create_cli(const char *name, void (*body)(void *), void *arg);
uint32_t dos_create_cli_io(const char *name, void (*body)(void *), void *arg, uint32_t in, uint32_t out);
int dos_open_console(M68K *c, const char *spec, uint32_t *in, uint32_t *out);
const char *dos_assign_path(const char *name);
void dos_assign(const char *name, const char *path);
int dos_assign_count(void);
const char *dos_assign_name(int i);
const char *dos_assign_target(int i);
uint32_t dos_lock(const char *apath);
void dos_unlock(uint32_t bl);
const char *dos_lock_host_path(uint32_t bl);
void dos_amiga_name(const char *hp, char *out, int max);
void dos_name_to_amiga(const char *utf8, char *latin1, int max);
uint32_t dos_protection(const char *host_path);           /* Amiga protection bits */
void dos_set_protection(const char *host_path, uint32_t prot);
int shell_run_script(const char *host_path, int argc, char argv[][512]);
int shell_run_script_args(const char *host_path, const char *args);

/* native shell */
void shell_init(void);
int shell_execute(const char *line);              /* returns command result, -2 = EndCLI */
const char *shell_prompt(void);
void shell_run_background(const char *line);

/* exec helpers */
uint32_t exec_wait(uint32_t mask);
uint32_t exec_waitport(uint32_t port);
void exec_reply_msg(uint32_t msg);
void exec_put_msg(uint32_t port, uint32_t msg);
void exec_remove_task(uint32_t task);
void timer_init(void);
void math_init(void);
void locale_init(void);
void diskfont_init(void);
void iffparse_init(void);
int input_buttons(void);
void clipboard_init(void);
void keyboard_init(void);
void inputdevice_init(void);
void commodities_init(void);
void cybergfx_init(void);
void reqtools_init(void);
void cia_init(void);
void rexxsyslib_init(void);
uint32_t timer_systime_secs(void);
uint32_t exec_allocmem(uint32_t size, uint32_t flags);
void exec_freemem(uint32_t addr, uint32_t size);
uint32_t exec_allocvec(uint32_t size, uint32_t flags);
void exec_freevec(uint32_t addr);
uint32_t exec_openlibrary(const char *name, uint32_t version);

#define BADDR(b) ((uint32_t)(b) << 2)
#define MKBADDR(a) ((uint32_t)(a) >> 2)

#endif
