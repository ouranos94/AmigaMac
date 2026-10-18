/* BOOPSI: intuition's object system, native.
 *
 * Classes are real IClass structures and objects real _Object-prefixed blocks,
 * so 68k code (amiga.lib's DoMethodA/DoSuperMethodA, third-party classes,
 * disk-based gadget libraries) sees exactly what it expects. Built-in classes
 * have native dispatchers reached through trap stubs placed in h_Entry.
 */
#include "os.h"
#include "intui.h"
#include "sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OM_NEW 0x101
#define OM_DISPOSE 0x102
#define OM_SET 0x103
#define OM_GET 0x104
#define OM_ADDTAIL 0x105
#define OM_REMOVE 0x106
#define OM_NOTIFY 0x107
#define OM_UPDATE 0x108
#define OM_ADDMEMBER 0x109
#define OM_REMMEMBER 0x10A
#define GM_HITTEST 0
#define GM_RENDER 1
#define GM_GOACTIVE 2
#define GM_HANDLEINPUT 3
#define GM_GOINACTIVE 4
#define GM_LAYOUT 6
#define IM_DRAW 0x202
#define IM_HITTEST 0x203
#define IM_ERASE 0x204
#define IM_DRAWFRAME 0x206
#define IM_FRAMEBOX 0x207
#define GMR_GADGETHIT 4
#define GMR_MEACTIVE 0
#define GMR_NOREUSE 2
#define GMR_VERIFY 8
#define GREDRAW_REDRAW 1
#define GREDRAW_UPDATE 2
#define IDS_NORMAL 0
#define IDS_SELECTED 1
#define IDS_DISABLED 2

#define TAG_USER 0x80000000u
#define IA(n) (TAG_USER + 0x20000 + (n))
#define GA(n) (TAG_USER + 0x30000 + (n))
#define PGA(n) (TAG_USER + 0x31000 + (n))
#define STRINGA(n) (TAG_USER + 0x32000 + (n))
#define ICA_TARGET (TAG_USER + 0x40001)
#define ICA_MAP (TAG_USER + 0x40002)
#define ICTARGET_IDCMP 0xFFFFFFFFu
#define IDCMP_IDCMPUPDATE 0x00800000
#define GTYP_CUSTOMGADGET 5
#define OPUF_INTERIM 1
#define PI_AUTOKNOB_ 0x1
#define PI_FREEHORIZ_ 0x2
#define PI_FREEVERT_ 0x4

/* ---- class registry ---------------------------------------------------------- */

typedef struct NClass { uint32_t cl; Dispatcher fn; char name[32]; struct NClass *next; } NClass;
static NClass *classes;                     /* every IClass we know (built-in or made) */
static uint32_t public_classes;              /* exec list of public classes (cl_Reserved used as node) */

static NClass *nclass_of(uint32_t cl) {
    for (NClass *n = classes; n; n = n->next) if (n->cl == cl) return n;
    return 0;
}

static uint32_t find_public(const char *name) {
    for (NClass *n = classes; n; n = n->next) if (n->name[0] && !strcmp(n->name, name)) return n->cl;
    return 0;
}

uint32_t bo_class_of(uint32_t obj) { return obj ? rd32(obj - 4) : 0; }

int bo_is_object(uint32_t p) {
    if (!p || p < 16 || p >= mem_size) return 0;
    return nclass_of(rd32(p - 4)) != 0;
}

/* call a class dispatcher (native directly, 68k through its hook) */
uint32_t bo_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    if (!cl) return 0;
    NClass *n = nclass_of(cl);
    if (n && n->fn) return n->fn(c, cl, obj, msg);
    return call_hook(c, cl + IClass_cl_Dispatcher, obj, msg);
}

static uint32_t super_method(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    return bo_dispatch(c, rd32(cl + IClass_cl_Super), obj, msg);
}

/* messages are built in 68k memory */
static uint32_t msg_new(int nwords, const uint32_t *w) {
    uint32_t m = exec_allocmem((uint32_t)(4 * nwords + 32), MEMF_PUBLIC | MEMF_CLEAR);
    for (int i = 0; i < nwords; i++) wr32(m + 4 * (uint32_t)i, w[i]);
    return m;
}
static void msg_free(uint32_t m, int nwords) { exec_freemem(m, (uint32_t)(4 * nwords + 32)); }

uint32_t bo_domethod(M68K *c, uint32_t obj, int n, const uint32_t *w) {
    uint32_t m = msg_new(n, w);
    uint32_t r = bo_dispatch(c, bo_class_of(obj), obj, m);
    msg_free(m, n);
    return r;
}

/* ---- instance data access ---------------------------------------------------- */

static uint32_t inst(uint32_t cl, uint32_t obj) { return obj + rd16(cl + IClass_cl_InstOffset); }

/* ---- rootclass ----------------------------------------------------------------- */

static uint32_t obj_size(uint32_t cl) { return _Object_SIZEOF + rd16(cl + IClass_cl_InstOffset) + rd16(cl + IClass_cl_InstSize); }

static uint32_t root_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    (void)c;
    switch (rd32(msg)) {
    case OM_NEW: {
        uint32_t trueclass = obj, size = obj_size(trueclass);
        uint32_t blk = exec_allocmem(size, MEMF_PUBLIC | MEMF_CLEAR);
        if (!blk) return 0;
        wr32(blk + _Object_o_Class, trueclass);
        wr32(trueclass + IClass_cl_ObjectCount, rd32(trueclass + IClass_cl_ObjectCount) + 1);
        return blk + _Object_SIZEOF;
    }
    case OM_DISPOSE: {
        uint32_t trueclass = bo_class_of(obj);
        wr32(trueclass + IClass_cl_ObjectCount, rd32(trueclass + IClass_cl_ObjectCount) - 1);
        exec_freemem(obj - _Object_SIZEOF, obj_size(trueclass));
        return 0;
    }
    case OM_ADDTAIL: list_addtail(rd32(msg + 4), obj - _Object_SIZEOF); return 0;
    case OM_REMOVE: list_remove(obj - _Object_SIZEOF); return 0;
    }
    (void)cl;
    return 0;
}

/* pointerclass: the object only stands for a pointer image (the Mac's pointer is used) */
static uint32_t root_dispatch_ptr(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    if (rd32(msg) == OM_GET) { wr32(rd32(msg + 8), 0); return 1; }
    return super_method(c, cl, obj, msg);
}

/* ---- notification (icclass behaviour, shared by gadgets) ------------------------ */

static uint32_t clone_tags(uint32_t tags) {
    uint32_t st = tags, ti, n = 0;
    while (tag_next(&st)) n++;
    uint32_t a = exec_allocvec(8 * (n + 1), MEMF_PUBLIC | MEMF_CLEAR), i = 0;
    st = tags;
    while ((ti = tag_next(&st))) { wr32(a + 8 * i, rd32(ti)); wr32(a + 8 * i + 4, rd32(ti + 4)); i++; }
    return a;
}

static void map_tags(uint32_t tags, uint32_t map) {
    if (!map) return;
    uint32_t st = tags, ti;
    while ((ti = tag_next(&st)))
        for (uint32_t m = map; rd32(m); m += 8)
            if (rd32(m) == rd32(ti)) { wr32(ti, rd32(m + 4)); break; }
}

static int notify_depth;

static void notify(M68K *c, uint32_t target, uint32_t map, uint32_t tags, uint32_t ginfo, uint32_t flags) {
    if (!target || notify_depth > 8) return;
    uint32_t t = clone_tags(tags);
    map_tags(t, map);
    if (target == ICTARGET_IDCMP) {
        uint32_t win = ginfo ? rd32(ginfo + GadgetInfo_gi_Window) : 0;
        NWindow *w = win ? nwin_of(win) : 0;
        if (w) send_idcmp(w, IDCMP_IDCMPUPDATE, 0, 0, t, 0, 0);   /* the tag list leaks with the message */
        return;
    }
    notify_depth++;
    uint32_t w4[4] = { OM_UPDATE, t, ginfo, flags };
    bo_domethod(c, target, 4, w4);
    notify_depth--;
    exec_freevec(t);
}

