/* commodities.library, written from the commodities.library autodocs and
 * libraries/commodities.h: brokers and their networks of filter, sender,
 * signal, translate and custom objects, hotkey descriptions (ParseIX/MatchIX).
 * The network sits in the input stream at priority 53, ahead of Intuition, as
 * on AmigaOS: input.device hands it each event (cx_filter).
 *
 * Objects are native; programs hold small Amiga blocks as their handles.
 */
#include "os.h"
#include "sched.h"
#include "console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

uint32_t CxBase;

enum { CX_INVALID, CX_FILTER, CX_TYPEFILTER, CX_SEND, CX_SIGNAL, CX_TRANSLATE, CX_BROKER, CX_DEBUG, CX_CUSTOM, CX_ZERO };
#define CXM_IEVENT 0x20
#define CXM_COMMAND 0x40
#define CXCMD_UNIQUE 25
#define CXCMD_LIST_CHG 27
#define COERR_ISNULL 1
#define COERR_NULLATTACH 2
#define COERR_BADFILTER 4
#define COERR_BADTYPE 8
#define CBERR_OK 0
#define CBERR_SYSERR 1
#define CBERR_DUP 2
#define CBERR_VERSION 3
#define NBU_UNIQUE 1
#define NBU_NOTIFY 2

/* InputEvent: ie_NextEvent 0, ie_Class 4, ie_SubClass 5, ie_Code 6, ie_Qualifier 8, ie_X 10, ie_Y 12, ie_TimeStamp 14 */
#define IE_SIZE 22
#define IECLASS_NULL 0
#define IECLASS_RAWKEY 1
#define IECLASS_RAWMOUSE 2

typedef struct { uint8_t version, cls; uint16_t code, codemask, qual, qualmask, qualsame; } IX;

typedef struct Cx Cx;
struct Cx {
    uint32_t handle;                     /* what the program holds */
    int type, pri, active, error;
    Cx *parent, *kids, *next;            /* kids in order */
    /* broker */
    char name[25], title[41], descr[41];
    int unique, flags;
    uint32_t port;
    /* filter */
    IX ix;
    int ix_valid;
    /* sender, custom, debug: arg1/arg2 */
    uint32_t arg1, arg2;
    uint32_t events;                     /* translate: InputEvent list */
};

static Cx *objs[1024];
static int nobjs;
static Cx *brokers;                      /* by priority */
static uint32_t cx_reply_port;           /* replied CxMsgs come back here */

static Cx *cx_of(uint32_t h) {
    if (!h) return 0;
    for (int i = 0; i < nobjs; i++) if (objs[i]->handle == h) return objs[i];
    return 0;
}

static Cx *cx_new(int type) {
    if (nobjs == 1024) return 0;
    Cx *o = calloc(1, sizeof *o);
    o->type = type;
    o->active = 1;
    o->handle = exec_allocmem(8, MEMF_PUBLIC | MEMF_CLEAR);
    wr32(o->handle, (uint32_t)type);
    objs[nobjs++] = o;
    return o;
}

static void unlink_cx(Cx *o) {
    Cx **pp = o->parent ? &o->parent->kids : (o->type == CX_BROKER ? &brokers : 0);
    if (pp) {
        while (*pp && *pp != o) pp = &(*pp)->next;
        if (*pp) *pp = o->next;
    }
    o->parent = 0;
    o->next = 0;
}

static void cx_free(Cx *o, int all) {
    unlink_cx(o);
    while (o->kids) {
        Cx *k = o->kids;
        unlink_cx(k);
        if (all) cx_free(k, 1);
    }
    for (int i = 0; i < nobjs; i++) if (objs[i] == o) { objs[i] = objs[--nobjs]; break; }
    exec_freemem(o->handle, 8);
    free(o);
}

/* ---- input expressions ------------------------------------------------------------ */

