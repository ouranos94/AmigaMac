/* ScreenDemo: a custom 32-colour lores screen with a backdrop window */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <stdio.h>

int main(void) {
    struct Screen *s = OpenScreenTags(NULL, SA_Width, 320, SA_Height, 256, SA_Depth, 5,
        SA_DisplayID, 0 /* LORES */, SA_Title, (ULONG)"ScreenDemo : écran 320x256, 32 couleurs", TAG_DONE);
    if (!s) { printf("OpenScreen a échoué\n"); return 20; }
    for (int i = 0; i < 32; i++)                      /* a blue-to-orange gradient */
        SetRGB4(&s->ViewPort, i, i / 2, (i * 3 / 4) & 15, 15 - i / 2);
    struct Window *w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)s, WA_Left, 0, WA_Top, s->BarHeight + 1,
        WA_Width, 320, WA_Height, 256 - s->BarHeight - 1, WA_Backdrop, TRUE, WA_Borderless, TRUE,
        WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_VANILLAKEY, TAG_DONE);
    if (!w) { CloseScreen(s); return 20; }
    struct RastPort *rp = w->RPort;
    for (int i = 0; i < 32; i++) {
        SetAPen(rp, i);
        RectFill(rp, i * 10, 0, i * 10 + 9, 120);
    }
    for (int i = 0; i < 40; i++) {
        SetAPen(rp, 1 + i % 31);
        Move(rp, 160, 190);
        Draw(rp, i * 8, 130);
    }
    SetAPen(rp, 31);
    SetBPen(rp, 0);
    SetDrMd(rp, JAM2);
    Move(rp, 64, 230);
    Text(rp, "Clic ou touche: fermer", 22);
    WaitPort(w->UserPort);
    struct IntuiMessage *m;
    while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) ReplyMsg((struct Message *)m);
    CloseWindow(w);
    CloseScreen(s);
    printf("ScreenDemo terminé\n");
    return 0;
}
