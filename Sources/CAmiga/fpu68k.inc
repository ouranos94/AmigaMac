/* 68881/68882 floating point unit, written from the MC68881/MC68882 user's
 * manual. Included by cpu68k.c (it uses the core's effective address code).
 *
 * Registers are kept as host doubles: the 80-bit extended format is
 * converted on the way in and out of memory, so results carry 53 bits of
 * mantissa instead of 64. Exceptions are never trapped (no program enables
 * them on AmigaOS); the condition codes, quotient byte and accrued bits that
 * programs test are maintained. FSAVE/FRESTORE use the null state frame. */
#include <math.h>

#define FPCC_N 0x08000000u
#define FPCC_Z 0x04000000u
#define FPCC_I 0x02000000u
#define FPCC_NAN 0x01000000u

enum { FMT_L, FMT_S, FMT_X, FMT_P, FMT_W, FMT_D, FMT_B, FMT_PK };
static const int fmt_size[8] = { 4, 4, 12, 12, 2, 8, 1, 12 };

static void fp_setcc(M68K *c, double v) {
    uint32_t cc = 0;
    if (isnan(v)) cc |= FPCC_NAN;
    else if (isinf(v)) cc |= FPCC_I;
    else if (v == 0) cc |= FPCC_Z;
    if (signbit(v)) cc |= FPCC_N;
    c->fpsr = (c->fpsr & 0x00FFFFFFu) | cc;
}

/* rounding precision of FPCR: extended (kept as double), single, double */
static double fp_prec(M68K *c, double v) {
    return ((c->fpcr >> 6) & 3) == 1 ? (double)(float)v : v;
}

/* rounding mode of FPCR, for FINT and conversions to integers */
static double fp_rint(M68K *c, double v) {
    switch ((c->fpcr >> 4) & 3) {
    case 1: return trunc(v);
    case 2: return floor(v);
    case 3: return ceil(v);
    }
    return nearbyint(v);                                /* host default: to nearest even */
}

static int32_t fp_to_int(M68K *c, double v, int sz) {
    double lo = sz == 1 ? -128.0 : sz == 2 ? -32768.0 : -2147483648.0;
    double hi = sz == 1 ? 127.0 : sz == 2 ? 32767.0 : 2147483647.0;
    if (isnan(v)) { c->fpsr |= 0x2000 | 0x80; return (int32_t)hi; }      /* OPERR, AIOP */
    v = fp_rint(c, v);
    if (v < lo) { c->fpsr |= 0x2000 | 0x80; return (int32_t)lo; }
    if (v > hi) { c->fpsr |= 0x2000 | 0x80; return (int32_t)hi; }
    return (int32_t)v;
}

/* ---- the extended and packed decimal formats -------------------------------- */

static double x_to_d(uint32_t w0, uint32_t m0, uint32_t m1) {
    int sign = w0 >> 31, exp = (w0 >> 16) & 0x7FFF;
    uint64_t man = (uint64_t)m0 << 32 | m1;
    double v;
    if (exp == 0x7FFF) v = (man << 1) ? NAN : INFINITY;
    else if (!man) v = 0;
    else v = ldexp((double)man, exp - 16383 - 63);
    return sign ? -v : v;
}

static void d_to_x(double v, uint32_t *w0, uint32_t *m0, uint32_t *m1) {
    uint32_t s = signbit(v) ? 0x80000000u : 0;
    uint64_t man = 0;
    int exp = 0;
    if (isnan(v)) { exp = 0x7FFF; man = 0xFFFFFFFFFFFFFFFFull; }
    else if (isinf(v)) exp = 0x7FFF;
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);                    /* [0.5, 1) */
        man = (uint64_t)ldexp(m, 64);
        exp = e - 1 + 16383;
        if (exp <= 0) { man >>= 1 - exp; exp = 0; }      /* denormal */
    }
    *w0 = s | (uint32_t)exp << 16;
    *m0 = (uint32_t)(man >> 32);
    *m1 = (uint32_t)man;
}

