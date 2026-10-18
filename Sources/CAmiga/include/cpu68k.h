/* 68000/68020 instruction interpreter.
 *
 * Only the processor is interpreted: no chipset, no cycle timing. A-line
 * opcodes ($Axxx) are traps into native code, which is how library jump
 * tables reach the native AmigaOS implementation.
 */
#ifndef AMIGA_CPU68K_H
#define AMIGA_CPU68K_H

#include <stdint.h>

enum { CPU_68000 = 0, CPU_68020 = 2 };

/* condition code bits */
enum { CCR_C = 0x01, CCR_V = 0x02, CCR_Z = 0x04, CCR_N = 0x08, CCR_X = 0x10 };
enum { SR_S = 0x2000, SR_T = 0x8000 };

/* exception vectors used by the core */
enum {
    VEC_BUS_ERROR = 2, VEC_ADDRESS_ERROR = 3, VEC_ILLEGAL = 4, VEC_ZERO_DIVIDE = 5,
    VEC_CHK = 6, VEC_TRAPV = 7, VEC_PRIVILEGE = 8, VEC_TRACE = 9,
    VEC_LINE_A = 10, VEC_LINE_F = 11, VEC_TRAP0 = 32
};

typedef struct M68K M68K;

struct M68K {
    uint32_t d[8];
    uint32_t a[8];          /* a[7] is the active stack pointer */
    uint32_t pc;
    uint32_t sr;            /* full SR, CCR in the low byte */
    uint32_t usp, ssp;      /* inactive stack pointer is parked here */
    uint32_t vbr, cacr, sfc, dfc;
    int model;              /* CPU_68000 or CPU_68020 */
    uint32_t insn_pc;       /* address of the instruction being executed */
    uint16_t ir;            /* its opcode */
    int stop;               /* set to leave cpu68k_run() */
    uint64_t insns;         /* instructions executed */
    int fpu;                /* a 68881/68882 is present */
    double fp[8];           /* FP0-FP7 */
    uint32_t fpcr, fpsr, fpiar;
    uint32_t hist[32];      /* recent instruction addresses (cpu68k_trace) */
    unsigned hidx;

    /* native hooks */
    void (*aline)(M68K *cpu, uint16_t opcode);  /* $Axxx trap, pc already past it */
    /* exception: return non-zero if handled natively (pc/sr set by handler),
       zero to let the core take the 68k vector through VBR */
    int (*exception)(M68K *cpu, int vector);
    void *user;
};

extern int cpu68k_trace;
void cpu68k_init(M68K *cpu, int model);
/* run until cpu->stop is set or max_insns executed; returns instructions run */
uint64_t cpu68k_run(M68K *cpu, uint64_t max_insns);
void cpu68k_set_sr(M68K *cpu, uint32_t sr);
/* raise exception (used by traps from native code too) */
void cpu68k_exception(M68K *cpu, int vector);
/* push/pop helpers for native code */
void cpu68k_push32(M68K *cpu, uint32_t v);
uint32_t cpu68k_pop32(M68K *cpu);
/* one-line disassembly-free state dump */
void cpu68k_dump(M68K *cpu, char *buf, int len);

#endif
