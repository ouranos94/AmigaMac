/* 68000/68020 instruction interpreter.
 *
 * Decoding is a switch on the top nibble then on bit patterns, following the
 * Motorola M68000 Family Programmer's Reference Manual. Effective addresses
 * are resolved once (side effects included) into an EA, then read/written.
 */
#include "cpu68k.h"
#include "mem.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---- size helpers ------------------------------------------------------- */

static inline uint32_t MASK(int sz) { return sz == 1 ? 0xFF : sz == 2 ? 0xFFFF : 0xFFFFFFFF; }
static inline uint32_t MSB(int sz) { return sz == 1 ? 0x80 : sz == 2 ? 0x8000 : 0x80000000; }
static inline int32_t SEXT(uint32_t v, int sz) {
    return sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
}

static inline uint32_t rdsz(uint32_t a, int sz) { return sz == 1 ? rd8(a) : sz == 2 ? rd16(a) : rd32(a); }
static inline void wrsz(uint32_t a, uint32_t v, int sz) {
    if (sz == 1) wr8(a, v); else if (sz == 2) wr16(a, v); else wr32(a, v);
}

static inline uint32_t fetch16(M68K *c) { uint32_t v = rd16(c->pc); c->pc += 2; return v; }
static inline uint32_t fetch32(M68K *c) { uint32_t v = rd32(c->pc); c->pc += 4; return v; }

/* ---- flags -------------------------------------------------------------- */

#define FLAG(c, f) (((c)->sr & (f)) != 0)
static inline void setf(M68K *c, uint32_t f, int on) { if (on) c->sr |= f; else c->sr &= ~f; }

static void flags_logic(M68K *c, uint32_t r, int sz) {
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (r & MSB(sz)) c->sr |= CCR_N;
    if (!(r & MASK(sz))) c->sr |= CCR_Z;
}

static uint32_t do_add(M68K *c, uint32_t s, uint32_t d, int sz, int x, int keepz) {
    uint32_t m = MASK(sz), r = (d + s + x) & m;
    uint64_t wide = (uint64_t)(d & m) + (s & m) + x;
    int carry = (wide >> (sz * 8)) & 1;
    int ov = (((s ^ r) & (d ^ r)) & MSB(sz)) != 0;
    c->sr &= ~(CCR_N | CCR_V | CCR_C | CCR_X | (keepz ? 0 : CCR_Z));
    if (r & MSB(sz)) c->sr |= CCR_N;
    if (keepz) { if (r) c->sr &= ~CCR_Z; } else if (!r) c->sr |= CCR_Z;
    if (ov) c->sr |= CCR_V;
    if (carry) c->sr |= CCR_C | CCR_X;
    return r;
}

/* d - s - x */
static uint32_t do_sub(M68K *c, uint32_t s, uint32_t d, int sz, int x, int keepz, int setx) {
    uint32_t m = MASK(sz), r = (d - s - x) & m;
    int borrow = (uint64_t)(s & m) + x > (d & m);
    int ov = (((s ^ d) & (r ^ d)) & MSB(sz)) != 0;
    c->sr &= ~(CCR_N | CCR_V | CCR_C | (setx ? CCR_X : 0) | (keepz ? 0 : CCR_Z));
    if (r & MSB(sz)) c->sr |= CCR_N;
    if (keepz) { if (r) c->sr &= ~CCR_Z; } else if (!r) c->sr |= CCR_Z;
    if (ov) c->sr |= CCR_V;
    if (borrow) c->sr |= CCR_C | (setx ? CCR_X : 0);
    return r;
}

static int cond(M68K *c, int cc) {
    int C = FLAG(c, CCR_C), V = FLAG(c, CCR_V), Z = FLAG(c, CCR_Z), N = FLAG(c, CCR_N);
    switch (cc & 15) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !C && !Z;
    case 3: return C || Z;
    case 4: return !C;
    case 5: return C;
    case 6: return !Z;
    case 7: return Z;
    case 8: return !V;
    case 9: return V;
    case 10: return !N;
    case 11: return N;
    case 12: return N == V;
    case 13: return N != V;
    case 14: return !Z && N == V;
    default: return Z || N != V;
    }
}

/* ---- supervisor state ---------------------------------------------------- */

void cpu68k_set_sr(M68K *c, uint32_t sr) {
    int was_s = (c->sr & SR_S) != 0, now_s = (sr & SR_S) != 0;
    if (was_s != now_s) {
        if (now_s) { c->usp = c->a[7]; c->a[7] = c->ssp; }
        else { c->ssp = c->a[7]; c->a[7] = c->usp; }
    }
    c->sr = sr & 0xF71F;
}

void cpu68k_push32(M68K *c, uint32_t v) { c->a[7] -= 4; wr32(c->a[7], v); }
uint32_t cpu68k_pop32(M68K *c) { uint32_t v = rd32(c->a[7]); c->a[7] += 4; return v; }
static void push16(M68K *c, uint32_t v) { c->a[7] -= 2; wr16(c->a[7], v); }
static uint32_t pop16(M68K *c) { uint32_t v = rd16(c->a[7]); c->a[7] += 2; return v; }

void cpu68k_exception(M68K *c, int vec) {
    if (c->exception && c->exception(c, vec)) return;
    uint32_t old = c->sr, pc = c->pc;
    cpu68k_set_sr(c, (c->sr | SR_S) & ~SR_T);
    if (c->model >= CPU_68020) {
        if (vec == VEC_ZERO_DIVIDE || vec == VEC_CHK || vec == VEC_TRAPV || vec == VEC_TRACE) {
            cpu68k_push32(c, c->insn_pc);             /* format 2: instruction address */
            push16(c, 0x2000 | vec * 4);
        } else push16(c, vec * 4);                    /* format 0 */
    }
    cpu68k_push32(c, pc);
    push16(c, old);
    c->pc = rd32(c->vbr + vec * 4);
}

static void illegal(M68K *c) { c->pc = c->insn_pc; cpu68k_exception(c, VEC_ILLEGAL); }
static int need_020(M68K *c) { if (c->model < CPU_68020) { illegal(c); return 0; } return 1; }
static int need_super(M68K *c) {
    if (!(c->sr & SR_S)) { c->pc = c->insn_pc; cpu68k_exception(c, VEC_PRIVILEGE); return 0; }
    return 1;
}

/* ---- effective addresses ------------------------------------------------- */

enum { EA_D, EA_A, EA_M, EA_I };
typedef struct { int kind, reg; uint32_t addr, imm; } EA;

static uint32_t index_ea(M68K *c, uint32_t base) {
    uint32_t ext = fetch16(c);
    int r = ext >> 12;
    uint32_t idx = r < 8 ? c->d[r] : c->a[r - 8];
    if (!(ext & 0x800)) idx = (uint32_t)(int16_t)idx;
    int scale = c->model >= CPU_68020 ? (ext >> 9) & 3 : 0;
    idx <<= scale;
    if (!(ext & 0x100) || c->model < CPU_68020)        /* brief format */
        return base + (int8_t)ext + idx;
    /* 68020 full format */
    uint32_t bd = 0, od = 0;
    if (ext & 0x80) base = 0;                           /* base suppress */
    if (ext & 0x40) idx = 0;                            /* index suppress */
    switch ((ext >> 4) & 3) {
    case 2: bd = (uint32_t)(int16_t)fetch16(c); break;
    case 3: bd = fetch32(c); break;
    }
    int iis = ext & 7;
    if (iis == 0) return base + bd + idx;
    switch (iis & 3) {
    case 2: od = (uint32_t)(int16_t)fetch16(c); break;
    case 3: od = fetch32(c); break;
    }
    if (iis < 4) return rd32(base + bd + idx) + od;     /* pre-indexed */
    return rd32(base + bd) + idx + od;                  /* post-indexed */
}