static double p_to_d(uint32_t w0, uint32_t w1, uint32_t w2) {
    if (((w0 >> 16) & 0x7FFF) == 0x7FFF) return (w1 | w2) ? NAN : (w0 >> 31 ? -INFINITY : INFINITY);
    char s[64];
    int k = 0;
    if (w0 >> 31) s[k++] = '-';
    s[k++] = (char)('0' + (w0 & 0xF));
    s[k++] = '.';
    for (int i = 0; i < 8; i++) s[k++] = (char)('0' + ((w1 >> (28 - 4 * i)) & 0xF));
    for (int i = 0; i < 8; i++) s[k++] = (char)('0' + ((w2 >> (28 - 4 * i)) & 0xF));
    s[k++] = 'e';
    if (w0 & 0x40000000) s[k++] = '-';
    s[k++] = (char)('0' + ((w0 >> 24) & 0xF));
    s[k++] = (char)('0' + ((w0 >> 20) & 0xF));
    s[k++] = (char)('0' + ((w0 >> 16) & 0xF));
    s[k] = 0;
    return strtod(s, 0);
}

/* k > 0: significant digits; k <= 0: digits right of the decimal point */
static void d_to_p(double v, int k, uint32_t *w0, uint32_t *w1, uint32_t *w2) {
    *w0 = *w1 = *w2 = 0;
    if (signbit(v)) *w0 |= 0x80000000u;
    if (isnan(v) || isinf(v)) { *w0 |= 0x7FFF0000u; if (isnan(v)) *w1 = 0xFFFFFFFFu; return; }
    if (v == 0) return;
    int e10 = (int)floor(log10(fabs(v)));
    int digits = k > 0 ? k : e10 + 1 - k;
    if (digits < 1) digits = 1;
    if (digits > 17) digits = 17;
    char s[64];
    snprintf(s, sizeof s, "%.*e", digits - 1, fabs(v));
    char *ep = strchr(s, 'e');
    int ex = atoi(ep + 1);
    *w0 |= (uint32_t)(s[0] - '0');
    int n = 0;
    for (char *p = s + 2; p < ep && n < 16; p++, n++) {
        uint32_t d = (uint32_t)(*p - '0');
        if (n < 8) *w1 |= d << (28 - 4 * n); else *w2 |= d << (28 - 4 * (n - 8));
    }
    if (ex < 0) { *w0 |= 0x40000000u; ex = -ex; }
    *w0 |= (uint32_t)(ex / 100 % 10) << 24 | (uint32_t)(ex / 10 % 10) << 20 | (uint32_t)(ex % 10) << 16;
}

/* ---- operands --------------------------------------------------------------- */

/* the address of a memory operand of the given size (immediates point into the code) */
static int fp_addr(M68K *c, int mode, int reg, int size, uint32_t *addr) {
    switch (mode) {
    case 2: *addr = c->a[reg]; return 1;
    case 3: *addr = c->a[reg]; c->a[reg] += (reg == 7 && size == 1) ? 2 : (uint32_t)size; return 1;
    case 4: c->a[reg] -= (reg == 7 && size == 1) ? 2 : (uint32_t)size; *addr = c->a[reg]; return 1;
    case 7:
        if (reg == 4) {                                   /* #imm */
            *addr = c->pc + (size == 1 ? 1 : 0);
            c->pc += size == 1 ? 2 : (uint32_t)size;
            return 1;
        }
        if (reg > 4) return 0;
        /* fall through */
    default: {
        if (mode < 2) return 0;
        EA e = ea_decode(c, mode, reg, 4);
        *addr = e.addr;
        return 1;
    }
    }
}

