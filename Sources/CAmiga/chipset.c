/* The Amiga's display chips, for the programs that drive them themselves.
 *
 * AmigaOS here is native: Intuition and graphics.library draw without any
 * custom chip. Some programs (trackers, games, demos) bypass the OS: they
 * install their own copper list, draw with the blitter and show their pointer
 * with a sprite. For them AmigaMac reproduces, from the chips' public
 * description (Amiga Hardware Reference Manual), what those registers do:
 *
 *  - the copper (MOVE, WAIT, SKIP, COPJMP), run as the beam moves down each
 *    frame, fifty times a second;
 *  - the bitplanes (1 to 6, low or high resolution, dual playfield, extra
 *    half-brite, HAM), the display window and the data fetch, the scrolling
 *    delays, the modulos, the 32 colour registers;
 *  - the 8 sprites (DMA, attached pairs);
 *  - the blitter: area mode (4 channels, shifts, masks, minterms, descending
 *    mode, fills) and line mode, done at once when BLTSIZE is written.
 *
 * Its picture is a screen of its own on the Mac (AMIGA_CHIP_SCREEN), shown
 * from the first copper list a program starts until it gives the display back
 * (the system copper list, GfxBase->copinit, in COP1LC) or ends. Not cycle
 * exact: the blitter is instantaneous and the copper takes no time.
 */
#include "os.h"
#include "sched.h"
#include "audio.h"
#include "intui.h"
#include "ndk_offsets.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t GfxBase;

static uint16_t R[256];                     /* the custom registers as last written */
#define REG(r) R[((r) & 0x1FE) >> 1]

static uint32_t ptr_of(int reg) { return ((uint32_t)REG(reg) << 16 | REG(reg + 2)) & ~1u; }
static void set_ptr(int reg, uint32_t a) { REG(reg) = (uint16_t)(a >> 16); REG(reg + 2) = (uint16_t)(a & 0xFFFE); }

/* ---- the picture ------------------------------------------------------------------------ */

#define LINES 313                           /* PAL */
#define HW 912                              /* a line: 228 colour clocks, 4 high-resolution pixels each */
static uint32_t fb[LINES][HW];              /* the beam's whole frame, 0xFFRRGGBB */
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *out;                       /* the shown part, as the host reads it */
static int ox, oy, ow, oh;                  /* that part, in beam high-resolution pixels and lines */
static int active, shown;
static uint32_t owner;                      /* the task that started the display */
static uint32_t frames;
static uint32_t sys_coplist;                /* GfxBase->copinit: putting it back ends the display */
static int hidden[16], nhidden;             /* Intuition screens the chips' display covers */
static int seen, ux0, ux1, uy0, uy1;        /* frames since the start; where bitplanes were shown */

/* per line */
static uint8_t lidx[HW];                    /* playfield colour index (raw bits) */
static uint8_t sidx[HW];                    /* sprite colour index, 0 none */
static int line_mode;                       /* 0 normal, 1 dual playfield (already resolved), 2 HAM, 3 EHB */
static int line_fetched, x_done;
static uint32_t ham_rgb;

/* copper */
static uint32_t cop_pc;
static int cop_halt;

/* the first line shown with bitplanes: its plane 0 address, line, first pixel, resolution */
static uint32_t f_addr, l_addr;
static int f_line = -1, f_x, f_hires, l_line = -1, l_x, l_hires;

/* sprite 0, the pointer: where it was last shown, and in which frame */
static int ptr_x, ptr_y;
static uint32_t ptr_frame, ptr_used;

/* sprites */
typedef struct { int state, vstart, vstop, hstart, attach; uint16_t a, b; } Spr;
static Spr spr[8];
enum { SP_FETCH, SP_WAIT, SP_ACTIVE, SP_DONE };

static uint32_t rgb12(uint16_t c) {
    uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return 0xFF000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}
static uint32_t colour(int i) { return rgb12(REG(0x180 + 2 * (i & 31))); }

static int dmacon_on(uint16_t bits) {
    uint32_t d = paula_dmacon();
    return (d & 0x200) && (d & bits) == bits;
}