static const struct { const char *name; uint16_t q; uint16_t same; } quals[] = {
    { "lshift", 0x0001, 0 }, { "rshift", 0x0002, 0 }, { "capslock", 0x0004, 0 }, { "caps", 0x0007, 2 },
    { "control", 0x0008, 0 }, { "ctrl", 0x0008, 0 }, { "lalt", 0x0010, 0 }, { "ralt", 0x0020, 0 },
    { "alt", 0x0030, 4 }, { "lcommand", 0x0040, 0 }, { "lamiga", 0x0040, 0 }, { "rcommand", 0x0080, 0 },
    { "ramiga", 0x0080, 0 }, { "numericpad", 0x0100, 0 }, { "numpad", 0x0100, 0 }, { "repeat", 0x0200, 0 },
    { "midbutton", 0x1000, 0 }, { "mbutton", 0x1000, 0 }, { "rbutton", 0x2000, 0 }, { "leftbutton", 0x4000, 0 },
    { "lbutton", 0x4000, 0 }, { "relativemouse", 0x8000, 0 }, { "shift", 0x0003, 1 },
};
static const struct { const char *name; int cls; } classes[] = {
    { "rawkey", 1 }, { "rawmouse", 2 }, { "event", 3 }, { "pointerpos", 4 }, { "timer", 6 },
    { "newprefs", 14 }, { "diskremoved", 15 }, { "diskinserted", 16 },
};
static const struct { const char *name; int code; } keys[] = {
    { "space", 0x40 }, { "spacebar", 0x40 }, { "backspace", 0x41 }, { "tab", 0x42 }, { "enter", 0x43 },
    { "return", 0x44 }, { "esc", 0x45 }, { "escape", 0x45 }, { "del", 0x46 }, { "delete", 0x46 },
    { "up", 0x4C }, { "cursor_up", 0x4C }, { "down", 0x4D }, { "cursor_down", 0x4D }, { "right", 0x4E },
    { "cursor_right", 0x4E }, { "left", 0x4F }, { "cursor_left", 0x4F }, { "f1", 0x50 }, { "f2", 0x51 },
    { "f3", 0x52 }, { "f4", 0x53 }, { "f5", 0x54 }, { "f6", 0x55 }, { "f7", 0x56 }, { "f8", 0x57 },
    { "f9", 0x58 }, { "f10", 0x59 }, { "help", 0x5F },
    /* rawmouse */
    { "mouse_leftpress", 0x68 }, { "mouse_rightpress", 0x69 }, { "mouse_middlepress", 0x6A },
};

/* "[class] {[-](qualifier|synonym)} [[-]upstroke] [key]" */
static int parse_ix(const char *desc, IX *ix) {
    memset(ix, 0, sizeof *ix);
    ix->version = 2;
    ix->cls = IECLASS_RAWKEY;
    ix->codemask = 0xFFFF;
    ix->qualmask = 0x7FFF;                                   /* IX_NORMALQUALS */
    int have_key = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", desc ? desc : "");
    for (char *t = strtok(buf, " \t"); t; t = strtok(0, " \t")) {
        int dontcare = *t == '-';
        if (dontcare) t++;
        int done = 0;
        for (unsigned i = 0; i < sizeof classes / sizeof *classes && !done; i++)
            if (!strcasecmp(t, classes[i].name)) {
                ix->cls = (uint8_t)classes[i].cls;
                if (ix->cls != IECLASS_RAWKEY) { ix->codemask = 0; if (ix->cls == IECLASS_RAWMOUSE) ix->codemask = 0xFFFF; }
                done = 1;
            }
        for (unsigned i = 0; i < sizeof quals / sizeof *quals && !done; i++)
            if (!strcasecmp(t, quals[i].name)) {
                if (dontcare) ix->qualmask &= (uint16_t)~quals[i].q;
                else { ix->qual |= quals[i].q; ix->qualsame |= quals[i].same; }
                done = 1;
            }
        if (done) continue;
        if (!strcasecmp(t, "upstroke")) {
            if (dontcare) ix->codemask &= (uint16_t)~0x80; else ix->code |= 0x80;
            continue;
        }
        for (unsigned i = 0; i < sizeof keys / sizeof *keys && !done; i++)
            if (!strcasecmp(t, keys[i].name)) { ix->code = (uint16_t)((ix->code & 0x80) | keys[i].code); done = have_key = 1; }
        if (done) continue;
        if (strlen(t) == 1) {                                    /* an ANSI character: its key */
            int q = 0, raw = keymap_raw_for((unsigned char)tolower((unsigned char)t[0]), &q);
            if (raw < 0) return -1;
            ix->code = (uint16_t)((ix->code & 0x80) | raw);
            have_key = 1;
            continue;
        }
        return -1;                                               /* unknown word */
    }
    if (ix->cls == IECLASS_RAWKEY && !have_key) ix->codemask = 0x80;   /* qualifiers only */
    /* synonyms: either key of the pair satisfies */
    if (ix->qualsame & 1) ix->qual &= (uint16_t)~0x0002;
    if (ix->qualsame & 4) ix->qual &= (uint16_t)~0x0020;
    return 0;
}