static int fp_read(M68K *c, int mode, int reg, int fmt, double *v) {
    if (mode == 0) {                                      /* Dn: integer and single formats only */
        uint32_t d = c->d[reg];
        switch (fmt) {
        case FMT_L: *v = (int32_t)d; return 1;
        case FMT_W: *v = (int16_t)d; return 1;
        case FMT_B: *v = (int8_t)d; return 1;
        case FMT_S: { float f; memcpy(&f, &d, 4); *v = f; return 1; }
        }
        return 0;
    }
    if (mode == 1) return 0;
    uint32_t a;
    if (!fp_addr(c, mode, reg, fmt_size[fmt], &a)) return 0;
    switch (fmt) {
    case FMT_L: *v = (int32_t)rd32(a); break;
    case FMT_W: *v = (int16_t)rd16(a); break;
    case FMT_B: *v = (int8_t)rd8(a); break;
    case FMT_S: { uint32_t b = rd32(a); float f; memcpy(&f, &b, 4); *v = f; break; }
    case FMT_D: { uint64_t b = (uint64_t)rd32(a) << 32 | rd32(a + 4); memcpy(v, &b, 8); break; }
    case FMT_X: *v = x_to_d(rd32(a), rd32(a + 4), rd32(a + 8)); break;
    default: *v = p_to_d(rd32(a), rd32(a + 4), rd32(a + 8)); break;
    }
    return 1;
}

static int fp_write(M68K *c, int mode, int reg, int fmt, double v, int k) {
    if (mode == 0) {
        switch (fmt) {
        case FMT_L: c->d[reg] = (uint32_t)fp_to_int(c, v, 4); return 1;
        case FMT_W: c->d[reg] = (c->d[reg] & 0xFFFF0000u) | ((uint32_t)fp_to_int(c, v, 2) & 0xFFFF); return 1;
        case FMT_B: c->d[reg] = (c->d[reg] & 0xFFFFFF00u) | ((uint32_t)fp_to_int(c, v, 1) & 0xFF); return 1;
        case FMT_S: { float f = (float)v; memcpy(&c->d[reg], &f, 4); return 1; }
        }
        return 0;
    }
    if (mode == 1 || (mode == 7 && reg > 1)) return 0;
    uint32_t a;
    if (!fp_addr(c, mode, reg, fmt_size[fmt], &a)) return 0;
    switch (fmt) {
    case FMT_L: wr32(a, (uint32_t)fp_to_int(c, v, 4)); break;
    case FMT_W: wr16(a, (uint32_t)fp_to_int(c, v, 2) & 0xFFFF); break;
    case FMT_B: wr8(a, (uint32_t)fp_to_int(c, v, 1) & 0xFF); break;
    case FMT_S: { float f = (float)v; uint32_t b; memcpy(&b, &f, 4); wr32(a, b); break; }
    case FMT_D: { uint64_t b; memcpy(&b, &v, 8); wr32(a, (uint32_t)(b >> 32)); wr32(a + 4, (uint32_t)b); break; }
    case FMT_X: { uint32_t w0, m0, m1; d_to_x(v, &w0, &m0, &m1); wr32(a, w0); wr32(a + 4, m0); wr32(a + 8, m1); break; }
    default: { uint32_t w0, w1, w2; d_to_p(v, k, &w0, &w1, &w2); wr32(a, w0); wr32(a + 4, w1); wr32(a + 8, w2); break; }
    }
    return 1;
}

/* ---- arithmetic --------------------------------------------------------------- */

static double fp_const(int n) {
    switch (n) {
    case 0x00: return M_PI;
    case 0x0B: return log10(2.0);
    case 0x0C: return M_E;
    case 0x0D: return M_LOG2E;
    case 0x0E: return M_LOG10E;
    case 0x0F: return 0.0;
    case 0x30: return M_LN2;
    case 0x31: return M_LN10;
    }
    if (n == 0x32) return 1.0;
    if (n > 0x32 && n <= 0x3F) return pow(10.0, (double)(1 << (n - 0x33)));   /* 1e1, 1e2, 1e4 ... 1e4096 */
    return 0.0;
}

static void fp_quotient(M68K *c, double q) {
    uint32_t qb = (uint32_t)(fmod(fabs(q), 128.0)) & 0x7F;
    if (signbit(q)) qb |= 0x80;
    c->fpsr = (c->fpsr & 0xFF00FFFFu) | qb << 16;
}