static void diw(int *hs, int *he, int *vs, int *ve) {
    uint16_t s = REG(0x8E), e = REG(0x90);
    *hs = s & 0xFF;
    *he = (e & 0xFF) | 0x100;
    *vs = s >> 8;
    *ve = e >> 8;
    if (!(*ve & 0x80)) *ve |= 0x100;
}

/* the bitplanes' data for line v: their colour indexes, by beam position */
static void fetch_line(int v) {
    line_fetched = 1;
    memset(lidx, 0, sizeof lidx);
    line_mode = 0;
    int hs, he, vs, ve;
    diw(&hs, &he, &vs, &ve);
    if (v < vs || v >= ve || !dmacon_on(0x100)) return;
    uint16_t con0 = REG(0x100);
    int hires = (con0 & 0x8000) != 0, n = (con0 >> 12) & 7;
    if (n > 6) n = 6;
    if (hires && n > 4) n = 4;
    if (!n) return;
    if (v < uy0) uy0 = v;
    if (v + 1 > uy1) uy1 = v + 1;
    if (hs * 2 < ux0) ux0 = hs * 2;
    if (he * 2 > ux1) ux1 = he * 2;
    int dpf = (con0 & 0x400) != 0, ham = (con0 & 0x800) && !hires && n >= 5;
    int ddfstrt = REG(0x92) & 0xFC, ddfstop = REG(0x94) & 0xFC;
    if (ddfstop < ddfstrt) return;
    int words = hires ? (ddfstop - ddfstrt) / 4 + 2 : (ddfstop - ddfstrt) / 8 + 1;
    if (words > 57) words = 57;
    int first = (ddfstrt * 2 + (hires ? 9 : 17)) * 2;          /* the first pixel, in beam high-res pixels */
    uint16_t con1 = REG(0x102);
    if (f_line < 0) { f_line = v; f_addr = ptr_of(0xE0); f_x = first; f_hires = hires; }
    for (int p = 0; p < n; p++) {
        uint32_t a = ptr_of(0xE0 + 4 * p);
        int delay = ((p & 1) ? (con1 >> 4) : con1) & 15;
        int step = hires ? 1 : 2;
        int x = first + delay * 2;
        for (int w = 0; w < words; w++) {
            uint16_t d = (uint16_t)rd16(a + 2 * (uint32_t)w);
            for (int b = 15; b >= 0; b--, x += step) {
                if (!((d >> b) & 1)) continue;
                for (int k = 0; k < step; k++) if (x + k >= 0 && x + k < HW) lidx[x + k] |= (uint8_t)(1 << p);
            }
        }
        int16_t mod = (int16_t)REG((p & 1) ? 0x10A : 0x108);
        set_ptr(0xE0 + 4 * p, a + (uint32_t)(words * 2) + (uint32_t)(int32_t)mod);
    }
    if (dpf) {
        int pf2pri = (REG(0x104) & 0x40) != 0;
        for (int x = 0; x < HW; x++) {
            uint8_t i = lidx[x];
            int p1 = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
            int p2 = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
            lidx[x] = (uint8_t)(pf2pri ? (p2 ? 8 + p2 : p1) : (p1 ? p1 : (p2 ? 8 + p2 : 0)));
        }
        line_mode = 1;
    } else if (ham) line_mode = 2;
    else if (n == 6) line_mode = 3;
}

