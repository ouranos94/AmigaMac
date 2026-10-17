/* IconTest: icon.library round trip (Put/Get), tool types, defaults, drawing.
 * Built with: tools/vcc -o System/C/IconTest Tests/c/icontest.c -lamiga -lauto
 */
#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

static STRPTR tts[] = { "FILETYPE=text|ascii", "DONOTWAIT", "PUBSCREEN=Workbench", NULL };

int main(void)
{
    static const char *tn[] = { "?", "DISK", "DRAWER", "TOOL", "PROJECT", "GARBAGE" };
    struct DiskObject *d = GetDefDiskObject(WBTOOL);
    if (!d) { printf("GetDefDiskObject: echec\n"); return 20; }
    printf("defaut outil : type=%s %dx%d pile=%ld\n", tn[d->do_Type], (int)d->do_Gadget.Width, (int)d->do_Gadget.Height, d->do_StackSize);
    d->do_ToolTypes = (STRPTR *)tts;
    d->do_DefaultTool = "SYS:C/Ed";
    d->do_CurrentX = 33; d->do_CurrentY = 44;
    if (!PutDiskObject("RAM:essai", d)) printf("PutDiskObject: echec\n");
    d->do_ToolTypes = NULL; d->do_DefaultTool = NULL;
    FreeDiskObject(d);

    struct DiskObject *e = GetDiskObject("RAM:essai");
    if (!e) { printf("GetDiskObject: echec\n"); return 20; }
    printf("relu : type=%s pos=%ld,%ld outil=%s\n", tn[e->do_Type], e->do_CurrentX, e->do_CurrentY, e->do_DefaultTool ? (char *)e->do_DefaultTool : "-");
    for (int i = 0; e->do_ToolTypes && e->do_ToolTypes[i]; i++) printf("  tooltype %d: %s\n", i, e->do_ToolTypes[i]);
    STRPTR ft = FindToolType(e->do_ToolTypes, "filetype");
    printf("FindToolType(FILETYPE)=%s match(ascii)=%d match(c)=%d\n", ft ? (char *)ft : "-", (int)MatchToolValue(ft, "ascii") != 0, (int)MatchToolValue(ft, "c") != 0);
    STRPTR dw = FindToolType(e->do_ToolTypes, "DONOTWAIT");
    printf("DONOTWAIT %s, ABSENT %s\n", dw ? "present" : "absent", FindToolType(e->do_ToolTypes, "ABSENT") ? "present" : "absent");
    struct DiskObject *f = DupDiskObjectA(e, NULL);
    printf("dup : %s\n", f && f->do_ToolTypes && !strcmp(f->do_ToolTypes[2], "PUBSCREEN=Workbench") ? "ok" : "ECHEC");
    FreeDiskObject(f);
    FreeDiskObject(e);
    DeleteDiskObject("RAM:essai");
    printf("apres DeleteDiskObject : %s\n", GetDiskObject("RAM:essai") ? "ECHEC" : "plus d'icone");

    char buf[40];
    BumpRevision(buf, "Lettre"); printf("%s / ", buf);
    BumpRevision(buf, "copy of Lettre"); printf("%s / ", buf);
    BumpRevision(buf, "copy 2 of Lettre"); printf("%s\n", buf);

    const char *names[] = { "SYS:", "SYS:C", "SYS:C/Hello", "SYS:S/Startup-Sequence" };
    struct DiskObject *icons[4];
    for (int i = 0; i < 4; i++) {
        icons[i] = GetDiskObjectNew((STRPTR)names[i]);
        printf("GetDiskObjectNew(%s) = %s\n", names[i], icons[i] ? tn[icons[i]->do_Type] : "NULL");
    }
    struct Window *w = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_Width, 400, WA_Height, 90,
                                      WA_Title, (ULONG)"Icônes", WA_DragBar, TRUE, WA_CloseGadget, TRUE, TAG_DONE);
    if (w) {
        static const char *labels[] = { "Disque", "C", "Hello", "Startup" };
        struct DiskObject *trash = GetDefDiskObject(WBGARBAGE);
        for (int i = 0; i < 4; i++)
            DrawIconState(w->RPort, icons[i], (STRPTR)labels[i], 20 + i * 70, 20, i == 2 ? IDS_SELECTED : IDS_NORMAL, TAG_DONE);
        DrawIconState(w->RPort, trash, "Corbeille", 300, 20, IDS_NORMAL, TAG_DONE);
        Delay(100);
        CloseWindow(w);
        FreeDiskObject(trash);
    }
    for (int i = 0; i < 4; i++) FreeDiskObject(icons[i]);
    printf("IconTest fini\n");
    return 0;
}
