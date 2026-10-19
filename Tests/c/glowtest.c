/* GlowTest: colour icons through IconControlA() - a NewIcon and a GlowIcon
 * (AmigaOS 3.5 format) read, one image changed and written back, read again.
 * Built with: tools/vcc -o System/C/GlowTest Tests/c/glowtest.c -lamiga -lauto
 * Usage: GlowTest <icon without .info> [<icon written to>]
 */
#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/dos.h>
#include <stdio.h>

static void show(struct DiskObject *d)
{
    LONG w = 0, h = 0, n1 = 0, n2 = 0, t1 = 0, mapped = 0, real2 = 0, isnew = 0;
    UBYTE *data = NULL;
    struct ColorRegister *pal = NULL;
    ULONG done = IconControl(d, ICONCTRLA_GetWidth, &w, ICONCTRLA_GetHeight, &h,
                             ICONCTRLA_GetPaletteSize1, &n1, ICONCTRLA_GetPaletteSize2, &n2,
                             ICONCTRLA_GetTransparentColor1, &t1, ICONCTRLA_IsPaletteMapped, &mapped,
                             ICONCTRLA_HasRealImage2, &real2, ICONCTRLA_IsNewIcon, &isnew,
                             ICONCTRLA_GetImageData1, &data, ICONCTRLA_GetPalette1, &pal, TAG_DONE);
    printf("  %lu tags : %ldx%ld, couleurs %ld/%ld, transparente %ld, couleur %s, image 2 %s, NewIcon %s\n",
           done, w, h, n1, n2, t1, mapped ? "oui" : "non", real2 ? "oui" : "non", isnew ? "oui" : "non");
    if (data && pal && w > 0 && h > 0) {
        UBYTE p = data[(h / 2) * w + w / 2];
        printf("  pixel central : couleur %u = #%02x%02x%02x\n", p, pal[p].red, pal[p].green, pal[p].blue);
    }
}

int main(int argc, char **argv)
{
    struct DiskObject *d;
    if (argc < 2) { printf("GlowTest icone [copie]\n"); return 5; }
    if (!(d = GetDiskObject(argv[1]))) { printf("pas d'icone %s\n", argv[1]); return 10; }
    printf("%s :\n", argv[1]);
    show(d);
    if (argc > 2) {
        LONG w = 0, h = 0, n = 0;
        UBYTE *data = NULL;
        IconControl(d, ICONCTRLA_GetWidth, &w, ICONCTRLA_GetHeight, &h, ICONCTRLA_GetPaletteSize1, &n,
                    ICONCTRLA_GetImageData1, &data, TAG_DONE);
        if (data && n > 1) {
            static UBYTE copy[256 * 256];
            LONG i;
            for (i = 0; i < w * h; i++) copy[i] = (i % w < w / 2) ? data[i] : (UBYTE)(n - 1);   /* right half: last colour */
            IconControl(d, ICONCTRLA_SetImageData1, copy, TAG_DONE);
        }
        if (!PutDiskObject(argv[2], d)) printf("PutDiskObject a echoue\n");
        FreeDiskObject(d);
        if (!(d = GetDiskObject(argv[2]))) { printf("relecture impossible\n"); return 10; }
        printf("%s (copie modifiee) :\n", argv[2]);
        show(d);
    }
    FreeDiskObject(d);
    return 0;
}
