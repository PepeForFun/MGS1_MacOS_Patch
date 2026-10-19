/* x87 helpers used by the generated code (kept apart so tests can link them alone). */
#include "recomp.h"

/* ------------------------------------------------------------------ x87 helpers */
double rt_f80_load(uint32_t a) {
    uint64_t mant = R64(a);
    uint16_t se = R16(a + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void rt_f80_store(uint32_t a, double v) {
    uint64_t mant;
    uint16_t se;
    if (v == 0) {
        mant = 0;
        se = signbit(v) ? 0x8000 : 0;
    } else if (isinf(v)) {
        mant = 0x8000000000000000ull;
        se = (v < 0 ? 0x8000 : 0) | 0x7FFF;
    } else if (isnan(v)) {
        mant = 0xC000000000000000ull;
        se = 0xFFFF;
    } else {
        int e;
        double f = frexp(fabs(v), &e); /* f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);
        se = (uint16_t)((v < 0 ? 0x8000 : 0) | (e - 1 + 16383));
    }
    W64(a, mant);
    W16(a + 8, se);
}

void rt_fxam(CPU *c) {
    double v = ST(0);
    uint16_t sw = c->fsw & ~0x4700;
    if (signbit(v)) sw |= 0x0200;
    if (isnan(v)) sw |= 0x0100;
    else if (isinf(v)) sw |= 0x0500;
    else if (v == 0) sw |= 0x4000;
    else sw |= 0x0400; /* normal */
    c->fsw = sw;
}

void rt_fprem(CPU *c, int ieee) {
    double a = ST(0), b = ST(1);
    ST(0) = ieee ? remainder(a, b) : fmod(a, b);
    c->fsw &= ~0x0400; /* C2 = 0: reduction complete */
}

void rt_fxtract(CPU *c) {
    double v = ST(0);
    int e;
    double f = frexp(v, &e);
    ST(0) = (double)(e - 1);
    fpush(c, f * 2);
}

void rt_fnstenv(CPU *c, uint32_t a) {
    W32(a, c->fcw | 0xFFFF0000u);
    W32(a + 4, fstsw(c) | 0xFFFF0000u);
    W32(a + 8, c->ftag | 0xFFFF0000u);
    for (int i = 12; i < 28; i += 4) W32(a + i, 0);
}
void rt_fldenv(CPU *c, uint32_t a) {
    c->fcw = R16(a);
    uint16_t sw = R16(a + 4);
    c->fsw = sw & ~0x3800;
    c->ftop = (sw >> 11) & 7;
    c->ftag = R16(a + 8);
}
void rt_fnsave(CPU *c, uint32_t a) {
    rt_fnstenv(c, a);
    for (int i = 0; i < 8; i++) rt_f80_store(a + 28 + 10 * i, ST(i));
    fninit(c);
}
void rt_frstor(CPU *c, uint32_t a) {
    rt_fldenv(c, a);
    for (int i = 0; i < 8; i++) ST(i) = rt_f80_load(a + 28 + 10 * i);
}
