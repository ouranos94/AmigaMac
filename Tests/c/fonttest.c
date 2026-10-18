/* FontTest: diskfont.library - OpenDiskFont, AvailFonts, drawing with a disk font.
 * Built with: tools/vcc -o System/C/FontTest Tests/c/fonttest.c -lamiga -lauto */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <diskfont/diskfont.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/diskfont.h>
#include <stdio.h>

int main(void)
{
    struct TextAttr ta = { "amigamac.font", 16, 0, 0 };
    struct TextFont *f = OpenDiskFont(&ta);
    printf("OpenDiskFont(amigamac 16) : %s, taille %d, largeur %d, base %d\n", f ? "ok" : "ECHEC",
           f ? f->tf_YSize : 0, f ? f->tf_XSize : 0, f ? f->tf_Baseline : 0);
    struct TextFont *g = OpenFont(&ta);
    printf("OpenFont trouve la police en memoire : %s\n", g == f ? "oui" : "non");
    if (g) CloseFont(g);
    UBYTE *buf = AllocVec(4096, MEMF_CLEAR);
    if (AvailFonts(buf, 4096, AFF_MEMORY | AFF_DISK) == 0) {
        struct AvailFontsHeader *h = (struct AvailFontsHeader *)buf;
        struct AvailFonts *a = (struct AvailFonts *)(h + 1);
        for (int i = 0; i < h->afh_NumEntries; i++)
            printf("  %s %s %d\n", a[i].af_Type == AFF_DISK ? "disque " : "memoire", a[i].af_Attr.ta_Name, a[i].af_Attr.ta_YSize);
    }
    FreeVec(buf);
    struct Window *w = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 40, WA_Width, 420, WA_Height, 80,
                                      WA_Title, (ULONG)"Police disque", WA_DragBar, TRUE, TAG_DONE);
    if (w && f) {
        SetFont(w->RPort, f);
        SetAPen(w->RPort, 1);
        Move(w->RPort, 20, 40);
        Text(w->RPort, "Police amigamac 16 points", 25);
        Move(w->RPort, 20, 60);
        Text(w->RPort, "\xc9t\xe9 \xe0 l'\xe9" "cole", 13);
        Delay(100);
    }
    if (w) CloseWindow(w);
    if (f) CloseFont(f);
    return 0;
}
