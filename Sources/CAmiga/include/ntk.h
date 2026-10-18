/* native UI toolkit helpers (ntk.c) */
#ifndef AMIGA_NTK_H
#define AMIGA_NTK_H
#include <stdint.h>
#include <stdlib.h>
#include "intui.h"

typedef struct { uint32_t *blk; int n, cap; } Arena;
uint32_t ar_alloc(Arena *a, uint32_t size);
uint32_t ar_str(Arena *a, const char *s);
uint32_t ar_tags(Arena *a, ...);                 /* tag, data, ..., 0 */
void ar_free(Arena *a);
void put_cstr(uint32_t dst, const char *s, int max);

typedef struct { uint32_t cls, code, qual, iaddr, secs, micros; int mx, my; } NMsg;
int ntk_getmsg(uint32_t win, NMsg *m);
uint32_t ntk_sigmask(uint32_t win);
int ntk_double_click(uint32_t s1, uint32_t m1, uint32_t s2, uint32_t m2);
int ntk_gadget_string(uint32_t g, char *buf, int max);

extern uint32_t GadToolsBase;
uint32_t ntk_new_gadget(M68K *c, Arena *A, int kind, uint32_t prev, uint32_t vi, int x, int y, int w, int h,
                        const char *label, int id, int flags, uint32_t tags);
void ntk_set_gattr(M68K *c, Arena *A, uint32_t g, uint32_t win, uint32_t tag, uint32_t data);
void to_latin1(const char *in, char *out, int max);
uint32_t ar_text(Arena *a, const char *utf8);

/* tags used by native UIs */
#define TAG_DONE_ 0
#define WA_(n) (0x80000000u + 99 + (n))
#define WA_Left_ WA_(1)
#define WA_Top_ WA_(2)
#define WA_Width_ WA_(3)
#define WA_Height_ WA_(4)
#define WA_IDCMP_ WA_(7)
#define WA_Flags_ WA_(8)
#define WA_Gadgets_ WA_(9)
#define WA_Title_ WA_(11)
#define WA_CustomScreen_ WA_(13)
#define WA_MinWidth_ WA_(15)
#define WA_MinHeight_ WA_(16)
#define WA_MaxWidth_ WA_(17)
#define WA_MaxHeight_ WA_(18)
#define WA_PubScreen_ WA_(22)
#define WA_SizeGadget_ WA_(30)
#define WA_DragBar_ WA_(31)
#define WA_DepthGadget_ WA_(32)
#define WA_CloseGadget_ WA_(33)
#define WA_Backdrop_ WA_(34)
#define WA_ReportMouse_ WA_(35)
#define WA_Borderless_ WA_(37)
#define WA_Activate_ WA_(38)
#define WA_RMBTrap_ WA_(39)
#define WA_SimpleRefresh_ WA_(41)
#define WA_NewLookMenus_ WA_(48)
#define WA_Zoom_ WA_(0x1A)
#define WA_SizeBRight_ WA_(0x2B)
#define WA_SizeBBottom_ WA_(0x2C)
#endif
