/* ScreenMode: the Workbench screen mode preferences editor, as in AmigaOS.
 * Lists the display database, writes ENV:Sys/screenmode.prefs ("Utiliser")
 * and ENVARC:Sys/screenmode.prefs too ("Sauver"); the system then reopens the
 * Workbench screen in the new mode. A 68k program like the original.
 * Built with: tools/vcc -o System/Prefs/ScreenMode Prefs/screenmode.c -lamiga -lauto
 */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <libraries/gadtools.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <string.h>

#define MAXMODES 32
enum { ID_LIST = 1, ID_WIDTH, ID_HEIGHT, ID_COLORS, ID_SAVE, ID_USE, ID_CANCEL };

static struct Node nodes[MAXMODES];
static char names[MAXMODES][DISPLAYNAMELEN + 1];
static ULONG ids[MAXMODES];
static UWORD widths[MAXMODES], heights[MAXMODES];
static int nmodes;
static STRPTR colors[] = { "2", "4", "8", "16", "32", "64", "128", "256", NULL };
static const char version[] = "$VER: ScreenMode 45.1 (17.10.2026) AmigaMac";

static void scan_modes(struct List *list)
{
    ULONG id = INVALID_ID;
    NewList(list);
    while ((id = NextDisplayInfo(id)) != INVALID_ID && nmodes < MAXMODES) {
        struct NameInfo ni;
        struct DimensionInfo di;
        if (ModeNotAvailable(id)) continue;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof ni, DTAG_NAME, id)) continue;
        if (!GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof di, DTAG_DIMS, id)) continue;
        strncpy(names[nmodes], (char *)ni.Name, DISPLAYNAMELEN);
        ids[nmodes] = id;
        widths[nmodes] = di.Nominal.MaxX - di.Nominal.MinX + 1;
        heights[nmodes] = di.Nominal.MaxY - di.Nominal.MinY + 1;
        nodes[nmodes].ln_Name = names[nmodes];
        AddTail(list, &nodes[nmodes]);
        nmodes++;
    }
}

/* FORM PREF { PRHD, SCRM } */
static BOOL read_prefs(const char *path, ULONG *id, UWORD *depth)
{
    UBYTE b[128];
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n, p;
    if (!fh) return FALSE;
    n = Read(fh, b, sizeof b);
    Close(fh);
    if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "PREF", 4)) return FALSE;
    for (p = 12; p + 8 <= n;) {
        ULONG sz = (ULONG)b[p + 4] << 24 | (ULONG)b[p + 5] << 16 | (ULONG)b[p + 6] << 8 | b[p + 7];
        if (!memcmp(b + p, "SCRM", 4) && p + 8 + 28 <= n) {
            UBYTE *d = b + p + 8;
            *id = (ULONG)d[16] << 24 | (ULONG)d[17] << 16 | (ULONG)d[18] << 8 | d[19];
            *depth = d[24] << 8 | d[25];
            return TRUE;
        }
        p += 8 + ((sz + 1) & ~1);
    }
    return FALSE;
}

