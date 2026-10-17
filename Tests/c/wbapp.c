/* WBApp: AppWindow, AppIcon and AppMenuItem from workbench.library.
 * Built with: tools/vcc -o System/C/WBApp Tests/c/wbapp.c -lamiga -lauto
 */
#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/icon.h>
#include <proto/wb.h>
#include <stdio.h>

int main(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct Window *win = OpenWindowTags(NULL, WA_Left, 300, WA_Top, 150, WA_Width, 300, WA_Height, 90,
        WA_Title, (ULONG)"Deposez des icones ici", WA_DragBar, TRUE, WA_CloseGadget, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW, TAG_DONE);
    struct DiskObject *dobj = GetDefDiskObject(WBPROJECT);
    struct AppWindow *aw = AddAppWindow(1, 100, win, port, NULL);
    struct AppIcon *ai = AddAppIcon(2, 200, "AppIcone", port, 0, dobj, NULL);
    struct AppMenuItem *am = AddAppMenuItem(3, 300, "Mon outil", port, NULL);
    printf("AppWindow %s, AppIcon %s, AppMenuItem %s\n", aw ? "ok" : "ECHEC", ai ? "ok" : "ECHEC", am ? "ok" : "ECHEC");
    printf("OpenWorkbenchObject(RAM:) = %ld\n", (long)OpenWorkbenchObject("RAM:", TAG_DONE));
    int got = 0;
    while (got < 3) {
        ULONG sigs = Wait((1L << port->mp_SigBit) | (1L << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C) break;
        struct IntuiMessage *im;
        int closed = 0;
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) { if (im->Class == IDCMP_CLOSEWINDOW) closed = 1; ReplyMsg((struct Message *)im); }
        if (closed) break;
        struct AppMessage *m;
        while ((m = (struct AppMessage *)GetMsg(port))) {
            printf("AppMessage type=%d id=%ld userdata=%ld args=%ld", (int)m->am_Type, m->am_ID, m->am_UserData, m->am_NumArgs);
            for (int i = 0; i < m->am_NumArgs; i++) {
                char dir[256] = "";
                NameFromLock(m->am_ArgList[i].wa_Lock, dir, sizeof dir);
                printf(" [%s + %s]", dir, m->am_ArgList[i].wa_Name);
            }
            printf("\n");
            ReplyMsg((struct Message *)m);
            got++;
        }
    }
    RemoveAppMenuItem(am);
    RemoveAppIcon(ai);
    RemoveAppWindow(aw);
    FreeDiskObject(dobj);
    CloseWindow(win);
    DeleteMsgPort(port);
    printf("WBApp termine\n");
    return 0;
}
