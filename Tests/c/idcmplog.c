/* IdcmpLog: opens a window and prints every IDCMP message (testing aid) */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <stdio.h>
int main(void) {
    struct Window *w = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 24, WA_Width, 300, WA_Height, 100,
        WA_Title, (ULONG)"IdcmpLog", WA_CloseGadget, TRUE, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
        WA_SizeGadget, TRUE, WA_MaxWidth, -1, WA_MaxHeight, -1, WA_Activate, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_NEWSIZE |
                  IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW | IDCMP_CHANGEWINDOW, TAG_DONE);
    if (!w) return 20;
    for (int done = 0; !done;) {
        WaitPort(w->UserPort);
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
            printf("class=%08lx code=%04x qual=%04x x=%d y=%d\n", m->Class, m->Code, m->Qualifier, m->MouseX, m->MouseY);
            if (m->Class == IDCMP_CLOSEWINDOW) done = 1;
            ReplyMsg((struct Message *)m);
        }
    }
    printf("fenetre %dx%d en %d,%d\n", w->Width, w->Height, w->LeftEdge, w->TopEdge);
    CloseWindow(w);
    return 0;
}
