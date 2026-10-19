/* Font: the font preferences editor, as in AmigaOS. Three fonts: the
 * Workbench's icons, the system's text (fixed width: Shells), the screens'
 * (title bars, menus, requesters). Writes ENV:Sys/font.prefs ("Utiliser")
 * and ENVARC:Sys/font.prefs too ("Sauver"); the system then opens the
 * Workbench screen anew with them. "Ajouter des polices" copies Amiga bitmap
 * fonts (name.font and the drawer of its sizes) from a disk or a drawer into
 * FONTS:. A 68k program like the original.
 * Built with: tools/vcc -o System/Prefs/Font Prefs/font.c -lamiga -lauto
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/text.h>
#include <prefs/prefhdr.h>
#include <prefs/font.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <stdio.h>
#include <string.h>

enum { ID_TEXT = 1, ID_CHOOSE = 4, ID_ADD = 7, ID_SAVE, ID_USE, ID_CANCEL };

static const char version[] = "$VER: Font 45.1 (19.10.2026) AmigaMac";
static char names[3][FONTNAMESIZE];
static UWORD sizes[3];
static char shown[3][FONTNAMESIZE + 16];
static const char *labels[3] = { "Ic\xf4nes du Workbench", "Texte syst\xe8me", "\xc9" "cran" };
static const char *titles[3] = { "Police des ic\xf4nes", "Police du texte syst\xe8me (chasse fixe)", "Police de l'\xe9" "cran" };
static struct Window *win;

/* ".font" at the end, in any case */
static BOOL is_font(const char *f)
{
    int l = (int)strlen(f);
    const char *e = f + l - 5;
    int i;
    if (l < 6) return FALSE;
    for (i = 0; i < 5; i++) if ((e[i] | 0x20) != ".font"[i]) return FALSE;
    return TRUE;
}

static ULONG be32(const UBYTE *p) { return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]; }

/* FORM PREF { PRHD, FONT, FONT, FONT } */
static BOOL read_prefs(const char *path)
{
    static UBYTE b[2048];
    LONG n, at;
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) return FALSE;
    n = Read(fh, b, sizeof b);
    Close(fh);
    if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "PREF", 4)) return FALSE;
    for (at = 12; at + 8 <= n;) {
        ULONG sz = be32(b + at + 4);
        if (at + 8 + (LONG)sz > n) break;
        if (!memcmp(b + at, "FONT", 4) && sz >= sizeof(struct FontPrefs)) {
            struct FontPrefs *fp = (struct FontPrefs *)(b + at + 8);
            if (fp->fp_Type <= FP_SCREENFONT && fp->fp_Name[0]) {
                strncpy(names[fp->fp_Type], (char *)fp->fp_Name, FONTNAMESIZE - 1);
                sizes[fp->fp_Type] = fp->fp_TextAttr.ta_YSize;
            }
        }
        at += 8 + ((sz + 1) & ~1);
    }
    return TRUE;
}

static BOOL write_prefs(const char *dir)
{
    static UBYTE b[12 + 8 + 6 + 3 * (8 + sizeof(struct FontPrefs))];
    char path[64];
    ULONG at = 12, k;
    BPTR fh;
    BOOL ok;
    memset(b, 0, sizeof b);
    memcpy(b, "FORM", 4);
    memcpy(b + 8, "PREF", 4);
    memcpy(b + at, "PRHD", 4); b[at + 7] = 6; at += 8 + 6;
    for (k = 0; k < 3; k++) {
        struct FontPrefs *fp;
        memcpy(b + at, "FONT", 4);
        b[at + 6] = sizeof(struct FontPrefs) >> 8; b[at + 7] = sizeof(struct FontPrefs) & 0xFF;
        fp = (struct FontPrefs *)(b + at + 8);
        fp->fp_Type = (UWORD)k;
        fp->fp_FrontPen = 1;
        fp->fp_DrawMode = JAM1;
        fp->fp_TextAttr.ta_YSize = sizes[k];
        strncpy((char *)fp->fp_Name, names[k], FONTNAMESIZE - 1);
        at += 8 + sizeof(struct FontPrefs);
    }
    b[4] = (UBYTE)((at - 8) >> 24); b[5] = (UBYTE)((at - 8) >> 16); b[6] = (UBYTE)((at - 8) >> 8); b[7] = (UBYTE)(at - 8);
    sprintf(path, "%s/font.prefs", dir);
    if (!(fh = Open(path, MODE_NEWFILE))) return FALSE;
    ok = Write(fh, b, at) == (LONG)at;
    Close(fh);
    return ok;
}

static void describe(int k)
{
    char base[FONTNAMESIZE];
    char *dot;
    strncpy(base, names[k], sizeof base - 1);
    base[sizeof base - 1] = 0;
    if ((dot = strrchr(base, '.')) && !strcmp(dot, ".font")) *dot = 0;
    sprintf(shown[k], "%s %u", base, (unsigned)sizes[k]);
}

static void say(const char *title, const char *text)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, NULL, NULL, "OK" };
    es.es_Title = (UBYTE *)title;
    es.es_TextFormat = (UBYTE *)"%s";
    EasyRequest(win, &es, NULL, text);
}

