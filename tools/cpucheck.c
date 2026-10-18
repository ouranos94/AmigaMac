/* cpucheck: compare the AmigaMac 68k core with Musashi, one random
 * instruction at a time. Musashi is only a test oracle, never shipped.
 * Build: see tools/cpucheck.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu68k.h"
#include "mem.h"
#include "m68k.h"

#define MSIZE (1u << 20)
uint8_t *mem;
uint32_t mem_size;
uint32_t mem_fault(uint32_t a, int w, int s) { (void)a; (void)w; (void)s; return 0; }

static uint8_t mus[MSIZE], pristine[MSIZE];

unsigned int m68k_read_memory_8(unsigned int a) { return a < MSIZE ? mus[a] : 0; }
unsigned int m68k_read_memory_16(unsigned int a) { return a < MSIZE - 1 ? (mus[a] << 8 | mus[a + 1]) : 0; }
unsigned int m68k_read_memory_32(unsigned int a) {
    return a < MSIZE - 3 ? ((unsigned)mus[a] << 24 | mus[a + 1] << 16 | mus[a + 2] << 8 | mus[a + 3]) : 0;
}
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (a < MSIZE) mus[a] = v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { if (a < MSIZE - 1) { mus[a] = v >> 8; mus[a + 1] = v; } }
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (a < MSIZE - 3) { mus[a] = v >> 24; mus[a + 1] = v >> 16; mus[a + 2] = v >> 8; mus[a + 3] = v; }
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }

/* flags Motorola leaves undefined for some instructions */
static uint32_t undefined_flags(uint16_t op, uint32_t sr_a, uint32_t sr_b) {
    if ((op & 0xF1F0) == 0xC100 || (op & 0xF1F0) == 0x8100 || (op & 0xFFC0) == 0x4800)
        return CCR_N | CCR_V;                               /* ABCD, SBCD, NBCD */
    if ((op & 0xF0C0) == 0x80C0 || (op & 0xFFC0) == 0x4C40) {   /* DIVU/DIVS(.L) */
        if ((sr_a | sr_b) & CCR_V) return CCR_N | CCR_Z | CCR_C;
    }
    if ((op & 0xF040) == 0x4000 && (op & 0x0180) && (op & 0x1C0) != 0x1C0 && (op & 0xF000) == 0x4000 && (op & 0x100))
        return CCR_Z | CCR_V | CCR_C;                       /* CHK */
    if ((op & 0xF9C0) == 0x00C0) return CCR_N | CCR_V;      /* CHK2/CMP2 */
    return 0;
}

