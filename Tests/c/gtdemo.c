/* GTDemo: a GadTools program in the usual style - CreateContext/CreateGadget
 * for every kind, CreateMenus/LayoutMenus, GT_GetIMsg loop. Prints what it gets.
 * Built with: tools/vcc -o System/C/GTDemo Tests/c/gtdemo.c -lamiga -lauto
 */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include <string.h>

enum { ID_BUTTON = 1, ID_CHECK, ID_CYCLE, ID_MX, ID_SLIDER, ID_SCROLL, ID_STRING, ID_INT, ID_LIST, ID_PAL, ID_TEXT, ID_NUM };

static STRPTR cycle_labels[] = { "Rouge", "Vert", "Bleu", NULL };
static STRPTR mx_labels[] = { "Petit", "Moyen", "Grand", NULL };
static char *names[] = { "Workbench", "Shell", "Prefs", "Utilities", "Tools", "System", "Devs", "Libs", "Fonts", "C" };

static struct NewMenu newmenu[] = {
    { NM_TITLE, "Projet", 0, 0, 0, 0 },
    { NM_ITEM, "Nouveau", "N", 0, 0, (APTR)101 },
    { NM_ITEM, "Ouvrir...", "O", 0, 0, (APTR)102 },
    { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM, "Exporter", 0, 0, 0, 0 },
    { NM_SUB, "Texte", 0, 0, 0, (APTR)111 },
    { NM_SUB, "Image", 0, 0, 0, (APTR)112 },
    { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM, "Quitter", "Q", 0, 0, (APTR)199 },
    { NM_TITLE, "Options", 0, 0, 0, 0 },
    { NM_ITEM, "Sons", 0, CHECKIT | MENUTOGGLE | CHECKED, 0, (APTR)201 },
    { NM_ITEM, "D\xe9sactiv\xe9", 0, NM_ITEMDISABLED, 0, (APTR)202 },
    { NM_END, 0, 0, 0, 0, 0 }
};