static int fp_arith(M68K *c, int op, double src, int dst) {
    double d = c->fp[dst], r;
    int single = 0, dbl = 0;
    if (op >= 0x40) {                                     /* 68040 FSxxx/FDxxx forms */
        single = !(op & 4);
        dbl = !!(op & 4);
    }
    switch (op) {
    case 0x00: case 0x40: case 0x44: r = src; break;
    case 0x01: r = fp_rint(c, src); break;                                    /* FINT */
    case 0x02: r = sinh(src); break;
    case 0x03: r = trunc(src); break;                                         /* FINTRZ */
    case 0x04: case 0x41: case 0x45: r = sqrt(src); break;
    case 0x06: r = log1p(src); break;
    case 0x08: r = expm1(src); break;
    case 0x09: r = tanh(src); break;
    case 0x0A: r = atan(src); break;
    case 0x0C: r = asin(src); break;
    case 0x0D: r = atanh(src); break;
    case 0x0E: r = sin(src); break;
    case 0x0F: r = tan(src); break;
    case 0x10: r = exp(src); break;
    case 0x11: r = exp2(src); break;
    case 0x12: r = pow(10.0, src); break;
    case 0x14: r = log(src); break;
    case 0x15: r = log10(src); break;
    case 0x16: r = log2(src); break;
    case 0x18: case 0x58: case 0x5C: r = fabs(src); break;
    case 0x19: r = cosh(src); break;
    case 0x1A: case 0x5A: case 0x5E: r = -src; break;
    case 0x1C: r = acos(src); break;
    case 0x1D: r = cos(src); break;
    case 0x1E: r = src == 0 ? src : isinf(src) || isnan(src) ? NAN : (double)ilogb(src); break;
    case 0x1F: {
        if (src == 0 || isnan(src)) r = src;
        else if (isinf(src)) r = NAN;
        else { int e; r = frexp(src, &e) * 2.0; }
        break;
    }
    case 0x20: case 0x60: case 0x64: r = d / src; break;
    case 0x21: r = fmod(d, src); fp_quotient(c, trunc(d / src)); break;
    case 0x22: case 0x62: case 0x66: r = d + src; break;
    case 0x23: case 0x63: case 0x67: r = d * src; break;
    case 0x24: r = (float)(d / src); break;                                   /* FSGLDIV */
    case 0x25: r = remainder(d, src); fp_quotient(c, nearbyint(d / src)); break;
    case 0x26: r = ldexp(d, (int)trunc(src)); break;                         /* FSCALE */
    case 0x27: r = (float)((float)d * (float)src); break;                    /* FSGLMUL */
    case 0x28: case 0x68: case 0x6C: r = d - src; break;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        c->fp[op & 7] = fp_prec(c, cos(src));                                 /* FSINCOS */
        r = sin(src);
        break;
    case 0x38: {                                                              /* FCMP */
        uint32_t cc = 0;
        if (isnan(d) || isnan(src)) cc = FPCC_NAN;
        else if (d == src) cc = FPCC_Z | (signbit(d) ? FPCC_N : 0);
        else if (d < src) cc = FPCC_N;
        c->fpsr = (c->fpsr & 0x00FFFFFFu) | cc;
        return 1;
    }
    case 0x3A: fp_setcc(c, src); return 1;                                    /* FTST */
    default: return 0;
    }
    if (single) r = (float)r;
    else if (!dbl) r = fp_prec(c, r);
    c->fp[dst] = r;
    fp_setcc(c, r);
    if (isnan(r) && !isnan(src) && !isnan(d)) c->fpsr |= 0x2000 | 0x80;     /* OPERR */
    return 1;
}