/* icclass instance: target, map */
static uint32_t ic_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: case OM_SET: case OM_UPDATE: {
        uint32_t o = id == OM_NEW ? super_method(c, cl, obj, msg) : obj;
        if (!o) return 0;
        uint32_t d = inst(cl, o), tags = rd32(msg + 4);
        uint32_t t = tag_find(tags, ICA_TARGET), m = tag_find(tags, ICA_MAP);
        if (t) wr32(d, rd32(t + 4));
        if (m) wr32(d + 4, rd32(m + 4));
        if (id == OM_UPDATE) notify(c, rd32(d), rd32(d + 4), tags, rd32(msg + 8), rd32(msg + 12));
        return id == OM_NEW ? o : 1;
    }
    case OM_NOTIFY: {
        uint32_t d = inst(cl, obj);
        notify(c, rd32(d), rd32(d + 4), rd32(msg + 4), rd32(msg + 8), rd32(msg + 12));
        return 0;
    }
    case OM_GET:
        if (rd32(msg + 4) == ICA_TARGET) { wr32(rd32(msg + 8), rd32(inst(cl, obj))); return 1; }
        return 0;
    }
    return super_method(c, cl, obj, msg);
}

/* modelclass: icclass + a member list notified on every update */
static uint32_t model_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: {
        uint32_t o = super_method(c, cl, obj, msg);
        if (o) list_new(inst(cl, o));
        return o;
    }
    case OM_ADDMEMBER: {
        uint32_t w2[2] = { OM_ADDTAIL, inst(cl, obj) };
        bo_domethod(c, rd32(msg + 4), 2, w2);
        return 1;
    }
    case OM_REMMEMBER: {
        uint32_t w1[1] = { OM_REMOVE };
        bo_domethod(c, rd32(msg + 4), 1, w1);
        return 1;
    }
    case OM_DISPOSE: {
        uint32_t n;
        while ((n = list_remhead(inst(cl, obj)))) {
            uint32_t w1[1] = { OM_DISPOSE };
            bo_domethod(c, n + _Object_SIZEOF, 1, w1);
        }
        return super_method(c, cl, obj, msg);
    }
    case OM_UPDATE: case OM_SET:
        if (notify_depth < 8) {
            notify_depth++;
            for (uint32_t n = rd32(inst(cl, obj)); rd32(n + LN_SUCC); n = rd32(n + LN_SUCC)) {
                uint32_t w4[4] = { OM_UPDATE, rd32(msg + 4), rd32(msg + 8), id == OM_UPDATE ? rd32(msg + 12) : 0 };
                bo_domethod(c, n + _Object_SIZEOF, 4, w4);
            }
            notify_depth--;
        }
        return super_method(c, cl, obj, msg);
    }
    return super_method(c, cl, obj, msg);
}

/* ---- images ------------------------------------------------------------------- */

/* the instance is a struct Image whose Depth is CUSTOMIMAGEDEPTH, so DrawImage()
   knows to send IM_DRAW */
static void image_set(uint32_t img, uint32_t tags) {
    uint32_t st = tags, ti;
    while ((ti = tag_next(&st))) {
        uint32_t d = rd32(ti + 4);
        switch (rd32(ti)) {
        case IA(1): wr16(img + Image_LeftEdge, d); break;
        case IA(2): wr16(img + Image_LeftEdge + 2, d); break;
        case IA(3): wr16(img + Image_Width, d); break;
        case IA(4): wr16(img + Image_Width + 2, d); break;
        case IA(5): wr8(img + Image_PlanePick, d); break;        /* FGPen (IM_FGPEN) */
        case IA(6): wr8(img + Image_PlaneOnOff, d); break;       /* BGPen */
        case IA(7): wr32(img + Image_ImageData, d); break;       /* Data */
        }
    }
}

static uint32_t image_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    switch (rd32(msg)) {
    case OM_NEW: {
        uint32_t o = super_method(c, cl, obj, msg);
        if (!o) return 0;
        wr16(o + Image_Depth, 0xFFFF);                          /* CUSTOMIMAGEDEPTH */
        wr8(o + Image_PlanePick, 1);
        image_set(o, rd32(msg + 4));
        return o;
    }
    case OM_SET: case OM_UPDATE: image_set(obj, rd32(msg + 4)); return 1;
    case OM_GET: {
        uint32_t id = rd32(msg + 4), st = rd32(msg + 8), v;
        switch (id) {
        case IA(1): v = (uint32_t)(int16_t)rd16(obj + Image_LeftEdge); break;
        case IA(2): v = (uint32_t)(int16_t)rd16(obj + Image_LeftEdge + 2); break;
        case IA(3): v = rd16(obj + Image_Width); break;
        case IA(4): v = rd16(obj + Image_Width + 2); break;
        case IA(7): v = rd32(obj + Image_ImageData); break;
        default: return super_method(c, cl, obj, msg);
        }
        wr32(st, v);
        return 1;
    }
    case IM_HITTEST: {
        int px = (int16_t)rd16(msg + 4), py = (int16_t)rd16(msg + 6);
        int x = (int16_t)rd16(obj + Image_LeftEdge), y = (int16_t)rd16(obj + Image_LeftEdge + 2);
        return px >= x && py >= y && px < x + (int16_t)rd16(obj + Image_Width) && py < y + (int16_t)rd16(obj + Image_Width + 2);
    }
    case IM_ERASE: {
        uint32_t rp = rd32(msg + 4);
        int x = (int16_t)rd16(msg + 8) + (int16_t)rd16(obj + Image_LeftEdge), y = (int16_t)rd16(msg + 10) + (int16_t)rd16(obj + Image_LeftEdge + 2);
        rp_fill(rp, x, y, x + rd16(obj + Image_Width) - 1, y + rd16(obj + Image_Width + 2) - 1, 0, 0);
        return 0;
    }
    case IM_DRAW: case IM_DRAWFRAME: return 0;
    }
    return super_method(c, cl, obj, msg);
}

/* drawing parameters common to IM_DRAW (impDraw) and IM_DRAWFRAME */
typedef struct { uint32_t rp; int x, y, w, h, state; uint32_t dri; } DrawArgs;

static void draw_args(uint32_t obj, uint32_t msg, DrawArgs *a) {
    a->rp = rd32(msg + impDraw_imp_RPort);
    a->x = (int16_t)rd16(msg + impDraw_imp_Offset) + (int16_t)rd16(obj + Image_LeftEdge);
    a->y = (int16_t)rd16(msg + impDraw_imp_Offset + 2) + (int16_t)rd16(obj + Image_LeftEdge + 2);
    a->state = (int)rd32(msg + impDraw_imp_State);
    a->dri = rd32(msg + impDraw_imp_DrInfo);
    a->w = rd16(obj + Image_Width);
    a->h = rd16(obj + Image_Width + 2);
    if (rd32(msg) == IM_DRAWFRAME) {
        a->w = (int16_t)rd16(msg + impDraw_imp_Dimensions);
        a->h = (int16_t)rd16(msg + impDraw_imp_Dimensions + 2);
    }
}

static int pen_of(uint32_t dri, int which, int def) {
    if (!dri) return def;
    uint32_t pens = rd32(dri + DrawInfo_dri_Pens);
    return pens ? (int)rd16(pens + 2 * (uint32_t)which) : def;
}