static EA ea_decode(M68K *c, int mode, int reg, int sz) {
    EA e = { EA_M, reg, 0, 0 };
    switch (mode) {
    case 0: e.kind = EA_D; break;
    case 1: e.kind = EA_A; break;
    case 2: e.addr = c->a[reg]; break;
    case 3: e.addr = c->a[reg]; c->a[reg] += (reg == 7 && sz == 1) ? 2 : sz; break;
    case 4: c->a[reg] -= (reg == 7 && sz == 1) ? 2 : sz; e.addr = c->a[reg]; break;
    case 5: e.addr = c->a[reg] + (int16_t)fetch16(c); break;
    case 6: e.addr = index_ea(c, c->a[reg]); break;
    default:
        switch (reg) {
        case 0: e.addr = (uint32_t)(int16_t)fetch16(c); break;
        case 1: e.addr = fetch32(c); break;
        case 2: { uint32_t b = c->pc; e.addr = b + (int16_t)fetch16(c); break; }
        case 3: { uint32_t b = c->pc; e.addr = index_ea(c, b); break; }
        case 4:
            e.kind = EA_I;
            e.imm = sz == 4 ? fetch32(c) : sz == 2 ? fetch16(c) : fetch16(c) & 0xFF;
            break;
        default: e.kind = -1;
        }
    }
    return e;
}

static uint32_t ea_read(M68K *c, EA *e, int sz) {
    switch (e->kind) {
    case EA_D: return c->d[e->reg] & MASK(sz);
    case EA_A: return c->a[e->reg] & MASK(sz);
    case EA_I: return e->imm & MASK(sz);
    default: return rdsz(e->addr, sz);
    }
}

static void ea_write(M68K *c, EA *e, uint32_t v, int sz) {
    switch (e->kind) {
    case EA_D: c->d[e->reg] = (c->d[e->reg] & ~MASK(sz)) | (v & MASK(sz)); break;
    case EA_A: c->a[e->reg] = sz == 2 ? (uint32_t)(int16_t)v : v; break;
    case EA_M: wrsz(e->addr, v, sz); break;
    default: break;
    }
}

/* control addressing (LEA, PEA, JMP, JSR, MOVEM...): address only */
static int ea_control(M68K *c, int mode, int reg, uint32_t *addr) {
    if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) return 0;
    EA e = ea_decode(c, mode, reg, 4);
    *addr = e.addr;
    return 1;
}

static const int size_tab[4] = { 1, 2, 4, 0 };   /* 00 byte, 01 word, 10 long */

/* ---- group 0: immediate, bit ops, MOVEP, CAS, CMP2/CHK2 ------------------ */

static void bitop(M68K *c, int op, uint32_t bit, int mode, int reg) {
    EA e;
    int sz = mode == 0 ? 4 : 1;
    bit &= sz == 4 ? 31 : 7;
    e = ea_decode(c, mode, reg, sz);
    uint32_t v = ea_read(c, &e, sz);
    setf(c, CCR_Z, !(v & (1u << bit)));
    switch (op) {
    case 0: return;                                  /* BTST */
    case 1: v ^= 1u << bit; break;                   /* BCHG */
    case 2: v &= ~(1u << bit); break;                /* BCLR */
    case 3: v |= 1u << bit; break;                   /* BSET */
    }
    ea_write(c, &e, v, sz);
}

static void cmp2(M68K *c, int sz, int mode, int reg) {
    /* bounds are sign-extended; a data register is compared on its operand
       size, an address register on 32 bits. lower > upper means the range
       wraps around (unsigned bounds). */
    uint32_t ext = fetch16(c), addr;
    if (!ea_control(c, mode, reg, &addr)) { illegal(c); return; }
    int r = ext >> 12;
    int32_t lo = SEXT(rdsz(addr, sz), sz), hi = SEXT(rdsz(addr + sz, sz), sz);
    int32_t v = r < 8 ? SEXT(c->d[r], sz) : (int32_t)c->a[r - 8];
    setf(c, CCR_Z, v == lo || v == hi);
    int out = lo <= hi ? (v < lo || v > hi) : (v > hi && v < lo);
    setf(c, CCR_C, out);
    if (out && (ext & 0x800)) cpu68k_exception(c, VEC_CHK);
}

static void group0(M68K *c) {
    uint16_t ir = c->ir;
    int mode = (ir >> 3) & 7, reg = ir & 7;

    if (ir & 0x100) {
        if (mode == 1) {                                 /* MOVEP */
            int dr = (ir >> 9) & 7, opm = (ir >> 6) & 3;
            uint32_t a = c->a[reg] + (int16_t)fetch16(c);
            switch (opm) {
            case 0: c->d[dr] = (c->d[dr] & 0xFFFF0000) | rd8(a) << 8 | rd8(a + 2); break;
            case 1: c->d[dr] = rd8(a) << 24 | rd8(a + 2) << 16 | rd8(a + 4) << 8 | rd8(a + 6); break;
            case 2: wr8(a, c->d[dr] >> 8); wr8(a + 2, c->d[dr]); break;
            case 3: wr8(a, c->d[dr] >> 24); wr8(a + 2, c->d[dr] >> 16); wr8(a + 4, c->d[dr] >> 8); wr8(a + 6, c->d[dr]); break;
            }
            return;
        }
        bitop(c, (ir >> 6) & 3, c->d[(ir >> 9) & 7], mode, reg);   /* dynamic bit op */
        return;
    }

    int op = (ir >> 9) & 7, szc = (ir >> 6) & 3;

    if (op == 4) {                                       /* static bit op */
        uint32_t bit = fetch16(c);
        bitop(c, szc, bit, mode, reg);
        return;
    }
    if (szc == 3) {
        if (!need_020(c)) return;
        if (op <= 2) { cmp2(c, size_tab[op], mode, reg); return; }   /* CMP2/CHK2 */
        if (op >= 5) {                                   /* CAS Dc,Du,<ea> */
            int sz = size_tab[op - 5];
            if (!sz) { illegal(c); return; }             /* CAS2 not supported */
            uint32_t ext = fetch16(c);
            EA e = ea_decode(c, mode, reg, sz);
            uint32_t dst = ea_read(c, &e, sz), dc = ext & 7, du = (ext >> 6) & 7;
            do_sub(c, c->d[dc] & MASK(sz), dst, sz, 0, 0, 0);
            if (FLAG(c, CCR_Z)) ea_write(c, &e, c->d[du], sz);
            else c->d[dc] = (c->d[dc] & ~MASK(sz)) | dst;
            return;
        }
        illegal(c);
        return;
    }

    if (op == 7) {                                       /* MOVES (privileged) */
        if (!need_020(c) || !need_super(c)) return;
        int msz = size_tab[szc];
        uint32_t ext = fetch16(c);
        int r = ext >> 12;
        EA e = ea_decode(c, mode, reg, msz);
        if (ext & 0x800) ea_write(c, &e, r < 8 ? c->d[r] : c->a[r - 8], msz);
        else {
            uint32_t v = ea_read(c, &e, msz);
            if (r < 8) c->d[r] = (c->d[r] & ~MASK(msz)) | v;
            else c->a[r - 8] = (uint32_t)SEXT(v, msz);
        }
        return;
    }
    int sz = size_tab[szc];
    /* to CCR / to SR */
    if (mode == 7 && reg == 4 && (op == 0 || op == 1 || op == 5)) {
        uint32_t imm = fetch16(c);
        if (szc == 0) {
            uint32_t ccr = c->sr & 0xFF;
            ccr = op == 0 ? ccr | imm : op == 1 ? ccr & imm : ccr ^ imm;
            c->sr = (c->sr & 0xFF00) | (ccr & 0x1F);
            return;
        }
        if (szc == 1) {
            if (!need_super(c)) return;
            uint32_t sr = op == 0 ? c->sr | imm : op == 1 ? c->sr & imm : c->sr ^ imm;
            cpu68k_set_sr(c, sr);
            return;
        }
    }
    uint32_t imm = sz == 4 ? fetch32(c) : fetch16(c) & MASK(sz);
    EA e = ea_decode(c, mode, reg, sz);
    uint32_t v = ea_read(c, &e, sz), r;
    switch (op) {
    case 0: r = v | imm; flags_logic(c, r, sz); ea_write(c, &e, r, sz); break;      /* ORI */
    case 1: r = v & imm; flags_logic(c, r, sz); ea_write(c, &e, r, sz); break;      /* ANDI */
    case 2: r = do_sub(c, imm, v, sz, 0, 0, 1); ea_write(c, &e, r, sz); break;      /* SUBI */
    case 3: r = do_add(c, imm, v, sz, 0, 0); ea_write(c, &e, r, sz); break;         /* ADDI */
    case 5: r = v ^ imm; flags_logic(c, r, sz); ea_write(c, &e, r, sz); break;      /* EORI */
    case 6: do_sub(c, imm, v, sz, 0, 0, 0); break;                                  /* CMPI */
    default: illegal(c);
    }
}

