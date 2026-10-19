/* keyboard.device, written from the keyboard.device autodocs: the key matrix
 * (which keys are held down) and reset handlers. Key events reach programs
 * through input.device and Intuition, so KBD_READEVENT returns nothing. */
#include "os.h"
#include <stdlib.h>
#include <string.h>

#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define IO_ACTUAL 32
#define IO_LENGTH 36
#define IO_DATA 40
#define IOF_QUICK 1
#define KBD_READEVENT 9
#define KBD_READMATRIX 10
#define KBD_ADDRESETHANDLER 11
#define KBD_REMRESETHANDLER 12
#define KBD_RESETHANDLERDONE 13

extern void input_key_matrix(uint8_t out[16]);
static uint32_t KbdBase;

static uint32_t k_Open(M68K *c) { wr8(c->a[1] + IO_ERROR, 0); return 0; }
static uint32_t k_Close(M68K *c) { (void)c; return 0; }
static uint32_t k_Expunge(M68K *c) { (void)c; return 0; }

static uint32_t k_BeginIO(M68K *c) {
    uint32_t ior = c->a[1];
    wr8(ior + IO_ERROR, 0);
    wr32(ior + IO_ACTUAL, 0);
    switch (rd16(ior + IO_COMMAND)) {
    case KBD_READMATRIX: {
        uint8_t m[16];
        input_key_matrix(m);
        uint32_t n = rd32(ior + IO_LENGTH) < 16 ? rd32(ior + IO_LENGTH) : 16;
        for (uint32_t i = 0; i < n; i++) wr8(rd32(ior + IO_DATA) + i, m[i]);
        wr32(ior + IO_ACTUAL, n);
        break;
    }
    case KBD_READEVENT: case KBD_ADDRESETHANDLER: case KBD_REMRESETHANDLER: case KBD_RESETHANDLERDONE:
    case 1: case 5: case 8: break;                              /* CMD_RESET, CMD_CLEAR, CMD_FLUSH */
    default: wr8(ior + IO_ERROR, (uint8_t)-3);                   /* IOERR_NOCMD */
    }
    if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) exec_reply_msg(ior);
    return 0;
}
static uint32_t k_AbortIO(M68K *c) { (void)c; return 0; }

static const NativeEntry kbd_natives[] = {
    { "Open", k_Open }, { "Close", k_Close }, { "Expunge", k_Expunge },
    { "BeginIO", k_BeginIO }, { "AbortIO", k_AbortIO },
    { 0, 0 }
};

void keyboard_init(void) {
    KbdBase = lib_create("keyboard.device", 0, LIB_SIZE, kbd_natives, 40, 1, "keyboard 40.1 (AmigaMac)");
    wr8(KbdBase + LN_TYPE, NT_DEVICE);
    list_enqueue(SysBase + EB_DEVICELIST, KbdBase);
}

/* ---- input.device: the input handler chain ----------------------------------------
 * Every mouse, key and timer event goes through the handlers programs add
 * (IND_ADDHANDLER, an Interrupt {node, is_Data 14, is_Code 18}) before Intuition:
 * a handler gets the InputEvent list in A0 and is_Data in A1 and returns the
 * list it lets through. */
#include "sched.h"
#include "console.h"

#define IND_ADDHANDLER 9
#define IND_REMHANDLER 10
#define IND_WRITEEVENT 11
#define IE_SIZE 22
#define IECLASS_NULL 0
#define IECLASS_RAWKEY 1
#define IECLASS_RAWMOUSE 2
#define IECLASS_TIMER 6

static uint32_t InputBase;
static uint32_t handlers[32];                    /* Interrupts, highest priority first */
static int nhandlers;
static uint32_t ie_buf;
static int last_x = -1, last_y = -1;
extern int input_qualifier(void);

static uint32_t in_Open(M68K *c) { wr8(c->a[1] + IO_ERROR, 0); return 0; }

