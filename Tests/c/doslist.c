/* DosList: walks the DOS device list the old way (DOSBase->dl_Root->rn_Info->di_DevInfo)
 * and the new way (LockDosList/NextDosEntry), printing each entry.
 * Built with: tools/vcc -o System/C/DosList Tests/c/doslist.c -lamiga -lauto
 */
#include <exec/types.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

extern struct DosLibrary *DOSBase;

static void show(struct DosList *d)
{
    UBYTE *b = BADDR(d->dol_Name);
    printf("  type %ld task %s name \"%.*s\"\n", (long)d->dol_Type, d->dol_Task ? "oui" : "non", b ? b[0] : 0, b ? (char *)b + 1 : "");
}

int main(void)
{
    struct RootNode *rn = DOSBase->dl_Root;
    struct DosInfo *di = BADDR(rn->rn_Info);
    struct DosList *d;
    printf("dl_Root %s, rn_Info %s\n", rn ? "ok" : "NULL", di ? "ok" : "NULL");
    for (d = BADDR(di->di_DevInfo); d; d = BADDR(d->dol_Next)) show(d);
    printf("LockDosList :\n");
    d = LockDosList(LDF_ALL | LDF_READ);
    while ((d = NextDosEntry(d, LDF_ALL))) show(d);
    UnLockDosList(LDF_ALL | LDF_READ);
    return 0;
}