/* ---- MOVE --------------------------------------------------------------- */

static void move(M68K *c, int sz) {
    uint16_t ir = c->ir;
    EA s = ea_decode(c, (ir >> 3) & 7, ir & 7, sz);
    uint32_t v = ea_read(c, &s, sz);
    int dmode = (ir >> 6) & 7, dreg = (ir >> 9) & 7;
    if (dmode == 1) { c->a[dreg] = sz == 2 ? (uint32_t)(int16_t)v : v; return; }   /* MOVEA */
    EA d = ea_decode(c, dmode, dreg, sz);
    flags_logic(c, v, sz);
    ea_write(c, &d, v, sz);
}

/* ---- MOVEM -------------------------------------------------------------- */

static void movem(M68K *c) {
    uint16_t ir = c->ir;
    int sz = (ir & 0x40) ? 4 : 2, mode = (ir >> 3) & 7, reg = ir & 7;
    uint32_t mask = fetch16(c), addr;
    if (ir & 0x400) {                                   /* memory -> registers */
        if (mode == 3) addr = c->a[reg];
        else if (!ea_control(c, mode, reg, &addr)) { illegal(c); return; }
        for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
            uint32_t v = sz == 4 ? rd32(addr) : (uint32_t)(int16_t)rd16(addr);
            if (i < 8) c->d[i] = v; else c->a[i - 8] = v;
            addr += sz;
        }
        if (mode == 3) c->a[reg] = addr;
    } else if (mode == 4) {                             /* registers -> -(An), reversed mask */
        addr = c->a[reg];
        for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
            int r = 15 - i;
            addr -= sz;
            uint32_t v = r < 8 ? c->d[r] : c->a[r - 8];
            wrsz(addr, v, sz);
        }
        c->a[reg] = addr;
    } else {
        if (!ea_control(c, mode, reg, &addr)) { illegal(c); return; }
        for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
            wrsz(addr, i < 8 ? c->d[i] : c->a[i - 8], sz);
            addr += sz;
        }
    }
}

/* ---- 68020 long multiply / divide --------------------------------------- */

static void mul_l(M68K *c, int mode, int reg) {
    uint32_t ext = fetch16(c);
    EA e = ea_decode(c, mode, reg, 4);
    uint32_t s = ea_read(c, &e, 4);
    int dl = (ext >> 12) & 7, dh = ext & 7, sgn = ext & 0x800, quad = ext & 0x400;
    uint64_t r;
    if (sgn) r = (uint64_t)((int64_t)(int32_t)s * (int64_t)(int32_t)c->d[dl]);
    else r = (uint64_t)s * c->d[dl];
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (quad) {
        c->d[dh] = (uint32_t)(r >> 32); c->d[dl] = (uint32_t)r;
        if (r >> 63) c->sr |= CCR_N;
        if (!r) c->sr |= CCR_Z;
    } else {
        uint32_t lo = (uint32_t)r;
        c->d[dl] = lo;
        if (lo & 0x80000000) c->sr |= CCR_N;
        if (!lo) c->sr |= CCR_Z;
        if (sgn ? (int64_t)r != (int64_t)(int32_t)lo : (r >> 32) != 0) c->sr |= CCR_V;
    }
}

static void div_l(M68K *c, int mode, int reg) {
    uint32_t ext = fetch16(c);
    EA e = ea_decode(c, mode, reg, 4);
    uint32_t s = ea_read(c, &e, 4);
    int dq = (ext >> 12) & 7, dr = ext & 7, sgn = ext & 0x800, quad = ext & 0x400;
    if (!s) { cpu68k_exception(c, VEC_ZERO_DIVIDE); return; }
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (sgn) {
        int64_t n = quad ? (int64_t)((uint64_t)c->d[dr] << 32 | c->d[dq]) : (int64_t)(int32_t)c->d[dq];
        int64_t q = n / (int32_t)s, rem = n % (int32_t)s;
        if (q != (int32_t)q) { c->sr |= CCR_V; return; }
        c->d[dq] = (uint32_t)q;
        if (dr != dq) c->d[dr] = (uint32_t)rem;
        if ((int32_t)q < 0) c->sr |= CCR_N;
        if (!q) c->sr |= CCR_Z;
    } else {
        uint64_t n = quad ? ((uint64_t)c->d[dr] << 32 | c->d[dq]) : c->d[dq];
        uint64_t q = n / s, rem = n % s;
        if (q >> 32) { c->sr |= CCR_V; return; }
        c->d[dq] = (uint32_t)q;
        if (dr != dq) c->d[dr] = (uint32_t)rem;
        if ((uint32_t)q & 0x80000000) c->sr |= CCR_N;
        if (!q) c->sr |= CCR_Z;
    }
}

/* ---- group 4: miscellaneous --------------------------------------------- */

static void rte(M68K *c) {
    if (!need_super(c)) return;
    uint32_t sr = pop16(c), pc = cpu68k_pop32(c);
    if (c->model >= CPU_68020) {
        uint32_t fmt = pop16(c) >> 12;
        if (fmt == 2) c->a[7] += 4;                      /* instruction address */
        else if (fmt != 0) c->a[7] += fmt == 9 ? 12 : 0;
    }
    c->pc = pc;
    cpu68k_set_sr(c, sr);
}