static int fp_cond(M68K *c, int pred) {
    uint32_t s = c->fpsr;
    int n = !!(s & FPCC_N), z = !!(s & FPCC_Z), nan = !!(s & FPCC_NAN);
    if ((pred & 0x10) && nan) c->fpsr |= 0x8000 | 0x80;    /* BSUN, AIOP */
    switch (pred & 15) {
    case 0: return 0;
    case 1: return z;
    case 2: return !(nan || z || n);
    case 3: return z || !(nan || n);
    case 4: return n && !(nan || z);
    case 5: return z || (n && !nan);
    case 6: return !(nan || z);
    case 7: return !nan;
    case 8: return nan;
    case 9: return nan || z;
    case 10: return nan || !(n || z);
    case 11: return nan || z || !n;
    case 12: return nan || (n && !z);
    case 13: return nan || z || n;
    case 14: return !z;
    default: return 1;
    }
}

/* ---- FMOVEM ------------------------------------------------------------------------ */

static int fp_movem_ctrl(M68K *c, int to_mem, int list, int mode, int reg) {
    uint32_t *regs[3] = { &c->fpcr, &c->fpsr, &c->fpiar };
    int n = !!(list & 4) + !!(list & 2) + !!(list & 1);
    if (!n) return 0;
    if (mode <= 1) {                                      /* Dn/An: a single register */
        if (n != 1 || (mode == 1 && list != 1)) return 0;
        uint32_t *r = regs[list == 4 ? 0 : list == 2 ? 1 : 2];
        uint32_t *g = mode ? &c->a[reg] : &c->d[reg];
        if (to_mem) *g = *r; else *r = *g;
        return 1;
    }
    if (to_mem && mode == 7 && reg > 1) return 0;
    uint32_t a;
    if (!fp_addr(c, mode, reg, 4 * n, &a)) return 0;
    for (int i = 0; i < 3; i++) {
        if (!(list & (4 >> i))) continue;
        if (to_mem) wr32(a, *regs[i]); else *regs[i] = rd32(a);
        a += 4;
    }
    c->fpcr &= 0xFFF0;
    c->fpsr &= 0x0FFFFFF8u;
    return 1;
}

static int fp_movem_data(M68K *c, int to_mem, uint32_t ext, int mode, int reg) {
    int lmode = (ext >> 11) & 3;
    int list = (lmode & 1) ? (int)(c->d[(ext >> 4) & 7] & 0xFF) : (int)(ext & 0xFF);
    int predec = !(lmode & 2);
    int n = __builtin_popcount((unsigned)list);
    uint32_t a;
    if (mode == 4) {
        if (!to_mem || !predec) return 0;
        c->a[reg] -= 12u * (uint32_t)n;
        a = c->a[reg];
    } else if (mode == 3) {
        if (to_mem || predec) return 0;
        a = c->a[reg];
        c->a[reg] += 12u * (uint32_t)n;
    } else {
        if (mode < 2 || (mode == 7 && reg > (to_mem ? 1 : 3))) return 0;
        EA e = ea_decode(c, mode, reg, 4);
        a = e.addr;
    }
    for (int i = 0; i < 8; i++) {
        int in = predec ? (list >> i) & 1 : (list >> (7 - i)) & 1;
        if (!in) continue;
        if (to_mem) { uint32_t w0, m0, m1; d_to_x(c->fp[i], &w0, &m0, &m1); wr32(a, w0); wr32(a + 4, m0); wr32(a + 8, m1); }
        else c->fp[i] = x_to_d(rd32(a), rd32(a + 4), rd32(a + 8));
        a += 12;
    }
    return 1;
}

/* ---- dispatch ------------------------------------------------------------------------ */

static void fp_unimpl(M68K *c) { c->pc = c->insn_pc; cpu68k_exception(c, VEC_LINE_F); }