/* the sprites' DMA for line v: their pixels in sidx */
static void sprite_line(int v) {
    memset(sidx, 0, sizeof sidx);
    if (v < 25 || !dmacon_on(0x20)) return;
    for (int n = 0; n < 8; n++) {
        Spr *s = &spr[n];
        uint32_t p = ptr_of(0x120 + 4 * n);
        if (s->state == SP_FETCH) {
            uint16_t pos = (uint16_t)rd16(p), ctl = (uint16_t)rd16(p + 2);
            set_ptr(0x120 + 4 * n, p + 4);
            REG(0x140 + 8 * n) = pos;
            REG(0x142 + 8 * n) = ctl;
            s->vstart = (pos >> 8) | (ctl & 4) << 6;
            s->vstop = (ctl >> 8) | (ctl & 2) << 7;
            s->hstart = (pos & 0xFF) << 1 | (ctl & 1);
            s->attach = (ctl & 0x80) != 0;
            s->state = (!pos && !ctl) ? SP_DONE : SP_WAIT;
            continue;
        }
        if (s->state == SP_WAIT && v == s->vstart) {
            s->state = SP_ACTIVE;
            if (n == 0) { ptr_x = (s->hstart + 1) * 2; ptr_y = s->vstart; ptr_frame = frames + 1; }
        }
        if (s->state != SP_ACTIVE) continue;
        if (v >= s->vstop) { s->state = SP_FETCH; continue; }
        s->a = (uint16_t)rd16(p);
        s->b = (uint16_t)rd16(p + 2);
        set_ptr(0x120 + 4 * n, p + 4);
    }
    for (int n = 7; n >= 0; n--) {                              /* sprite 0 in front */
        Spr *s = &spr[n];
        if (s->state != SP_ACTIVE || v < s->vstart) continue;
        if ((n & 1) && s->attach) continue;                     /* drawn with its even partner */
        Spr *o = (!(n & 1) && spr[n + 1].attach && spr[n + 1].state == SP_ACTIVE) ? &spr[n + 1] : 0;
        int x = (s->hstart + 1) * 2;
        for (int i = 0; i < 16; i++, x += 2) {
            int bit = 15 - i;
            int val = ((s->b >> bit) & 1) << 1 | ((s->a >> bit) & 1);
            int idx;
            if (o) {
                val |= (((o->b >> bit) & 1) << 3) | (((o->a >> bit) & 1) << 2);
                idx = val ? 16 + val : 0;
            } else idx = val ? 16 + (n / 2) * 4 + val : 0;
            if (idx && x >= 0 && x + 1 < HW) sidx[x] = sidx[x + 1] = (uint8_t)idx;
        }
    }
}

/* the line's pixels from x_done up to x (beam high-res pixels), with the colours now */
static void flush(int v, int x) {
    if (x > HW) x = HW;
    if (x <= x_done) return;
    int hs, he, vs, ve;
    diw(&hs, &he, &vs, &ve);
    int inside_v = v >= vs && v < ve;
    uint32_t *row = fb[v];
    uint32_t c0 = colour(0);
    for (int i = x_done; i < x; i++) {
        uint32_t c;
        if (sidx[i]) c = colour(sidx[i]);
        else if (!inside_v || i < hs * 2 || i >= he * 2) c = c0;
        else {
            int k = lidx[i];
            switch (line_mode) {
            case 2: {                                           /* HAM: hold and modify */
                int ctl = k >> 4, val = k & 15;
                if (!ctl) ham_rgb = colour(val);
                else if (ctl == 1) ham_rgb = (ham_rgb & 0xFFFFFF00u) | (uint32_t)(val * 17);
                else if (ctl == 2) ham_rgb = (ham_rgb & 0xFF00FFFFu) | (uint32_t)(val * 17) << 16;
                else ham_rgb = (ham_rgb & 0xFFFF00FFu) | (uint32_t)(val * 17) << 8;
                c = ham_rgb;
                break;
            }
            case 3:                                             /* extra half-brite */
                c = colour(k & 31);
                if (k & 32) c = 0xFF000000u | ((c >> 1) & 0x7F7F7F);
                break;
            default: c = colour(k);
            }
        }
        row[i] = c;
    }
    x_done = x;
}

/* ---- the copper ---------------------------------------------------------------------- */

static void custom_write_from_copper(int reg, uint16_t v);

/* the beam at (v, h) has reached the WAIT/SKIP position of w1, w2 */
static int reached(int v, int h, uint16_t w1, uint16_t w2) {
    int ve = ((w2 >> 8) & 0x7F) | 0x80, he = w2 & 0xFE;
    int beam = ((v & 0xFF) & ve) << 8 | (h & he);
    int want = ((w1 >> 8) & ve) << 8 | (w1 & 0xFE & he);
    return beam >= want;
}