static void ix_read(uint32_t a, IX *ix) {
    ix->version = (uint8_t)rd8(a); ix->cls = (uint8_t)rd8(a + 1);
    ix->code = (uint16_t)rd16(a + 2); ix->codemask = (uint16_t)rd16(a + 4);
    ix->qual = (uint16_t)rd16(a + 6); ix->qualmask = (uint16_t)rd16(a + 8);
    ix->qualsame = ix->version >= 2 ? (uint16_t)rd16(a + 10) : 0;
}
static void ix_write(uint32_t a, const IX *ix) {
    wr8(a, ix->version); wr8(a + 1, ix->cls); wr16(a + 2, ix->code); wr16(a + 4, ix->codemask);
    wr16(a + 6, ix->qual); wr16(a + 8, ix->qualmask); wr16(a + 10, ix->qualsame);
}

static int match_ix(uint32_t ie, const IX *ix) {
    if (ix->cls == IECLASS_NULL || rd8(ie + 4) != ix->cls) return 0;
    if ((rd16(ie + 6) ^ ix->code) & ix->codemask) return 0;
    uint16_t q = (uint16_t)rd16(ie + 8), want = ix->qual;
    if ((ix->qualsame & 1) && (q & 0x0003)) q |= 0x0003;          /* IXSYM_SHIFT */
    if ((ix->qualsame & 2) && (q & 0x0007)) q |= 0x0007;          /* IXSYM_CAPS */
    if ((ix->qualsame & 4) && (q & 0x0030)) q |= 0x0030;          /* IXSYM_ALT */
    if (ix->qualsame & 1) want |= (want & 0x0001) ? 0x0003 : 0;
    if (ix->qualsame & 2) want |= (want & 0x0007) ? 0x0007 : 0;
    if (ix->qualsame & 4) want |= (want & 0x0010) ? 0x0030 : 0;
    return !((q ^ want) & ix->qualmask);
}

/* ---- messages --------------------------------------------------------------------- */
/* CxMsg: an exec Message, then cm_Type 20, cm_ID 24, cm_Data 28, the event copy at 32 */
#define CXMSG_SIZE (32 + IE_SIZE + 2)

static void free_replied(void) {
    uint32_t m;
    while ((m = list_remhead(cx_reply_port + MP_MSGLIST))) exec_freemem(m, CXMSG_SIZE);
}

static uint32_t new_cxmsg(int type, uint32_t id, uint32_t ie) {
    free_replied();
    uint32_t m = exec_allocmem(CXMSG_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!m) return 0;
    wr8(m + LN_TYPE, NT_MESSAGE);
    wr32(m + MN_REPLYPORT, cx_reply_port);
    wr16(m + MN_LENGTH, CXMSG_SIZE);
    wr32(m + 20, (uint32_t)type);
    wr32(m + 24, id);
    if (ie) {
        for (int i = 0; i < IE_SIZE; i++) wr8(m + 32 + (uint32_t)i, rd8(ie + (uint32_t)i));
        wr32(m + 32, 0);
        wr32(m + 28, m + 32);
    }
    return m;
}

static void send_command(Cx *b, uint32_t id) {
    if (!b || !b->port) return;
    uint32_t m = new_cxmsg(CXM_COMMAND, id, 0);
    if (m) exec_put_msg(b->port, m);
}