static void group4(M68K *c) {
    uint16_t ir = c->ir;
    int mode = (ir >> 3) & 7, reg = ir & 7;

    if (ir & 0x100) {
        if ((ir & 0xFFF8) == 0x49C0) {                   /* EXTB.L (shares LEA's space) */
            c->d[reg] = (uint32_t)(int8_t)c->d[reg];
            flags_logic(c, c->d[reg], 4);
            return;
        }
        if ((ir & 0x1C0) == 0x1C0) {                     /* LEA */
            uint32_t a;
            if (!ea_control(c, mode, reg, &a)) { illegal(c); return; }
            c->a[(ir >> 9) & 7] = a;
            return;
        }
        /* CHK.W (0x180) / CHK.L (0x100, 020) */
        int sz = (ir & 0x80) ? 2 : 4;
        if (sz == 4 && !need_020(c)) return;
        EA e = ea_decode(c, mode, reg, sz);
        int32_t bound = SEXT(ea_read(c, &e, sz), sz), v = SEXT(c->d[(ir >> 9) & 7], sz);
        setf(c, CCR_Z, v == 0);
        c->sr &= ~(CCR_V | CCR_C);
        if (v < 0) { c->sr |= CCR_N; cpu68k_exception(c, VEC_CHK); }
        else if (v > bound) { c->sr &= ~CCR_N; cpu68k_exception(c, VEC_CHK); }
        return;
    }

    switch ((ir >> 6) & 0x3F) {
    case 0x00: case 0x01: case 0x02: {                   /* NEGX */
        int sz = size_tab[(ir >> 6) & 3];
        EA e = ea_decode(c, mode, reg, sz);
        uint32_t r = do_sub(c, ea_read(c, &e, sz), 0, sz, FLAG(c, CCR_X), 1, 1);
        ea_write(c, &e, r, sz);
        return;
    }
    case 0x03: {                                         /* MOVE from SR */
        if (c->model >= CPU_68020 && !need_super(c)) return;
        EA e = ea_decode(c, mode, reg, 2);
        ea_write(c, &e, c->sr, 2);
        return;
    }
    case 0x08: case 0x09: case 0x0A: {                   /* CLR */
        int sz = size_tab[(ir >> 6) & 3];
        EA e = ea_decode(c, mode, reg, sz);
        ea_write(c, &e, 0, sz);
        c->sr = (c->sr & ~(CCR_N | CCR_V | CCR_C)) | CCR_Z;
        return;
    }
    case 0x0B: {                                         /* MOVE from CCR (010+) */
        if (!need_020(c)) return;
        EA e = ea_decode(c, mode, reg, 2);
        ea_write(c, &e, c->sr & 0x1F, 2);
        return;
    }
    case 0x10: case 0x11: case 0x12: {                   /* NEG */
        int sz = size_tab[(ir >> 6) & 3];
        EA e = ea_decode(c, mode, reg, sz);
        uint32_t r = do_sub(c, ea_read(c, &e, sz), 0, sz, 0, 0, 1);
        ea_write(c, &e, r, sz);
        return;
    }
    case 0x13: {                                         /* MOVE to CCR */
        EA e = ea_decode(c, mode, reg, 2);
        c->sr = (c->sr & 0xFF00) | (ea_read(c, &e, 2) & 0x1F);
        return;
    }
    case 0x18: case 0x19: case 0x1A: {                   /* NOT */
        int sz = size_tab[(ir >> 6) & 3];
        EA e = ea_decode(c, mode, reg, sz);
        uint32_t r = ~ea_read(c, &e, sz) & MASK(sz);
        flags_logic(c, r, sz);
        ea_write(c, &e, r, sz);
        return;
    }
    case 0x1B: {                                         /* MOVE to SR */
        if (!need_super(c)) return;
        EA e = ea_decode(c, mode, reg, 2);
        cpu68k_set_sr(c, ea_read(c, &e, 2));
        return;
    }
    case 0x20: {
        if (mode == 1) {                                 /* LINK.L (020) */
            if (!need_020(c)) return;
            int32_t disp = (int32_t)fetch32(c);
            c->a[7] -= 4;
            wr32(c->a[7], c->a[reg]);                    /* A7 itself: its decremented value */
            c->a[reg] = c->a[7];
            c->a[7] += disp;
            return;
        }
        /* NBCD: 0 - dst - X, decimal-adjusted as the ALU does */
        EA e = ea_decode(c, mode, reg, 1);
        uint32_t d = ea_read(c, &e, 1);
        uint32_t res = (0x9Au - d - FLAG(c, CCR_X)) & 0xFF;
        if (res != 0x9A) {
            uint32_t v = ~res;
            if ((res & 0x0F) == 0x0A) res = (res & 0xF0) + 0x10;
            res &= 0xFF;
            v &= res;
            setf(c, CCR_V, (v & 0x80) != 0);
            ea_write(c, &e, res, 1);
            if (res) c->sr &= ~CCR_Z;
            setf(c, CCR_C | CCR_X, 1);
        } else {
            setf(c, CCR_V, 0);
            setf(c, CCR_C | CCR_X, 0);
        }
        setf(c, CCR_N, (res & 0x80) != 0);              /* undefined: as the silicon */
        return;
    }
    case 0x21: {
        if (mode == 0) {                                 /* SWAP */
            uint32_t v = c->d[reg];
            v = v << 16 | v >> 16;
            c->d[reg] = v;
            flags_logic(c, v, 4);
            return;
        }
        if (mode == 1) { illegal(c); return; }           /* BKPT */
        uint32_t a;                                      /* PEA */
        if (!ea_control(c, mode, reg, &a)) { illegal(c); return; }
        cpu68k_push32(c, a);
        return;
    }
    case 0x22: case 0x23:
        if (mode == 0) {                                 /* EXT.W / EXT.L */
            if (ir & 0x40) { c->d[reg] = (uint32_t)(int16_t)c->d[reg]; flags_logic(c, c->d[reg], 4); }
            else { c->d[reg] = (c->d[reg] & 0xFFFF0000) | ((uint32_t)(int8_t)c->d[reg] & 0xFFFF); flags_logic(c, c->d[reg], 2); }
            return;
        }
        movem(c);
        return;
    case 0x27:
        if (mode == 0) {                                 /* EXTB.L */
            if (!need_020(c)) return;
            c->d[reg] = (uint32_t)(int8_t)c->d[reg];
            flags_logic(c, c->d[reg], 4);
            return;
        }
        illegal(c);
        return;
    case 0x28: case 0x29: case 0x2A: {                   /* TST */
        int sz = size_tab[(ir >> 6) & 3];
        EA e = ea_decode(c, mode, reg, sz);
        flags_logic(c, ea_read(c, &e, sz), sz);
        return;
    }
    case 0x2B: {
        if (ir == 0x4AFC) { illegal(c); return; }        /* ILLEGAL */
        EA e = ea_decode(c, mode, reg, 1);               /* TAS */
        uint32_t v = ea_read(c, &e, 1);
        flags_logic(c, v, 1);
        ea_write(c, &e, v | 0x80, 1);
        return;
    }
    case 0x30: if (!need_020(c)) return; mul_l(c, mode, reg); return;
    case 0x31: if (!need_020(c)) return; div_l(c, mode, reg); return;
    case 0x32: case 0x33: movem(c); return;
    case 0x39:
        if ((ir & 0x30) == 0x00) {                       /* TRAP #n */
            cpu68k_exception(c, VEC_TRAP0 + (ir & 15));
            return;
        }
        if ((ir & 0x38) == 0x10) {                       /* LINK.W */
            int16_t disp = (int16_t)fetch16(c);
            c->a[7] -= 4;
            wr32(c->a[7], c->a[reg]);
            c->a[reg] = c->a[7];
            c->a[7] += disp;
            return;
        }
        if ((ir & 0x38) == 0x18) {                       /* UNLK */
            c->a[7] = c->a[reg];
            c->a[reg] = cpu68k_pop32(c);
            return;
        }
        if ((ir & 0x30) == 0x20) {                       /* MOVE USP */
            if (!need_super(c)) return;
            if (ir & 8) c->a[reg] = c->usp; else c->usp = c->a[reg];
            return;
        }
        switch (ir) {
        case 0x4E70: need_super(c); return;              /* RESET: no hardware to reset */
        case 0x4E71: return;                             /* NOP */
        case 0x4E72: {                                   /* STOP */
            uint32_t sr = fetch16(c);
            if (!need_super(c)) return;
            cpu68k_set_sr(c, sr);
            c->stop = 1;
            return;
        }
        case 0x4E73: rte(c); return;
        case 0x4E74: {                                   /* RTD */
            if (!need_020(c)) return;
            int16_t d = (int16_t)fetch16(c);
            c->pc = cpu68k_pop32(c);
            c->a[7] += d;
            return;
        }
        case 0x4E75: c->pc = cpu68k_pop32(c); return;      /* RTS */
        case 0x4E76: if (FLAG(c, CCR_V)) cpu68k_exception(c, VEC_TRAPV); return;
        case 0x4E77: {                                   /* RTR */
            c->sr = (c->sr & 0xFF00) | (pop16(c) & 0x1F);
            c->pc = cpu68k_pop32(c);
            return;
        }
        case 0x4E7A: case 0x4E7B: {                      /* MOVEC */
            if (!need_020(c) || !need_super(c)) return;
            uint32_t ext = fetch16(c);
            int r = ext >> 12;
            uint32_t *gp = r < 8 ? &c->d[r] : &c->a[r - 8], *cr = 0;
            switch (ext & 0xFFF) {
            case 0x000: cr = &c->sfc; break;
            case 0x001: cr = &c->dfc; break;
            case 0x002: cr = &c->cacr; break;
            case 0x800: cr = &c->usp; break;
            case 0x801: cr = &c->vbr; break;
            default: illegal(c); return;
            }
            if (ir & 1) *cr = *gp; else *gp = *cr;
            return;
        }
        }
        illegal(c);
        return;
    case 0x3A: {                                         /* JSR */
        uint32_t a;
        if (!ea_control(c, mode, reg, &a)) { illegal(c); return; }
        cpu68k_push32(c, c->pc);
        c->pc = a;
        return;
    }
    case 0x3B: {                                         /* JMP */
        uint32_t a;
        if (!ea_control(c, mode, reg, &a)) { illegal(c); return; }
        c->pc = a;
        return;
    }
    }
    illegal(c);
}