int main(void)
{
    struct Screen *scr = LockPubScreen(NULL);
    if (!scr) return 20;
    APTR vi = GetVisualInfo(scr, TAG_DONE);
    struct Gadget *glist = NULL, *g, *gads[16];
    struct List lvlist;
    struct Node nodes[10];
    NewList(&lvlist);
    for (int i = 0; i < 10; i++) { nodes[i].ln_Name = names[i]; AddTail(&lvlist, &nodes[i]); }

    g = CreateContext(&glist);
    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    int top = scr->WBorTop + scr->Font->ta_YSize + 1;

    ng.ng_LeftEdge = 100; ng.ng_TopEdge = top + 6; ng.ng_Width = 120; ng.ng_Height = 14;
    ng.ng_GadgetText = "_Bouton"; ng.ng_GadgetID = ID_BUTTON;
    gads[ID_BUTTON] = g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);

    ng.ng_TopEdge += 18; ng.ng_Width = 26; ng.ng_Height = 11;
    ng.ng_GadgetText = "Case"; ng.ng_GadgetID = ID_CHECK; ng.ng_Flags = PLACETEXT_LEFT;
    gads[ID_CHECK] = g = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, TRUE, TAG_DONE);

    ng.ng_TopEdge += 16; ng.ng_Width = 120; ng.ng_Height = 14;
    ng.ng_GadgetText = "Couleur"; ng.ng_GadgetID = ID_CYCLE;
    gads[ID_CYCLE] = g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)cycle_labels, TAG_DONE);

    ng.ng_TopEdge += 20; ng.ng_Width = 17; ng.ng_Height = 9;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = ID_MX; ng.ng_Flags = PLACETEXT_RIGHT;
    gads[ID_MX] = g = CreateGadget(MX_KIND, g, &ng, GTMX_Labels, (ULONG)mx_labels, GTMX_Active, 1, GTMX_Spacing, 3, TAG_DONE);

    ng.ng_TopEdge += 40; ng.ng_Width = 120; ng.ng_Height = 11; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetText = "Niveau"; ng.ng_GadgetID = ID_SLIDER;
    gads[ID_SLIDER] = g = CreateGadget(SLIDER_KIND, g, &ng, GTSL_Min, 0, GTSL_Max, 20, GTSL_Level, 5,
                                       GTSL_LevelFormat, (ULONG)"%2ld", GTSL_MaxLevelLen, 2, GTSL_LevelPlace, PLACETEXT_RIGHT, TAG_DONE);

    ng.ng_TopEdge += 16; ng.ng_GadgetText = "D\xe9" "file"; ng.ng_GadgetID = ID_SCROLL;
    gads[ID_SCROLL] = g = CreateGadget(SCROLLER_KIND, g, &ng, GTSC_Total, 100, GTSC_Visible, 10, GTSC_Top, 0, GTSC_Arrows, 16, TAG_DONE);

    ng.ng_TopEdge += 16; ng.ng_Height = 14; ng.ng_GadgetText = "Nom"; ng.ng_GadgetID = ID_STRING;
    gads[ID_STRING] = g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)"Amiga", GTST_MaxChars, 40, TAG_DONE);

    ng.ng_TopEdge += 18; ng.ng_GadgetText = "Nombre"; ng.ng_GadgetID = ID_INT;
    gads[ID_INT] = g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, 1200, TAG_DONE);

    ng.ng_TopEdge += 18; ng.ng_GadgetText = "Texte"; ng.ng_GadgetID = ID_TEXT;
    gads[ID_TEXT] = g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"lecture seule", GTTX_Border, TRUE, TAG_DONE);

    ng.ng_TopEdge += 18; ng.ng_GadgetText = "Compteur"; ng.ng_GadgetID = ID_NUM;
    gads[ID_NUM] = g = CreateGadget(NUMBER_KIND, g, &ng, GTNM_Number, 0, GTNM_Border, TRUE, TAG_DONE);

    ng.ng_LeftEdge = 260; ng.ng_TopEdge = top + 20; ng.ng_Width = 140; ng.ng_Height = 84;
    ng.ng_GadgetText = "Tiroirs"; ng.ng_GadgetID = ID_LIST; ng.ng_Flags = PLACETEXT_ABOVE;
    gads[ID_LIST] = g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&lvlist, GTLV_ShowSelected, 0, TAG_DONE);

    ng.ng_TopEdge += 104; ng.ng_Height = 30; ng.ng_GadgetText = "Palette"; ng.ng_GadgetID = ID_PAL;
    gads[ID_PAL] = g = CreateGadget(PALETTE_KIND, g, &ng, GTPA_Depth, 3, GTPA_Color, 2, GTPA_IndicatorWidth, 20, TAG_DONE);

    if (!g) { printf("CreateGadget a echoue\n"); return 20; }

    struct Menu *menu = CreateMenus(newmenu, TAG_DONE);
    if (!menu || !LayoutMenus(menu, vi, GTMN_NewLookMenus, TRUE, TAG_DONE)) { printf("menus : echec\n"); return 20; }

    struct Window *win = OpenWindowTags(NULL,
        WA_Left, 20, WA_Top, 14, WA_Width, 430, WA_Height, 236,
        WA_Title, (ULONG)"AmigaMac : GadTools", WA_PubScreen, (ULONG)scr,
        WA_Gadgets, (ULONG)glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MENUPICK | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY |
                  BUTTONIDCMP | CHECKBOXIDCMP | CYCLEIDCMP | MXIDCMP | SLIDERIDCMP | SCROLLERIDCMP |
                  STRINGIDCMP | LISTVIEWIDCMP | PALETTEIDCMP,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE, TAG_DONE);
    if (!win) { printf("OpenWindow a echoue\n"); return 20; }
    UnlockPubScreen(NULL, scr);
    SetMenuStrip(win, menu);
    GT_RefreshWindow(win, NULL);
    DrawBevelBox(win->RPort, 250, top + 4, 160, 2, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_DONE);
    printf("GTDemo pret\n");

    BOOL done = FALSE;
    LONG clicks = 0, moves = 0;
    while (!done) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        while ((m = GT_GetIMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *ia = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            switch (cls) {
            case IDCMP_CLOSEWINDOW: done = TRUE; break;
            case IDCMP_REFRESHWINDOW: GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
            case IDCMP_VANILLAKEY: if (code == 'q') done = TRUE; break;
            case IDCMP_MOUSEMOVE: moves++; break;
            case IDCMP_GADGETDOWN:
                printf("GADGETDOWN id=%d code=%d\n", (int)ia->GadgetID, (int)code);
                break;
            case IDCMP_GADGETUP: {
                printf("GADGETUP id=%d code=%d", (int)ia->GadgetID, (int)code);
                if (ia->GadgetID == ID_STRING) {
                    STRPTR s = NULL;
                    GT_GetGadgetAttrs(ia, win, NULL, GTST_String, (ULONG)&s, TAG_DONE);
                    printf(" texte=%s", s ? (char *)s : "?");
                }
                if (ia->GadgetID == ID_INT) {
                    LONG n = 0;
                    GT_GetGadgetAttrs(ia, win, NULL, GTIN_Number, (ULONG)&n, TAG_DONE);
                    printf(" nombre=%ld", n);
                }
                if (ia->GadgetID == ID_LIST && code < 10) printf(" (%s)", names[code]);
                printf("\n");
                if (ia->GadgetID == ID_BUTTON) {
                    clicks++;
                    GT_SetGadgetAttrs(gads[ID_NUM], win, NULL, GTNM_Number, clicks, TAG_DONE);
                    GT_SetGadgetAttrs(gads[ID_SLIDER], win, NULL, GTSL_Level, clicks * 3, TAG_DONE);
                }
                if (ia->GadgetID == ID_CHECK)
                    GT_SetGadgetAttrs(gads[ID_CYCLE], win, NULL, GA_Disabled, code ? FALSE : TRUE, TAG_DONE);
                break;
            }
            case IDCMP_MENUPICK:
                while (code != MENUNULL) {
                    struct MenuItem *it = ItemAddress(menu, code);
                    if (!it) break;
                    ULONG ud = (ULONG)GTMENUITEM_USERDATA(it);
                    printf("MENUPICK menu=%d item=%d sub=%d userdata=%lu%s\n", (int)MENUNUM(code), (int)ITEMNUM(code), (int)SUBNUM(code), ud,
                           (it->Flags & CHECKIT) ? ((it->Flags & CHECKED) ? " [coche]" : " [decoche]") : "");
                    if (ud == 199) done = TRUE;
                    code = it->NextSelect;
                }
                break;
            }
        }
    }
    LONG lvl = -1, sel = -1, col = -1;
    GT_GetGadgetAttrs(gads[ID_SLIDER], win, NULL, GTSL_Level, (ULONG)&lvl, TAG_DONE);
    GT_GetGadgetAttrs(gads[ID_LIST], win, NULL, GTLV_Selected, (ULONG)&sel, TAG_DONE);
    GT_GetGadgetAttrs(gads[ID_PAL], win, NULL, GTPA_Color, (ULONG)&col, TAG_DONE);
    printf("fin : niveau=%ld liste=%ld couleur=%ld mouvements=%s\n", lvl, sel, col, moves ? "oui" : "non");
    ClearMenuStrip(win);
    CloseWindow(win);
    FreeMenus(menu);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    printf("GTDemo termine\n");
    return 0;
}
