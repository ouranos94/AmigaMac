/* System start: memory, traps, exec, devices, dos. os_start() (shell.c)
 * then creates the first Shell and hands over to the scheduler. */
#include "os.h"
#include "sched.h"
#include "gfx.h"
#include "wb.h"
#include "console.h"
#include "audio.h"
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE (256u << 20)                /* 256 MB of 68k address space (true-colour screens are big) */

int os_boot(const char *sysdir) {
    mem_init(MEM_SIZE);
    trap_init();
    heap_init(0x1000, MEM_SIZE);             /* below: vectors, SysBase pointer, stubs */
    sched_init();
    exec_init();
    timer_init();
    utility_init();
    math_init();
    console_init();
    locale_init();
    diskfont_init();
    iffparse_init();
    audio_init();
    clipboard_init();
    keyboard_init();
    inputdevice_init();
    commodities_init();
    cybergfx_init();
    cia_init();
    rexxsyslib_init();
    gfx_init();
    chip_init();
    layers_init();
    intuition_init();
    gadtools_init();
    icon_init();
    asl_init();
    workbench_init();
    datatypes_init();
    char abs[1024];
    dos_init(realpath(sysdir, abs) ? abs : sysdir);
    reqtools_init();
    shell_init();
    return 1;
}