/* ---- the network ------------------------------------------------------------------- */

/* events a translate object puts in the stream (they skip the network): in the
   input task right away, in place of the one taken; from a program, queued */
extern void input_dispatch_now(InputEv *ev);
static int in_network;
static void inject(uint32_t list) {
    for (uint32_t e = list; e; e = rd32(e)) {
        int cls = rd8(e + 4), code = rd16(e + 6), qual = rd16(e + 8);
        InputEv ev = { 0 };
        if (cls == IECLASS_RAWKEY) {
            char ch[4] = "";
            ev.type = IEV_KEY;
            ev.code = code;
            if (!(code & 0x80) && keymap_convert(code, qual, ch, sizeof ch) == 1) ev.ch = (uint8_t)ch[0];
        } else if (cls == IECLASS_RAWMOUSE && (code & 0x7F) >= 0x68 && (code & 0x7F) <= 0x69) {
            ev.type = IEV_BUTTON;
            ev.code = ((code & 0x7F) == 0x68 ? 1 : 2) * ((code & 0x80) ? -1 : 1);
        } else continue;
        ev.qual = qual | 0x10000;                                /* already through commodities */
        if (in_network) { ev.qual = qual; input_dispatch_now(&ev); }
        else amiga_input_event(&ev);
    }
}

typedef struct { uint32_t msg; int disposed; Cx *route; } Flow;
static Flow *cur_flow;

/* one event through a list of objects; 0 when it was swallowed */
static int run_list(M68K *c, Cx *o, uint32_t ie, Flow *f);

static int run_obj(M68K *c, Cx *o, uint32_t ie, Flow *f) {
    if (!o->active) return 1;
    switch (o->type) {
    case CX_FILTER:
        if (o->ix_valid && match_ix(ie, &o->ix)) return run_list(c, o->kids, ie, f);
        return 1;
    case CX_SEND: {
        uint32_t m = new_cxmsg(CXM_IEVENT, o->arg2, ie);
        if (m && o->arg1) exec_put_msg(o->arg1, m);
        return 1;
    }
    case CX_SIGNAL:
        if (o->arg1) sched_signal(o->arg1, 1u << (o->arg2 & 31));
        return 1;
    case CX_TRANSLATE:
        if (o->events) inject(o->events);
        return 0;
    case CX_CUSTOM: {
        /* action(CxMsg *, CxObj *) */
        uint32_t m = new_cxmsg(CXM_IEVENT, o->arg2, ie);
        if (!m) return 1;
        Flow *saved = cur_flow;
        f->msg = m; f->disposed = 0; f->route = 0;
        cur_flow = f;
        /* both ways, as programs expect: A0/A1 and the C stack */
        uint32_t d[8], a[8];
        memcpy(d, c->d, sizeof d); memcpy(a, c->a, sizeof a);
        cpu68k_push32(c, o->handle);
        cpu68k_push32(c, m);
        c->a[0] = m;
        c->a[1] = o->handle;
        cpu_call(c, o->arg1);
        memcpy(c->d, d, sizeof d); memcpy(c->a, a, sizeof a);
        cur_flow = saved;
        for (int i = 4; i < IE_SIZE; i++) wr8(ie + (uint32_t)i, rd8(m + 32 + (uint32_t)i));   /* the action may change it */
        exec_freemem(m, CXMSG_SIZE);
        if (f->disposed) return 0;
        if (f->route) { Cx *r = f->route; f->route = 0; return run_list(c, r, ie, f); }
        return 1;
    }
    default: return 1;
    }
}

static int run_list(M68K *c, Cx *o, uint32_t ie, Flow *f) {
    for (; o; o = o->next) if (!run_obj(c, o, ie, f)) return 0;
    return 1;
}

int cx_active(void) {
    for (Cx *b = brokers; b; b = b->next) if (b->active && b->kids) return 1;
    return 0;
}

/* called by input.device with the event; 0 when the network swallowed it */
int cx_filter(M68K *c, uint32_t ie) {
    Flow f = { 0, 0, 0 };
    int r = 1;
    in_network = 1;
    for (Cx *b = brokers; b && r; b = b->next)
        if (b->active && !run_list(c, b->kids, ie, &f)) r = 0;
    in_network = 0;
    return r;
}

