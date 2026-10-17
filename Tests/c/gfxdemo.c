/* GfxDemo: an ordinary Intuition program, compiled for the Amiga with vbcc.
 * Window with drawing, a button, a string gadget, menus, requesters and the
 * usual IDCMP loop. Built with: tools/vcc -o System/C/GfxDemo Tests/c/gfxdemo.c -lamiga -lauto
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/gfxmacros.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <string.h>
#include <stdio.h>

static struct Window *win;
static char strbuf[64] = "Tapez ici", undobuf[64];

/* --- gadgets: a button and a string field -------------------------------- */
static SHORT btn_xy[] = { 0, 13, 0, 0, 89, 0, 89, 13, 0, 13 };
static struct Border btn_border = { 0, 0, 1, 0, JAM1, 5, btn_xy, NULL };
static struct IntuiText btn_text = { 1, 0, JAM1, 13, 3, NULL, (UBYTE *)"Bonjour", NULL };
static struct StringInfo str_info = { (UBYTE *)strbuf, (UBYTE *)undobuf, 0, 64 };
static SHORT str_xy[] = { -2, -2, 161, -2, 161, 9, -2, 9, -2, -2 };
static struct Border str_border = { 0, 0, 1, 0, JAM1, 5, str_xy, NULL };
static struct Gadget str_gad = {
    NULL, 130, -20, 160, 8, GFLG_GADGHCOMP | GFLG_RELBOTTOM, GACT_RELVERIFY, GTYP_STRGADGET,
    &str_border, NULL, NULL, 0, &str_info, 2, NULL };
static struct Gadget btn_gad = {
    &str_gad, 20, -24, 90, 14, GFLG_GADGHCOMP | GFLG_RELBOTTOM, GACT_RELVERIFY, GTYP_BOOLGADGET,
    &btn_border, NULL, &btn_text, 0, NULL, 1, NULL };

/* --- menus ---------------------------------------------------------------- */
static struct IntuiText t_about = { 0, 1, JAM1, 2, 1, NULL, (UBYTE *)"A propos...", NULL };
static struct IntuiText t_clear = { 0, 1, JAM1, 2, 1, NULL, (UBYTE *)"Effacer", NULL };
static struct IntuiText t_quit = { 0, 1, JAM1, 2, 1, NULL, (UBYTE *)"Quitter", NULL };
static struct MenuItem i_quit = { NULL, 0, 20, 120, 10, ITEMTEXT | ITEMENABLED | HIGHCOMP | COMMSEQ, 0, &t_quit, NULL, 'Q', NULL, 0 };
static struct MenuItem i_clear = { &i_quit, 0, 10, 120, 10, ITEMTEXT | ITEMENABLED | HIGHCOMP | COMMSEQ, 0, &t_clear, NULL, 'E', NULL, 0 };
static struct MenuItem i_about = { &i_clear, 0, 0, 120, 10, ITEMTEXT | ITEMENABLED | HIGHCOMP, 0, &t_about, NULL, 0, NULL, 0 };
static struct Menu menu = { NULL, 4, 0, 80, 10, MENUENABLED, (BYTE *)"Projet", &i_about };

static struct EasyStruct about = {
    sizeof(struct EasyStruct), 0, (UBYTE *)"A propos",
    (UBYTE *)"GfxDemo pour AmigaMac\nAmigaOS reecrit pour macOS\n%ld clics jusqu'ici",
    (UBYTE *)"Super|Fermer" };

static long clicks;

static void draw(void)
{
    struct RastPort *rp = win->RPort;
    WORD l = win->BorderLeft, t = win->BorderTop;
    WORD r = win->Width - win->BorderRight - 1, b = win->Height - win->BorderBottom - 1;
    SetAPen(rp, 0);
    RectFill(rp, l, t, r, b - 28);
    for (int i = 1; i < 8; i++) {                     /* colour swatches */
        SetAPen(rp, i);
        RectFill(rp, l + 8 + (i - 1) * 22, t + 6, l + 26 + (i - 1) * 22, t + 20);
    }
    SetAPen(rp, 1);
    for (int i = 0; i <= 10; i++) {                   /* a fan of lines */
        Move(rp, l + 8, b - 32);
        Draw(rp, l + 8 + i * 14, t + 28);
    }
    SetAPen(rp, 3);
    DrawEllipse(rp, r - 70, t + 50, 50, 22);
    SetAPen(rp, 2);
    Move(rp, l + 180, t + 40);
    Text(rp, "graphics.library", 16);
    SetAPen(rp, 1);
    Move(rp, l + 180, t + 52);
    Text(rp, "Clic: point, touche: lettre", 27);
}

int main(void)
{
    win = OpenWindowTags(NULL,
        WA_Left, 40, WA_Top, 24, WA_Width, 420, WA_Height, 170,
        WA_Title, (ULONG)"AmigaMac : GfxDemo",
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_VANILLAKEY | IDCMP_MENUPICK |
                  IDCMP_NEWSIZE | IDCMP_GADGETUP,
        WA_Gadgets, (ULONG)&btn_gad,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE,
        WA_MinWidth, 300, WA_MinHeight, 120, WA_MaxWidth, -1, WA_MaxHeight, -1,
        WA_Activate, TRUE, TAG_DONE);
    if (!win) { printf("OpenWindow a échoué\n"); return 20; }
    SetMenuStrip(win, &menu);
    draw();
    RefreshGList(&btn_gad, win, NULL, -1);

    BOOL done = FALSE;
    WORD tx = 0;
    while (!done) {
        Wait(1L << win->UserPort->mp_SigBit);
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            WORD mx = m->MouseX, my = m->MouseY;
            APTR ia = m->IAddress;
            ReplyMsg((struct Message *)m);
            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                done = TRUE;
                break;
            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTDOWN) {
                    clicks++;
                    SetAPen(win->RPort, 3);
                    RectFill(win->RPort, mx - 2, my - 2, mx + 2, my + 2);
                }
                break;
            case IDCMP_VANILLAKEY: {
                char ch = (char)code;
                SetAPen(win->RPort, 1);
                Move(win->RPort, win->BorderLeft + 180 + tx * 8, win->BorderTop + 70);
                Text(win->RPort, &ch, 1);
                tx = (tx + 1) % 24;
                break;
            }
            case IDCMP_NEWSIZE:
                draw();
                RefreshGList(&btn_gad, win, NULL, -1);
                break;
            case IDCMP_GADGETUP:
                if (((struct Gadget *)ia)->GadgetID == 1)
                    EasyRequest(win, &about, NULL, clicks);
                else
                    printf("Texte saisi : %s\n", strbuf);
                break;
            case IDCMP_MENUPICK:
                if (MENUNUM(code) == 0) {
                    switch (ITEMNUM(code)) {
                    case 0: EasyRequest(win, &about, NULL, clicks); break;
                    case 1: draw(); break;
                    case 2: done = TRUE; break;
                    }
                }
                break;
            }
        }
    }
    ClearMenuStrip(win);
    CloseWindow(win);
    printf("GfxDemo terminé (%ld clics)\n", clicks);
    return 0;
}