static uint32_t in_BeginIO(M68K *c) {
    uint32_t ior = c->a[1], is = rd32(ior + IO_DATA);
    wr8(ior + IO_ERROR, 0);
    switch (rd16(ior + IO_COMMAND)) {
    case IND_ADDHANDLER: {
        if (nhandlers == 32) { wr8(ior + IO_ERROR, (uint8_t)-1); break; }
        int8_t pri = (int8_t)rd8(is + LN_PRI);
        int at = 0;
        while (at < nhandlers && (int8_t)rd8(handlers[at] + LN_PRI) >= pri) at++;
        memmove(&handlers[at + 1], &handlers[at], sizeof *handlers * (size_t)(nhandlers - at));
        handlers[at] = is;
        nhandlers++;
        break;
    }
    case IND_REMHANDLER:
        for (int i = 0; i < nhandlers; i++)
            if (handlers[i] == is) { memmove(&handlers[i], &handlers[i + 1], sizeof *handlers * (size_t)(nhandlers - i - 1)); nhandlers--; break; }
        break;
    case IND_WRITEEVENT:                                        /* a synthetic key or button */
        for (uint32_t e = is; e; e = rd32(e)) {
            int cls = rd8(e + 4), code = rd16(e + 6), qual = rd16(e + 8);
            InputEv ev = { 0 };
            if (cls == IECLASS_RAWKEY) {
                char ch[4] = "";
                ev.type = IEV_KEY;
                ev.code = code;
                ev.qual = qual;
                if (!(code & 0x80) && keymap_convert(code, qual, ch, sizeof ch) == 1) ev.ch = (uint8_t)ch[0];
                amiga_input_event(&ev);
            }
        }
        break;
    default: break;                                             /* thresholds, mouse port settings */
    }
    if (!(rd8(ior + IO_FLAGS) & IOF_QUICK)) exec_reply_msg(ior);
    return 0;
}

/* PeekQualifier(): the qualifier keys and buttons held now */
static uint32_t in_PeekQualifier(M68K *c) { (void)c; return (uint32_t)input_qualifier() & 0xFFFF; }

/* On the display chips' picture the program moves its own pointer (sprite 0)
   by the mouse's counts, each in its own way. While the Mac's mouse moves, its
   moves go as counts, at the gain the program showed (picture pixels per
   count, a power of two from 1/4 to 8, kept here times 4). Once the mouse and
   the sprite have both stopped, the counts sent since the last stop and the
   distance the sprite went give the gain, and one move brings the sprite onto
   the Mac's pointer. While moving, the sprite lags behind the counts already
   sent: comparing them then would count those moves twice. */
static int gx = 8, gy = 4;                                      /* picture pixels per count, times 4 */
static int rem_x, rem_y;                                        /* what did not make a whole count */
static int acc_x, acc_y;                                        /* counts sent since the last stop */
static int stop_x = -1, stop_y, last_px = -1, last_py;          /* the sprite at the last stop, at the last tick */
static int target_x, target_y, still_ticks;                     /* the Mac's pointer; ticks since it moved */
static int chip_target_known;

static int snap_gain(int moved, int sent) {
    int g = (4 * moved + sent / 2) / sent;
    return g >= 24 ? 32 : g >= 12 ? 16 : g >= 6 ? 8 : g >= 3 ? 4 : g >= 2 ? 2 : 1;
}

/* a move of the Mac's pointer by (mx, my) picture pixels: the counts to send */
static void chip_mouse_counts(int mx, int my, int *dx, int *dy) {
    int nx = mx * 4 + rem_x, ny = my * 4 + rem_y;
    *dx = nx / gx; rem_x = nx - *dx * gx;
    *dy = ny / gy; rem_y = ny - *dy * gy;
    acc_x += *dx; acc_y += *dy;
}

/* every input.device tick (ten a second): once all is still, the gain and the correction */
static void chip_mouse_tick(void) {
    if (!chip_target_known || !chip_active()) { last_px = -1; return; }
    int px, py;
    if (!chip_pointer_now(&px, &py)) { last_px = -1; return; }
    int settled = px == last_px && py == last_py;
    last_px = px; last_py = py;
    if (++still_ticks < 2 || !settled) return;
    if (stop_x >= 0) {
        if (abs(acc_x) >= 8 && px != stop_x && (px - stop_x > 0) == (acc_x > 0)) gx = snap_gain(px - stop_x, acc_x);
        if (abs(acc_y) >= 8 && py != stop_y && (py - stop_y > 0) == (acc_y > 0)) gy = snap_gain(py - stop_y, acc_y);
    }
    stop_x = px; stop_y = py;
    acc_x = acc_y = 0;
    rem_x = rem_y = 0;
    int ex = target_x - px, ey = target_y - py;
    if (abs(ex) * 4 >= gx || abs(ey) * 4 >= gy) {               /* off by a count or more: a move to put it right */
        InputEv e = { IEV_MOUSE, AMIGA_CHIP_SCREEN, target_x, target_y, 0, 0x20000, 0 };
        amiga_input_event(&e);
    }
}

/* called by the input task for each event; 0 when a handler ate it */
extern int cx_active(void);
extern int cx_filter(M68K *c, uint32_t ie);

