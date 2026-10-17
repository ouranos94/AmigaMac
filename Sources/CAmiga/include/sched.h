/* exec's scheduler on macOS threads.
 *
 * Every Amiga task has its own host thread and its own 68k context, but only
 * the task holding the baton (the global lock) runs. Blocking inside native
 * code (Wait, WaitPort, DoIO, console reads) just passes the baton on.
 * The Amiga-visible state (ThisTask, TaskReady/TaskWait lists, tc_State,
 * signals) is kept exactly as exec keeps it.
 */
#ifndef AMIGA_SCHED_H
#define AMIGA_SCHED_H

#include <pthread.h>
#include <stdint.h>
#include "cpu68k.h"

typedef struct NTask NTask;
struct NTask {
    uint32_t task;                  /* Amiga Task / Process */
    M68K cpu;                       /* its 68k context */
    pthread_t thread;
    pthread_cond_t cond;
    int dead;
    void (*native)(void *arg);      /* native body (shell, CLI...), or NULL */
    void *arg;
    uint32_t initpc, finalpc;       /* 68k body */
    int8_t tdnest, idnest;          /* Forbid/Disable state, saved across Wait */
    int console_wait;               /* waiting for a console line */
    int input_wait;                 /* waiting for an input event */
    NTask *next;
};

void sched_init(void);
NTask *sched_current(void);
NTask *sched_find(uint32_t task);
NTask *sched_tasks(void);           /* all tasks (check ->dead) */

/* create the host side of a task and make it ready (its thread starts
   running when it gets the baton) */
NTask *sched_add(uint32_t task, void (*native)(void *), void *arg, uint32_t initpc, uint32_t finalpc);
/* boot: hand the baton to the first task; returns immediately */
void sched_start(void);

void sched_block(void);             /* current task leaves TS_RUN (state set by caller) */
void sched_wake(uint32_t task);     /* waiting task whose signals arrived -> ready */
void sched_signal(uint32_t task, uint32_t sigs);   /* exec Signal() */
void sched_preempt_point(void);     /* called by running code: quantum / higher priority */
void sched_exit(void) __attribute__((noreturn));   /* current task ends */
void sched_kill(uint32_t task);     /* RemTask of another task */
int sched_forbidden(void);
extern volatile int sched_need_resched;

/* run the current task's 68k code until cpu->stop, with preemption */
void sched_run_cpu(M68K *c);

/* time */
uint64_t now_us(void);
enum { TIMER_REPLY, TIMER_SIGNAL };
void sched_timer_add(uint64_t when_us, int kind, uint32_t a, uint32_t b);
int sched_timer_remove(int kind, uint32_t a);

/* input events (mouse, keys, ticks) from the host, any thread */
enum { IEV_MOUSE = 1, IEV_BUTTON, IEV_KEY, IEV_TICK, IEV_CLOSE_SCREEN };
typedef struct {
    int type, screen;           /* screen: host id of the screen window */
    int x, y;                   /* screen pixels */
    int code;                   /* IEV_BUTTON: 1 left, 2 right, 3 middle (negative: release)
                                   IEV_KEY: Amiga raw key, | 0x80 on release */
    int qual;                   /* IEQUALIFIER bits */
    int ch;                     /* IEV_KEY: Latin-1 character or 0 */
} InputEv;
void amiga_input_event(const InputEv *e);
int sched_input_read(InputEv *e);   /* blocks the calling task (input.device) */

/* console: lines come from the host, any thread */
void amiga_console_input(const char *s, int len);
void amiga_console_break(void);     /* Ctrl-C to the console's foreground task */
int sched_console_read(char *buf, int max);       /* blocks the task, not the system */
int sched_console_ready(void);
void sched_set_console_owner(uint32_t task);

#endif