static void fpu_general(M68K *c) {
    uint32_t ext = fetch16(c);
    int mode = (c->ir >> 3) & 7, reg = c->ir & 7;
    int opclass = ext >> 13, rx = (ext >> 10) & 7, ry = (ext >> 7) & 7;
    switch (opclass) {
    case 0: case 2: {
        double src;
        if (opclass == 2 && rx == 7) {                    /* FMOVECR */
            c->fp[ry] = fp_prec(c, fp_const(ext & 0x7F));
            fp_setcc(c, c->fp[ry]);
            return;
        }
        c->fpiar = c->insn_pc;
        if (opclass == 0) src = c->fp[rx];
        else if (!fp_read(c, mode, reg, rx, &src)) { fp_unimpl(c); return; }
        if (!fp_arith(c, (int)(ext & 0x7F), src, ry)) fp_unimpl(c);
        return;
    }
    case 3: {
        int k = 0;
        if (rx == FMT_P) k = (int8_t)(ext << 1) >> 1;
        else if (rx == FMT_PK) k = (int8_t)(c->d[(ext >> 4) & 7] << 1) >> 1;
        c->fpiar = c->insn_pc;
        if (!fp_write(c, mode, reg, rx == FMT_PK ? FMT_P : rx, c->fp[ry], k)) fp_unimpl(c);
        return;
    }
    case 4: case 5:
        if (!fp_movem_ctrl(c, opclass == 5, rx, mode, reg)) fp_unimpl(c);
        return;
    case 6: case 7:
        if (!fp_movem_data(c, opclass == 7, ext, mode, reg)) fp_unimpl(c);
        return;
    }
    fp_unimpl(c);
}

static void fpu_exec(M68K *c) {
    int type = (c->ir >> 6) & 7, mode = (c->ir >> 3) & 7, reg = c->ir & 7;
    switch (type) {
    case 0: fpu_general(c); return;
    case 1: {                                             /* FScc / FDBcc / FTRAPcc */
        uint32_t ext = fetch16(c);
        int t = fp_cond(c, (int)(ext & 0x3F));
        if (mode == 1) {                                  /* FDBcc */
            uint32_t base = c->pc;
            int32_t disp = (int16_t)fetch16(c);
            if (!t) {
                uint32_t w = (c->d[reg] - 1) & 0xFFFF;
                c->d[reg] = (c->d[reg] & 0xFFFF0000u) | w;
                if (w != 0xFFFF) c->pc = base + (uint32_t)disp;
            }
            return;
        }
        if (mode == 7 && reg >= 2 && reg <= 4) {          /* FTRAPcc */
            if (reg == 2) c->pc += 2; else if (reg == 3) c->pc += 4;
            if (t) cpu68k_exception(c, VEC_TRAPV);
            return;
        }
        if (mode == 1 || (mode == 7 && reg > 1)) { fp_unimpl(c); return; }
        EA e = ea_decode(c, mode, reg, 1);
        ea_write(c, &e, t ? 0xFF : 0, 1);
        return;
    }
    case 2: case 3: {                                     /* FBcc.W / FBcc.L (FNOP is FBF.W #0) */
        uint32_t base = c->pc;
        int32_t disp = type == 3 ? (int32_t)fetch32(c) : (int16_t)fetch16(c);
        if (fp_cond(c, c->ir & 0x3F)) c->pc = base + (uint32_t)disp;
        return;
    }
    case 4: {                                             /* FSAVE: the null frame */
        if (!need_super(c)) return;
        uint32_t a;
        if (mode == 3 || !fp_addr(c, mode, reg, 4, &a)) { fp_unimpl(c); return; }
        wr32(a, 0);
        return;
    }
    case 5: {                                             /* FRESTORE */
        if (!need_super(c)) return;
        uint32_t a;
        if (mode == 4 || !fp_addr(c, mode, reg, 4, &a)) { fp_unimpl(c); return; }
        uint32_t f = rd32(a);
        if (!(f >> 24)) {                                 /* null frame: reset */
            for (int i = 0; i < 8; i++) c->fp[i] = NAN;
            c->fpcr = c->fpsr = c->fpiar = 0;
        } else if (mode == 3) c->a[reg] += ((f >> 16) & 0xFF);   /* skip an idle frame */
        return;
    }
    }
    fp_unimpl(c);
}
