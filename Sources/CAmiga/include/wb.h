/* icon.library, asl.library and the Workbench: what they share natively */
#ifndef AMIGA_WB_H
#define AMIGA_WB_H
#include <stdint.h>
#include "intui.h"

/* workbench/workbench.h */
#define WBDISK 1
#define WBDRAWER 2
#define WBTOOL 3
#define WBPROJECT 4
#define WBGARBAGE 5
#define WBDEVICE 6
#define WBKICK 7
#define WBAPPICON 8
#define NO_ICON_POSITION 0x80000000u
#define WB_DISKMAGIC 0xE310
#define WB_DISKVERSION 1
#define WB_DISKREVISION 1

/* icon.c */
void icon_init(void);
uint32_t icon_get(const char *name);                       /* the .info or 0 */
uint32_t icon_get_new(const char *name);                   /* the .info or a default one */
uint32_t icon_default(int type);
int icon_put(const char *name, uint32_t dobj);
void icon_free(uint32_t dobj);
int icon_type_of(const char *name);                        /* WBDISK/WBDRAWER/WBTOOL/WBPROJECT, 0 if missing */
void icon_size(uint32_t dobj, int *w, int *h);             /* the image (hit box) */
uint32_t icon_env_default(const char *name);   /* ENV:Sys/def_<name>.info */
void icon_draw(uint32_t rp, uint32_t dobj, const char *label, int x, int y, int selected, const uint16_t *pens);
uint32_t icon_find_tooltype(uint32_t array, const char *name);
extern uint32_t IconBase;

/* asl.c */
void asl_init(void);
extern uint32_t AslBase;

/* workbench.c */
void workbench_init(void);
void workbench_start(void);
int workbench_running(void);
extern uint32_t WorkbenchBase;

#endif
