/* graphics / intuition shared internals */
#ifndef AMIGA_GFX_H
#define AMIGA_GFX_H

#include <stdint.h>
#include "ndk_offsets.h"

enum { JAM1 = 0, JAM2 = 1, COMPLEMENT = 2, INVERSVID = 4 };

/* a BitMap as native code sees it */
typedef struct {
    uint32_t addr;
    int bpr, rows, depth;
    uint32_t planes[8];
} BM;

int bm_load(uint32_t bitmap, BM *b);
uint32_t gfx_alloc_bitmap(int width, int height, int depth, int clear);
void gfx_free_bitmap(uint32_t bitmap);

/* drawing on a BitMap (clipped to it) */
void bm_pixel(BM *b, int x, int y, int pen, int mode, int mask);
int bm_read(BM *b, int x, int y);
void bm_fill(BM *b, int x0, int y0, int x1, int y1, int pen, int mode, int mask);
void bm_line(BM *b, int x0, int y0, int x1, int y1, int pen, int mode, int mask);
void bm_copy(BM *s, int sx, int sy, BM *d, int dx, int dy, int w, int h, int minterm, int mask);

/* RastPort helpers used by intuition */
void rp_init(uint32_t rp);
void rp_text(uint32_t rp, const char *s, int n);           /* Text() at cp_x/cp_y */
void rp_move(uint32_t rp, int x, int y);
void rp_draw(uint32_t rp, int x, int y);                   /* Draw() with pattern and mode */
void rp_fill(uint32_t rp, int x0, int y0, int x1, int y1, int pen, int complement);
int rp_text_length(uint32_t rp, const char *s, int n);
uint32_t gfx_default_font(void);
uint32_t gfx_open_font(const char *name, int ysize);
uint32_t font_in_memory(const char *name, int ysize, int exact);   /* diskfont.c */

/* palettes: 32-bit 0x00RRGGBB per pen, owned by intuition's screens */
uint32_t *gfx_palette_of_vp(uint32_t viewport);   /* provided by intuition.c */
void gfx_touch(void);                                /* something was drawn */

/* utility.c: tag lists and hooks, used everywhere */
uint32_t tag_next(uint32_t *state);
uint32_t tag_find(uint32_t list, uint32_t tag);
uint32_t tag_get(uint32_t list, uint32_t tag, uint32_t def);
#ifdef AMIGA_CPU68K_H
uint32_t call_hook(M68K *c, uint32_t hook, uint32_t obj, uint32_t msg);
#endif
void utility_init(void);

void gfx_init(void);
void layers_init(void);
void intuition_init(void);
void gadtools_init(void);
extern uint32_t GfxBase, LayersBase, IntuitionBase;

#endif
