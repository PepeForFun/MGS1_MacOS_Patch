/* Shared definitions for the generated code: CPU state, guest memory access,
 * x87 and MMX helpers. Guest addresses are 32-bit offsets into M. */
#pragma once
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

typedef union {
    uint32_t d;
    uint16_t w;
    struct { uint8_t l, h; } b;
} GReg;

typedef struct CPU {
    GReg r[8];
    uint32_t df;        /* direction flag */
    uint32_t fs_base;   /* guest address of this thread's TEB */
    double st[8];       /* x87 stack, physical slots */
    uint32_t ftop;
    uint16_t fcw, fsw, ftag, pad;
    uint64_t mm[8];     /* MMX (kept apart from x87) */
    uint32_t eip;       /* start of the last block entered (debugging) */
    uint32_t depth;
    void *thread;
} CPU;

/* Structured exception handling: a function with an MSVC __try/__except frame registers a landing
 * (sigsetjmp) on entry; a fault inside the frame resumes in its __except handler (seh_dispatch) */
typedef struct SehLanding {
    sigjmp_buf jb;
    uint32_t frame;    /* the function's EBP */
    uint32_t target;   /* handler address after a fault */
} SehLanding;
void seh_push(SehLanding *l);

extern uint8_t *M;

#define EAX (c->r[0].d)
#define ECX (c->r[1].d)
#define EDX (c->r[2].d)
#define EBX (c->r[3].d)
#define ESP (c->r[4].d)
#define EBP (c->r[5].d)
#define ESI (c->r[6].d)
#define EDI (c->r[7].d)

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;

static inline uint8_t R8(uint32_t a) { return *(volatile uint8_t *)(M + a); }
static inline uint16_t R16(uint32_t a) { return *(volatile u16u *)(M + a); }
static inline uint32_t R32(uint32_t a) { return *(volatile u32u *)(M + a); }
static inline uint64_t R64(uint32_t a) { return *(volatile u64u *)(M + a); }
static inline void W8(uint32_t a, uint8_t v) { *(volatile uint8_t *)(M + a) = v; }
static inline void W16(uint32_t a, uint16_t v) { *(volatile u16u *)(M + a) = v; }
static inline void W32(uint32_t a, uint32_t v) { *(volatile u32u *)(M + a) = v; }
static inline void W64(uint32_t a, uint64_t v) { *(volatile u64u *)(M + a) = v; }
static inline float RF32(uint32_t a) { uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; }
static inline double RF64(uint32_t a) { uint64_t v = R64(a); double d; memcpy(&d, &v, 8); return d; }
static inline void WF32(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); W32(a, v); }
static inline void WF64(uint32_t a, double d) { uint64_t v; memcpy(&v, &d, 8); W64(a, v); }

#define PUSH32(v) do { uint32_t _v = (v); ESP -= 4; W32(ESP, _v); } while (0)
#define PUSH16(v) do { uint16_t _v = (v); ESP -= 2; W16(ESP, _v); } while (0)
#define BLK(a) (c->eip = (a))

static inline uint32_t PF8(uint32_t x) { return !__builtin_parity(x & 0xFF); }

/* ---- runtime services used by generated code (runtime sources) */
void call_indirect(CPU *c, uint32_t target);
__attribute__((noreturn)) void rt_trap(CPU *c, uint32_t addr, const char *why);
__attribute__((noreturn)) void rt_div_fault(CPU *c, uint32_t addr);
void rt_cpuid(CPU *c);
void rt_rdtsc(CPU *c);