static void bevel_rp(uint32_t rp, int x0, int y0, int x1, int y1, int shine, int shadow, int recessed) {
    int t = recessed ? shadow : shine, d = recessed ? shine : shadow;
    rp_fill(rp, x0, y0, x1, y0, t, 0);
    rp_fill(rp, x0, y0, x0, y1, t, 0);
    rp_fill(rp, x0 + 1, y1, x1, y1, d, 0);
    rp_fill(rp, x1, y0 + 1, x1, y1, d, 0);
}

/* frameiclass instance (after the Image): recessed, edgesonly, frametype */
static uint32_t frame_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: case OM_SET: case OM_UPDATE: {
        uint32_t o = id == OM_NEW ? super_method(c, cl, obj, msg) : (super_method(c, cl, obj, msg), obj);
        if (!o) return 0;
        uint32_t d = inst(cl, o), tags = rd32(msg + 4), ti;
        if ((ti = tag_find(tags, IA(0x15)))) wr8(d, rd32(ti + 4) ? 1 : 0);     /* IA_Recessed */
        if ((ti = tag_find(tags, IA(0x17)))) wr8(d + 1, rd32(ti + 4) ? 1 : 0); /* IA_EdgesOnly */
        if ((ti = tag_find(tags, IA(0x1B)))) wr8(d + 2, rd32(ti + 4));          /* IA_FrameType */
        return id == OM_NEW ? o : 1;
    }
    case IM_DRAW: case IM_DRAWFRAME: {
        DrawArgs a;
        draw_args(obj, msg, &a);
        uint32_t d = inst(cl, obj);
        int rec = rd8(d) ^ (a.state == IDS_SELECTED);
        int shine = pen_of(a.dri, SHINEPEN, 2), shadow = pen_of(a.dri, SHADOWPEN, 1);
        if (a.w <= 0 || a.h <= 0) return 0;
        if (!rd8(d + 1)) rp_fill(a.rp, a.x + 1, a.y + 1, a.x + a.w - 2, a.y + a.h - 2,
                                 a.state == IDS_SELECTED ? pen_of(a.dri, FILLPEN, 3) : pen_of(a.dri, BACKGROUNDPEN, 0), 0);
        bevel_rp(a.rp, a.x, a.y, a.x + a.w - 1, a.y + a.h - 1, shine, shadow, rec);
        if (rd8(d + 2) == 2) bevel_rp(a.rp, a.x + 1, a.y + 1, a.x + a.w - 2, a.y + a.h - 2, shine, shadow, !rec);   /* ridge */
        return 0;
    }
    case IM_FRAMEBOX: {
        /* contents box -> frame box: 2 pixels around, more sideways (hires) */
        uint32_t cb = rd32(msg + impFrameBox_imp_ContentsBox), fb = rd32(msg + impFrameBox_imp_FrameBox);
        wr16(fb, rd16(cb) - 6);
        wr16(fb + 2, rd16(cb + 2) - 3);
        wr16(fb + 4, rd16(cb + 4) + 12);
        wr16(fb + 6, rd16(cb + 6) + 6);
        return 1;
    }
    }
    return super_method(c, cl, obj, msg);
}

/* sysiclass instance: which, size */
#define CHECKIMAGE 0x0E
#define MXIMAGE 0x0F
static uint32_t sysi_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: {
        uint32_t o = super_method(c, cl, obj, msg);
        if (!o) return 0;
        uint32_t tags = rd32(msg + 4), d = inst(cl, o);
        int which = (int)tag_get(tags, IA(0x0D), 0);
        wr8(d, (uint32_t)which);
        wr32(d + 4, tag_get(tags, IA(0x18), 0));               /* SYSIA_DrawInfo */
        static const int sizes[][2] = { [0] = { 23, 11 }, [1] = { 23, 11 }, [2] = { 18, 10 }, [3] = { 20, 11 },
                                        [5] = { 23, 11 }, [10] = { 16, 10 }, [11] = { 18, 9 }, [12] = { 16, 10 },
                                        [13] = { 18, 9 }, [14] = { 26, 11 }, [15] = { 17, 9 }, [16] = { 15, 8 },
                                        [17] = { 23, 8 } };
        if (!rd16(o + Image_Width) && which < 18) {
            wr16(o + Image_Width, (uint32_t)sizes[which][0]);
            wr16(o + Image_Width + 2, (uint32_t)sizes[which][1]);
        }
        return o;
    }
    case IM_DRAW: case IM_DRAWFRAME: {
        DrawArgs a;
        draw_args(obj, msg, &a);
        uint32_t d = inst(cl, obj);
        if (!a.dri) a.dri = rd32(d + 4);
        int which = rd8(d), sel = a.state == IDS_SELECTED;
        int shine = pen_of(a.dri, SHINEPEN, 2), shadow = pen_of(a.dri, SHADOWPEN, 1), text = pen_of(a.dri, TEXTPEN, 1);
        int x = a.x, y = a.y, w = a.w, h = a.h;
        switch (which) {
        case CHECKIMAGE:
            bevel_rp(a.rp, x, y, x + w - 1, y + h - 1, shine, shadow, 0);
            rp_fill(a.rp, x + 1, y + 1, x + w - 2, y + h - 2, pen_of(a.dri, BACKGROUNDPEN, 0), 0);
            if (sel)                                             /* a tick */
                for (int i = 0; i < 6; i++) {
                    rp_fill(a.rp, x + w / 2 - 6 + i * 2, y + 3 + (i < 2 ? i : 0) + (i >= 2 ? 3 - (i - 2) : 0),
                            x + w / 2 - 5 + i * 2, y + 4 + (i < 2 ? i : 0) + (i >= 2 ? 3 - (i - 2) : 0) + 1, text, 0);
                }
            break;
        case MXIMAGE:
            rp_fill(a.rp, x + 2, y, x + w - 3, y, shadow, 0);
            rp_fill(a.rp, x, y + 2, x, y + h - 3, shadow, 0);
            rp_fill(a.rp, x + 2, y + h - 1, x + w - 3, y + h - 1, shine, 0);
            rp_fill(a.rp, x + w - 1, y + 2, x + w - 1, y + h - 3, shine, 0);
            if (sel) rp_fill(a.rp, x + 4, y + 3, x + w - 5, y + h - 4, pen_of(a.dri, FILLPEN, 3), 0);
            break;
        case 0x0A: case 0x0B: case 0x0C: case 0x0D: {            /* arrows */
            bevel_rp(a.rp, x, y, x + w - 1, y + h - 1, shine, shadow, sel);
            int cx = x + w / 2, cy = y + h / 2;
            for (int i = 0; i < 4; i++) {
                if (which == 0x0A) rp_fill(a.rp, cx - 2 + i, cy - i, cx - 1 + i, cy + i, text, 0);
                if (which == 0x0C) rp_fill(a.rp, cx + 1 - i, cy - i, cx + 2 - i, cy + i, text, 0);
                if (which == 0x0B) rp_fill(a.rp, cx - i * 2, cy - 2 + i, cx + i * 2, cy - 2 + i, text, 0);
                if (which == 0x0D) rp_fill(a.rp, cx - i * 2, cy + 2 - i, cx + i * 2, cy + 2 - i, text, 0);
            }
            break;
        }
        case 0x10:                                               /* MENUCHECK */
            for (int i = 0; i < 4; i++) rp_fill(a.rp, x + 2 + i * 2, y + h / 2 - 1 + (i < 2 ? i : 3 - i), x + 3 + i * 2, y + h / 2 + (i < 2 ? i : 3 - i), text, 0);
            break;
        case 0x11:                                               /* AMIGAKEY: a little framed A */
            bevel_rp(a.rp, x, y, x + w - 1, y + h - 1, text, text, 0);
            rp_move(a.rp, x + (w - 8) / 2, y + 6);
            { uint32_t ofg = rd8(a.rp + RastPort_FgPen); wr8(a.rp + RastPort_FgPen, (uint32_t)text); rp_text(a.rp, "A", 1); wr8(a.rp + RastPort_FgPen, ofg); }
            break;
        default:                                                 /* window gadgets: a raised box */
            bevel_rp(a.rp, x, y, x + w - 1, y + h - 1, shine, shadow, sel);
            break;
        }
        return 0;
    }
    }
    return super_method(c, cl, obj, msg);
}

