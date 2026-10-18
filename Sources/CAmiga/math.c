/* The math libraries, from their autodocs:
 *   mathieeedoubbas / mathieeedoubtrans : IEEE double, in D0/D1 (high, low)
 *   mathieeesingbas / mathieeesingtrans : IEEE single, in D0
 *   mathffp / mathtrans                 : Motorola Fast Floating Point, in D0
 * All computed with the host's floating point. Operand order follows the
 * libraries: the first register operand is the left one (Sub, Div, Pow, Cmp).
 * Cmp and Tst also set the condition codes, as assembly callers test them. */
#include "os.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t MathIeeeDoubBasBase, MathIeeeDoubTransBase, MathIeeeSingBasBase, MathIeeeSingTransBase, MathBase, MathTransBase;

/* ---- formats ---------------------------------------------------------------- */

static double dp(uint32_t hi, uint32_t lo) { uint64_t b = (uint64_t)hi << 32 | lo; double v; memcpy(&v, &b, 8); return v; }
static uint32_t ret_dp(M68K *c, double v) { uint64_t b; memcpy(&b, &v, 8); c->d[1] = (uint32_t)b; return (uint32_t)(b >> 32); }
static double sp(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t to_sp(double v) { float f = (float)v; uint32_t b; memcpy(&b, &f, 4); return b; }

/* FFP: 24-bit mantissa (bits 31-8, top bit set), sign bit 7, exponent bits 6-0 excess 64 */
static double ffp(uint32_t b) {
    uint32_t m = b >> 8;
    if (!m) return 0.0;
    double v = ldexp((double)m, (int)(b & 0x7F) - 64 - 24);
    return (b & 0x80) ? -v : v;
}
static uint32_t to_ffp(double v) {
    if (v == 0 || isnan(v)) return 0;
    uint32_t s = v < 0 ? 0x80 : 0;
    if (isinf(v)) return 0xFFFFFF7Fu | s;
    int e;
    double m = frexp(fabs(v), &e);
    uint32_t mant = (uint32_t)(m * 16777216.0 + 0.5);
    if (mant >= 0x1000000) { mant >>= 1; e++; }
    int ex = e + 64;
    if (ex > 127) return 0xFFFFFF7Fu | s;                  /* overflow: the largest value */
    if (ex < 0) return 0;
    return mant << 8 | s | (uint32_t)ex;
}

static uint32_t set_cc(M68K *c, int32_t r) {
    c->sr &= ~0x0Fu;
    if (r == 0) c->sr |= CCR_Z;
    else if (r < 0) c->sr |= CCR_N;
    return (uint32_t)r;
}
static int32_t cmp(double a, double b) { return a > b ? 1 : a < b ? -1 : 0; }
static int32_t fix(double v) {
    if (isnan(v)) return 0;
    if (v >= 2147483647.0) return 0x7FFFFFFF;
    if (v <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)v;
}

/* ---- IEEE double ---------------------------------------------------------------- */

#define D0 dp(c->d[0], c->d[1])
#define D2 dp(c->d[2], c->d[3])
static uint32_t dFix(M68K *c) { return (uint32_t)fix(D0); }
static uint32_t dFlt(M68K *c) { return ret_dp(c, (double)(int32_t)c->d[0]); }
static uint32_t dCmp(M68K *c) { return set_cc(c, cmp(D0, D2)); }
static uint32_t dTst(M68K *c) { return set_cc(c, cmp(D0, 0)); }
static uint32_t dAbs(M68K *c) { return ret_dp(c, fabs(D0)); }
static uint32_t dNeg(M68K *c) { return ret_dp(c, -D0); }
static uint32_t dAdd(M68K *c) { return ret_dp(c, D0 + D2); }
static uint32_t dSub(M68K *c) { return ret_dp(c, D0 - D2); }
static uint32_t dMul(M68K *c) { return ret_dp(c, D0 * D2); }
static uint32_t dDiv(M68K *c) { return ret_dp(c, D0 / D2); }
static uint32_t dFloor(M68K *c) { return ret_dp(c, floor(D0)); }
static uint32_t dCeil(M68K *c) { return ret_dp(c, ceil(D0)); }
static uint32_t dAtan(M68K *c) { return ret_dp(c, atan(D0)); }
static uint32_t dSin(M68K *c) { return ret_dp(c, sin(D0)); }
static uint32_t dCos(M68K *c) { return ret_dp(c, cos(D0)); }
static uint32_t dTan(M68K *c) { return ret_dp(c, tan(D0)); }
static uint32_t dSincos(M68K *c) {
    double x = D0, cs = cos(x);
    uint64_t b;
    memcpy(&b, &cs, 8);
    wr32(c->a[0], (uint32_t)(b >> 32));
    wr32(c->a[0] + 4, (uint32_t)b);
    return ret_dp(c, sin(x));
}
static uint32_t dSinh(M68K *c) { return ret_dp(c, sinh(D0)); }
static uint32_t dCosh(M68K *c) { return ret_dp(c, cosh(D0)); }
static uint32_t dTanh(M68K *c) { return ret_dp(c, tanh(D0)); }
static uint32_t dExp(M68K *c) { return ret_dp(c, exp(D0)); }
static uint32_t dLog(M68K *c) { return ret_dp(c, log(D0)); }
static uint32_t dPow(M68K *c) { return ret_dp(c, pow(D0, D2)); }     /* arg (D0/D1) ^ exp (D2/D3) */
static uint32_t dSqrt(M68K *c) { return ret_dp(c, sqrt(D0)); }
static uint32_t dTieee(M68K *c) { return to_sp(D0); }
static uint32_t dFieee(M68K *c) { return ret_dp(c, sp(c->d[0])); }
static uint32_t dAsin(M68K *c) { return ret_dp(c, asin(D0)); }
static uint32_t dAcos(M68K *c) { return ret_dp(c, acos(D0)); }
static uint32_t dLog10(M68K *c) { return ret_dp(c, log10(D0)); }

static const NativeEntry dbas[] = {
    { "IEEEDPFix", dFix }, { "IEEEDPFlt", dFlt }, { "IEEEDPCmp", dCmp }, { "IEEEDPTst", dTst },
    { "IEEEDPAbs", dAbs }, { "IEEEDPNeg", dNeg }, { "IEEEDPAdd", dAdd }, { "IEEEDPSub", dSub },
    { "IEEEDPMul", dMul }, { "IEEEDPDiv", dDiv }, { "IEEEDPFloor", dFloor }, { "IEEEDPCeil", dCeil },
    { 0, 0 }
};
static const NativeEntry dtrans[] = {
    { "IEEEDPAtan", dAtan }, { "IEEEDPSin", dSin }, { "IEEEDPCos", dCos }, { "IEEEDPTan", dTan },
    { "IEEEDPSincos", dSincos }, { "IEEEDPSinh", dSinh }, { "IEEEDPCosh", dCosh }, { "IEEEDPTanh", dTanh },
    { "IEEEDPExp", dExp }, { "IEEEDPLog", dLog }, { "IEEEDPPow", dPow }, { "IEEEDPSqrt", dSqrt },
    { "IEEEDPTieee", dTieee }, { "IEEEDPFieee", dFieee }, { "IEEEDPAsin", dAsin }, { "IEEEDPAcos", dAcos },
    { "IEEEDPLog10", dLog10 },
    { 0, 0 }
};

/* ---- IEEE single ------------------------------------------------------------------ */

#define S0 sp(c->d[0])
#define S1 sp(c->d[1])
static uint32_t sFix(M68K *c) { return (uint32_t)fix(S0); }
static uint32_t sFlt(M68K *c) { return to_sp((double)(int32_t)c->d[0]); }
static uint32_t sCmp(M68K *c) { return set_cc(c, cmp(S0, S1)); }
static uint32_t sTst(M68K *c) { return set_cc(c, cmp(S0, 0)); }
static uint32_t sAbs(M68K *c) { return to_sp(fabs(S0)); }
static uint32_t sNeg(M68K *c) { return to_sp(-S0); }
static uint32_t sAdd(M68K *c) { return to_sp(S0 + S1); }
static uint32_t sSub(M68K *c) { return to_sp(S0 - S1); }
static uint32_t sMul(M68K *c) { return to_sp(S0 * S1); }
static uint32_t sDiv(M68K *c) { return to_sp(S0 / S1); }
static uint32_t sFloor(M68K *c) { return to_sp(floor(S0)); }
static uint32_t sCeil(M68K *c) { return to_sp(ceil(S0)); }
static uint32_t sAtan(M68K *c) { return to_sp(atan(S0)); }
static uint32_t sSin(M68K *c) { return to_sp(sin(S0)); }
static uint32_t sCos(M68K *c) { return to_sp(cos(S0)); }
static uint32_t sTan(M68K *c) { return to_sp(tan(S0)); }
static uint32_t sSincos(M68K *c) { wr32(c->a[0], to_sp(cos(S0))); return to_sp(sin(S0)); }
static uint32_t sSinh(M68K *c) { return to_sp(sinh(S0)); }
static uint32_t sCosh(M68K *c) { return to_sp(cosh(S0)); }
static uint32_t sTanh(M68K *c) { return to_sp(tanh(S0)); }
static uint32_t sExp(M68K *c) { return to_sp(exp(S0)); }
static uint32_t sLog(M68K *c) { return to_sp(log(S0)); }
static uint32_t sPow(M68K *c) { return to_sp(pow(S0, S1)); }         /* arg (D0) ^ exp (D1) */
static uint32_t sSqrt(M68K *c) { return to_sp(sqrt(S0)); }
static uint32_t sIdent(M68K *c) { return c->d[0]; }
static uint32_t sAsin(M68K *c) { return to_sp(asin(S0)); }
static uint32_t sAcos(M68K *c) { return to_sp(acos(S0)); }
static uint32_t sLog10(M68K *c) { return to_sp(log10(S0)); }

static const NativeEntry sbas[] = {
    { "IEEESPFix", sFix }, { "IEEESPFlt", sFlt }, { "IEEESPCmp", sCmp }, { "IEEESPTst", sTst },
    { "IEEESPAbs", sAbs }, { "IEEESPNeg", sNeg }, { "IEEESPAdd", sAdd }, { "IEEESPSub", sSub },
    { "IEEESPMul", sMul }, { "IEEESPDiv", sDiv }, { "IEEESPFloor", sFloor }, { "IEEESPCeil", sCeil },
    { 0, 0 }
};
static const NativeEntry strans[] = {
    { "IEEESPAtan", sAtan }, { "IEEESPSin", sSin }, { "IEEESPCos", sCos }, { "IEEESPTan", sTan },
    { "IEEESPSincos", sSincos }, { "IEEESPSinh", sSinh }, { "IEEESPCosh", sCosh }, { "IEEESPTanh", sTanh },
    { "IEEESPExp", sExp }, { "IEEESPLog", sLog }, { "IEEESPPow", sPow }, { "IEEESPSqrt", sSqrt },
    { "IEEESPTieee", sIdent }, { "IEEESPFieee", sIdent }, { "IEEESPAsin", sAsin }, { "IEEESPAcos", sAcos },
    { "IEEESPLog10", sLog10 },
    { 0, 0 }
};

/* ---- Motorola FFP -------------------------------------------------------------------- */

#define F0 ffp(c->d[0])
#define F1 ffp(c->d[1])
static uint32_t fFix(M68K *c) { return (uint32_t)fix(F0); }
static uint32_t fFlt(M68K *c) { return to_ffp((double)(int32_t)c->d[0]); }
static uint32_t fCmp(M68K *c) { return set_cc(c, cmp(F0, F1)); }
static uint32_t fTst(M68K *c) { return set_cc(c, cmp(F1, 0)); }          /* SPTst takes D1 */
static uint32_t fAbs(M68K *c) { return to_ffp(fabs(F0)); }
static uint32_t fNeg(M68K *c) { return to_ffp(-F0); }
static uint32_t fAdd(M68K *c) { return to_ffp(F0 + F1); }
static uint32_t fSub(M68K *c) { return to_ffp(F0 - F1); }
static uint32_t fMul(M68K *c) { return to_ffp(F0 * F1); }
static uint32_t fDiv(M68K *c) { return to_ffp(F0 / F1); }
static uint32_t fFloor(M68K *c) { return to_ffp(floor(F0)); }
static uint32_t fCeil(M68K *c) { return to_ffp(ceil(F0)); }
static uint32_t fAtan(M68K *c) { return to_ffp(atan(F0)); }
static uint32_t fSin(M68K *c) { return to_ffp(sin(F0)); }
static uint32_t fCos(M68K *c) { return to_ffp(cos(F0)); }
static uint32_t fTan(M68K *c) { return to_ffp(tan(F0)); }
static uint32_t fSincos(M68K *c) { wr32(c->d[1], to_ffp(cos(F0))); return to_ffp(sin(F0)); }
static uint32_t fSinh(M68K *c) { return to_ffp(sinh(F0)); }
static uint32_t fCosh(M68K *c) { return to_ffp(cosh(F0)); }
static uint32_t fTanh(M68K *c) { return to_ffp(tanh(F0)); }
static uint32_t fExp(M68K *c) { return to_ffp(exp(F0)); }
static uint32_t fLog(M68K *c) { return to_ffp(log(F0)); }
static uint32_t fPow(M68K *c) { return to_ffp(pow(F0, F1)); }
static uint32_t fSqrt(M68K *c) { return to_ffp(sqrt(F0)); }
static uint32_t fTieee(M68K *c) { return to_sp(F0); }
static uint32_t fFieee(M68K *c) { return to_ffp(sp(c->d[0])); }
static uint32_t fAsin(M68K *c) { return to_ffp(asin(F0)); }
static uint32_t fAcos(M68K *c) { return to_ffp(acos(F0)); }
static uint32_t fLog10(M68K *c) { return to_ffp(log10(F0)); }

static const NativeEntry ffpbas[] = {
    { "SPFix", fFix }, { "SPFlt", fFlt }, { "SPCmp", fCmp }, { "SPTst", fTst }, { "SPAbs", fAbs },
    { "SPNeg", fNeg }, { "SPAdd", fAdd }, { "SPSub", fSub }, { "SPMul", fMul }, { "SPDiv", fDiv },
    { "SPFloor", fFloor }, { "SPCeil", fCeil },
    { 0, 0 }
};
static const NativeEntry ffptrans[] = {
    { "SPAtan", fAtan }, { "SPSin", fSin }, { "SPCos", fCos }, { "SPTan", fTan }, { "SPSincos", fSincos },
    { "SPSinh", fSinh }, { "SPCosh", fCosh }, { "SPTanh", fTanh }, { "SPExp", fExp }, { "SPLog", fLog },
    { "SPPow", fPow }, { "SPSqrt", fSqrt }, { "SPTieee", fTieee }, { "SPFieee", fFieee }, { "SPAsin", fAsin },
    { "SPAcos", fAcos }, { "SPLog10", fLog10 },
    { 0, 0 }
};

static uint32_t mk(const char *name, const char *fd, const NativeEntry *n) {
    char id[80];
    snprintf(id, sizeof id, "%.30s 40.1 (AmigaMac)", name);
    uint32_t b = lib_create(name, fd, 64, n, 40, 1, strdup(id));
    lib_add(b);
    return b;
}

void math_init(void) {
    MathIeeeDoubBasBase = mk("mathieeedoubbas.library", "mathieeedoubbas", dbas);
    MathIeeeDoubTransBase = mk("mathieeedoubtrans.library", "mathieeedoubtrans", dtrans);
    MathIeeeSingBasBase = mk("mathieeesingbas.library", "mathieeesingbas", sbas);
    MathIeeeSingTransBase = mk("mathieeesingtrans.library", "mathieeesingtrans", strans);
    MathBase = mk("mathffp.library", "mathffp", ffpbas);
    MathTransBase = mk("mathtrans.library", "mathtrans", ffptrans);
}