static int skip(uint16_t op) {
    int top = op >> 12;
    if (top == 0xA || top == 0xF) return 1;                  /* traps / FPU: tested elsewhere */
    if (op == 0x4E70 || op == 0x4E72 || op == 0x4E73) return 1;   /* RESET, STOP, RTE */
    if ((op & 0xFFF8) == 0x4848) return 1;                   /* BKPT */
    if ((op & 0xFFC0) == 0x06C0) return 1;                   /* CALLM/RTM (68020 only, unused on Amiga) */
    if ((op & 0xF9FF) == 0x08FC) return 1;                   /* CAS2 */
    if ((op & 0xFFFE) == 0x4E7A) return 1;                   /* MOVEC: control registers differ */
    return 0;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 200000;
    int maxshow = argc > 2 ? atoi(argv[2]) : 25;
    if (argc > 3) rs ^= (uint64_t)atoll(argv[3]) * 0x9E3779B97F4A7C15ull;
    mem = malloc(MSIZE);
    mem_size = MSIZE;
    for (uint32_t i = 0; i < MSIZE; i++) pristine[i] = (uint8_t)rnd();
    /* reset vectors for Musashi's pulse reset */
    pristine[0] = 0; pristine[1] = 0x09; pristine[2] = 0; pristine[3] = 0;
    pristine[4] = 0; pristine[5] = 0x01; pristine[6] = 0; pristine[7] = 0;
    memcpy(mus, pristine, MSIZE);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m68k_pulse_reset();
    m68k_execute(1);                     /* eats the reset cycles without executing */

    M68K c;
    memset(&c, 0, sizeof c);
    long fails = 0, done = 0, oracle_skip = 0;
    static int shown[65536];
    int nshown = 0;
    for (long t = 0; t < n; t++) {
        uint16_t op = (uint16_t)rnd();
        if (getenv("OP")) op = (uint16_t)strtol(getenv("OP"), 0, 16);
        else if (skip(op)) { t--; continue; }
        memcpy(mus, pristine, MSIZE);
        memcpy(mem, pristine, MSIZE);
        uint32_t pc = 0x10000;
        uint8_t code[12];
        code[0] = op >> 8; code[1] = (uint8_t)op;
        for (int i = 2; i < 12; i++) code[i] = (uint8_t)rnd();
        /* keep extension words sane half of the time: small displacements */
        if (rnd() & 1) for (int i = 2; i < 12; i += 2) { code[i] = 0; }
        memcpy(mus + pc, code, 12);
        memcpy(mem + pc, code, 12);

        uint32_t d[8], a[8];
        for (int i = 0; i < 8; i++) {
            uint32_t r = rnd();
            d[i] = (r & 3) == 0 ? (r >> 8) & 0xFF : (r & 3) == 1 ? (r >> 4) & 0xFFFF : rnd();
            a[i] = 0x20000 + ((rnd() % 0x60000) & ~1u);
        }
        int super = rnd() & 1;
        uint32_t sr = (rnd() & 0x1F) | (super ? 0x2000 : 0) | 0x0700;
        uint32_t usp = 0x80000 + ((rnd() % 0x8000) & ~1u), isp = 0x90000 + ((rnd() % 0x8000) & ~1u);
        a[7] = super ? isp : usp;

        /* Musashi */
        /* SR first: Musashi's set_reg(SR) does not swap stacks, so the two
           stack registers are set after, relative to the final mode */
        m68k_set_reg(M68K_REG_SR, sr);
        m68k_set_reg(M68K_REG_USP, usp);
        m68k_set_reg(M68K_REG_ISP, isp);
        for (int i = 0; i < 8; i++) { m68k_set_reg(M68K_REG_D0 + i, d[i]); m68k_set_reg(M68K_REG_A0 + i, a[i]); }
        m68k_set_reg(M68K_REG_VBR, 0);
        m68k_set_reg(M68K_REG_PC, pc);
        m68k_execute(1);

        /* ours */
        cpu68k_init(&c, CPU_68020);
        c.sr = sr;
        memcpy(c.d, d, sizeof d);
        memcpy(c.a, a, sizeof a);
        if (super) c.usp = usp; else c.ssp = isp;
        c.pc = pc;
        cpu68k_run(&c, 1);
        done++;

        uint32_t msr = m68k_get_reg(0, M68K_REG_SR);
        /* M bit (master stack): never used on the Amiga, not modelled */
        if ((msr | c.sr) & 0x1000) { done--; t--; continue; }
        /* where Musashi departs from the PRM / real silicon (WinUAE agrees
           with us): PACK/UNPK memory byte order, CHK2/CMP2 on An and
           wrapped bounds */
        if (((op & 0xF1F8) == 0x8148 || (op & 0xF1F8) == 0x8188) || (op & 0xF9C0) == 0x00C0) { oracle_skip++; continue; }
        uint32_t und = undefined_flags(op, msr, c.sr);
        int bad = 0;
        char why[256] = "";
        for (int i = 0; i < 8 && !bad; i++) {
            if (c.d[i] != m68k_get_reg(0, M68K_REG_D0 + i)) { bad = 1; sprintf(why, "D%d %08X/%08X", i, c.d[i], m68k_get_reg(0, M68K_REG_D0 + i)); }
            else if (c.a[i] != m68k_get_reg(0, M68K_REG_A0 + i)) { bad = 1; sprintf(why, "A%d %08X/%08X", i, c.a[i], m68k_get_reg(0, M68K_REG_A0 + i)); }
        }
        if (!bad && c.pc != m68k_get_reg(0, M68K_REG_PC)) { bad = 1; sprintf(why, "PC %08X/%08X", c.pc, m68k_get_reg(0, M68K_REG_PC)); }
        if (!bad && ((c.sr ^ msr) & 0xF71F & ~und)) { bad = 1; sprintf(why, "SR %04X/%04X", c.sr, msr); }
        if (!bad && memcmp(mem, mus, MSIZE)) {
            uint32_t i = 0;
            while (mem[i] == mus[i]) i++;
            bad = 1;
            sprintf(why, "mem[$%05X] %02X/%02X", i, mem[i], mus[i]);
        }
        if (getenv("OP")) {
            printf("init: SR=%04X usp=%08X isp=%08X\n", sr, usp, isp);
            for (int i = 0; i < 8; i++) printf(" D%d=%08X A%d=%08X | ours D%08X A%08X | mus D%08X A%08X\n", i, d[i], i, a[i],
                c.d[i], c.a[i], m68k_get_reg(0, M68K_REG_D0 + i), m68k_get_reg(0, M68K_REG_A0 + i));
            printf(" PC ours %08X mus %08X  SR ours %04X mus %04X  %s\n", c.pc, m68k_get_reg(0, M68K_REG_PC), c.sr, msr, why);
        }
        if (bad) {
            fails++;
            uint16_t key = op & 0xFFC0;                      /* group by opcode/size, not EA */
            if (!shown[key] && nshown < maxshow) {
                shown[key] = 1;
                nshown++;
                char dis[128];
                m68k_disassemble(dis, pc, M68K_CPU_TYPE_68020);
                printf("%04X %-36s  %s  (SR in %04X)\n", op, dis, why, sr);
            }
        }
    }
    printf("%ld instructions comparées, %ld différences (%.3f%%), %ld écartées (divergences connues de l'oracle)\n",
           done, fails, 100.0 * fails / done, oracle_skip);
    return fails != 0;
}