/* itexticlass: IA_Data is an IntuiText chain */
static uint32_t itext_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    if (rd32(msg) == IM_DRAW) {
        DrawArgs a;
        draw_args(obj, msg, &a);
        draw_itext(a.rp, rd32(obj + Image_ImageData), a.x, a.y);
        return 0;
    }
    return super_method(c, cl, obj, msg);
}

/* fillrectclass: a filled box (IA_APattern ignored) */
static uint32_t fillrect_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    if (rd32(msg) == IM_DRAW || rd32(msg) == IM_DRAWFRAME) {
        DrawArgs a;
        draw_args(obj, msg, &a);
        rp_fill(a.rp, a.x, a.y, a.x + a.w - 1, a.y + a.h - 1, rd8(obj + Image_PlanePick), 0);
        return 0;
    }
    return super_method(c, cl, obj, msg);
}

/* ---- gadgets ------------------------------------------------------------------ */

/* gadgetclass instance = struct Gadget, then ICA target and map */
#define GI_TARGET Gadget_SIZEOF
#define GI_MAP (Gadget_SIZEOF + 4)
#define GADGET_INST (Gadget_SIZEOF + 8)

static void gadget_set(M68K *c, uint32_t g, uint32_t tags) {
    (void)c;
    uint32_t st = tags, ti;
    while ((ti = tag_next(&st))) {
        uint32_t d = rd32(ti + 4), f = rd16(g + Gadget_Flags), act = rd16(g + Gadget_Activation);
        switch (rd32(ti)) {
        case GA(1): wr16(g + Gadget_LeftEdge, d); wr16(g + Gadget_Flags, f & ~GFLG_RELRIGHT); break;
        case GA(2): wr16(g + Gadget_LeftEdge, d); wr16(g + Gadget_Flags, f | GFLG_RELRIGHT); break;
        case GA(3): wr16(g + Gadget_LeftEdge + 2, d); wr16(g + Gadget_Flags, f & ~GFLG_RELBOTTOM); break;
        case GA(4): wr16(g + Gadget_LeftEdge + 2, d); wr16(g + Gadget_Flags, f | GFLG_RELBOTTOM); break;
        case GA(5): wr16(g + Gadget_Width, d); wr16(g + Gadget_Flags, f & ~GFLG_RELWIDTH); break;
        case GA(6): wr16(g + Gadget_Width, d); wr16(g + Gadget_Flags, f | GFLG_RELWIDTH); break;
        case GA(7): wr16(g + Gadget_Width + 2, d); wr16(g + Gadget_Flags, f & ~GFLG_RELHEIGHT); break;
        case GA(8): wr16(g + Gadget_Width + 2, d); wr16(g + Gadget_Flags, f | GFLG_RELHEIGHT); break;
        case GA(9): case GA(34): wr32(g + Gadget_GadgetText, d); break;
        case GA(10): wr32(g + Gadget_GadgetRender, d); wr16(g + Gadget_Flags, rd16(g + Gadget_Flags) | GFLG_GADGIMAGE); break;
        case GA(11): wr32(g + Gadget_GadgetRender, d); wr16(g + Gadget_Flags, rd16(g + Gadget_Flags) & ~GFLG_GADGIMAGE); break;
        case GA(12): wr32(g + Gadget_SelectRender, d); break;
        case GA(13): wr16(g + Gadget_Flags, (f & ~3u) | (d & 3)); break;
        case GA(14): wr16(g + Gadget_Flags, d ? f | GFLG_DISABLED : f & ~GFLG_DISABLED); break;
        case GA(16): wr16(g + Gadget_GadgetID, d); break;
        case GA(17): wr32(g + Gadget_UserData, d); break;
        case GA(18): wr32(g + Gadget_SpecialInfo, d); break;
        case GA(19): wr16(g + Gadget_Flags, d ? f | GFLG_SELECTED : f & ~GFLG_SELECTED); break;
        case GA(20): wr16(g + Gadget_Activation, d ? act | GACT_ENDGADGET : act & ~GACT_ENDGADGET); break;
        case GA(21): wr16(g + Gadget_Activation, d ? act | GACT_IMMEDIATE : act & ~GACT_IMMEDIATE); break;
        case GA(22): wr16(g + Gadget_Activation, d ? act | GACT_RELVERIFY : act & ~GACT_RELVERIFY); break;
        case GA(23): wr16(g + Gadget_Activation, d ? act | GACT_FOLLOWMOUSE : act & ~GACT_FOLLOWMOUSE); break;
        case GA(28): wr16(g + Gadget_Activation, d ? act | GACT_TOGGLESELECT : act & ~GACT_TOGGLESELECT); break;
        case GA(31): if (d) { wr32(g + Gadget_NextGadget, rd32(d + Gadget_NextGadget)); wr32(d + Gadget_NextGadget, g); } break;
        case ICA_TARGET: wr32(g + GI_TARGET, d); break;
        case ICA_MAP: wr32(g + GI_MAP, d); break;
        }
    }
}

static uint32_t gadget_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: {
        uint32_t o = super_method(c, cl, obj, msg);
        if (!o) return 0;
        wr16(o + Gadget_GadgetType, GTYP_CUSTOMGADGET);
        wr16(o + Gadget_Flags, GFLG_GADGHCOMP);
        gadget_set(c, o, rd32(msg + 4));
        return o;
    }
    case OM_SET: case OM_UPDATE: gadget_set(c, obj, rd32(msg + 4)); return 0;
    case OM_GET: {
        uint32_t aid = rd32(msg + 4), st = rd32(msg + 8), v;
        switch (aid) {
        case GA(1): v = (uint32_t)(int16_t)rd16(obj + Gadget_LeftEdge); break;
        case GA(3): v = (uint32_t)(int16_t)rd16(obj + Gadget_LeftEdge + 2); break;
        case GA(5): v = rd16(obj + Gadget_Width); break;
        case GA(7): v = rd16(obj + Gadget_Width + 2); break;
        case GA(14): v = (rd16(obj + Gadget_Flags) & GFLG_DISABLED) != 0; break;
        case GA(16): v = rd16(obj + Gadget_GadgetID); break;
        case GA(17): v = rd32(obj + Gadget_UserData); break;
        case GA(19): v = (rd16(obj + Gadget_Flags) & GFLG_SELECTED) != 0; break;
        default: return super_method(c, cl, obj, msg);
        }
        wr32(st, v);
        return 1;
    }
    case OM_NOTIFY:
        notify(c, rd32(obj + GI_TARGET), rd32(obj + GI_MAP), rd32(msg + 4), rd32(msg + 8), rd32(msg + 12));
        return 0;
    case GM_HITTEST: return GMR_GADGETHIT;
    case GM_RENDER: case GM_GOINACTIVE: case GM_LAYOUT: return 0;
    case GM_GOACTIVE: case GM_HANDLEINPUT: return GMR_NOREUSE;
    }
    return super_method(c, cl, obj, msg);
}