/* ---- adding fonts ------------------------------------------------------------------- */

static BOOL copy_file(const char *from, const char *to)
{
    static UBYTE buf[8192];
    BPTR in = Open((STRPTR)from, MODE_OLDFILE), out;
    LONG n;
    BOOL ok = TRUE;
    if (!in) return FALSE;
    if (!(out = Open((STRPTR)to, MODE_NEWFILE))) { Close(in); return FALSE; }
    while ((n = Read(in, buf, sizeof buf)) > 0) if (Write(out, buf, n) != n) { ok = FALSE; break; }
    if (n < 0) ok = FALSE;
    Close(out);
    Close(in);
    return ok;
}

/* "dir/name.font" and "dir/name/" (the sizes) into FONTS: */
static BOOL add_font(const char *dir, const char *file)
{
    char from[300], to[300], sub[300], fsub[300];
    BPTR lock;
    struct FileInfoBlock *fib;
    BOOL ok;
    int l = (int)strlen(file);
    if (!is_font(file)) return FALSE;
    sprintf(from, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != ':' ? "/" : "", file);
    sprintf(to, "FONTS:%s", file);
    if (!(ok = copy_file(from, to))) return FALSE;
    sprintf(sub, "%.*s", (int)(strlen(from) - 5), from);         /* the drawer of its sizes */
    sprintf(fsub, "FONTS:%.*s", l - 5, file);
    if (!(lock = Lock((STRPTR)sub, ACCESS_READ))) return TRUE;   /* no sizes drawer: contents only */
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        BPTR made = CreateDir((STRPTR)fsub);
        if (made) UnLock(made);
        if (Examine(lock, fib))
            while (ExNext(lock, fib)) {
                char f1[400], f2[400];
                if (fib->fib_DirEntryType > 0) continue;
                sprintf(f1, "%s/%s", sub, fib->fib_FileName);
                sprintf(f2, "%s/%s", fsub, fib->fib_FileName);
                if (!copy_file(f1, f2)) ok = FALSE;
            }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return ok;
}

static void add_fonts(void)
{
    struct FileRequester *fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, "Ajouter des polices : une police, ou un tiroir entier",
        ASLFR_Window, win, ASLFR_InitialPattern, "#?.font", ASLFR_DoPatterns, TRUE,
        ASLFR_PositiveText, "Ajouter", TAG_DONE);
    int added = 0, failed = 0;
    char msg[200];
    if (!fr) return;
    if (AslRequest(fr, NULL)) {
        if (fr->fr_File[0]) {
            if (add_font(fr->fr_Drawer, fr->fr_File)) added++; else failed++;
        } else {                                                  /* no file: every font of the drawer */
            BPTR lock = Lock(fr->fr_Drawer, ACCESS_READ);
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
            if (lock && fib && Examine(lock, fib))
                while (ExNext(lock, fib)) {
                    if (fib->fib_DirEntryType < 0 && is_font(fib->fib_FileName)) {
                        if (add_font(fr->fr_Drawer, fib->fib_FileName)) added++; else failed++;
                    }
                }
            if (fib) FreeDosObject(DOS_FIB, fib);
            if (lock) UnLock(lock);
        }
        if (!added && !failed) sprintf(msg, "Aucune police (fichier .font) dans\n%s", fr->fr_Drawer);
        else if (failed) sprintf(msg, "%d police(s) ajout\xe9" "e(s) dans FONTS:,\n%d n'ont pas pu l'\xea" "tre.", added, failed);
        else sprintf(msg, "%d police(s) ajout\xe9" "e(s) dans FONTS:.\nElles sont maintenant propos\xe9" "es par \xab Choisir \xbb.", added);
        say("Polices", msg);
    }
    FreeAslRequest(fr);
}

/* ---- choosing ------------------------------------------------------------------------ */

static BOOL choose(int k)
{
    struct FontRequester *fo = AllocAslRequestTags(ASL_FontRequest,
        ASLFO_TitleText, titles[k], ASLFO_Window, win,
        ASLFO_InitialName, names[k], ASLFO_InitialSize, sizes[k],
        ASLFO_FixedWidthOnly, k == FP_SYSFONT, ASLFO_MaxHeight, 64, TAG_DONE);
    BOOL ok = FALSE;
    if (!fo) return FALSE;
    if (AslRequest(fo, NULL)) {
        strncpy(names[k], (char *)fo->fo_Attr.ta_Name, FONTNAMESIZE - 1);
        sizes[k] = fo->fo_Attr.ta_YSize;
        ok = TRUE;
    }
    FreeAslRequest(fo);
    return ok;
}

