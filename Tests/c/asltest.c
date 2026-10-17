/* AslTest: file, font and screen mode requesters.
 * Built with: tools/vcc -o System/C/AslTest Tests/c/asltest.c -lamiga -lauto
 */
#include <exec/types.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct FileRequester *fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"AslTest : ouvrir", ASLFR_InitialDrawer, (ULONG)"SYS:C",
        ASLFR_InitialFile, (ULONG)"Hello", ASLFR_DoPatterns, TRUE, TAG_DONE);
    if (!fr) { printf("AllocAslRequest: echec\n"); return 20; }
    if (AslRequest(fr, NULL)) {
        char path[256];
        strcpy(path, fr->fr_Drawer);
        AddPart(path, fr->fr_File, sizeof path);
        printf("fichier : drawer=%s file=%s -> %s (args=%ld)\n", fr->fr_Drawer, fr->fr_File, path, fr->fr_NumArgs);
    } else printf("fichier : annule\n");
    FreeAslRequest(fr);

    struct FontRequester *fo = AllocAslRequest(ASL_FontRequest, NULL);
    if (fo && AslRequest(fo, NULL)) printf("police : %s %d\n", fo->fo_Attr.ta_Name, (int)fo->fo_Attr.ta_YSize);
    else printf("police : annule\n");
    FreeAslRequest(fo);

    struct ScreenModeRequester *sm = AllocAslRequest(ASL_ScreenModeRequest, NULL);
    if (sm && AslRequest(sm, NULL)) printf("mode : %08lx %ldx%ld profondeur %d\n", sm->sm_DisplayID, sm->sm_DisplayWidth, sm->sm_DisplayHeight, (int)sm->sm_DisplayDepth);
    else printf("mode : annule\n");
    FreeAslRequest(sm);
    return 0;
}