/* buttongclass: highlights while pressed, verifies on release inside */
static void button_render(M68K *c, uint32_t g, uint32_t ginfo, uint32_t rp) {
    uint32_t win = ginfo ? rd32(ginfo + GadgetInfo_gi_Window) : 0;
    NWindow *w = win ? nwin_of(win) : 0;
    if (!w) return;
    int x, y, gw, gh;
    gadget_box(w, g, &x, &y, &gw, &gh);
    uint32_t fl = rd16(g + Gadget_Flags), img = rd32(g + Gadget_GadgetRender);
    int sel = (fl & GFLG_SELECTED) != 0;
    uint32_t dri = ginfo ? rd32(ginfo + GadgetInfo_gi_DrInfo) : 0;
    if ((fl & GFLG_GADGIMAGE) && img) {
        if (rd16(img + Image_Depth) == 0xFFFF) {
            uint32_t m[8] = { IM_DRAWFRAME, rp, (uint32_t)(x & 0xFFFF) << 16 | (uint32_t)(y & 0xFFFF), (uint32_t)(sel ? IDS_SELECTED : IDS_NORMAL), dri,
                              (uint32_t)(gw & 0xFFFF) << 16 | (uint32_t)(gh & 0xFFFF) };
            bo_domethod(c, img, 6, m);
        } else draw_image(rp, img, x, y, sel);
    } else if (img) {
        draw_border(rp, img, x, y);
    }
    uint32_t text = rd32(g + Gadget_GadgetText);
    if (text) {
        /* centre a plain IntuiText */
        int tw = itext_length(text);
        uint32_t ofg = rd8(rp + RastPort_FgPen), omode = rd8(rp + RastPort_DrawMode);
        int pen = pen_of(dri, sel ? FILLTEXTPEN : TEXTPEN, 1);
        char buf[128];
        int n = str_get(rd32(text + IntuiText_IText), buf, sizeof buf);
        wr8(rp + RastPort_FgPen, (uint32_t)pen);
        wr8(rp + RastPort_DrawMode, JAM1);
        rp_move(rp, x + (gw - tw) / 2, y + (gh - 8) / 2 + 6);
        rp_text(rp, buf, n);
        wr8(rp + RastPort_FgPen, ofg);
        wr8(rp + RastPort_DrawMode, omode);
    }
    if (!((fl & GFLG_GADGIMAGE) && img) && sel && (fl & 3) == GFLG_GADGHCOMP) rp_fill(rp, x, y, x + gw - 1, y + gh - 1, 0, 1);
    if (fl & GFLG_DISABLED)
        for (int yy = y; yy < y + gh; yy++) for (int xx = x + (yy & 1); xx < x + gw; xx += 2) rp_fill(rp, xx, yy, xx, yy, pen_of(dri, BLOCKPEN, 1), 0);
}

/* redraw through the object's own class, so subclasses get their GM_RENDER */
static void rerender(M68K *c, uint32_t obj, uint32_t ginfo) {
    uint32_t win = rd32(ginfo + GadgetInfo_gi_Window);
    if (!win) return;
    uint32_t m[4] = { GM_RENDER, ginfo, rd32(win + Window_RPort), GREDRAW_UPDATE };
    bo_domethod(c, obj, 4, m);
}

static uint32_t button_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    switch (rd32(msg)) {
    case GM_RENDER: {
        uint32_t ginfo = rd32(msg + gpRender_gpr_GInfo), rp = rd32(msg + gpRender_gpr_RPort);
        if (rp) button_render(c, obj, ginfo, rp);
        return 0;
    }
    case GM_GOACTIVE: {
        uint32_t ginfo = rd32(msg + gpInput_gpi_GInfo);
        wr16(obj + Gadget_Flags, rd16(obj + Gadget_Flags) | GFLG_SELECTED);
        rerender(c, obj, ginfo);
        return GMR_MEACTIVE;
    }
    case GM_HANDLEINPUT: {
        uint32_t ginfo = rd32(msg + gpInput_gpi_GInfo), ie = rd32(msg + gpInput_gpi_IEvent);
        int mx = (int16_t)rd16(msg + gpInput_gpi_Mouse), my = (int16_t)rd16(msg + gpInput_gpi_Mouse + 2);
        uint32_t win = rd32(ginfo + GadgetInfo_gi_Window);
        NWindow *w = nwin_of(win);
        int x, y, gw, gh;
        gadget_box(w, obj, &x, &y, &gw, &gh);
        int inside = mx >= 0 && my >= 0 && mx < gw && my < gh;
        uint32_t fl = rd16(obj + Gadget_Flags);
        if (!!(fl & GFLG_SELECTED) != inside) {
            wr16(obj + Gadget_Flags, inside ? fl | GFLG_SELECTED : fl & ~GFLG_SELECTED);
            rerender(c, obj, ginfo);
        }
        if (rd8(ie + InputEvent_ie_Class) == 2 && rd16(ie + InputEvent_ie_Code) == (0x68 | 0x80)) {   /* SELECTUP */
            wr16(obj + Gadget_Flags, rd16(obj + Gadget_Flags) & ~GFLG_SELECTED);
            rerender(c, obj, ginfo);
            if (inside) {
                uint32_t tags = exec_allocmem(24, MEMF_PUBLIC | MEMF_CLEAR);
                wr32(tags, GA(16)); wr32(tags + 4, rd16(obj + Gadget_GadgetID));
                uint32_t m[4] = { OM_NOTIFY, tags, ginfo, 0 };
                bo_domethod(c, obj, 4, m);
                exec_freemem(tags, 24);
                return GMR_NOREUSE | GMR_VERIFY;
            }
            return GMR_NOREUSE;
        }
        return GMR_MEACTIVE;
    }
    case GM_GOINACTIVE:
        wr16(obj + Gadget_Flags, rd16(obj + Gadget_Flags) & ~GFLG_SELECTED);
        return 0;
    }
    return super_method(c, cl, obj, msg);
}

/* propgclass: a real proportional gadget (input handled natively), with
   Top/Total/Visible on top of the pots */
#define PG_PROPINFO GADGET_INST
#define PG_TOP (GADGET_INST + PropInfo_SIZEOF)
#define PG_TOTAL (PG_TOP + 4)
#define PG_VISIBLE (PG_TOP + 8)

static void prop_from_tvt(uint32_t g) {
    uint32_t pi = g + PG_PROPINFO;
    uint32_t top = rd32(g + PG_TOP), total = rd32(g + PG_TOTAL), vis = rd32(g + PG_VISIBLE);
    uint32_t body = total > vis && total ? (uint32_t)((uint64_t)0xFFFF * vis / total) : 0xFFFF;
    uint32_t pot = total > vis ? (uint32_t)((uint64_t)0xFFFF * top / (total - vis)) : 0;
    if (pot > 0xFFFF) pot = 0xFFFF;
    if (rd16(pi + PropInfo_Flags) & PI_FREEHORIZ_) { wr16(pi + PropInfo_HorizBody, body); wr16(pi + PropInfo_HorizPot, pot); }
    else { wr16(pi + PropInfo_VertBody, body); wr16(pi + PropInfo_VertPot, pot); }
}

uint32_t prop_top_from_pot(uint32_t g) {
    uint32_t pi = g + PG_PROPINFO, total = rd32(g + PG_TOTAL), vis = rd32(g + PG_VISIBLE);
    uint32_t pot = (rd16(pi + PropInfo_Flags) & PI_FREEHORIZ_) ? rd16(pi + PropInfo_HorizPot) : rd16(pi + PropInfo_VertPot);
    return total > vis ? (uint32_t)(((uint64_t)pot * (total - vis) + 0x7FFF) / 0xFFFF) : 0;
}