/* ---- library functions -------------------------------------------------------------- */

static uint32_t x_CreateCxObj(M68K *c) {
    int type = (int)c->d[0];
    if (type < CX_FILTER || type > CX_CUSTOM || type == CX_BROKER) return 0;
    Cx *o = cx_new(type);
    if (!o) return 0;
    o->arg1 = c->a[0];
    o->arg2 = c->a[1];
    if (type == CX_FILTER) {
        char d[256];
        o->ix_valid = 0;
        if (o->arg1) {
            str_get(o->arg1, d, sizeof d);
            if (parse_ix(d, &o->ix) == 0) o->ix_valid = 1; else o->error |= COERR_BADFILTER;
        }
    }
    if (type == CX_TRANSLATE) o->events = c->a[0];
    return o->handle;
}

/* CxBroker(newbroker, &error) */
static uint32_t x_CxBroker(M68K *c) {
    uint32_t nb = c->a[0], errp = c->d[0];
    int err = CBERR_OK;
    Cx *b = 0;
    if ((int8_t)rd8(nb) < 5) err = CBERR_VERSION;
    else {
        char name[25];
        str_get(rd32(nb + 2), name, sizeof name);
        int unique = (int16_t)rd16(nb + 14);
        for (Cx *x = brokers; x; x = x->next)
            if (!strcasecmp(x->name, name) && (unique & NBU_UNIQUE)) {
                if (unique & NBU_NOTIFY) send_command(x, CXCMD_UNIQUE);
                err = CBERR_DUP;
            }
        if (!err && !(b = cx_new(CX_BROKER))) err = CBERR_SYSERR;
        if (b) {
            snprintf(b->name, sizeof b->name, "%s", name);
            if (rd32(nb + 6)) str_get(rd32(nb + 6), b->title, sizeof b->title);
            if (rd32(nb + 10)) str_get(rd32(nb + 10), b->descr, sizeof b->descr);
            b->unique = unique;
            b->flags = (int16_t)rd16(nb + 16);
            b->pri = (int8_t)rd8(nb + 18);
            b->port = rd32(nb + 20);
            b->active = 0;                                       /* until ActivateCxObj */
            Cx **pp = &brokers;
            while (*pp && (*pp)->pri >= b->pri) pp = &(*pp)->next;
            b->next = *pp;
            *pp = b;
            for (Cx *x = brokers; x; x = x->next) if (x != b) send_command(x, CXCMD_LIST_CHG);
        }
    }
    if (errp) wr32(errp, (uint32_t)err);
    return b ? b->handle : 0;
}

static uint32_t x_ActivateCxObj(M68K *c) {
    Cx *o = cx_of(c->a[0]);
    if (!o) return 0;
    int old = o->active;
    o->active = c->d[0] != 0;
    return (uint32_t)old;
}
static uint32_t x_DeleteCxObj(M68K *c) { Cx *o = cx_of(c->a[0]); if (o) cx_free(o, 0); return 0; }
static uint32_t x_DeleteCxObjAll(M68K *c) { Cx *o = cx_of(c->a[0]); if (o) cx_free(o, 1); return 0; }
static uint32_t x_CxObjType(M68K *c) { Cx *o = cx_of(c->a[0]); return o ? (uint32_t)o->type : CX_INVALID; }
static uint32_t x_CxObjError(M68K *c) { Cx *o = cx_of(c->a[0]); return o ? (uint32_t)o->error : COERR_ISNULL; }
static uint32_t x_ClearCxObjError(M68K *c) { Cx *o = cx_of(c->a[0]); if (o) o->error = 0; return 0; }
static uint32_t x_SetCxObjPri(M68K *c) {
    Cx *o = cx_of(c->a[0]);
    if (!o) return 0;
    int old = o->pri;
    o->pri = (int8_t)c->d[0];
    return (uint32_t)old;
}