static void copper_line(int v) {
    int h = 0;
    line_fetched = 0;
    x_done = 0;
    ham_rgb = colour(0);
    sprite_line(v);
    int ddf = REG(0x92) & 0xFC;
    for (int guard = 0; !cop_halt && guard < 4096; guard++) {
        if (!line_fetched && h >= ddf) fetch_line(v);
        uint16_t w1 = (uint16_t)rd16(cop_pc), w2 = (uint16_t)rd16(cop_pc + 2);
        if (!(w1 & 1)) {                                         /* MOVE */
            int reg = w1 & 0x1FE;
            if (reg < 0x40 || (reg < 0x80 && !(REG(0x2E) & 2))) { cop_halt = 1; break; }
            cop_pc += 4;
            flush(v, h * 4);
            custom_write_from_copper(reg, w2);
            continue;
        }
        if (w2 & 1) {                                            /* SKIP */
            cop_pc += reached(v, h, w1, w2) ? 8 : 4;
            continue;
        }
        int hh = h;                                              /* WAIT: later on this line? */
        while (hh <= 0xE2 && !reached(v, hh, w1, w2)) hh += 2;
        if (hh > 0xE2) break;                                    /* not on this line */
        if (!line_fetched && hh >= ddf) { fetch_line(v); }
        flush(v, hh * 4);
        h = hh;
        cop_pc += 4;
    }
    if (!line_fetched) fetch_line(v);
    flush(v, HW);
}

/* ---- the frame ------------------------------------------------------------------------ */

static void hide_screens(void) {
    nhidden = 0;
    for (NScreen *s = screens; s && nhidden < 16; s = s->next)
        if (!s->workbench) { hidden[nhidden++] = s->id; if (host.screen_closed) host.screen_closed(s->id); }
}
static void show_screens(void) {
    for (int i = 0; i < nhidden; i++)
        for (NScreen *s = screens; s; s = s->next)
            if (s->id == hidden[i] && host.screen_opened) host.screen_opened(s->id, "");
    nhidden = 0;
}

static void stop_display(void) {
    if (!active) return;
    active = 0;
    if (shown && host.screen_closed) host.screen_closed(AMIGA_CHIP_SCREEN);
    shown = 0;
    show_screens();
}

void cia_kick(void);

static void start_display(void) {
    if (active) return;
    active = 1;
    shown = 0;
    seen = 0;
    ux0 = uy0 = 1 << 20;
    ux1 = uy1 = 0;
    owner = sched_current() ? sched_current()->task : 0;
    cia_kick();                                                 /* the frames start */
}

int chip_active(void) { return active; }

/* a task ended: if it had the display, the system gets it back */
void chip_task_gone(uint32_t task) { if (active && task == owner) stop_display(); }

void chip_frame(void) {
    if (!active) return;
    cop_pc = ptr_of(0x80);                                     /* at the vertical blank: from COP1LC */
    cop_halt = !dmacon_on(0x80);
    for (int n = 0; n < 8; n++) spr[n].state = SP_FETCH;
    f_line = -1;
    for (int v = 0; v < LINES; v++) {
        if (cop_halt) {
            line_fetched = 0; x_done = 0; ham_rgb = colour(0);
            sprite_line(v);
            fetch_line(v);
            flush(v, HW);
        } else copper_line(v);
    }
    l_line = f_line; l_addr = f_addr; l_x = f_x; l_hires = f_hires;
    if (!shown && ++seen < 10) return;                         /* a few frames to settle */
    if (!shown) {                                               /* the window: where bitplanes were shown */
        ox = ux0; ow = ux1 - ux0; oy = uy0; oh = uy1 - uy0;
        if (ow < 64 || ox < 0 || ox + ow > HW || oh < 32 || oy + oh > LINES) { ox = 0x81 * 2; ow = 640; oy = 0x2C; oh = 256; }
        pthread_mutex_lock(&out_lock);
        free(out);
        out = calloc((size_t)ow * (size_t)oh, 4);
        pthread_mutex_unlock(&out_lock);
    }
    pthread_mutex_lock(&out_lock);
    for (int y = 0; y < oh; y++) memcpy(out + (size_t)y * (size_t)ow, &fb[oy + y][ox], (size_t)ow * 4);
    frames++;
    pthread_mutex_unlock(&out_lock);
    if (!shown) {
        shown = 1;
        hide_screens();
        if (host.screen_opened) {
            char name[64] = "Amiga";
            if (owner) str_get(rd32(owner + LN_NAME), name, sizeof name);
            host.screen_opened(AMIGA_CHIP_SCREEN, name);
        }
    }
}

