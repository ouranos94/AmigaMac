/* LocTest: locale.library - language, texts, dates, FormatString, catalogs, collation.
 * Built with: tools/vcc -o System/C/LocTest Tests/c/loctest.c -lamiga -lauto */
#include <exec/types.h>
#include <libraries/locale.h>
#include <dos/dos.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/locale.h>
#include <stdio.h>
#include <string.h>

static char out[200];
static int outn;
static ULONG putch(__reg("a0") struct Hook *h, __reg("a2") APTR loc, __reg("a1") ULONG c)
{
    if (outn < (int)sizeof out - 1) out[outn++] = (char)c;
    return 0;
}

int main(void)
{
    struct Locale *loc = OpenLocale(NULL);
    if (!loc) { printf("OpenLocale: echec\n"); return 20; }
    printf("langue %s, prefere %s, decimal '%s'\n", loc->loc_LanguageName, loc->loc_PrefLanguages[0], loc->loc_DecimalPoint);
    printf("mois 1 %s, jour 2 %s, oui %s\n", GetLocaleStr(loc, MON_1), GetLocaleStr(loc, DAY_2), GetLocaleStr(loc, YESSTR));

    struct Hook hook;
    memset(&hook, 0, sizeof hook);
    hook.h_Entry = (ULONG (*)())putch;
    struct DateStamp ds = { 17818, 13 * 60 + 37, 0 };          /* 14 October 2026, 13:37 */
    outn = 0;
    FormatDate(loc, "%A %e %B %Y %H:%M", &ds, &hook);
    printf("date : %s\n", out);
    LONG args[] = { 42, (LONG)"RAM:" };
    outn = 0;
    FormatString(loc, "%ld fichiers dans %s", args, &hook);
    printf("format : %s\n", out);

    struct Catalog *cat = OpenCatalog(NULL, "loctest.catalog", OC_BuiltInLanguage, (ULONG)"english", TAG_DONE);
    printf("catalogue %s (v%d) : %s\n", cat ? (char *)cat->cat_Language : "aucun", cat ? (int)cat->cat_Version : 0,
           GetCatalogStr(cat, 1, "Hello, world!"));
    printf("absent : %s\n", GetCatalogStr(cat, 99, "texte par defaut"));
    CloseCatalog(cat);

    printf("StrnCmp(\xe9t\xe9, ETE, COLLATE1) = %ld, ASCII = %ld\n",
           StrnCmp(loc, "\xe9t\xe9", "ETE", -1, SC_COLLATE1), StrnCmp(loc, "abc", "abd", -1, SC_ASCII));
    printf("IsAlpha(\xe9) %d, ConvToUpper(\xe9) %c\n", IsAlpha(loc, 0xE9) != 0, (char)ConvToUpper(loc, 0xE9));
    CloseLocale(loc);
    return 0;
}