/* ---- group 5: ADDQ/SUBQ, Scc, DBcc, TRAPcc ------------------------------ */

static void group5(M68K *c) {
    uint16_t ir = c->ir;
    int mode = (ir >> 3) & 7, reg = ir & 7;
    if (((ir >> 6) & 3) == 3) {
        int cc = (ir >> 8) & 15;
        if (mode == 1) {                                 /* DBcc */
            uint32_t base = c->pc;
            int16_t disp = (int16_t)fetch16(c);
            if (!cond(c, cc)) {
                uint16_t w = (uint16_t)(c->d[reg] - 1);
                c->d[reg] = (c->d[reg] & 0xFFFF0000) | w;
                if (w != 0xFFFF) c->pc = base + disp;
            }
            return;
        }
        if (mode == 7 && reg >= 2) {                     /* TRAPcc */
            if (!need_020(c)) return;
            if (reg == 2) fetch16(c); else if (reg == 3) fetch32(c);
            else if (reg != 4) { illegal(c); return; }
            if (cond(c, cc)) cpu68k_exception(c, VEC_TRAPV);
            return;
        }
        EA e = ea_decode(c, mode, reg, 1);               /* Scc */
        ea_write(c, &e, cond(c, cc) ? 0xFF : 0, 1);
        return;
    }
    int sz = size_tab[(ir >> 6) & 3];
    uint32_t q = (ir >> 9) & 7;
    if (!q) q = 8;
    if (mode == 1) {                                     /* address register: no flags, long */
        c->a[reg] += (ir & 0x100) ? -q : q;
        return;
    }
    EA e = ea_decode(c, mode, reg, sz);
    uint32_t v = ea_read(c, &e, sz);
    uint32_t r = (ir & 0x100) ? do_sub(c, q, v, sz, 0, 0, 1) : do_add(c, q, v, sz, 0, 0);
    ea_write(c, &e, r, sz);
}

/* ---- group 6: Bcc/BRA/BSR ------------------------------------------------ */

static void group6(M68K *c) {
    uint16_t ir = c->ir;
    uint32_t base = c->pc;
    int32_t disp = (int8_t)ir;
    if (disp == 0) disp = (int16_t)fetch16(c);
    else if (disp == -1 && c->model >= CPU_68020) disp = (int32_t)fetch32(c);
    int cc = (ir >> 8) & 15;
    if (cc == 1) { cpu68k_push32(c, c->pc); c->pc = base + disp; return; }   /* BSR */
    if (cond(c, cc)) c->pc = base + disp;
}

/* ---- BCD ---------------------------------------------------------------- */

/* decimal adjust as the 68k ALU does it, including invalid BCD digits;
   N and V are officially undefined, set as the silicon does */
static uint32_t bcd_add(M68K *c, uint32_t s, uint32_t d) {
    uint32_t res = (s & 15) + (d & 15) + FLAG(c, CCR_X);
    uint32_t v = ~res;
    if (res > 9) res += 6;
    res += (s & 0xF0) + (d & 0xF0);
    int carry = res > 0x99;
    if (carry) res -= 0xA0;
    v &= res;
    setf(c, CCR_C | CCR_X, carry);
    setf(c, CCR_V, (v & 0x80) != 0);
    setf(c, CCR_N, (res & 0x80) != 0);
    res &= 0xFF;
    if (res) c->sr &= ~CCR_Z;
    return res;
}

static uint32_t bcd_sub(M68K *c, uint32_t s, uint32_t d) {   /* d - s - X */
    uint32_t res = (d & 15) - (s & 15) - FLAG(c, CCR_X);
    uint32_t v = ~res;
    if (res > 9) res -= 6;
    res += (d & 0xF0) - (s & 0xF0);
    int carry = res > 0x99;
    if (carry) res += 0xA0;
    res &= 0xFF;
    v &= ~res;
    setf(c, CCR_C | CCR_X, carry);
    setf(c, CCR_V, (v & 0x80) != 0);
    setf(c, CCR_N, (res & 0x80) != 0);
    if (res) c->sr &= ~CCR_Z;
    return res;
}

/* ABCD/SBCD/ADDX/SUBX share the register/predecrement form */
static void xop(M68K *c, int sz, uint32_t (*f)(M68K *, uint32_t, uint32_t, int)) {
    uint16_t ir = c->ir;
    int rx = ir & 7, ry = (ir >> 9) & 7;
    if (ir & 8) {
        int step = sz;
        c->a[rx] -= (rx == 7 && sz == 1) ? 2 : step;
        uint32_t s = rdsz(c->a[rx], sz);
        c->a[ry] -= (ry == 7 && sz == 1) ? 2 : step;
        uint32_t d = rdsz(c->a[ry], sz);
        wrsz(c->a[ry], f(c, s, d, sz), sz);
    } else {
        uint32_t r = f(c, c->d[rx] & MASK(sz), c->d[ry] & MASK(sz), sz);
        c->d[ry] = (c->d[ry] & ~MASK(sz)) | (r & MASK(sz));
    }
}
static uint32_t f_abcd(M68K *c, uint32_t s, uint32_t d, int sz) { (void)sz; return bcd_add(c, s, d); }
static uint32_t f_sbcd(M68K *c, uint32_t s, uint32_t d, int sz) { (void)sz; return bcd_sub(c, s, d); }
static uint32_t f_addx(M68K *c, uint32_t s, uint32_t d, int sz) { return do_add(c, s, d, sz, FLAG(c, CCR_X), 1); }
static uint32_t f_subx(M68K *c, uint32_t s, uint32_t d, int sz) { return do_sub(c, s, d, sz, FLAG(c, CCR_X), 1, 1); }

/* ---- groups 8/9/B/C/D: OR, SUB, CMP/EOR, AND, ADD and friends ------------ */

static uint32_t pd_byte(M68K *c, int r) { c->a[r] -= r == 7 ? 2 : 1; return c->a[r]; }