static void attach(Cx *head, Cx *o, Cx *after, int by_pri) {
    unlink_cx(o);
    o->parent = head;
    Cx **pp = &head->kids;
    if (by_pri) while (*pp && (*pp)->pri >= o->pri) pp = &(*pp)->next;          /* by priority */
    else if (after) { while (*pp && *pp != after) pp = &(*pp)->next; if (*pp) pp = &(*pp)->next; }
    else while (*pp) pp = &(*pp)->next;                                        /* at the end */
    o->next = *pp;
    *pp = o;
}
static uint32_t x_AttachCxObj(M68K *c) {
    Cx *h = cx_of(c->a[0]), *o = cx_of(c->a[1]);
    if (!h) return 0;
    if (!o) { h->error |= COERR_NULLATTACH; return 0; }
    attach(h, o, 0, 0);
    return 0;
}
static uint32_t x_EnqueueCxObj(M68K *c) {
    Cx *h = cx_of(c->a[0]), *o = cx_of(c->a[1]);
    if (!h) return 0;
    if (!o) { h->error |= COERR_NULLATTACH; return 0; }
    attach(h, o, 0, 1);
    return 0;
}
static uint32_t x_InsertCxObj(M68K *c) {
    Cx *h = cx_of(c->a[0]), *o = cx_of(c->a[1]), *p = cx_of(c->a[2]);
    if (!h) return 0;
    if (!o) { h->error |= COERR_NULLATTACH; return 0; }
    if (p) attach(h, o, p, 0);
    else { unlink_cx(o); o->parent = h; o->next = h->kids; h->kids = o; }
    return 0;
}
static uint32_t x_RemoveCxObj(M68K *c) { Cx *o = cx_of(c->a[0]); if (o && o->type != CX_BROKER) unlink_cx(o); return 0; }

static uint32_t x_SetTranslate(M68K *c) {
    Cx *o = cx_of(c->a[0]);
    if (!o) return 0;
    if (o->type != CX_TRANSLATE) { o->error |= COERR_BADTYPE; return 0; }
    o->events = c->a[1];
    return 0;
}
static uint32_t x_SetFilter(M68K *c) {
    Cx *o = cx_of(c->a[0]);
    if (!o) return 0;
    if (o->type != CX_FILTER) { o->error |= COERR_BADTYPE; return 0; }
    char d[256] = "";
    if (c->a[1]) str_get(c->a[1], d, sizeof d);
    if (c->a[1] && parse_ix(d, &o->ix) == 0) { o->ix_valid = 1; o->error &= ~COERR_BADFILTER; }
    else { o->ix_valid = 0; o->error |= COERR_BADFILTER; }
    return 0;
}
static uint32_t x_SetFilterIX(M68K *c) {
    Cx *o = cx_of(c->a[0]);
    if (!o) return 0;
    if (o->type != CX_FILTER) { o->error |= COERR_BADTYPE; return 0; }
    ix_read(c->a[1], &o->ix);
    o->ix_valid = 1;
    return 0;
}
static uint32_t x_ParseIX(M68K *c) {
    char d[256] = "";
    IX ix;
    if (c->a[0]) str_get(c->a[0], d, sizeof d);
    if (parse_ix(d, &ix)) return 0xFFFFFFFF;
    if (c->a[1]) ix_write(c->a[1], &ix);
    return 0;
}

static uint32_t x_CxMsgType(M68K *c) { return c->a[0] ? rd32(c->a[0] + 20) : 0; }
static uint32_t x_CxMsgData(M68K *c) { return c->a[0] ? rd32(c->a[0] + 28) : 0; }
static uint32_t x_CxMsgID(M68K *c) { return c->a[0] ? rd32(c->a[0] + 24) : 0; }

/* inside a custom object: send the event elsewhere, or drop it */
static uint32_t x_RouteCxMsg(M68K *c) { if (cur_flow && cur_flow->msg == c->a[0]) cur_flow->route = cx_of(c->a[1]); return 0; }
static uint32_t x_DivertCxMsg(M68K *c) {
    Cx *h = cx_of(c->a[1]);
    if (cur_flow && cur_flow->msg == c->a[0]) cur_flow->route = h ? h->kids : 0;
    return 0;
}
static uint32_t x_DisposeCxMsg(M68K *c) {
    if (cur_flow && cur_flow->msg == c->a[0]) cur_flow->disposed = 1;
    else if (c->a[0] && rd32(c->a[0] + MN_REPLYPORT) == cx_reply_port) exec_freemem(c->a[0], CXMSG_SIZE);
    return 0;
}