static uint32_t prop_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: case OM_SET: case OM_UPDATE: {
        uint32_t o = super_method(c, cl, obj, msg);
        if (id != OM_NEW) o = obj;
        if (!o) return 0;
        uint32_t tags = rd32(msg + 4), pi = o + PG_PROPINFO;
        if (id == OM_NEW) {
            wr16(o + Gadget_GadgetType, GTYP_PROPGADGET);
            wr32(o + Gadget_SpecialInfo, pi);
            wr16(pi + PropInfo_Flags, PI_AUTOKNOB_ | 0x10 | PI_FREEVERT_);
            wr16(pi + PropInfo_HorizBody, 0xFFFF);
            wr16(pi + PropInfo_VertBody, 0xFFFF);
            wr32(o + PG_TOTAL, 1);
            wr32(o + PG_VISIBLE, 1);
        }
        uint32_t st = tags, ti;
        int tvt = 0;
        while ((ti = tag_next(&st))) {
            uint32_t d = rd32(ti + 4);
            switch (rd32(ti)) {
            case PGA(1): wr16(pi + PropInfo_Flags, (rd16(pi + PropInfo_Flags) & ~6u) | (d & 6)); break;
            case PGA(2): if (d) wr16(pi + PropInfo_Flags, rd16(pi + PropInfo_Flags) | 8); break;
            case PGA(3): wr16(pi + PropInfo_HorizPot, d); break;
            case PGA(4): wr16(pi + PropInfo_HorizBody, d); break;
            case PGA(5): wr16(pi + PropInfo_VertPot, d); break;
            case PGA(6): wr16(pi + PropInfo_VertBody, d); break;
            case PGA(7): wr32(o + PG_TOTAL, d); tvt = 1; break;
            case PGA(8): wr32(o + PG_VISIBLE, d); tvt = 1; break;
            case PGA(9): wr32(o + PG_TOP, d); tvt = 1; break;
            }
        }
        if (tvt) prop_from_tvt(o);
        return id == OM_NEW ? o : 1;
    }
    case OM_GET: {
        uint32_t aid = rd32(msg + 4), sp = rd32(msg + 8);
        switch (aid) {
        case PGA(9): wr32(sp, prop_top_from_pot(obj)); return 1;
        case PGA(7): wr32(sp, rd32(obj + PG_TOTAL)); return 1;
        case PGA(8): wr32(sp, rd32(obj + PG_VISIBLE)); return 1;
        case PGA(3): wr32(sp, rd16(obj + PG_PROPINFO + PropInfo_HorizPot)); return 1;
        case PGA(5): wr32(sp, rd16(obj + PG_PROPINFO + PropInfo_VertPot)); return 1;
        }
        break;
    }
    }
    return super_method(c, cl, obj, msg);
}

/* strgclass: a real string gadget with its own StringInfo and buffers */
#define SG_INFO GADGET_INST
#define SG_BUF (GADGET_INST + StringInfo_SIZEOF)
static uint32_t strg_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    uint32_t id = rd32(msg);
    switch (id) {
    case OM_NEW: case OM_SET: case OM_UPDATE: {
        uint32_t o = super_method(c, cl, obj, msg);
        if (id != OM_NEW) o = obj;
        if (!o) return 0;
        uint32_t si = o + SG_INFO, tags = rd32(msg + 4);
        if (id == OM_NEW) {
            wr16(o + Gadget_GadgetType, GTYP_STRGADGET);
            wr32(o + Gadget_SpecialInfo, si);
            int max = (int)tag_get(tags, STRINGA(1), 128);
            uint32_t buf = tag_get(tags, STRINGA(2), 0);
            if (!buf) buf = exec_allocmem((uint32_t)max + 1, MEMF_PUBLIC | MEMF_CLEAR);
            wr32(si + StringInfo_Buffer, buf);
            wr16(si + StringInfo_MaxChars, (uint32_t)max);
        }
        uint32_t ti;
        if ((ti = tag_find(tags, STRINGA(0x12)))) {              /* TextVal */
            char t[512];
            str_get(rd32(ti + 4), t, sizeof t);
            uint32_t buf = rd32(si + StringInfo_Buffer);
            int max = rd16(si + StringInfo_MaxChars);
            int n = (int)strlen(t) < max - 1 ? (int)strlen(t) : max - 1;
            for (int i = 0; i < n; i++) wr8(buf + (uint32_t)i, (uint8_t)t[i]);
            wr8(buf + (uint32_t)n, 0);
            wr16(si + StringInfo_BufferPos, (uint32_t)n);
        }
        if ((ti = tag_find(tags, STRINGA(0x11)))) {              /* LongVal */
            char t[32];
            snprintf(t, sizeof t, "%d", (int32_t)rd32(ti + 4));
            uint32_t buf = rd32(si + StringInfo_Buffer);
            for (int i = 0; i <= (int)strlen(t); i++) wr8(buf + (uint32_t)i, (uint8_t)t[i]);
            wr32(si + StringInfo_LongInt, rd32(ti + 4));
            wr16(o + Gadget_Activation, rd16(o + Gadget_Activation) | GACT_LONGINT);
        }
        return id == OM_NEW ? o : 1;
    }
    case OM_GET: {
        uint32_t aid = rd32(msg + 4), sp = rd32(msg + 8), si = obj + SG_INFO;
        if (aid == STRINGA(0x12)) { wr32(sp, rd32(si + StringInfo_Buffer)); return 1; }
        if (aid == STRINGA(0x11)) {
            char t[64];
            str_get(rd32(si + StringInfo_Buffer), t, sizeof t);
            wr32(sp, (uint32_t)strtol(t, 0, 10));
            return 1;
        }
        break;
    }
    }
    (void)SG_BUF;
    return super_method(c, cl, obj, msg);
}

/* groupgclass: members are added to the window with the group */
static uint32_t group_dispatch(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) {
    if (rd32(msg) == OM_ADDMEMBER) {
        /* chain the member after the group (and its previous members) */
        uint32_t m = rd32(msg + 4), last = obj;
        while (rd32(last + Gadget_NextGadget)) last = rd32(last + Gadget_NextGadget);
        wr32(last + Gadget_NextGadget, m);
        return 1;
    }
    return super_method(c, cl, obj, msg);
}

/* ---- the input side: custom gadgets in windows ---------------------------------- */