static void pack_unpk(M68K *c, int unpk) {
    uint16_t ir = c->ir;
    int rx = ir & 7, ry = (ir >> 9) & 7, mem_ = ir & 8;
    uint32_t adj = fetch16(c);
    if (!unpk) {                                         /* PACK: word -> packed byte */
        uint32_t v;
        if (mem_) { v = rd8(pd_byte(c, rx)); v |= rd8(pd_byte(c, rx)) << 8; }
        else v = c->d[rx] & 0xFFFF;
        v = (v + adj) & 0xFFFF;
        uint32_t r = ((v >> 4) & 0xF0) | (v & 0x0F);
        if (mem_) wr8(pd_byte(c, ry), r);
        else c->d[ry] = (c->d[ry] & ~0xFFu) | r;
    } else {                                             /* UNPK: packed byte -> word */
        uint32_t v = mem_ ? rd8(pd_byte(c, rx)) : c->d[rx] & 0xFF;
        uint32_t r = ((((v & 0xF0) << 4) | (v & 0x0F)) + adj) & 0xFFFF;
        if (mem_) { wr8(pd_byte(c, ry), r); wr8(pd_byte(c, ry), r >> 8); }
        else c->d[ry] = (c->d[ry] & ~0xFFFFu) | r;
    }
}

static void arith(M68K *c, int grp) {
    uint16_t ir = c->ir;
    int mode = (ir >> 3) & 7, reg = ir & 7, dr = (ir >> 9) & 7, opm = (ir >> 6) & 7;

    /* word multiply/divide and address-register forms */
    if (opm == 3 || opm == 7) {
        if (grp == 0x8 || grp == 0xC) {
            EA e = ea_decode(c, mode, reg, 2);
            uint32_t s = ea_read(c, &e, 2);
            if (grp == 0x8) {                            /* DIVU.W / DIVS.W */
                if (!s) { cpu68k_exception(c, VEC_ZERO_DIVIDE); return; }
                c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
                if (opm == 3) {
                    uint32_t q = c->d[dr] / s, r = c->d[dr] % s;
                    if (q > 0xFFFF) { c->sr |= CCR_V; return; }
                    c->d[dr] = r << 16 | q;
                    if (q & 0x8000) c->sr |= CCR_N;
                    if (!q) c->sr |= CCR_Z;
                } else {
                    int32_t n = (int32_t)c->d[dr], dv = (int16_t)s;
                    if (n == INT32_MIN && dv == -1) { c->sr |= CCR_V; return; }
                    int32_t q = n / dv, r = n % dv;
                    if (q < -32768 || q > 32767) { c->sr |= CCR_V; return; }
                    c->d[dr] = ((uint32_t)r & 0xFFFF) << 16 | ((uint32_t)q & 0xFFFF);
                    if (q < 0) c->sr |= CCR_N;
                    if (!q) c->sr |= CCR_Z;
                }
            } else {                                     /* MULU.W / MULS.W */
                uint32_t r = opm == 3 ? (c->d[dr] & 0xFFFF) * s
                                      : (uint32_t)((int32_t)(int16_t)c->d[dr] * (int32_t)(int16_t)s);
                c->d[dr] = r;
                flags_logic(c, r, 4);
            }
            return;
        }
        /* SUBA / CMPA / ADDA */
        int sz = opm == 3 ? 2 : 4;
        EA e = ea_decode(c, mode, reg, sz);
        uint32_t s = ea_read(c, &e, sz);
        if (sz == 2) s = (uint32_t)(int16_t)s;
        if (grp == 0x9) c->a[dr] -= s;
        else if (grp == 0xD) c->a[dr] += s;
        else do_sub(c, s, c->a[dr], 4, 0, 0, 0);
        return;
    }

    int sz = size_tab[opm & 3];
    if (opm >= 4) {
        /* special register-register forms */
        if (mode <= 1) {
            switch (grp) {
            case 0x8:
                if (opm == 4) { xop(c, 1, f_sbcd); return; }
                if (opm == 5 || opm == 6) { if (!need_020(c)) return; pack_unpk(c, opm == 6); return; }
                break;
            case 0xC:
                if (opm == 4) { xop(c, 1, f_abcd); return; }
                if (opm == 5) {                          /* EXG */
                    uint32_t t;
                    if (mode == 0) { t = c->d[dr]; c->d[dr] = c->d[reg]; c->d[reg] = t; }
                    else { t = c->a[dr]; c->a[dr] = c->a[reg]; c->a[reg] = t; }
                    return;
                }
                if (opm == 6 && mode == 1) {
                    uint32_t t = c->d[dr]; c->d[dr] = c->a[reg]; c->a[reg] = t;
                    return;
                }
                break;
            case 0x9: xop(c, sz, f_subx); return;
            case 0xD: xop(c, sz, f_addx); return;
            case 0xB:
                if (mode == 1) {                         /* CMPM (Ay)+,(Ax)+ */
                    uint32_t s = rdsz(c->a[reg], sz);
                    c->a[reg] += (reg == 7 && sz == 1) ? 2 : sz;
                    uint32_t d = rdsz(c->a[dr], sz);
                    c->a[dr] += (dr == 7 && sz == 1) ? 2 : sz;
                    do_sub(c, s, d, sz, 0, 0, 0);
                    return;
                }
                break;                                   /* EOR Dn,Dn */
            }
        }
        /* Dn op <ea> -> <ea> */
        EA e = ea_decode(c, mode, reg, sz);
        uint32_t d = ea_read(c, &e, sz), s = c->d[dr] & MASK(sz), r;
        switch (grp) {
        case 0x8: r = d | s; flags_logic(c, r, sz); break;
        case 0x9: r = do_sub(c, s, d, sz, 0, 0, 1); break;
        case 0xB: r = d ^ s; flags_logic(c, r, sz); break;
        case 0xC: r = d & s; flags_logic(c, r, sz); break;
        default: r = do_add(c, s, d, sz, 0, 0); break;
        }
        ea_write(c, &e, r, sz);
        return;
    }
    /* <ea> op Dn -> Dn */
    EA e = ea_decode(c, mode, reg, sz);
    uint32_t s = ea_read(c, &e, sz), d = c->d[dr] & MASK(sz), r;
    switch (grp) {
    case 0x8: r = d | s; flags_logic(c, r, sz); break;
    case 0x9: r = do_sub(c, s, d, sz, 0, 0, 1); break;
    case 0xB: do_sub(c, s, d, sz, 0, 0, 0); return;   /* CMP */
    case 0xC: r = d & s; flags_logic(c, r, sz); break;
    default: r = do_add(c, s, d, sz, 0, 0); break;
    }
    c->d[dr] = (c->d[dr] & ~MASK(sz)) | (r & MASK(sz));
}

/* ---- group E: shifts, rotates, bit fields -------------------------------- */

