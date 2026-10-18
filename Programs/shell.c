/* Shell: the icon on the System disk that opens a Shell window, as the Shell
 * icon of the Workbench disk does on AmigaOS. A 68k program: from the
 * Workbench or from a Shell it starts NEWSHELL and returns.
 * Built with: tools/vcc -o System/System/Shell Programs/shell.c -lamiga -lauto
 */
#include <exec/types.h>
#include <utility/tagitem.h>
#include <dos/dostags.h>
#include <proto/dos.h>

static const char version[] = "$VER: Shell 45.1 (17.10.2026) AmigaMac";

int main(void)
{
    struct TagItem tags[] = { { SYS_Asynch, TRUE }, { SYS_Input, 0 }, { SYS_Output, 0 }, { TAG_DONE, 0 } };
    return SystemTagList("NEWSHELL", tags) == -1 ? 20 : 0;
}
