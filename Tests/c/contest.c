/* ConTest: a CON: window driven with console control sequences, and a line read back.
 * Built with: tools/vcc -o System/C/ConTest Tests/c/contest.c -lamiga -lauto */
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    BPTR con = Open("CON:20/20/420/120/Console de test/CLOSE", MODE_NEWFILE);
    if (!con) { printf("Open CON: a echoue\n"); return 20; }
    FPuts(con, "\x9b" "1mTitre\x9b" "0m  \x9b" "32mvert\x9b" "39m \x9b" "33mjaune\x9b" "39m \x9b" "7minverse\x9b" "0m\n");
    for (int i = 1; i <= 16; i++) FPrintf(con, "ligne %ld\n", (long)i);
    FPuts(con, "\x9b" "3;30Hici\x9b" "18;1H");
    FPuts(con, "Votre nom : ");
    Flush(con);
    char buf[80];
    LONG n = Read(con, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    printf("lu %ld octets : %s", (long)n, buf);
    FPuts(con, "\x9b" "0 q");                               /* window bounds report */
    SetMode(con, 1);
    char rep[32];
    n = Read(con, rep, sizeof rep - 1);
    rep[n > 0 ? n : 0] = 0;
    printf("rapport :");
    for (int i = 0; i < n; i++) printf(" %02x", (unsigned char)rep[i]);
    printf("\n");
    SetMode(con, 0);
    Close(con);
    return 0;
}
