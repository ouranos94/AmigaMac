/* HwTest: the hardware music programs drive - a sine on Paula's channel 0
 * through its registers, CIA-B timer A interrupts through ciab.resource.
 * Built with: tools/vcc -o System/C/HwTest Tests/c/hwtest.c -lamiga -lauto
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

#define CUSTOM(o) (*(volatile UWORD *)(0xDFF000 + (o)))
#define CUSTOML(o) (*(volatile ULONG *)(0xDFF000 + (o)))
#define CIAB(r) (*(volatile UBYTE *)(0xBFD000 + ((r) << 8)))

struct Library *CiabBase;
static volatile LONG ticks;

/* ciab.resource: AddICRVector(-6), RemICRVector(-12), AbleICR(-18), SetICR(-24) */
LONG AddICR(__reg("a6") struct Library *, __reg("d0") LONG, __reg("a1") struct Interrupt *) = "\tjsr\t-6(a6)";
void RemICR(__reg("a6") struct Library *, __reg("d0") LONG, __reg("a1") struct Interrupt *) = "\tjsr\t-12(a6)";

static void handler(void) { ticks++; }
static void (*hptr)(void) = handler;

int main(void)
{
    static struct Interrupt irq;
    BYTE *sample = AllocMem(32, MEMF_CHIP);
    int i;
    for (i = 0; i < 32; i++) {                                 /* one period of a sine */
        static const BYTE sine[8] = { 0, 49, 90, 117, 127, 117, 90, 49 };
        sample[i] = i < 8 ? sine[i] : i < 16 ? sine[16 - i - 1 + 1 > 7 ? 7 : 16 - i] : i < 24 ? -sine[i - 16] : -sine[32 - i > 7 ? 7 : 32 - i];
    }
    CUSTOML(0xA0) = (ULONG)sample;                             /* AUD0LC */
    CUSTOM(0xA4) = 16;                                         /* AUD0LEN: 16 words */
    CUSTOM(0xA6) = 254;                                        /* AUD0PER: about 440 Hz */
    CUSTOM(0xA8) = 48;                                         /* AUD0VOL */
    CUSTOM(0x96) = 0x8201;                                     /* DMACON: DMAEN | AUD0EN */
    printf("DMACONR = %04x\n", CUSTOM(0x02) & 0x020F);

    if (!(CiabBase = OpenResource("ciab.resource"))) { printf("pas de ciab.resource\n"); return 20; }
    irq.is_Node.ln_Type = NT_INTERRUPT;
    irq.is_Node.ln_Name = "HwTest";
    irq.is_Code = (void (*)())hptr;
    if (AddICR(CiabBase, 0, &irq)) { printf("timer A deja pris\n"); return 20; }
    CIAB(14) = 0;                                              /* CRA: stopped, continuous */
    CIAB(4) = 14187 & 0xFF;                                    /* 709379 / 14187 = 50 Hz */
    CIAB(5) = 14187 >> 8;
    CIAB(14) = 0x11;                                           /* load and start */
    Delay(100);                                                /* two seconds */
    CIAB(14) = 0;
    RemICR(CiabBase, 0, &irq);
    printf("interruptions du timer A en 2 s : %ld (50 Hz : 100 attendues)\n", ticks);
    CUSTOM(0x96) = 0x0001;                                     /* AUD0 off */
    printf("DMACONR apres arret = %04x\n", CUSTOM(0x02) & 0x020F);
    FreeMem(sample, 32);
    return 0;
}