int input_handlers_filter(InputEv *ev) {
    if (ev->qual & 0x10000) { ev->qual &= 0xFFFF; return 1; }  /* put in by commodities: on to Intuition */
    int move_dx = 0, move_dy = 0;                               /* a move, as the mouse counts it */
    if (ev->type == IEV_TICK) chip_mouse_tick();
    if (ev->type == IEV_MOUSE) {
        if (last_x >= 0) { move_dx = ev->x - last_x; move_dy = ev->y - last_y; }
        if (ev->screen == AMIGA_CHIP_SCREEN) {
            int px, py;
            if (ev->qual & 0x20000) {                           /* the correction, once still */
                ev->qual &= 0xFFFF;
                if (chip_pointer_now(&px, &py)) { move_dx = ev->x - px; move_dy = ev->y - py; }
                else move_dx = move_dy = 0;
            } else still_ticks = 0;
            if (!chip_target_known) {                           /* the first move: from where the sprite is */
                chip_target_known = 1;
                if (chip_pointer_now(&px, &py)) { move_dx = ev->x - px; move_dy = ev->y - py; }
            }
            target_x = ev->x; target_y = ev->y;
            chip_mouse_counts(move_dx, move_dy, &move_dx, &move_dy);
        }
        last_x = ev->x; last_y = ev->y;
        chip_mouse_moved(move_dx, move_dy);
    }
    int cx = cx_active();
    if (!nhandlers && !cx) return 1;
    NTask *me = sched_current();
    M68K *c = &me->cpu;
    if (!c->a[7]) c->a[7] = rd32(me->task + TC_SPUPPER);
    if (!ie_buf) ie_buf = exec_allocmem(IE_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t e = ie_buf;
    for (int i = 0; i < IE_SIZE; i++) wr8(e + (uint32_t)i, 0);
    int cls, code = 0, qual = input_qualifier() & 0xFFFF, x = 0, y = 0;
    switch (ev->type) {
    case IEV_KEY: cls = IECLASS_RAWKEY; code = ev->code; qual = (qual & 0xFF00) | (ev->qual & 0xFF); break;
    case IEV_MOUSE:
        cls = IECLASS_RAWMOUSE; code = 0xFF; qual |= 0x8000;      /* IEQUALIFIER_RELATIVEMOUSE */
        x = move_dx; y = move_dy;
        break;
    case IEV_BUTTON:
        cls = IECLASS_RAWMOUSE; qual |= 0x8000;
        code = ev->code == 1 ? 0x68 : ev->code == -1 ? 0xE8 : ev->code == 2 ? 0x69 : 0xE9;
        break;
    case IEV_TICK: cls = IECLASS_TIMER; break;
    default: return 1;
    }
    wr8(e + 4, (uint8_t)cls);
    wr16(e + 6, (uint16_t)code);
    wr16(e + 8, (uint16_t)qual);
    wr16(e + 10, (uint16_t)x);
    wr16(e + 12, (uint16_t)y);
    uint64_t us = now_us();
    wr32(e + 14, (uint32_t)(us / 1000000));
    wr32(e + 18, (uint32_t)(us % 1000000));
    uint32_t list = e;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
    int cx_done = !cx;
    for (int i = 0; i <= nhandlers && list; i++) {
        /* the commodities network is the handler of priority 53 */
        if (!cx_done && (i == nhandlers || (int8_t)rd8(handlers[i] + LN_PRI) <= 53)) {
            cx_done = 1;
            if (!cx_filter(c, list)) list = 0;
            if (!list) break;
        }
        if (i == nhandlers) break;
        c->a[0] = list;
        c->a[1] = rd32(handlers[i] + 14);
        c->a[6] = InputBase;
        cpu_call(c, rd32(handlers[i] + 18));
        list = c->d[0];
    }
    memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
    if (list != e || rd8(e + 4) == IECLASS_NULL) return 0;      /* eaten (or replaced: ignored) */
    if (ev->type == IEV_KEY && rd16(e + 6) != (uint16_t)code) ev->code = rd16(e + 6);   /* remapped */
    return 1;
}

static const NativeEntry input_natives[] = {
    { "Open", in_Open }, { "Close", k_Close }, { "Expunge", k_Expunge },
    { "BeginIO", in_BeginIO }, { "AbortIO", k_AbortIO }, { "PeekQualifier", in_PeekQualifier },
    { 0, 0 }
};

/* input.device's own timer events: ten a second, as on the Amiga */
#include <pthread.h>
#include <unistd.h>
static void *ticker(void *arg) {
    (void)arg;
    for (;;) {
        usleep(100000);
        InputEv ev = { IEV_TICK, 0, 0, 0, 0, 0, 0 };
        amiga_input_event(&ev);
    }
    return 0;
}

void inputdevice_init(void) {
    pthread_t t;
    pthread_create(&t, 0, ticker, 0);
    pthread_detach(t);
    InputBase = lib_create("input.device", "input", LIB_SIZE, input_natives, 40, 1, "input 40.1 (AmigaMac)");
    wr8(InputBase + LN_TYPE, NT_DEVICE);
    list_enqueue(SysBase + EB_DEVICELIST, InputBase);
}