static uint32_t shift(M68K *c, int type, int left, uint32_t v, int cnt, int sz) {
    uint32_t m = MASK(sz), msb = MSB(sz);
    int bits = sz * 8;
    v &= m;
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (cnt == 0) {
        if (type == 2 && FLAG(c, CCR_X)) c->sr |= CCR_C;      /* ROX with 0: C = X */
        if (v & msb) c->sr |= CCR_N;
        if (!v) c->sr |= CCR_Z;
        return v;
    }
    int carry = 0, ovf = 0;
    switch (type) {
    case 0:                                              /* AS */
        if (left) {
            for (int i = 0; i < cnt; i++) {
                carry = (v & msb) != 0;
                uint32_t nv = (v << 1) & m;
                if ((nv ^ v) & msb) ovf = 1;
                v = nv;
            }
        } else {
            for (int i = 0; i < cnt; i++) {
                carry = v & 1;
                v = ((v >> 1) | (v & msb)) & m;
            }
        }
        setf(c, CCR_X, carry);
        break;
    case 1:                                              /* LS */
        if (left) for (int i = 0; i < cnt; i++) { carry = (v & msb) != 0; v = (v << 1) & m; }
        else for (int i = 0; i < cnt; i++) { carry = v & 1; v >>= 1; }
        setf(c, CCR_X, carry);
        break;
    case 2: {                                            /* ROX */
        int x = FLAG(c, CCR_X);
        for (int i = 0; i < cnt; i++) {
            if (left) { carry = (v & msb) != 0; v = ((v << 1) | x) & m; }
            else { carry = v & 1; v = (v >> 1) | (x ? msb : 0); }
            x = carry;
        }
        setf(c, CCR_X, x);
        carry = x;
        break;
    }
    default: {                                           /* RO: X unaffected */
        int n = cnt % bits;
        if (left) { if (n) v = ((v << n) | (v >> (bits - n))) & m; carry = v & 1; }
        else { if (n) v = ((v >> n) | (v << (bits - n))) & m; carry = (v & msb) != 0; }
        break;
    }
    }
    if (carry) c->sr |= CCR_C;
    if (ovf) c->sr |= CCR_V;
    if (v & msb) c->sr |= CCR_N;
    if (!v) c->sr |= CCR_Z;
    return v;
}

static void bitfield(M68K *c) {
    uint16_t ir = c->ir;
    int mode = (ir >> 3) & 7, reg = ir & 7, op = (ir >> 8) & 7;
    uint32_t ext = fetch16(c);
    int32_t off = (ext & 0x800) ? (int32_t)c->d[(ext >> 6) & 7] : (int32_t)((ext >> 6) & 31);
    int width = (ext & 0x20) ? (int)(c->d[ext & 7] & 31) : (int)(ext & 31);
    if (!width) width = 32;
    uint32_t mask = width == 32 ? 0xFFFFFFFF : ((1u << width) - 1);
    uint32_t field;
    uint64_t data = 0;
    uint32_t addr = 0;

    if (mode == 0) {
        off &= 31;
        uint32_t v = c->d[reg];
        uint32_t rot = off ? (v << off) | (v >> (32 - off)) : v;
        field = width == 32 ? rot : rot >> (32 - width);
    } else {
        if (!ea_control(c, mode, reg, &addr)) { illegal(c); return; }
        addr += off >> 3;                                /* arithmetic shift: negative offsets */
        off &= 7;
        for (int i = 0; i < 5; i++) data = data << 8 | rd8(addr + i);
        field = (uint32_t)(data >> (40 - off - width)) & mask;
    }
    field &= mask;
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (field & (1u << (width - 1))) c->sr |= CCR_N;
    if (!field) c->sr |= CCR_Z;

    uint32_t newf = field;
    int write = 0;
    switch (op) {
    case 0: break;                                       /* BFTST */
    case 1: c->d[(ext >> 12) & 7] = field; break;        /* BFEXTU */
    case 2: newf = ~field & mask; write = 1; break;      /* BFCHG */
    case 3: {                                            /* BFEXTS */
        uint32_t v = field;
        if (width < 32 && (v & (1u << (width - 1)))) v |= ~mask;
        c->d[(ext >> 12) & 7] = v;
        break;
    }
    case 4: newf = 0; write = 1; break;                  /* BFCLR */
    case 5: {                                            /* BFFFO */
        int i = 0;
        while (i < width && !(field & (1u << (width - 1 - i)))) i++;
        int32_t base = (ext & 0x800) ? (int32_t)c->d[(ext >> 6) & 7] : (int32_t)((ext >> 6) & 31);
        if (mode == 0) base &= 31;
        c->d[(ext >> 12) & 7] = (uint32_t)(base + i);
        break;
    }
    case 6: newf = mask; write = 1; break;               /* BFSET */
    case 7:                                              /* BFINS */
        newf = c->d[(ext >> 12) & 7] & mask;
        c->sr &= ~(CCR_N | CCR_Z);
        if (newf & (1u << (width - 1))) c->sr |= CCR_N;
        if (!newf) c->sr |= CCR_Z;
        write = 1;
        break;
    }
    if (!write) return;
    if (mode == 0) {
        uint32_t v = c->d[reg];
        uint32_t fm = mask << (32 - width);              /* field mask in rotated space */
        uint32_t rot = off ? (v << off) | (v >> (32 - off)) : v;
        rot = (rot & ~fm) | ((newf << (32 - width)) & fm);
        c->d[reg] = off ? (rot >> off) | (rot << (32 - off)) : rot;
    } else {
        int sh = 40 - off - width;
        uint64_t fm = (uint64_t)mask << sh;
        data = (data & ~fm) | ((uint64_t)newf << sh & fm);
        for (int i = 4; i >= 0; i--) { wr8(addr + i, (uint32_t)data); data >>= 8; }
    }
}

static void groupE(M68K *c) {
    uint16_t ir = c->ir;
    if ((ir & 0xC0) == 0xC0) {
        if (ir & 0x800) { if (need_020(c)) bitfield(c); return; }
        /* memory shift, word, by 1 */
        int type = (ir >> 9) & 3, left = (ir >> 8) & 1;
        EA e = ea_decode(c, (ir >> 3) & 7, ir & 7, 2);
        uint32_t v = ea_read(c, &e, 2);
        ea_write(c, &e, shift(c, type, left, v, 1, 2), 2);
        return;
    }
    int sz = size_tab[(ir >> 6) & 3], reg = ir & 7, left = (ir >> 8) & 1, type = (ir >> 3) & 3;
    int cnt = (ir >> 9) & 7;
    if (ir & 0x20) cnt = c->d[cnt] & 63;
    else if (!cnt) cnt = 8;
    uint32_t r = shift(c, type, left, c->d[reg], cnt, sz);
    c->d[reg] = (c->d[reg] & ~MASK(sz)) | (r & MASK(sz));
}

/* ---- opcode legality ------------------------------------------------------
 * Addressing-mode classes of the M68000 PRM, checked once per opcode into a
 * table, so the executors can assume a valid encoding.
 */
enum { M_D = 1, M_A = 2, M_AI = 4, M_PI = 8, M_PD = 16, M_DI = 32, M_IX = 64,
       M_AW = 128, M_AL = 256, M_PCD = 512, M_PCX = 1024, M_IMM = 2048 };
#define C_ALL 0xFFF
#define C_DATA (C_ALL & ~M_A)
#define C_ALT (M_D | M_A | M_AI | M_PI | M_PD | M_DI | M_IX | M_AW | M_AL)
#define C_DALT (C_ALT & ~M_A)
#define C_MALT (C_DALT & ~M_D)
#define C_CTRL (M_AI | M_DI | M_IX | M_AW | M_AL | M_PCD | M_PCX)
#define C_CALT (M_AI | M_DI | M_IX | M_AW | M_AL)

static int modebit(int mode, int reg) {
    if (mode < 7) return 1 << mode;
    return reg <= 4 ? 128 << reg : 0;
}
static int eaok(int op, unsigned cls) { return (modebit((op >> 3) & 7, op & 7) & cls) != 0; }

