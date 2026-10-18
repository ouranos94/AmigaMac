/* IFFTest: iffparse.library - write an ILBM with PushChunk/PopChunk (one size
 * unknown), then read it back with PropChunk, CollectionChunk, StopChunk.
 * Built with: tools/vcc -o System/C/IFFTest Tests/c/ifftest.c -lamiga -lauto */
#include <exec/types.h>
#include <libraries/iffparse.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/iffparse.h>
#include <stdio.h>
#include <string.h>

#define ID_ILBM MAKE_ID('I','L','B','M')
#define ID_BMHD MAKE_ID('B','M','H','D')
#define ID_CMAP MAKE_ID('C','M','A','P')
#define ID_ANNO MAKE_ID('A','N','N','O')
#define ID_BODY MAKE_ID('B','O','D','Y')

int main(void)
{
    struct IFFHandle *iff = AllocIFF();
    UBYTE bmhd[20] = { 0, 32, 0, 16, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 10, 11, 0, 32, 0, 16 };
    UBYTE cmap[12] = { 170, 170, 170, 0, 0, 0, 255, 255, 255, 102, 136, 187 };
    UBYTE body[128];
    for (int i = 0; i < 128; i++) body[i] = (UBYTE)i;
    iff->iff_Stream = Open("RAM:test.iff", MODE_NEWFILE);
    InitIFFasDOS(iff);
    LONG e = OpenIFF(iff, IFFF_WRITE);
    e |= PushChunk(iff, ID_ILBM, ID_FORM, IFFSIZE_UNKNOWN);
    e |= PushChunk(iff, 0, ID_BMHD, 20); WriteChunkBytes(iff, bmhd, 20); e |= PopChunk(iff);
    e |= PushChunk(iff, 0, ID_ANNO, IFFSIZE_UNKNOWN); WriteChunkBytes(iff, "premier", 7); e |= PopChunk(iff);
    e |= PushChunk(iff, 0, ID_CMAP, 12); WriteChunkBytes(iff, cmap, 12); e |= PopChunk(iff);
    e |= PushChunk(iff, 0, ID_ANNO, 6); WriteChunkBytes(iff, "second", 6); e |= PopChunk(iff);
    e |= PushChunk(iff, 0, ID_BODY, 128); WriteChunkBytes(iff, body, 128); e |= PopChunk(iff);
    e |= PopChunk(iff);
    CloseIFF(iff);
    Close(iff->iff_Stream);
    printf("ecriture : %s\n", e ? "ERREUR" : "ok");

    iff->iff_Stream = Open("RAM:test.iff", MODE_OLDFILE);
    InitIFFasDOS(iff);
    OpenIFF(iff, IFFF_READ);
    PropChunk(iff, ID_ILBM, ID_BMHD);
    PropChunk(iff, ID_ILBM, ID_CMAP);
    CollectionChunk(iff, ID_ILBM, ID_ANNO);
    StopChunk(iff, ID_ILBM, ID_BODY);
    e = ParseIFF(iff, IFFPARSE_SCAN);
    struct StoredProperty *sp = FindProp(iff, ID_ILBM, ID_BMHD);
    struct StoredProperty *cm = FindProp(iff, ID_ILBM, ID_CMAP);
    struct ContextNode *cn = CurrentChunk(iff);
    char ids[8];
    printf("ParseIFF %ld, chunk %s taille %ld, BMHD %s %dx%d plans %d, CMAP %ld octets couleur 3 = %d,%d,%d\n",
           e, cn ? IDtoStr(cn->cn_ID, ids) : "?", cn ? cn->cn_Size : 0L, sp ? "ok" : "absent",
           sp ? ((UBYTE *)sp->sp_Data)[1] : 0, sp ? ((UBYTE *)sp->sp_Data)[3] : 0, sp ? ((UBYTE *)sp->sp_Data)[8] : 0,
           cm ? cm->sp_Size : 0L, cm ? ((UBYTE *)cm->sp_Data)[9] : 0, cm ? ((UBYTE *)cm->sp_Data)[10] : 0, cm ? ((UBYTE *)cm->sp_Data)[11] : 0);
    for (struct CollectionItem *ci = FindCollection(iff, ID_ILBM, ID_ANNO); ci; ci = ci->ci_Next)
        printf("  ANNO : %.*s\n", (int)ci->ci_Size, (char *)ci->ci_Data);
    UBYTE got[128];
    LONG n = ReadChunkBytes(iff, got, 200);
    int ok = n == 128;
    for (int i = 0; i < 128 && ok; i++) if (got[i] != i) ok = 0;
    printf("BODY : %ld octets %s\n", n, ok ? "corrects" : "FAUX");
    printf("suite : %ld (fin attendue %ld)\n", ParseIFF(iff, IFFPARSE_SCAN), (LONG)IFFERR_EOF);
    CloseIFF(iff);
    Close(iff->iff_Stream);
    FreeIFF(iff);
    return 0;
}