uint32_t bo_ginfo(NWindow *w) {
    uint32_t gi = exec_allocmem(GadgetInfo_SIZEOF + 8, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(gi + GadgetInfo_gi_Screen, w->s->scr);
    wr32(gi + GadgetInfo_gi_Window, w->win);
    wr32(gi + GadgetInfo_gi_RastPort, w->rp);
    wr32(gi + GadgetInfo_gi_Layer, w->layer);
    wr16(gi + GadgetInfo_gi_Domain, 0);
    wr16(gi + GadgetInfo_gi_Domain + 2, 0);
    wr16(gi + GadgetInfo_gi_Domain + 4, (uint32_t)w->w);
    wr16(gi + GadgetInfo_gi_Domain + 6, (uint32_t)w->h);
    wr8(gi + GadgetInfo_gi_Pens, rd8(w->win + Window_DetailPen));
    wr8(gi + GadgetInfo_gi_Pens + 1, rd8(w->win + Window_BlockPen));
    wr32(gi + GadgetInfo_gi_DrInfo, w->s->drawinfo);
    return gi;
}
void bo_ginfo_free(uint32_t gi) { exec_freemem(gi, GadgetInfo_SIZEOF + 8); }

void bo_render(M68K *c, NWindow *w, uint32_t g, int redraw) {
    uint32_t gi = bo_ginfo(w);
    uint32_t m[3] = { GM_RENDER, gi, w->rp };
    uint32_t mm[4] = { m[0], m[1], m[2], (uint32_t)redraw };
    bo_domethod(c, g, 4, mm);
    bo_ginfo_free(gi);
}

int bo_hittest(M68K *c, NWindow *w, uint32_t g, int gx, int gy) {
    uint32_t gi = bo_ginfo(w);
    uint32_t m[3] = { GM_HITTEST, gi, (uint32_t)(gx & 0xFFFF) << 16 | (uint32_t)(gy & 0xFFFF) };
    uint32_t r = bo_domethod(c, g, 3, m);
    bo_ginfo_free(gi);
    return (r & GMR_GADGETHIT) != 0;
}

/* GM_GOACTIVE / GM_HANDLEINPUT with a synthetic InputEvent; *code gets the termination */
uint32_t bo_input(M68K *c, NWindow *w, uint32_t g, int method, int ieclass, int iecode, int qual, int gx, int gy, uint32_t *code) {
    uint32_t gi = bo_ginfo(w), ie = exec_allocmem(InputEvent_SIZEOF + 4, MEMF_PUBLIC | MEMF_CLEAR), term = exec_allocmem(4, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(ie + InputEvent_ie_Class, (uint32_t)ieclass);
    wr16(ie + InputEvent_ie_Code, (uint32_t)iecode);
    wr16(ie + InputEvent_ie_Qualifier, (uint32_t)qual);
    wr16(ie + InputEvent_ie_position, (uint32_t)gx);
    wr16(ie + InputEvent_ie_position + 2, (uint32_t)gy);
    uint32_t m[5] = { (uint32_t)method, gi, ie, term, (uint32_t)(gx & 0xFFFF) << 16 | (uint32_t)(gy & 0xFFFF) };
    uint32_t r = bo_domethod(c, g, 5, m);
    *code = rd32(term);
    exec_freemem(term, 4);
    exec_freemem(ie, InputEvent_SIZEOF + 4);
    bo_ginfo_free(gi);
    return r;
}

void bo_goinactive(M68K *c, NWindow *w, uint32_t g, int abort_) {
    uint32_t gi = bo_ginfo(w);
    uint32_t m[3] = { GM_GOINACTIVE, gi, (uint32_t)abort_ };
    bo_domethod(c, g, 3, m);
    bo_ginfo_free(gi);
}

/* a built-in prop/string gadget changed: tell its ICA target (BOOPSI only) */
void bo_gadget_changed(M68K *c, NWindow *w, uint32_t g, int interim) {
    if (!bo_is_object(g)) return;
    uint32_t tags = exec_allocmem(32, MEMF_PUBLIC | MEMF_CLEAR);
    int type = rd16(g + Gadget_GadgetType) & 7;
    if (type == GTYP_PROPGADGET) { wr32(tags, PGA(9)); wr32(tags + 4, prop_top_from_pot(g)); }
    else { wr32(tags, STRINGA(0x12)); wr32(tags + 4, rd32(rd32(g + Gadget_SpecialInfo) + StringInfo_Buffer)); }
    wr32(tags + 8, GA(16)); wr32(tags + 12, rd16(g + Gadget_GadgetID));
    uint32_t gi = bo_ginfo(w);
    uint32_t m[4] = { OM_NOTIFY, tags, gi, interim ? OPUF_INTERIM : 0 };
    bo_domethod(c, g, 4, m);
    bo_ginfo_free(gi);
    exec_freemem(tags, 32);
}

/* IM_DRAW for DrawImage() on a BOOPSI image */
void bo_draw_image(M68K *c, uint32_t rp, uint32_t img, int x, int y, int state, uint32_t dri) {
    uint32_t m[5] = { IM_DRAW, rp, (uint32_t)(x & 0xFFFF) << 16 | (uint32_t)(y & 0xFFFF), (uint32_t)state, dri };
    bo_domethod(c, img, 5, m);
}

/* ---- intuition.library functions ------------------------------------------------- */

static uint32_t make_class(const char *name, uint32_t super, int instsize, Dispatcher fn) {
    uint32_t cl = exec_allocmem(IClass_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    uint32_t off = super ? rd16(super + IClass_cl_InstOffset) + rd16(super + IClass_cl_InstSize) : 0;
    wr32(cl + IClass_cl_Super, super);
    wr16(cl + IClass_cl_InstOffset, off);
    wr16(cl + IClass_cl_InstSize, (uint32_t)instsize);
    if (name) wr32(cl + IClass_cl_ID, str_alloc(name));
    if (super) wr32(super + IClass_cl_SubclassCount, rd32(super + IClass_cl_SubclassCount) + 1);
    NClass *n = calloc(1, sizeof *n);
    n->cl = cl;
    n->fn = fn;
    if (name && fn && name[0] != '_') snprintf(n->name, sizeof n->name, "%s", name);   /* '_name' = private */
    n->next = classes;
    classes = n;
    if (fn) {
        /* a 68k-callable entry for code that calls h_Entry itself (DoSuperMethodA) */
        static int k;
        char *tn = malloc(48);
        snprintf(tn, 48, "<dispatch:%s>", name ? name : "?");
        (void)k;
        extern uint32_t bo_trap_entry(Dispatcher fn, const char *name);
        wr32(cl + Hook_h_Entry, bo_trap_entry(fn, tn));
    }
    return cl;
}

/* the trap behind a native dispatcher: A0 = class, A2 = object, A1 = message */
typedef struct { Dispatcher fn; } TrapDisp;
static TrapDisp tdisp[64];
static int ntdisp;
#define DISP_TRAP(i) static uint32_t disp_trap##i(M68K *c) { return tdisp[i].fn(c, c->a[0], c->a[2], c->a[1]); }
DISP_TRAP(0) DISP_TRAP(1) DISP_TRAP(2) DISP_TRAP(3) DISP_TRAP(4) DISP_TRAP(5) DISP_TRAP(6) DISP_TRAP(7)
DISP_TRAP(8) DISP_TRAP(9) DISP_TRAP(10) DISP_TRAP(11) DISP_TRAP(12) DISP_TRAP(13) DISP_TRAP(14) DISP_TRAP(15)
DISP_TRAP(16) DISP_TRAP(17) DISP_TRAP(18) DISP_TRAP(19) DISP_TRAP(20) DISP_TRAP(21) DISP_TRAP(22) DISP_TRAP(23)
DISP_TRAP(24) DISP_TRAP(25) DISP_TRAP(26) DISP_TRAP(27) DISP_TRAP(28) DISP_TRAP(29) DISP_TRAP(30) DISP_TRAP(31)
static NativeFn disp_traps[32] = { disp_trap0, disp_trap1, disp_trap2, disp_trap3, disp_trap4, disp_trap5, disp_trap6, disp_trap7,
                                   disp_trap8, disp_trap9, disp_trap10, disp_trap11, disp_trap12, disp_trap13, disp_trap14, disp_trap15,
                                   disp_trap16, disp_trap17, disp_trap18, disp_trap19, disp_trap20, disp_trap21, disp_trap22, disp_trap23,
                                   disp_trap24, disp_trap25, disp_trap26, disp_trap27, disp_trap28, disp_trap29, disp_trap30, disp_trap31 };

uint32_t bo_trap_entry(Dispatcher fn, const char *name) {
    if (ntdisp >= 32) { os_log("BOOPSI : trop de classes natives"); return 0; }
    tdisp[ntdisp].fn = fn;
    uint32_t id = trap_new(name, disp_traps[ntdisp]);
    ntdisp++;
    uint32_t stub = exec_allocmem(4, MEMF_PUBLIC);
    wr16(stub, 0xA000 | id);
    wr16(stub + 2, 0x4E75);
    return stub;
}

static uint32_t i_MakeClass(M68K *c) {
    /* classID A0, superClassID A1, superClassPtr A2, instanceSize D0, flags D1 */
    uint32_t super = c->a[2];
    if (!super && c->a[1]) { char n[64]; str_get(c->a[1], n, sizeof n); super = find_public(n); }
    if (!super) return 0;
    char name[64] = "";
    if (c->a[0]) str_get(c->a[0], name, sizeof name);
    uint32_t cl = make_class(c->a[0] ? name : 0, super, (int)(c->d[0] & 0xFFFF), 0);
    if (c->a[0]) wr32(cl + IClass_cl_ID, c->a[0]);              /* keep the caller's string */
    return cl;
}
static uint32_t i_AddClass(M68K *c) {
    NClass *n = nclass_of(c->a[0]);
    if (n && rd32(c->a[0] + IClass_cl_ID)) str_get(rd32(c->a[0] + IClass_cl_ID), n->name, sizeof n->name);
    wr32(c->a[0] + IClass_cl_Flags, rd32(c->a[0] + IClass_cl_Flags) | 1);   /* CLF_INLIST */
    return 0;
}
static uint32_t i_RemoveClass(M68K *c) {
    NClass *n = nclass_of(c->a[0]);
    if (n) n->name[0] = 0;
    wr32(c->a[0] + IClass_cl_Flags, rd32(c->a[0] + IClass_cl_Flags) & ~1u);
    return 0;
}
static uint32_t i_FreeClass(M68K *c) {
    uint32_t cl = c->a[0];
    if (!cl || rd32(cl + IClass_cl_ObjectCount) || rd32(cl + IClass_cl_SubclassCount)) return 0;
    uint32_t super = rd32(cl + IClass_cl_Super);
    if (super) wr32(super + IClass_cl_SubclassCount, rd32(super + IClass_cl_SubclassCount) - 1);
    NClass **pp = &classes;
    while (*pp && (*pp)->cl != cl) pp = &(*pp)->next;
    if (*pp) { NClass *n = *pp; *pp = n->next; free(n); }
    exec_freemem(cl, IClass_SIZEOF);
    return DOSTRUE;
}
static uint32_t i_NewObjectA(M68K *c) {
    uint32_t cl = c->a[0];
    if (!cl) { char n[64]; str_get(c->a[1], n, sizeof n); cl = find_public(n); if (!cl) { os_log("NewObject : classe « %s » inconnue", n); return 0; } }
    uint32_t m[3] = { OM_NEW, c->a[2], 0 };
    uint32_t msg = msg_new(3, m);
    uint32_t o = bo_dispatch(c, cl, cl, msg);                   /* OM_NEW: the object slot carries the true class */
    msg_free(msg, 3);
    return o;
}
static uint32_t i_DisposeObject(M68K *c) {
    if (!c->a[0]) return 0;
    uint32_t m[1] = { OM_DISPOSE };
    bo_domethod(c, c->a[0], 1, m);
    return 0;
}
static uint32_t i_SetAttrsA(M68K *c) {
    uint32_t m[3] = { OM_SET, c->a[1], 0 };
    return bo_domethod(c, c->a[0], 3, m);
}
static uint32_t i_SetGadgetAttrsA(M68K *c) {
    NWindow *w = c->a[1] ? nwin_of(c->a[1]) : 0;
    uint32_t gi = w ? bo_ginfo(w) : 0;
    uint32_t m[3] = { OM_SET, c->a[3], gi };
    uint32_t r = bo_domethod(c, c->a[0], 3, m);
    if (w) {
        int type = rd16(c->a[0] + Gadget_GadgetType) & 7;
        if (type == GTYP_CUSTOMGADGET) bo_render(c, w, c->a[0], GREDRAW_UPDATE);
        else gadget_render_all(w, c->a[0], 1);
        bo_ginfo_free(gi);
    }
    return r;
}
static uint32_t i_GetAttr(M68K *c) {
    uint32_t m[3] = { OM_GET, c->d[0], c->a[1] };
    return bo_domethod(c, c->a[0], 3, m);
}
static uint32_t i_NextObject(M68K *c) {
    /* objectPtrPtr A0: points at an object's MinNode, iterates the list */
    uint32_t pp = c->a[0], node = rd32(pp);
    if (!node) return 0;
    uint32_t next = rd32(node + LN_SUCC);
    if (!next) return 0;
    wr32(pp, next);
    return node + _Object_SIZEOF;
}
static uint32_t i_ObtainGIRPort(M68K *c) { return c->a[0] ? rd32(c->a[0] + GadgetInfo_gi_RastPort) : 0; }
static uint32_t i_ReleaseGIRPort(M68K *c) { (void)c; return 0; }
static uint32_t i_DoGadgetMethodA(M68K *c) {
    /* gad A0, win A1, req A2, msg A3: fill in the GadgetInfo where methods have one */
    NWindow *w = c->a[1] ? nwin_of(c->a[1]) : 0;
    uint32_t msg = c->a[3], id = rd32(msg), gi = 0;
    if (w && (id <= GM_LAYOUT || id == OM_SET || id == OM_UPDATE || id == OM_NOTIFY)) {
        gi = bo_ginfo(w);
        wr32(msg + 4 + (id == OM_SET || id == OM_UPDATE || id == OM_NOTIFY ? 4 : 0), gi);
    }
    uint32_t r = bo_dispatch(c, bo_class_of(c->a[0]), c->a[0], msg);
    if (gi) bo_ginfo_free(gi);
    return r;
}

const NativeEntry boopsi_natives[] = {
    { "MakeClass", i_MakeClass }, { "AddClass", i_AddClass }, { "RemoveClass", i_RemoveClass },
    { "FreeClass", i_FreeClass }, { "NewObjectA", i_NewObjectA }, { "DisposeObject", i_DisposeObject },
    { "SetAttrsA", i_SetAttrsA }, { "SetGadgetAttrsA", i_SetGadgetAttrsA }, { "GetAttr", i_GetAttr },
    { "NextObject", i_NextObject }, { "ObtainGIRPort", i_ObtainGIRPort }, { "ReleaseGIRPort", i_ReleaseGIRPort },
    { "DoGadgetMethodA", i_DoGadgetMethodA },
    { 0, 0 }
};

void boopsi_init(void) {
    uint32_t root = make_class("rootclass", 0, 0, root_dispatch);
    uint32_t image = make_class("imageclass", root, Image_SIZEOF, image_dispatch);
    make_class("frameiclass", image, 4, frame_dispatch);
    make_class("sysiclass", image, 8, sysi_dispatch);
    make_class("itexticlass", image, 0, itext_dispatch);
    make_class("fillrectclass", image, 0, fillrect_dispatch);
    uint32_t gadget = make_class("gadgetclass", root, GADGET_INST, gadget_dispatch);
    uint32_t button = make_class("buttongclass", gadget, 0, button_dispatch);
    make_class("frbuttonclass", button, 0, button_dispatch);
    make_class("propgclass", gadget, PropInfo_SIZEOF + 12, prop_dispatch);
    make_class("strgclass", gadget, StringInfo_SIZEOF + 4, strg_dispatch);
    make_class("groupgclass", gadget, 0, group_dispatch);
    make_class("pointerclass", root, 0, root_dispatch_ptr);
    uint32_t ic = make_class("icclass", root, 8, ic_dispatch);
    make_class("modelclass", ic, LH_SIZE + 2, model_dispatch);
    (void)public_classes;
}

/* ---- for other native modules (gadtools...) ------------------------------------ */

uint32_t bo_native_class(const char *name, const char *super, int instsize, Dispatcher fn) {
    return make_class(name, find_public(super), instsize, fn);
}
uint32_t bo_new(M68K *c, uint32_t cl, uint32_t tags) {
    uint32_t m[3] = { OM_NEW, tags, 0 };
    uint32_t msg = msg_new(3, m);
    uint32_t o = bo_dispatch(c, cl, cl, msg);
    msg_free(msg, 3);
    return o;
}
uint32_t bo_inst(uint32_t cl, uint32_t obj) { return inst(cl, obj); }
uint32_t bo_super(M68K *c, uint32_t cl, uint32_t obj, uint32_t msg) { return super_method(c, cl, obj, msg); }
