/* CgxTest: true colour through cybergraphics.library - screen attributes,
 * a rainbow drawn with WritePixelArray, a pixel read back, LockBitMapTagList.
 * Built with: tools/vcc -o System/C/CgxTest Tests/c/cgxtest.c -lamiga -lauto
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <inline/cybergraphics_protos.h>
#include <stdio.h>

struct Library *CyberGfxBase;
#define CYBRMATTR_PIXFMT 0x80000004
#define CYBRMATTR_DEPTH 0x80000007
#define CYBRMATTR_ISCYBERGFX 0x80000008
#define CYBRBIDTG_NOMINALWIDTH 0x80050002
#define CYBRBIDTG_NOMINALHEIGHT 0x80050003
#define LBMI_BYTESPERROW 0x84001006
#define LBMI_BASEADDRESS 0x84001007
#define RECTFMT_RGB 0

static UBYTE pix[256 * 128 * 3];

int main(void)
{
    struct Screen *scr;
    struct Window *win;
    ULONG id, bpr = 0, base = 0, argb;
    APTR lock;
    int x, y;
    if (!(CyberGfxBase = OpenLibrary("cybergraphics.library", 41))) { printf("pas de cybergraphics.library\n"); return 20; }
    scr = LockPubScreen(NULL);
    printf("Workbench : cybergfx %ld, profondeur %ld, format %ld\n", (long)(GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX) != 0),
           (long)GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_DEPTH), (long)GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT));
    id = BestCModeIDTags(CYBRBIDTG_NOMINALWIDTH, 1000, CYBRBIDTG_NOMINALHEIGHT, 700, TAG_DONE);
    printf("BestCModeID(1000x700) = %08lx, IsCyberModeID %ld\n", id, (long)(IsCyberModeID(id) != 0));
    win = OpenWindowTags(NULL, WA_Left, 700, WA_Top, 420, WA_InnerWidth, 256, WA_InnerHeight, 128,
                         WA_Title, (ULONG)"Couleurs vraies", WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_PubScreen, (ULONG)scr, TAG_DONE);
    UnlockPubScreen(NULL, scr);
    if (!win) return 20;
    for (y = 0; y < 128; y++)
        for (x = 0; x < 256; x++) {
            int h = x * 6, f = h & 255, r, g, b, v = 255 - y;        /* a hue across, darker down */
            switch (h >> 8) {
            case 0: r = 255; g = f; b = 0; break;
            case 1: r = 255 - f; g = 255; b = 0; break;
            case 2: r = 0; g = 255; b = f; break;
            case 3: r = 0; g = 255 - f; b = 255; break;
            case 4: r = f; g = 0; b = 255; break;
            default: r = 255; g = 0; b = 255 - f; break;
            }
            pix[(y * 256 + x) * 3] = (UBYTE)(r * v / 255);
            pix[(y * 256 + x) * 3 + 1] = (UBYTE)(g * v / 255);
            pix[(y * 256 + x) * 3 + 2] = (UBYTE)(b * v / 255);
        }
    WritePixelArray(pix, 0, 0, 256 * 3, win->RPort, win->BorderLeft, win->BorderTop, 256, 128, RECTFMT_RGB);
    argb = ReadRGBPixel(win->RPort, win->BorderLeft + 100, win->BorderTop + 10);
    printf("pixel (100,10) relu : %06lx (ecrit %02x%02x%02x)\n", argb & 0xFFFFFF, pix[(10 * 256 + 100) * 3], pix[(10 * 256 + 100) * 3 + 1], pix[(10 * 256 + 100) * 3 + 2]);
    lock = LockBitMapTags(win->RPort->BitMap, LBMI_BYTESPERROW, (ULONG)&bpr, LBMI_BASEADDRESS, (ULONG)&base, TAG_DONE);
    printf("LockBitMap : %s, %ld octets par ligne\n", lock ? "ok" : "refuse", bpr);
    if (lock) UnLockBitMap(lock);
    printf("CgxTest pret\n");
    Delay(150);
    CloseWindow(win);
    CloseLibrary(CyberGfxBase);
    return 0;
}