int main(void)
{
    struct Screen *scr;
    APTR vi;
    struct Gadget *glist = NULL, *g, *texts[3];
    struct NewGadget ng;
    int action = 0, k, fh, bh, lw = 0, W, H, top, y;
    struct Library *AslBase = OpenLibrary("asl.library", 39);
    if (!AslBase || !version[0]) return 20;
    for (k = 0; k < 3; k++) { strcpy(names[k], "topaz.font"); sizes[k] = 8; }
    if (!read_prefs("ENV:Sys/font.prefs")) read_prefs("ENVARC:Sys/font.prefs");
    for (k = 0; k < 3; k++) describe(k);
    if (!(scr = LockPubScreen(NULL))) { CloseLibrary(AslBase); return 20; }
    vi = GetVisualInfo(scr, TAG_DONE);
    fh = scr->Font->ta_YSize;
    bh = fh + 6;
    top = scr->WBorTop + fh + 1;
    for (k = 0; k < 3; k++) {
        struct IntuiText it = { 0, 0, 0, 0, 0, NULL, NULL, NULL };
        int w;
        it.ITextFont = scr->Font;
        it.IText = (UBYTE *)labels[k];
        w = IntuiTextLength(&it);
        if (w > lw) lw = w;
    }
    W = 12 + lw + 8 + 220 + 8 + 100 + 12;
    H = top + 10 + 3 * (bh + 6) + 6 + bh + 14 + bh + 10;
    g = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;
    y = top + 10;
    for (k = 0; k < 3; k++) {
        ng.ng_LeftEdge = 12 + lw + 8; ng.ng_TopEdge = y; ng.ng_Width = 220; ng.ng_Height = bh;
        ng.ng_GadgetText = (UBYTE *)labels[k]; ng.ng_GadgetID = ID_TEXT + k; ng.ng_Flags = PLACETEXT_LEFT;
        texts[k] = g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, shown[k], GTTX_Border, TRUE, TAG_DONE);
        ng.ng_LeftEdge += 228; ng.ng_Width = 100; ng.ng_GadgetText = "Choisir..."; ng.ng_GadgetID = ID_CHOOSE + k;
        ng.ng_Flags = PLACETEXT_IN;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
        y += bh + 6;
    }
    ng.ng_LeftEdge = 12 + lw + 8; ng.ng_TopEdge = y + 6; ng.ng_Width = 220 + 8 + 100; ng.ng_Height = bh;
    ng.ng_GadgetText = "Ajouter des _polices..."; ng.ng_GadgetID = ID_ADD; ng.ng_Flags = PLACETEXT_IN;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    ng.ng_TopEdge = H - bh - 8; ng.ng_Width = 100;
    ng.ng_LeftEdge = 12; ng.ng_GadgetText = "_Sauver"; ng.ng_GadgetID = ID_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    ng.ng_LeftEdge = (W - 100) / 2; ng.ng_GadgetText = "_Utiliser"; ng.ng_GadgetID = ID_USE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);
    ng.ng_LeftEdge = W - 112; ng.ng_GadgetText = "_Annuler"; ng.ng_GadgetID = ID_CANCEL;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_DONE);

    win = g ? OpenWindowTags(NULL, WA_Left, (scr->Width - W) / 2, WA_Top, (scr->Height - H) / 2 > top ? (scr->Height - H) / 2 : top,
                             WA_Width, W, WA_Height, H, WA_Title, "Polices", WA_PubScreen, scr,
                             WA_Gadgets, glist, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                             WA_Activate, TRUE,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | TEXTIDCMP | IDCMP_VANILLAKEY,
                             TAG_DONE) : NULL;
    UnlockPubScreen(NULL, scr);
    if (win) {
        GT_RefreshWindow(win, NULL);
        while (!action) {
            struct IntuiMessage *m;
            WaitPort(win->UserPort);
            while (!action && (m = GT_GetIMsg(win->UserPort))) {
                ULONG cls = m->Class;
                UWORD code = m->Code;
                struct Gadget *gad = (struct Gadget *)m->IAddress;
                GT_ReplyIMsg(m);
                if (cls == IDCMP_CLOSEWINDOW) action = ID_CANCEL;
                else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); }
                else if (cls == IDCMP_VANILLAKEY) {
                    if (code == 's' || code == 'S') action = ID_SAVE;
                    else if (code == 'u' || code == 'U') action = ID_USE;
                    else if (code == 'a' || code == 'A' || code == 27) action = ID_CANCEL;
                    else if (code == 'p' || code == 'P') add_fonts();
                } else if (cls == IDCMP_GADGETUP) {
                    int id = gad->GadgetID;
                    if (id >= ID_CHOOSE && id < ID_CHOOSE + 3) {
                        k = id - ID_CHOOSE;
                        if (choose(k)) { describe(k); GT_SetGadgetAttrs(texts[k], win, NULL, GTTX_Text, shown[k], TAG_DONE); }
                    } else if (id == ID_ADD) add_fonts();
                    else if (id >= ID_SAVE) action = id;
                }
            }
        }
        CloseWindow(win);
        win = NULL;
    }
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    /* the window is gone: the Workbench screen may now open anew with the new fonts */
    if (action == ID_SAVE && !write_prefs("ENVARC:Sys")) printf("Font : impossible d'\xe9" "crire ENVARC:Sys/font.prefs\n");
    if ((action == ID_SAVE || action == ID_USE) && !write_prefs("ENV:Sys")) printf("Font : impossible d'\xe9" "crire ENV:Sys/font.prefs\n");
    CloseLibrary(AslBase);
    return 0;
}