uint32_t chip_frames(void) { return frames; }

/* a point of the chips' picture on the Intuition screen whose bitmap it shows, if one does */
int chip_screen_point(int cx, int cy, int *id, int *sx, int *sy) {
    if (l_line < 0) return 0;
    for (NScreen *s = screens; s; s = s->next) {
        uint32_t bm = s->custom_bm ? s->custom_bm : s->scr + Screen_BitMap;
        uint32_t p0 = rd32(bm + BitMap_Planes), bpr = rd16(bm + BitMap_BytesPerRow), rows = rd16(bm + BitMap_Rows);
        if (!p0 || !bpr || l_addr < p0 || l_addr >= p0 + bpr * rows) continue;
        int row0 = (int)((l_addr - p0) / bpr), col0 = (int)((l_addr - p0) % bpr) * 8;
        int k = l_hires ? 1 : 2;                                /* picture pixels per bitmap pixel */
        *id = s->id;
        *sx = col0 + (cx + ox - l_x) / k;
        *sy = row0 + (cy + oy - l_line);
        return 1;
    }
    return 0;
}

/* the program's pointer (sprite 0) on the chips' picture, as last shown */
int chip_pointer_now(int *x, int *y) {
    if (!shown || !ptr_frame || ptr_frame + 2 < frames) return 0;
    *x = ptr_x - ox;
    *y = ptr_y - oy;
    return 1;
}

/* the program's pointer (sprite 0) on the chips' picture, once per new frame */
int chip_pointer(int *x, int *y) {
    if (!shown || !ptr_frame || ptr_frame == ptr_used || ptr_frame + 2 < frames) return 0;
    ptr_used = ptr_frame;
    *x = ptr_x - ox;
    *y = ptr_y - oy;
    return 1;
}

int chip_display_size(int *w, int *h) {
    if (!shown) return 0;
    *w = ow; *h = oh;
    return 1;
}

int chip_display_render(uint32_t *dst, int w, int h) {
    pthread_mutex_lock(&out_lock);
    if (!out) { pthread_mutex_unlock(&out_lock); return 0; }
    for (int y = 0; y < h && y < oh; y++) memcpy(dst + (size_t)y * (size_t)w, out + (size_t)y * (size_t)ow, (size_t)(w < ow ? w : ow) * 4);
    pthread_mutex_unlock(&out_lock);
    return 1;
}

/* ---- the blitter ---------------------------------------------------------------------- */

static int bzero_flag = 1;

static uint16_t minterm(uint8_t mt, uint16_t a, uint16_t b, uint16_t c) {
    uint16_t d = 0;
    for (int i = 0; i < 8; i++)
        if (mt & (1 << i)) d |= (uint16_t)(((i & 4) ? a : ~a) & ((i & 2) ? b : ~b) & ((i & 1) ? c : ~c));
    return d;
}

