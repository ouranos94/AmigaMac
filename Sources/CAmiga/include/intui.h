/* intuition internals shared by intuition.c (screens, windows, display)
 * and intuition_input.c (input handling, gadgets, menus, requesters). */
#ifndef AMIGA_INTUI_H
#define AMIGA_INTUI_H

#include <pthread.h>
#include <stdint.h>
#include "os.h"
#include "gfx.h"
#include "sched.h"

/* Window flags (intuition/intuition.h) */
#define WFLG_SIZEGADGET 0x1
#define WFLG_DRAGBAR 0x2
#define WFLG_DEPTHGADGET 0x4
#define WFLG_CLOSEGADGET 0x8
#define WFLG_SIZEBRIGHT 0x10
#define WFLG_SIZEBBOTTOM 0x20
#define WFLG_SIMPLE_REFRESH 0x40
#define WFLG_BACKDROP 0x100
#define WFLG_REPORTMOUSE 0x200
#define WFLG_GIMMEZEROZERO 0x400
#define WFLG_BORDERLESS 0x800
#define WFLG_ACTIVATE 0x1000
#define WFLG_WINDOWACTIVE 0x2000
#define WFLG_MENUSTATE 0x8000
#define WFLG_RMBTRAP 0x10000
#define WFLG_NOCAREREFRESH 0x20000
#define WFLG_NW_EXTENDED 0x40000
#define WFLG_WBENCHWINDOW 0x2000000
#define WFLG_HASZOOM 0x20000000

/* IDCMP classes */
#define IDCMP_SIZEVERIFY 0x1
#define IDCMP_NEWSIZE 0x2
#define IDCMP_REFRESHWINDOW 0x4
#define IDCMP_MOUSEBUTTONS 0x8
#define IDCMP_MOUSEMOVE 0x10
#define IDCMP_GADGETDOWN 0x20
#define IDCMP_GADGETUP 0x40
#define IDCMP_MENUPICK 0x100
#define IDCMP_CLOSEWINDOW 0x200
#define IDCMP_RAWKEY 0x400
#define IDCMP_ACTIVEWINDOW 0x40000
#define IDCMP_INACTIVEWINDOW 0x80000
#define IDCMP_VANILLAKEY 0x200000
#define IDCMP_INTUITICKS 0x400000
#define IDCMP_CHANGEWINDOW 0x2000000

#define SELECTDOWN 0x68
#define SELECTUP 0xE8
#define MENUDOWN 0x69
#define MENUUP 0xE9
#define MENUNULL 0xFFFF

/* Gadget flags / activation / types */
#define GFLG_GADGHIGHBITS 0x3
#define GFLG_GADGHCOMP 0x0
#define GFLG_GADGHBOX 0x1
#define GFLG_GADGHIMAGE 0x2
#define GFLG_GADGHNONE 0x3
#define GFLG_GADGIMAGE 0x4
#define GFLG_RELBOTTOM 0x8
#define GFLG_RELRIGHT 0x10
#define GFLG_RELWIDTH 0x20
#define GFLG_RELHEIGHT 0x40
#define GFLG_SELECTED 0x80
#define GFLG_DISABLED 0x100
#define GACT_RELVERIFY 0x1
#define GACT_IMMEDIATE 0x2
#define GACT_ENDGADGET 0x4
#define GACT_FOLLOWMOUSE 0x8
#define GACT_TOGGLESELECT 0x100
#define GACT_STRINGCENTER 0x200
#define GACT_STRINGRIGHT 0x400
#define GACT_LONGINT 0x800
#define GTYP_GTYPEMASK 0x7
#define GTYP_BOOLGADGET 1
#define GTYP_PROPGADGET 3
#define GTYP_STRGADGET 4
#define GTYP_SYSGADGET 0x8000

/* DrawInfo pens */
enum { DETAILPEN, BLOCKPEN, TEXTPEN, SHINEPEN, SHADOWPEN, FILLPEN, FILLTEXTPEN,
       BACKGROUNDPEN, HIGHLIGHTTEXTPEN, BARDETAILPEN, BARBLOCKPEN, BARTRIMPEN, NUMDRIPENS };

typedef struct NWindow NWindow;
typedef struct NScreen NScreen;

struct NScreen {
    uint32_t scr;                   /* struct Screen */
    int id;                         /* host window id */
    int w, h, depth, hires, lace;   /* hires: 0 low, 1 high, 2 super-high, 3 square pixels */
    uint32_t modeid;                /* display database ID */
    uint8_t pen_used[256];          /* pens handed out by screen_obtain_pen */
    uint32_t pal[256];              /* 0x00RRGGBB */
    uint16_t pens[NUMDRIPENS];
    uint32_t drawinfo;
    int locks;                      /* LockPubScreen count */
    int workbench;
    uint32_t custom_bm;             /* CUSTOMBITMAP / SA_BitMap: the program's own BitMap */
    NWindow *wins;                  /* front to back */
    uint8_t *ov;                    /* menu overlay: pen per pixel, 0xFF transparent */
    NScreen *next;
};