static void put32(UBYTE *p, ULONG v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(UBYTE *p, UWORD v) { p[0] = v >> 8; p[1] = v; }

static BOOL write_prefs(const char *dir, ULONG id, UWORD w, UWORD h, UWORD depth)
{
    UBYTE b[62];
    char path[64];
    BPTR lock, fh;
    BOOL ok;
    memset(b, 0, sizeof b);
    memcpy(b, "FORM", 4); put32(b + 4, 4 + 8 + 6 + 8 + 28);
    memcpy(b + 8, "PREF", 4);
    memcpy(b + 12, "PRHD", 4); put32(b + 16, 6);              /* version 0, type 0, flags 0 */
    memcpy(b + 26, "SCRM", 4); put32(b + 30, 28);
    put32(b + 34 + 16, id);
    put16(b + 34 + 20, w);
    put16(b + 34 + 22, h);
    put16(b + 34 + 24, depth);
    put16(b + 34 + 26, 0);                                    /* smp_Control */
    if ((lock = Lock((STRPTR)dir, SHARED_LOCK))) UnLock(lock);
    else if ((lock = CreateDir((STRPTR)dir))) UnLock(lock);
    sprintf(path, "%s/screenmode.prefs", dir);
    if (!(fh = Open(path, MODE_NEWFILE))) return FALSE;
    ok = Write(fh, b, 62) == 62;
    Close(fh);
    return ok;
}

int main(void)
{
    struct Screen *scr = LockPubScreen(NULL);
    struct List list;
    struct Gadget *glist = NULL, *g, *gw, *gh;
    struct NewGadget ng;
    struct Window *win;
    APTR vi;
    ULONG cur_id = 0x50031000;                     /* the default: AmigaMac 1280x720 */
    UWORD cur_depth = 8;
    int sel = 0, depth_idx, top, i, W = 400, H = 160, action = 0;

    if (!scr) return 20;
    vi = GetVisualInfo(scr, TAG_DONE);
    scan_modes(&list);
    if (!read_prefs("ENV:Sys/screenmode.prefs", &cur_id, &cur_depth))
        read_prefs("ENVARC:Sys/screenmode.prefs", &cur_id, &cur_depth);
    for (i = 0; i < nmodes; i++) if (ids[i] == cur_id) sel = i;
    depth_idx = cur_depth >= 1 && cur_depth <= 8 ? cur_depth - 1 : 2;

    top = scr->WBorTop + scr->Font->ta_YSize + 1;
    g = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;

    ng.ng_LeftEdge = 12; ng.ng_TopEdge = top + 18; ng.ng_Width = 220; ng.ng_Height = 88;
    ng.ng_GadgetText = "Mode d'\xe9""cran"; ng.ng_GadgetID = ID_LIST; ng.ng_Flags = PLACETEXT_ABOVE;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, &list, GTLV_ShowSelected, NULL,
                     GTLV_Selected, sel, GTLV_MakeVisible, sel, TAG_DONE);

    ng.ng_LeftEdge = 320; ng.ng_TopEdge = top + 18; ng.ng_Width = 64; ng.ng_Height = 12;
    ng.ng_GadgetText = "Largeur"; ng.ng_GadgetID = ID_WIDTH; ng.ng_Flags = PLACETEXT_LEFT;
    gw = g = CreateGadget(NUMBER_KIND, g, &ng, GTNM_Number, nmodes ? widths[sel] : 0, GTNM_Border, TRUE, TAG_DONE);
    ng.ng_TopEdge += 16; ng.ng_GadgetText = "Hauteur"; ng.ng_GadgetID = ID_HEIGHT;
    gh = g = CreateGadget(NUMBER_KIND, g, &ng, GTNM_Number, nmodes ? heights[sel] : 0, GTNM_Border, TRUE, TAG_DONE);
    ng.ng_TopEdge += 20; ng.ng_Height = 14; ng.ng_GadgetText = "Couleurs"; ng.ng_GadgetID = ID_COLORS;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, colors, GTCY_Active, depth_idx, TAG_DONE);

    ng.ng_TopEdge = top + H - top - 22; ng.ng_Width = 100; ng.ng_Height = 14; ng.ng_Flags = PLACETEXT_IN;
    ng.ng_LeftEdge = 12; ng.ng_GadgetText = "_Sauver"; ng.ng_GadgetID = ID_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    ng.ng_LeftEdge = (W - 100) / 2; ng.ng_GadgetText = "_Utiliser"; ng.ng_GadgetID = ID_USE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    ng.ng_LeftEdge = W - 112; ng.ng_GadgetText = "_Annuler"; ng.ng_GadgetID = ID_CANCEL;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);

    win = g ? OpenWindowTags(NULL, WA_Left, (scr->Width - W) / 2, WA_Top, (scr->Height - H) / 2 > top ? (scr->Height - H) / 2 : top,
                             WA_Width, W, WA_Height, H, WA_Title, "Mode d'\xe9""cran", WA_PubScreen, scr,
                             WA_Gadgets, glist, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                             WA_Activate, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP | CYCLEIDCMP | IDCMP_VANILLAKEY,
                             TAG_DONE) : NULL;
    UnlockPubScreen(NULL, scr);
    if (!win) { FreeGadgets(glist); FreeVisualInfo(vi); return 20; }
    GT_RefreshWindow(win, NULL);

    while (!action) {
        struct IntuiMessage *m;
        WaitPort(win->UserPort);
        while (!action && (m = GT_GetIMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *gad = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            switch (cls) {
            case IDCMP_CLOSEWINDOW: action = ID_CANCEL; break;
            case IDCMP_REFRESHWINDOW: GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
            case IDCMP_VANILLAKEY:
                if (code == 's' || code == 'S') action = ID_SAVE;
                else if (code == 'u' || code == 'U') action = ID_USE;
                else if (code == 'a' || code == 'A' || code == 27) action = ID_CANCEL;
                break;
            case IDCMP_GADGETUP:
                switch (gad->GadgetID) {
                case ID_LIST:
                    sel = code;
                    GT_SetGadgetAttrs(gw, win, NULL, GTNM_Number, widths[sel], TAG_DONE);
                    GT_SetGadgetAttrs(gh, win, NULL, GTNM_Number, heights[sel], TAG_DONE);
                    break;
                case ID_COLORS: depth_idx = code; break;
                default: action = gad->GadgetID; break;
                }
                break;
            }
        }
    }
    /* the window goes first, so that the Workbench screen can be reopened */
    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    if (action != ID_CANCEL && nmodes) {
        if (action == ID_SAVE && !write_prefs("ENVARC:Sys", ids[sel], widths[sel], heights[sel], depth_idx + 1))
            PrintFault(IoErr(), "ScreenMode");
        if (!write_prefs("ENV:Sys", ids[sel], widths[sel], heights[sel], depth_idx + 1))
            PrintFault(IoErr(), "ScreenMode");
    }
    return 0;
}