static int is_legal(unsigned op) {
    int mode = (op >> 3) & 7, sz = (op >> 6) & 3;
    switch (op >> 12) {
    case 0x0: {
        int op9 = (op >> 9) & 7;
        if (op & 0x100) {
            if (mode == 1) return 1;                                   /* MOVEP */
            return eaok(op, sz == 0 ? C_DATA : C_DALT);                /* dynamic bit op */
        }
        if (op9 == 4) return eaok(op, sz == 0 ? C_DATA & ~M_IMM : C_DALT);   /* static bit op */
        if (sz == 3) {
            if (op9 <= 2) return eaok(op, C_CTRL);                     /* CMP2/CHK2 */
            if (op9 >= 5) return eaok(op, C_MALT);                     /* CAS */
            return 0;
        }
        if (op9 == 7) return eaok(op, C_MALT);                         /* MOVES */
        if (op9 == 0 || op9 == 1 || op9 == 5) {
            if (mode == 7 && (op & 7) == 4) return sz <= 1;            /* to CCR / SR */
            return eaok(op, C_DALT);
        }
        if (op9 == 6) return eaok(op, C_DATA & ~M_IMM);                /* CMPI (020: PC rel ok) */
        return eaok(op, C_DALT);                                       /* SUBI/ADDI */
    }
    case 0x1: case 0x2: case 0x3: {
        int msz = (op >> 12) == 1 ? 1 : 2;
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        if (!eaok(op, msz == 1 ? C_ALL & ~M_A : C_ALL)) return 0;
        if (dmode == 1) return msz != 1;                               /* MOVEA */
        return (modebit(dmode, dreg) & C_DALT) != 0;
    }
    case 0x4:
        if (op & 0x100) {
            if ((op & 0xFFF8) == 0x49C0) return 1;                     /* EXTB.L */
            switch (op & 0x1C0) {
            case 0x1C0: return eaok(op, C_CTRL);                       /* LEA */
            case 0x180: case 0x100: return eaok(op, C_DATA);           /* CHK.W / CHK.L */
            default: return 0;
            }
        }
        switch ((op >> 6) & 0x3F) {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12:
        case 0x18: case 0x19: case 0x1A: return eaok(op, C_DALT);
        case 0x13: case 0x1B: return eaok(op, C_DATA);                 /* MOVE to CCR/SR */
        case 0x20: return mode == 1 || eaok(op, C_DALT);               /* LINK.L / NBCD */
        case 0x21: return mode == 0 || (mode != 1 && eaok(op, C_CTRL));   /* SWAP / PEA */
        case 0x22: case 0x23: return mode == 0 || eaok(op, C_CALT | M_PD);   /* EXT / MOVEM */
        case 0x27: return mode == 0;                                   /* EXTB */
        case 0x28: case 0x29: case 0x2A: return eaok(op, sz == 0 ? C_ALL & ~M_A : C_ALL);   /* TST */
        case 0x2B: return op == 0x4AFC || eaok(op, C_DALT);            /* ILLEGAL / TAS */
        case 0x30: case 0x31: return eaok(op, C_DATA);                 /* MUL.L / DIV.L */
        case 0x32: case 0x33: return eaok(op, C_CTRL | M_PI);          /* MOVEM */
        case 0x39:
            if (op <= 0x4E77) return 1;                                /* TRAP..RTR */
            return op == 0x4E7A || op == 0x4E7B;                       /* MOVEC */
        case 0x3A: case 0x3B: return eaok(op, C_CTRL);                 /* JSR / JMP */
        default: return 0;
        }
    case 0x5:
        if (sz == 3) {
            if (mode == 1) return 1;                                   /* DBcc */
            if (mode == 7 && (op & 7) >= 2) return (op & 7) <= 4;      /* TRAPcc */
            return eaok(op, C_DALT);                                   /* Scc */
        }
        return sz == 0 ? eaok(op, C_DALT) : eaok(op, C_ALT);           /* ADDQ/SUBQ */
    case 0x6: return 1;
    case 0x7: return !(op & 0x100);
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int grp = op >> 12, opm = (op >> 6) & 7;
        if (opm == 3 || opm == 7) return eaok(op, (grp == 0x8 || grp == 0xC) ? C_DATA : C_ALL);
        if (opm < 3) {
            if (grp == 0x8 || grp == 0xC) return eaok(op, C_DATA);     /* OR/AND <ea>,Dn */
            return eaok(op, opm == 0 ? C_ALL & ~M_A : C_ALL);          /* SUB/ADD/CMP */
        }
        if (mode <= 1) {
            switch (grp) {
            case 0x8: return 1;                                        /* SBCD/PACK/UNPK */
            case 0x9: case 0xD: return 1;                              /* SUBX/ADDX */
            case 0xB: return 1;                                        /* CMPM / EOR Dn,Dn */
            case 0xC: return opm == 4 || opm == 5 || (opm == 6 && mode == 1);   /* ABCD/EXG */
            }
        }
        return eaok(op, grp == 0xB ? C_DALT : C_MALT);
    }
    case 0xE:
        if (sz == 3) {
            if (op & 0x800) {
                int t = (op >> 8) & 7;
                return eaok(op, (t == 0 || t == 1 || t == 3 || t == 5) ? (M_D | C_CTRL) : (M_D | C_CALT));
            }
            return eaok(op, C_MALT);                                   /* memory shift */
        }
        return 1;
    }
    return 1;                                                          /* A-line, F-line: traps */
}

static uint8_t legal_tab[65536];
static int legal_ready;

static void legal_init(void) {
    for (unsigned op = 0; op < 65536; op++) legal_tab[op] = (uint8_t)is_legal(op);
    legal_ready = 1;
}

#include "fpu68k.inc"

/* ---- main loop ---------------------------------------------------------- */

void cpu68k_init(M68K *c, int model) {
    memset(c, 0, offsetof(M68K, aline));
    c->model = model;
    c->fpu = model >= CPU_68020;
    for (int i = 0; i < 8; i++) c->fp[i] = NAN;
    c->sr = 0;                                           /* user mode, interrupts enabled */
    if (!legal_ready) legal_init();
}

int cpu68k_trace;

uint64_t cpu68k_run(M68K *c, uint64_t max) {
    uint64_t n = 0;
    c->stop = 0;
    while (!c->stop && n < max) {
        c->insn_pc = c->pc;
        if (cpu68k_trace) c->hist[c->hidx++ & 31] = c->pc;
        c->ir = (uint16_t)fetch16(c);
        n++;
        if (!legal_tab[c->ir]) { illegal(c); continue; }
        switch (c->ir >> 12) {
        case 0x0: group0(c); break;
        case 0x1: move(c, 1); break;
        case 0x2: move(c, 4); break;
        case 0x3: move(c, 2); break;
        case 0x4: group4(c); break;
        case 0x5: group5(c); break;
        case 0x6: group6(c); break;
        case 0x7:                                        /* MOVEQ */
            if (c->ir & 0x100) { illegal(c); break; }
            c->d[(c->ir >> 9) & 7] = (uint32_t)(int8_t)c->ir;
            flags_logic(c, c->d[(c->ir >> 9) & 7], 4);
            break;
        case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: arith(c, c->ir >> 12); break;
        case 0xA:
            if (c->aline) c->aline(c, c->ir);
            else { c->pc = c->insn_pc; cpu68k_exception(c, VEC_LINE_A); }
            break;
        case 0xE: groupE(c); break;
        case 0xF:
            if (c->fpu && ((c->ir >> 9) & 7) == 1) fpu_exec(c);
            else { c->pc = c->insn_pc; cpu68k_exception(c, VEC_LINE_F); }
            break;
        }
    }
    c->insns += n;
    return n;
}

void cpu68k_dump(M68K *c, char *buf, int len) {
    snprintf(buf, len,
             "PC=%08X SR=%04X  D0-7 %08X %08X %08X %08X %08X %08X %08X %08X\n"
             "                  A0-7 %08X %08X %08X %08X %08X %08X %08X %08X",
             c->insn_pc, c->sr, c->d[0], c->d[1], c->d[2], c->d[3], c->d[4], c->d[5], c->d[6], c->d[7],
             c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5], c->a[6], c->a[7]);
}