/* ---- x87 */
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(CPU *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline double fpop(CPU *c) { double v = c->st[c->ftop]; c->ftop = (c->ftop + 1) & 7; return v; }
static inline void fcom_sw(CPU *c, double a, double b) {
    uint16_t sw = c->fsw & ~0x4500;
    if (a != a || b != b) sw |= 0x4500;
    else if (a < b) sw |= 0x0100;
    else if (a == b) sw |= 0x4000;
    c->fsw = sw;
}
static inline uint16_t fstsw(CPU *c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)); }
static inline double fround(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline uint32_t fist32(CPU *c, double v) {
    double r = fround(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static inline uint16_t fist16(CPU *c, double v) {
    double r = fround(c, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000u;
    return (uint16_t)(int16_t)r;
}
static inline uint64_t fist64(CPU *c, double v) {
    double r = fround(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return 0x8000000000000000ull;
    return (uint64_t)(int64_t)r;
}
double rt_f80_load(uint32_t a);
void rt_f80_store(uint32_t a, double v);
void rt_fxam(CPU *c);
void rt_fprem(CPU *c, int ieee);
void rt_fxtract(CPU *c);
void rt_fnstenv(CPU *c, uint32_t a);
void rt_fldenv(CPU *c, uint32_t a);
void rt_fnsave(CPU *c, uint32_t a);
void rt_frstor(CPU *c, uint32_t a);
static inline void fninit(CPU *c) { c->fcw = 0x37F; c->fsw = 0; c->ftop = 0; c->ftag = 0xFFFF; }

/* ---- MMX, lane-wise on uint64_t */
#define MMX_LANES(T, N, EXPR) \
    uint64_t r = 0; \
    for (int i = 0; i < N; i++) { \
        T x = (T)(a >> (i * (64 / N))), y = (T)(b >> (i * (64 / N))); \
        (void)y; \
        r |= ((uint64_t)(EXPR) & (((uint64_t)1 << (64 / N)) - 1 | (N == 1 ? ~0ull : 0))) << (i * (64 / N)); \
    } \
    return r;
static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int32_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }
static inline int32_t satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : v; }
static inline int32_t satu16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : v; }
static inline uint64_t mmx_paddb(uint64_t a, uint64_t b) { MMX_LANES(uint8_t, 8, (uint8_t)(x + y)) }
static inline uint64_t mmx_paddw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, (uint16_t)(x + y)) }
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b) { MMX_LANES(uint32_t, 2, (uint32_t)(x + y)) }
static inline uint64_t mmx_paddq(uint64_t a, uint64_t b) { return a + b; }
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b) { MMX_LANES(uint8_t, 8, (uint8_t)(x - y)) }
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, (uint16_t)(x - y)) }
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b) { MMX_LANES(uint32_t, 2, (uint32_t)(x - y)) }
static inline uint64_t mmx_psubq(uint64_t a, uint64_t b) { return a - b; }
static inline uint64_t mmx_paddsb(uint64_t a, uint64_t b) { MMX_LANES(int8_t, 8, (uint8_t)sat8(x + y)) }
static inline uint64_t mmx_paddsw(uint64_t a, uint64_t b) { MMX_LANES(int16_t, 4, (uint16_t)sat16(x + y)) }
static inline uint64_t mmx_psubsb(uint64_t a, uint64_t b) { MMX_LANES(int8_t, 8, (uint8_t)sat8(x - y)) }
static inline uint64_t mmx_psubsw(uint64_t a, uint64_t b) { MMX_LANES(int16_t, 4, (uint16_t)sat16(x - y)) }
static inline uint64_t mmx_paddusb(uint64_t a, uint64_t b) { MMX_LANES(uint8_t, 8, (uint8_t)satu8(x + y)) }
static inline uint64_t mmx_paddusw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, (uint16_t)satu16(x + y)) }
static inline uint64_t mmx_psubusb(uint64_t a, uint64_t b) { MMX_LANES(uint8_t, 8, (uint8_t)satu8(x - y)) }
static inline uint64_t mmx_psubusw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, (uint16_t)satu16(x - y)) }
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) { MMX_LANES(int16_t, 4, (uint16_t)((int32_t)x * y)) }
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) { MMX_LANES(int16_t, 4, (uint16_t)(((int32_t)x * y) >> 16)) }
static inline uint64_t mmx_pmulhuw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, (uint16_t)(((uint32_t)x * y) >> 16)) }
static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 2; i++) {
        int32_t s = (int32_t)(int16_t)(a >> (32 * i)) * (int16_t)(b >> (32 * i)) +
                    (int32_t)(int16_t)(a >> (32 * i + 16)) * (int16_t)(b >> (32 * i + 16));
        r |= (uint64_t)(uint32_t)s << (32 * i);
    }
    return r;
}
static inline uint64_t mmx_pand(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mmx_por(uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mmx_pxor(uint64_t a, uint64_t b) { return a ^ b; }
static inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b) { MMX_LANES(uint8_t, 8, x == y ? 0xFF : 0) }
static inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, x == y ? 0xFFFF : 0) }
static inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b) { MMX_LANES(uint32_t, 2, x == y ? 0xFFFFFFFFu : 0) }
static inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b) { MMX_LANES(int8_t, 8, x > y ? 0xFF : 0) }
static inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b) { MMX_LANES(int16_t, 4, x > y ? 0xFFFF : 0) }
static inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b) { MMX_LANES(int32_t, 2, x > y ? 0xFFFFFFFFu : 0) }
static inline uint64_t mmx_psllw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, b > 15 ? 0 : (uint16_t)(x << b)) }
static inline uint64_t mmx_pslld(uint64_t a, uint64_t b) { MMX_LANES(uint32_t, 2, b > 31 ? 0 : (uint32_t)(x << b)) }
static inline uint64_t mmx_psllq(uint64_t a, uint64_t n) { return n > 63 ? 0 : a << n; }
static inline uint64_t mmx_psrlw(uint64_t a, uint64_t b) { MMX_LANES(uint16_t, 4, b > 15 ? 0 : (uint16_t)(x >> b)) }
static inline uint64_t mmx_psrld(uint64_t a, uint64_t b) { MMX_LANES(uint32_t, 2, b > 31 ? 0 : (uint32_t)(x >> b)) }
static inline uint64_t mmx_psrlq(uint64_t a, uint64_t n) { return n > 63 ? 0 : a >> n; }
static inline uint64_t mmx_psraw(uint64_t a, uint64_t b) { MMX_LANES(int16_t, 4, (uint16_t)(x >> (b > 15 ? 15 : b))) }
static inline uint64_t mmx_psrad(uint64_t a, uint64_t b) { MMX_LANES(int32_t, 2, (uint32_t)(x >> (b > 31 ? 31 : b))) }
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= (uint64_t)(uint8_t)sat8((int16_t)(a >> (16 * i))) << (8 * i);
    for (int i = 0; i < 4; i++) r |= (uint64_t)(uint8_t)sat8((int16_t)(b >> (16 * i))) << (8 * i + 32);
    return r;
}
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= (uint64_t)(uint8_t)satu8((int16_t)(a >> (16 * i))) << (8 * i);
    for (int i = 0; i < 4; i++) r |= (uint64_t)(uint8_t)satu8((int16_t)(b >> (16 * i))) << (8 * i + 32);
    return r;
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 2; i++) r |= (uint64_t)(uint16_t)sat16((int32_t)(a >> (32 * i))) << (16 * i);
    for (int i = 0; i < 2; i++) r |= (uint64_t)(uint16_t)sat16((int32_t)(b >> (32 * i))) << (16 * i + 32);
    return r;
}
static inline uint64_t mmx_unpack(uint64_t a, uint64_t b, int bits, int high) {
    uint64_t r = 0;
    int n = 32 / bits;
    uint64_t m = ((uint64_t)1 << bits) - 1;
    for (int i = 0; i < n; i++) {
        int s = (high ? 32 : 0) + i * bits;
        r |= ((a >> s) & m) << (2 * i * bits);
        r |= ((b >> s) & m) << ((2 * i + 1) * bits);
    }
    return r;
}
static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 8, 0); }
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 16, 0); }
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 32, 0); }
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 8, 1); }
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 16, 1); }
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 32, 1); }