static void blit_area(int w, int h) {
    uint16_t con0 = REG(0x40), con1 = REG(0x42);
    int ash = con0 >> 12, bsh = con1 >> 12;
    int usea = (con0 >> 11) & 1, useb = (con0 >> 10) & 1, usec = (con0 >> 9) & 1, used = (con0 >> 8) & 1;
    int desc = (con1 & 2) != 0, ife = (con1 & 8) != 0, efe = (con1 & 16) != 0, fci = (con1 & 4) != 0;
    uint8_t mt = (uint8_t)con0;
    uint32_t apt = ptr_of(0x50), bpt = ptr_of(0x4C), cpt = ptr_of(0x48), dpt = ptr_of(0x54);
    int32_t amod = (int16_t)(REG(0x64) & 0xFFFE), bmod = (int16_t)(REG(0x62) & 0xFFFE);
    int32_t cmod = (int16_t)(REG(0x60) & 0xFFFE), dmod = (int16_t)(REG(0x66) & 0xFFFE);
    int step = desc ? -2 : 2;
    uint16_t afwm = REG(0x44), alwm = REG(0x46);
    uint16_t aprev = 0, bprev = 0;
    int zero = 1;
    for (int y = 0; y < h; y++) {
        int fc = fci;
        for (int x = 0; x < w; x++) {
            if (usea) { REG(0x74) = (uint16_t)rd16(apt); apt += (uint32_t)step; }
            if (useb) { REG(0x72) = (uint16_t)rd16(bpt); bpt += (uint32_t)step; }
            if (usec) { REG(0x70) = (uint16_t)rd16(cpt); cpt += (uint32_t)step; }
            uint16_t a = REG(0x74), b = REG(0x72), c = REG(0x70);
            if (x == 0) a &= afwm;
            if (x == w - 1) a &= alwm;
            uint16_t as, bs;
            if (!desc) {
                as = (uint16_t)((((uint32_t)aprev << 16) | a) >> ash);
                bs = (uint16_t)((((uint32_t)bprev << 16) | b) >> bsh);
            } else {
                as = (uint16_t)((((uint32_t)a << 16) | aprev) >> (16 - ash));
                bs = (uint16_t)((((uint32_t)b << 16) | bprev) >> (16 - bsh));
            }
            aprev = a;
            bprev = b;
            uint16_t d = minterm(mt, as, bs, c);
            if (desc && (ife || efe)) {                         /* area fill, right to left */
                uint16_t r = 0;
                for (int i = 0; i < 16; i++) {
                    int bit = (d >> i) & 1;
                    if (ife) { if (fc || bit) r |= (uint16_t)(1 << i); if (bit) fc ^= 1; }
                    else { if (bit) fc ^= 1; if (fc) r |= (uint16_t)(1 << i); }
                }
                d = r;
            }
            if (d) zero = 0;
            if (used) { wr16(dpt, d); dpt += (uint32_t)step; }
        }
        if (usea) apt += (uint32_t)(desc ? -amod : amod);
        if (useb) bpt += (uint32_t)(desc ? -bmod : bmod);
        if (usec) cpt += (uint32_t)(desc ? -cmod : cmod);
        if (used) dpt += (uint32_t)(desc ? -dmod : dmod);
    }
    set_ptr(0x50, apt); set_ptr(0x4C, bpt); set_ptr(0x48, cpt); set_ptr(0x54, dpt);
    bzero_flag = zero;
}

static void blit_line(int len) {
    uint16_t con0 = REG(0x40), con1 = REG(0x42);
    int shift = con0 >> 12;
    uint8_t mt = (uint8_t)con0;
    int sud = (con1 >> 4) & 1, sul = (con1 >> 3) & 1, aul = (con1 >> 2) & 1, sing = (con1 >> 1) & 1;
    int32_t acc = (int16_t)REG(0x52);                            /* BLTAPTL: the error term */
    int32_t amod = (int16_t)REG(0x64), bmod = (int16_t)REG(0x62), cmod = (int16_t)REG(0x60);
    int sign = (con1 & 0x40) != 0;
    uint32_t cpt = ptr_of(0x48), dpt = ptr_of(0x54);
    uint16_t adat = REG(0x74), bdat = REG(0x72);
    int bsh = con1 >> 12;
    uint16_t tex = (uint16_t)((bdat >> bsh) | (bdat << (16 - bsh)));
    int onedot = 0, zero = 1;
    for (int i = 0; i < len; i++) {
        uint16_t a = (uint16_t)(adat >> shift);
        uint16_t b = (tex & 1) ? 0xFFFF : 0;
        uint16_t c = (uint16_t)rd16(cpt);
        uint16_t d = minterm(mt, a, b, c);
        if (sing && onedot) d = c;
        if (d) zero = 0;
        wr16(i == 0 ? dpt : cpt, d);
        if (sing) onedot = 1;
        tex = (uint16_t)((tex >> 1) | (tex << 15));
        /* the next point */
#define INCX() do { if (++shift == 16) { shift = 0; cpt += 2; } } while (0)
#define DECX() do { if (--shift < 0) { shift = 15; cpt -= 2; } } while (0)
#define INCY() do { cpt += (uint32_t)cmod; onedot = 0; } while (0)
#define DECY() do { cpt -= (uint32_t)cmod; onedot = 0; } while (0)
        if (!sign) {
            acc += amod;
            if (sud) { if (sul) DECX(); else INCX(); }
            else { if (sul) DECY(); else INCY(); }
        } else acc += bmod;
        if (sud) { if (aul) DECY(); else INCY(); }
        else { if (aul) DECX(); else INCX(); }
        sign = (int16_t)acc < 0;
    }
    REG(0x52) = (uint16_t)acc;
    REG(0x40) = (uint16_t)((con0 & 0x0FFF) | shift << 12);
    set_ptr(0x48, cpt);
    set_ptr(0x54, cpt);
    bzero_flag = zero;
}