/* InvertKeyMap(ansi, event, keymap): the raw key that types it */
static uint32_t x_InvertKeyMap(M68K *c) {
    int q = 0, raw = keymap_raw_for((int)(c->d[0] & 0xFF), &q);
    if (raw < 0 || !c->a[0]) return 0;
    wr8(c->a[0] + 4, IECLASS_RAWKEY);
    wr16(c->a[0] + 6, (uint16_t)raw);
    wr16(c->a[0] + 8, (uint16_t)q);
    return DOSTRUE;
}
static uint32_t x_AddIEvents(M68K *c) { inject(c->a[0]); return 0; }
static uint32_t x_MatchIX(M68K *c) {
    IX ix;
    ix_read(c->a[1], &ix);
    return c->a[0] && match_ix(c->a[0], &ix) ? DOSTRUE : 0;
}

static const NativeEntry cx_natives[] = {
    { "CreateCxObj", x_CreateCxObj }, { "CxBroker", x_CxBroker }, { "ActivateCxObj", x_ActivateCxObj },
    { "DeleteCxObj", x_DeleteCxObj }, { "DeleteCxObjAll", x_DeleteCxObjAll }, { "CxObjType", x_CxObjType },
    { "CxObjError", x_CxObjError }, { "ClearCxObjError", x_ClearCxObjError }, { "SetCxObjPri", x_SetCxObjPri },
    { "AttachCxObj", x_AttachCxObj }, { "EnqueueCxObj", x_EnqueueCxObj }, { "InsertCxObj", x_InsertCxObj },
    { "RemoveCxObj", x_RemoveCxObj }, { "SetTranslate", x_SetTranslate }, { "SetFilter", x_SetFilter },
    { "SetFilterIX", x_SetFilterIX }, { "ParseIX", x_ParseIX }, { "CxMsgType", x_CxMsgType },
    { "CxMsgData", x_CxMsgData }, { "CxMsgID", x_CxMsgID }, { "DivertCxMsg", x_DivertCxMsg },
    { "RouteCxMsg", x_RouteCxMsg }, { "DisposeCxMsg", x_DisposeCxMsg }, { "InvertKeyMap", x_InvertKeyMap },
    { "AddIEvents", x_AddIEvents }, { "MatchIX", x_MatchIX },
    { 0, 0 }
};

/* for a controller (the Shell's EXCHANGE): the brokers, and commands to them */
int cx_broker_count(void) { int n = 0; for (Cx *b = brokers; b; b = b->next) n++; return n; }
const char *cx_broker_info(int i, const char **title, const char **descr, int *active) {
    Cx *b = brokers;
    while (b && i--) b = b->next;
    if (!b) return 0;
    if (title) *title = b->title;
    if (descr) *descr = b->descr;
    if (active) *active = b->active;
    return b->name;
}
int cx_broker_command(const char *name, uint32_t id) {
    for (Cx *b = brokers; b; b = b->next)
        if (!strcasecmp(b->name, name)) {
            if (id == 15) b->active = 0;                         /* CXCMD_DISABLE */
            if (id == 17) b->active = 1;                         /* CXCMD_ENABLE */
            send_command(b, id);
            return 1;
        }
    return 0;
}

void commodities_init(void) {
    cx_reply_port = exec_allocmem(MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    wr8(cx_reply_port + LN_TYPE, NT_MSGPORT);
    wr8(cx_reply_port + MP_FLAGS, PA_IGNORE);
    list_new(cx_reply_port + MP_MSGLIST);
    CxBase = lib_create("commodities.library", "commodities", LIB_SIZE, cx_natives, 40, 1, "commodities 40.1 (AmigaMac)");
    lib_add(CxBase);
}