struct NWindow {
    uint32_t win, rp, layer, bm;
    uint32_t ibm, irp;              /* GIMMEZEROZERO: the inner bitmap and the program's RastPort */
    int shared_bm;                  /* a borderless backdrop window at 0,0 draws into the screen's bitmap */
    NScreen *s;
    uint32_t task;                  /* opener: owner of the UserPort signal */
    int own_port;                   /* UserPort created by intuition */
    int x, y, w, h;
    /* native consumer of this window's keys and events (consoles) */
    void (*hook)(NWindow *w, int kind, int a, int b, int c);
    /* its pointer: 0 the system one, 1 SetPointer's image, 2 busy */
    int ptr_kind, ptr_w, ptr_h, ptr_hx, ptr_hy;
    int zx, zy, zw, zh;             /* WA_Zoom: the other box the zoom gadget switches to */
    uint8_t *ptr_pix;               /* pens 0-3, ptr_w x ptr_h */
    NWindow *next;                  /* towards the back */
};

enum { HOOK_KEY = 1, HOOK_CLOSE, HOOK_NEWSIZE };

extern pthread_mutex_t disp_lock;
extern NScreen *screens;
extern NWindow *active_win;
extern uint32_t ireply_port;        /* replied IntuiMessages come back here */

/* intuition.c */
NWindow *nwin_of(uint32_t win);
NScreen *nscr_of(uint32_t scr);
NScreen *workbench_screen(int open_it);
void draw_frame(NWindow *w);
void draw_screen_bar(NScreen *s);
void win_activate(NWindow *w);
void pointer_update(void);
void win_zip(NWindow *w);
int zoom_width(NWindow *w);
int frame_pen_inactive(NScreen *s);   /* the frame of inactive windows */
int desktop_pen(NScreen *s);          /* the Workbench backdrop */
int screen_obtain_pen(NScreen *s, uint32_t rgb);   /* a pen showing that colour (exact if free) */
void pointer_sprite0(const uint8_t *pix, int h, int on);   /* graphics: sprite 0 is the pointer */
void win_to_front(NWindow *w);
void win_to_back(NWindow *w);
void win_move(NWindow *w, int x, int y);
void win_resize(NWindow *w, int width, int height);
void send_idcmp(NWindow *w, uint32_t cls, uint32_t code, uint32_t qual, uint32_t iaddr, int mx, int my);
uint32_t open_window(M68K *c, uint32_t nw, uint32_t tags);
void close_window(NWindow *w);
int border_top(NScreen *s);

/* intuition_input.c */
void input_task_main(void *arg);
void gadget_render_all(NWindow *w, uint32_t first, int count);
void gadget_box(NWindow *w, uint32_t g, int *x, int *y, int *gw, int *gh);
void draw_itext(uint32_t rp, uint32_t it, int left, int top);
void draw_border(uint32_t rp, uint32_t border, int left, int top);
void draw_image(uint32_t rp, uint32_t img, int left, int top, int state);
void draw_image_ex(uint32_t rp, uint32_t img, int left, int top, int state, int clear0);
int itext_length(uint32_t it);
extern const NativeEntry intui_input_natives[];

/* boopsi.c */
typedef uint32_t (*Dispatcher)(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg);
uint32_t bo_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg);
uint32_t bo_class_of(uint32_t obj);
uint32_t bo_native_class(const char *name, const char *super, int instsize, Dispatcher fn);
uint32_t bo_new(M68K *c, uint32_t cl, uint32_t tags);
uint32_t bo_inst(uint32_t cl, uint32_t obj);
uint32_t bo_super(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg);
uint32_t bo_ginfo(NWindow *w);
void bo_ginfo_free(uint32_t gi);
void boopsi_init(void);
extern const NativeEntry boopsi_natives[];
int bo_is_object(uint32_t p);
void bo_render(M68K *c, NWindow *w, uint32_t g, int redraw);
int bo_hittest(M68K *c, NWindow *w, uint32_t g, int gx, int gy);
uint32_t bo_input(M68K *c, NWindow *w, uint32_t g, int method, int ieclass, int iecode, int qual, int gx, int gy, uint32_t *code);
void bo_goinactive(M68K *c, NWindow *w, uint32_t g, int abort_);
void bo_gadget_changed(M68K *c, NWindow *w, uint32_t g, int interim);
void bo_draw_image(M68K *c, uint32_t rp, uint32_t img, int x, int y, int state, uint32_t dri);
uint32_t bo_domethod(M68K *c, uint32_t obj, int n, const uint32_t *w);

/* host side of the display */
void host_screen_opened(NScreen *s, const char *title);
void host_screen_closed(NScreen *s);

#endif