static void blit(int w, int h) {
    if (REG(0x42) & 1) blit_line(h);
    else blit_area(w, h);
}

uint32_t chip_dmaconr_bits(void) { return bzero_flag ? 0x2000 : 0; }

/* ---- the registers -------------------------------------------------------------------- */

static int is_system_list(uint32_t a) {
    if (!a) return 1;
    if (a == sys_coplist) return 1;
    return GfxBase && (a == rd32(GfxBase + 38) || a == rd32(GfxBase + 50));
}

static void write_reg(int reg, uint16_t v, int from_copper) {
    reg &= 0x1FE;
    if (reg == 0x96 || reg == 0x9A || reg == 0x9C || (reg >= 0xA0 && reg < 0xE0)) { paula_write(reg, v); return; }
    switch (reg) {
    case 0x58:                                                   /* BLTSIZE: the blit, at once */
        REG(reg) = v;
        blit((v & 63) ? (v & 63) : 64, (v >> 6) ? (v >> 6) : 1024);
        return;
    case 0x5A: REG(0x40) = (uint16_t)((REG(0x40) & 0xFF00) | (v & 0xFF)); return;   /* BLTCON0L */
    case 0x5E:                                                   /* BLTSIZH, after BLTSIZV */
        REG(reg) = v;
        blit((v & 0x7FF) ? (v & 0x7FF) : 2048, (REG(0x5C) & 0x7FFF) ? (REG(0x5C) & 0x7FFF) : 32768);
        return;
    case 0x88: case 0x8A:                                        /* COPJMP1, COPJMP2 */
        if (from_copper) { cop_pc = ptr_of(reg == 0x88 ? 0x80 : 0x84); return; }
        if (reg == 0x88) { if (is_system_list(ptr_of(0x80))) stop_display(); else start_display(); }
        return;
    }
    REG(reg) = v;
    if (!from_copper && reg == 0x82) {                          /* COP1LC complete: whose list is it? */
        if (is_system_list(ptr_of(0x80))) stop_display(); else start_display();
    }
}

static void custom_write_from_copper(int reg, uint16_t v) { write_reg(reg, v, 1); }

/* a write by the processor to $DFF000-$DFF1FF */
void chip_write(uint32_t addr, int size, uint32_t v) {
    int reg = (int)(addr & 0x1FE);
    if (size == 4) {
        write_reg(reg, (uint16_t)(v >> 16), 0);
        write_reg(reg + 2, (uint16_t)v, 0);
        return;
    }
    if (size == 1) v = (v & 0xFF) * 0x101;
    write_reg(reg, (uint16_t)v, 0);
}

/* the system's own copper list: a program that puts it back gives the display back */
void chip_init(void) {
    sys_coplist = exec_allocmem(8, MEMF_PUBLIC | MEMF_CLEAR);
    wr16(sys_coplist, 0xFFFF);
    wr16(sys_coplist + 2, 0xFFFE);
    wr32(GfxBase + 38, sys_coplist);                            /* gb_copinit */
    wr32(GfxBase + 50, sys_coplist);                            /* gb_LOFlist */
    REG(0x8E) = 0x2C81;                                          /* DIWSTRT, DIWSTOP, DDFSTRT, DDFSTOP: */
    REG(0x90) = 0x2CC1;                                          /* a 320 x 256 PAL display */
    REG(0x92) = 0x38;
    REG(0x94) = 0xD0;
    REG(0x180) = 0x0AAA;
}
