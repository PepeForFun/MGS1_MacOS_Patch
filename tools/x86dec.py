"""x86-32 instruction decoder (stdlib only).

Covers the full one-byte map, the 0F map (integer, MMX, and enough SSE to
keep lengths right in data regions) and x87. Mnemonics follow LLVM's Intel
syntax so the decoder can be checked against llvm-objdump.
"""
import struct

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
REG8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
SEGS = ["es", "cs", "ss", "ds", "fs", "gs", "seg6", "seg7"]
CC = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
SEGPFX = {0x26: "es", 0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x64: "fs", 0x65: "gs"}


class Reg:
    __slots__ = ("kind", "num", "size")

    def __init__(self, kind, num, size):
        self.kind, self.num, self.size = kind, num, size  # kind: g s st mm xmm cr dr

    @property
    def name(self):
        k = self.kind
        if k == "g":
            return {32: REG32, 16: REG16, 8: REG8}[self.size][self.num]
        if k == "s":
            return SEGS[self.num]
        if k == "st":
            return "st" if self.num == 0 and self.size == -1 else f"st({self.num})"
        return f"{k}{self.num}"

    def __repr__(self):
        return self.name


class Mem:
    __slots__ = ("size", "seg", "base", "index", "scale", "disp", "asz")

    def __init__(self, size, seg, base, index, scale, disp, asz=32):
        self.size, self.seg, self.base, self.index = size, seg, base, index
        self.scale, self.disp, self.asz = scale, disp, asz

    def __repr__(self):
        return fmt_op(self)


class Imm:
    __slots__ = ("val", "size")

    def __init__(self, val, size):
        self.val, self.size = val & ((1 << size) - 1), size

    @property
    def sval(self):
        v = self.val
        return v - (1 << self.size) if v >> (self.size - 1) else v

    def __repr__(self):
        return hex(self.val)


class Rel:
    __slots__ = ("target",)

    def __init__(self, target):
        self.target = target & 0xFFFFFFFF

    def __repr__(self):
        return hex(self.target)


class Far:
    __slots__ = ("seg", "off")

    def __init__(self, seg, off):
        self.seg, self.off = seg, off


class Insn:
    __slots__ = ("addr", "size", "mnem", "ops", "rep", "lock", "seg", "opsz", "asz", "op", "op2", "modrm", "bad")

    def __init__(self):
        self.ops = []
        self.rep = None
        self.lock = False
        self.seg = None
        self.opsz = 32
        self.asz = 32
        self.op = self.op2 = self.modrm = None
        self.bad = False

    def __repr__(self):
        return f"{self.addr:#x}: {fmt_insn(self)}"


class DecodeError(Exception):
    pass


# ---------------------------------------------------------------- tables
# Operand spec letters (Intel manual style):
#  Eb Ew Ed Ev: r/m   Gb Gw Gv Gd: reg field   Ib Iw Iz Ibs(imm8 sign-extended to opsize)
#  Jb Jz: relative   M Mp Mq Ma Mt: memory only   Ob Ov: moffs   AL eAX AX DX CL ONE
#  Sw: seg reg   Zb Zv Zd: reg in low opcode bits   Xb Xv Yb Yv: string operands
#  Pq Qq Qd Nq: MMX   Vx Wx: xmm (data only)   Ap: far immediate   Rd Cd Dd
ONE = {}


def _alu(base, mnem):
    ONE[base + 0] = (mnem, "Eb,Gb")
    ONE[base + 1] = (mnem, "Ev,Gv")
    ONE[base + 2] = (mnem, "Gb,Eb")
    ONE[base + 3] = (mnem, "Gv,Ev")
    ONE[base + 4] = (mnem, "AL,Ib")
    ONE[base + 5] = (mnem, "eAX,Iz")


for _b, _m in [(0x00, "add"), (0x08, "or"), (0x10, "adc"), (0x18, "sbb"), (0x20, "and"), (0x28, "sub"),
               (0x30, "xor"), (0x38, "cmp")]:
    _alu(_b, _m)
ONE.update({
    0x06: ("push", "ES"), 0x07: ("pop", "ES"), 0x0E: ("push", "CS"), 0x16: ("push", "SS"), 0x17: ("pop", "SS"),
    0x1E: ("push", "DS"), 0x1F: ("pop", "DS"), 0x27: ("daa", ""), 0x2F: ("das", ""), 0x37: ("aaa", ""),
    0x3F: ("aas", ""),
    0x60: ("pusha", ""), 0x61: ("popa", ""), 0x62: ("bound", "Gv,Ma"), 0x63: ("arpl", "Ew,Gw"),
    0x68: ("push", "Iz"), 0x69: ("imul", "Gv,Ev,Iz"), 0x6A: ("push", "Ibs"), 0x6B: ("imul", "Gv,Ev,Ibs"),
    0x6C: ("insb", "Yb,DX"), 0x6D: ("ins", "Yv,DX"), 0x6E: ("outsb", "DX,Xb"), 0x6F: ("outs", "DX,Xv"),
    0x84: ("test", "Eb,Gb"), 0x85: ("test", "Ev,Gv"), 0x86: ("xchg", "Eb,Gb"), 0x87: ("xchg", "Ev,Gv"),
    0x88: ("mov", "Eb,Gb"), 0x89: ("mov", "Ev,Gv"), 0x8A: ("mov", "Gb,Eb"), 0x8B: ("mov", "Gv,Ev"),
    0x8C: ("mov", "Esw,Sw"), 0x8D: ("lea", "Gv,M"), 0x8E: ("mov", "Sw,Ew"),
    0x90: ("nop", ""), 0x98: ("cwde", ""), 0x99: ("cdq", ""), 0x9A: ("lcall", "Ap"), 0x9B: ("wait", ""),
    0x9C: ("pushf", ""), 0x9D: ("popf", ""), 0x9E: ("sahf", ""), 0x9F: ("lahf", ""),
    0xA0: ("mov", "AL,Ob"), 0xA1: ("mov", "eAX,Ov"), 0xA2: ("mov", "Ob,AL"), 0xA3: ("mov", "Ov,eAX"),
    0xA4: ("movsb", "Yb,Xb"), 0xA5: ("movs", "Yv,Xv"), 0xA6: ("cmpsb", "Xb,Yb"), 0xA7: ("cmps", "Xv,Yv"),
    0xA8: ("test", "AL,Ib"), 0xA9: ("test", "eAX,Iz"), 0xAA: ("stosb", "Yb,AL"), 0xAB: ("stos", "Yv,eAX"),
    0xAC: ("lodsb", "AL,Xb"), 0xAD: ("lods", "eAX,Xv"), 0xAE: ("scasb", "AL,Yb"), 0xAF: ("scas", "eAX,Yv"),
    0xC2: ("ret", "Iw"), 0xC3: ("ret", ""), 0xC4: ("les", "Gv,Mp"), 0xC5: ("lds", "Gv,Mp"),
    0xC8: ("enter", "Iw,Ib"), 0xC9: ("leave", ""), 0xCA: ("lret", "Iw"), 0xCB: ("lret", ""),
    0xCC: ("int3", ""), 0xCD: ("int", "Ib"), 0xCE: ("into", ""), 0xCF: ("iret", ""),
    0xD4: ("aam", "Ib"), 0xD5: ("aad", "Ib"), 0xD6: ("salc", ""), 0xD7: ("xlatb", ""),
    0xE0: ("loopne", "Jb"), 0xE1: ("loope", "Jb"), 0xE2: ("loop", "Jb"), 0xE3: ("jecxz", "Jb"),
    0xE4: ("in", "AL,Ib"), 0xE5: ("in", "eAX,Ib"), 0xE6: ("out", "Ib,AL"), 0xE7: ("out", "Ib,eAX"),
    0xE8: ("call", "Jz"), 0xE9: ("jmp", "Jz"), 0xEA: ("ljmp", "Ap"), 0xEB: ("jmp", "Jb"),
    0xEC: ("in", "AL,DX"), 0xED: ("in", "eAX,DX"), 0xEE: ("out", "DX,AL"), 0xEF: ("out", "DX,eAX"),
    0xF1: ("int1", ""), 0xF4: ("hlt", ""), 0xF5: ("cmc", ""),
    0xF8: ("clc", ""), 0xF9: ("stc", ""), 0xFA: ("cli", ""), 0xFB: ("sti", ""), 0xFC: ("cld", ""),
    0xFD: ("std", ""),
})
for _i in range(8):
    ONE[0x40 + _i] = ("inc", "Zv")
    ONE[0x48 + _i] = ("dec", "Zv")
    ONE[0x50 + _i] = ("push", "Zv")
    ONE[0x58 + _i] = ("pop", "Zv")
    ONE[0xB0 + _i] = ("mov", "Zb,Ib")
    ONE[0xB8 + _i] = ("mov", "Zv,Iz")
    if _i:
        ONE[0x90 + _i] = ("xchg", "eAX,Zv")
for _i in range(16):
    ONE[0x70 + _i] = ("j" + CC[_i], "Jb")

GRP = {
    0x80: (["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"], "Eb,Ib"),
    0x81: (["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"], "Ev,Iz"),
    0x82: (["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"], "Eb,Ib"),
    0x83: (["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"], "Ev,Ibs"),
    0x8F: (["pop", None, None, None, None, None, None, None], "Ev"),
    0xC0: (["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"], "Eb,Ib"),
    0xC1: (["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"], "Ev,Ib"),
    0xC6: (["mov", None, None, None, None, None, None, None], "Eb,Ib"),
    0xC7: (["mov", None, None, None, None, None, None, None], "Ev,Iz"),
    0xD0: (["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"], "Eb,ONE"),
    0xD1: (["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"], "Ev,ONE"),
    0xD2: (["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"], "Eb,CL"),
    0xD3: (["rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"], "Ev,CL"),
    0xFE: (["inc", "dec", None, None, None, None, None, None], "Eb"),
}
# F6/F7 and FF have per-reg operand specs
GRP3 = [("test", "E,I"), ("test", "E,I"), ("not", "E"), ("neg", "E"), ("mul", "E"), ("imul", "E"), ("div", "E"),
        ("idiv", "E")]
GRP5 = [("inc", "Ev"), ("dec", "Ev"), ("call", "Ev"), ("lcall", "Mp"), ("jmp", "Ev"), ("ljmp", "Mp"),
        ("push", "Ev"), None]

TWO = {
    0x02: ("lar", "Gv,Ew"), 0x03: ("lsl", "Gv,Ew"), 0x05: ("syscall", ""), 0x06: ("clts", ""),
    0x07: ("sysret", ""), 0x08: ("invd", ""), 0x09: ("wbinvd", ""), 0x0B: ("ud2", ""),
    0x0D: ("prefetch", "M"), 0x0E: ("femms", ""),
    0x20: ("mov", "Rd,Cd"), 0x21: ("mov", "Rd,Dd"), 0x22: ("mov", "Cd,Rd"), 0x23: ("mov", "Dd,Rd"),
    0x30: ("wrmsr", ""), 0x31: ("rdtsc", ""), 0x32: ("rdmsr", ""), 0x33: ("rdpmc", ""),
    0x34: ("sysenter", ""), 0x35: ("sysexit", ""),
    0x60: ("punpcklbw", "Pq,Qd"), 0x61: ("punpcklwd", "Pq,Qd"), 0x62: ("punpckldq", "Pq,Qd"),
    0x63: ("packsswb", "Pq,Qq"), 0x64: ("pcmpgtb", "Pq,Qq"), 0x65: ("pcmpgtw", "Pq,Qq"),
    0x66: ("pcmpgtd", "Pq,Qq"), 0x67: ("packuswb", "Pq,Qq"), 0x68: ("punpckhbw", "Pq,Qq"),
    0x69: ("punpckhwd", "Pq,Qq"), 0x6A: ("punpckhdq", "Pq,Qq"), 0x6B: ("packssdw", "Pq,Qq"),
    0x6E: ("movd", "Pq,Ed"), 0x6F: ("movq", "Pq,Qq"), 0x70: ("pshufw", "Pq,Qq,Ib"),
    0x74: ("pcmpeqb", "Pq,Qq"), 0x75: ("pcmpeqw", "Pq,Qq"), 0x76: ("pcmpeqd", "Pq,Qq"), 0x77: ("emms", ""),
    0x7E: ("movd", "Ed,Pq"), 0x7F: ("movq", "Qq,Pq"),
    0xA0: ("push", "FS"), 0xA1: ("pop", "FS"), 0xA2: ("cpuid", ""), 0xA3: ("bt", "Ev,Gv"),
    0xA4: ("shld", "Ev,Gv,Ib"), 0xA5: ("shld", "Ev,Gv,CL"), 0xA8: ("push", "GS"), 0xA9: ("pop", "GS"),
    0xAA: ("rsm", ""), 0xAB: ("bts", "Ev,Gv"), 0xAC: ("shrd", "Ev,Gv,Ib"), 0xAD: ("shrd", "Ev,Gv,CL"),
    0xAF: ("imul", "Gv,Ev"),
    0xB0: ("cmpxchg", "Eb,Gb"), 0xB1: ("cmpxchg", "Ev,Gv"), 0xB2: ("lss", "Gv,Mp"), 0xB3: ("btr", "Ev,Gv"),
    0xB4: ("lfs", "Gv,Mp"), 0xB5: ("lgs", "Gv,Mp"), 0xB6: ("movzx", "Gv,Eb"), 0xB7: ("movzx", "Gv,Ew"),
    0xBB: ("btc", "Ev,Gv"), 0xBC: ("bsf", "Gv,Ev"), 0xBD: ("bsr", "Gv,Ev"), 0xBE: ("movsx", "Gv,Eb"),
    0xBF: ("movsx", "Gv,Ew"), 0xC0: ("xadd", "Eb,Gb"), 0xC1: ("xadd", "Ev,Gv"),
    0xC3: ("movnti", "M,Gd"), 0xC4: ("pinsrw", "Pq,Ed,Ib"), 0xC5: ("pextrw", "Gd,Nq,Ib"),
    0xD1: ("psrlw", "Pq,Qq"), 0xD2: ("psrld", "Pq,Qq"), 0xD3: ("psrlq", "Pq,Qq"), 0xD4: ("paddq", "Pq,Qq"),
    0xD5: ("pmullw", "Pq,Qq"), 0xD7: ("pmovmskb", "Gd,Nq"), 0xD8: ("psubusb", "Pq,Qq"),
    0xD9: ("psubusw", "Pq,Qq"), 0xDA: ("pminub", "Pq,Qq"), 0xDB: ("pand", "Pq,Qq"), 0xDC: ("paddusb", "Pq,Qq"),
    0xDD: ("paddusw", "Pq,Qq"), 0xDE: ("pmaxub", "Pq,Qq"), 0xDF: ("pandn", "Pq,Qq"),
    0xE0: ("pavgb", "Pq,Qq"), 0xE1: ("psraw", "Pq,Qq"), 0xE2: ("psrad", "Pq,Qq"), 0xE3: ("pavgw", "Pq,Qq"),
    0xE4: ("pmulhuw", "Pq,Qq"), 0xE5: ("pmulhw", "Pq,Qq"), 0xE7: ("movntq", "M,Pq"),
    0xE8: ("psubsb", "Pq,Qq"), 0xE9: ("psubsw", "Pq,Qq"), 0xEA: ("pminsw", "Pq,Qq"), 0xEB: ("por", "Pq,Qq"),
    0xEC: ("paddsb", "Pq,Qq"), 0xED: ("paddsw", "Pq,Qq"), 0xEE: ("pmaxsw", "Pq,Qq"), 0xEF: ("pxor", "Pq,Qq"),
    0xF1: ("psllw", "Pq,Qq"), 0xF2: ("pslld", "Pq,Qq"), 0xF3: ("psllq", "Pq,Qq"), 0xF4: ("pmuludq", "Pq,Qq"),
    0xF5: ("pmaddwd", "Pq,Qq"), 0xF6: ("psadbw", "Pq,Qq"), 0xF7: ("maskmovq", "Pq,Nq"),
    0xF8: ("psubb", "Pq,Qq"), 0xF9: ("psubw", "Pq,Qq"), 0xFA: ("psubd", "Pq,Qq"), 0xFB: ("psubq", "Pq,Qq"),
    0xFC: ("paddb", "Pq,Qq"), 0xFD: ("paddw", "Pq,Qq"), 0xFE: ("paddd", "Pq,Qq"),
}
for _i in range(16):
    TWO[0x40 + _i] = ("cmov" + CC[_i], "Gv,Ev")
    TWO[0x80 + _i] = ("j" + CC[_i], "Jz")
    TWO[0x90 + _i] = ("set" + CC[_i], "Eb")
for _i in range(8):
    TWO[0xC8 + _i] = ("bswap", "Zd")
for _i in range(0x19, 0x20):
    TWO[_i] = ("nop", "Ev")
# SSE ops: only decoded to keep instruction lengths right (they appear in data, not in code)
_SSE = {0x10: "movups", 0x11: "movups", 0x12: "movlps", 0x13: "movlps", 0x14: "unpcklps", 0x15: "unpckhps",
        0x16: "movhps", 0x17: "movhps", 0x28: "movaps", 0x29: "movaps", 0x2A: "cvtpi2ps", 0x2B: "movntps",
        0x2C: "cvttps2pi", 0x2D: "cvtps2pi", 0x2E: "ucomiss", 0x2F: "comiss", 0x50: "movmskps", 0x51: "sqrtps",
        0x52: "rsqrtps", 0x53: "rcpps", 0x54: "andps", 0x55: "andnps", 0x56: "orps", 0x57: "xorps",
        0x58: "addps", 0x59: "mulps", 0x5A: "cvtps2pd", 0x5B: "cvtdq2ps", 0x5C: "subps", 0x5D: "minps",
        0x5E: "divps", 0x5F: "maxps", 0x6C: "punpcklqdq", 0x6D: "punpckhqdq", 0x78: "vmread", 0x79: "vmwrite",
        0x7C: "haddpd", 0x7D: "hsubpd", 0xD0: "addsubpd", 0xD6: "movq", 0xE6: "cvttpd2dq", 0xF0: "lddqu"}
for _k, _v in _SSE.items():
    TWO[_k] = (_v, "SSE")
TWO[0xC2] = ("cmpps", "SSEI")
TWO[0xC6] = ("shufps", "SSEI")
GRP_TWO = {
    0x00: (["sldt", "str", "lldt", "ltr", "verr", "verw", None, None], "Ew"),
    0x01: (["sgdt", "sidt", "lgdt", "lidt", "smsw", None, "lmsw", "invlpg"], "M"),
    0x18: (["prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "nop", "nop", "nop", "nop"], "M"),
    0x71: ([None, None, "psrlw", None, "psraw", None, "psllw", None], "Nq,Ib"),
    0x72: ([None, None, "psrld", None, "psrad", None, "pslld", None], "Nq,Ib"),
    0x73: ([None, None, "psrlq", None, None, None, "psllq", None], "Nq,Ib"),
    0xAE: (["fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "xsave", "xrstor", "xsaveopt", "clflush"], "M"),
    0xBA: ([None, None, None, None, "bt", "bts", "btr", "btc"], "Ev,Ib"),
    0xC7: ([None, "cmpxchg8b", None, None, None, None, None, None], "Mq"),
}

# x87: memory forms by (opcode, reg) -> (mnem, size bits)
X87M = {
    0xD8: [("fadd", 32), ("fmul", 32), ("fcom", 32), ("fcomp", 32), ("fsub", 32), ("fsubr", 32), ("fdiv", 32),
           ("fdivr", 32)],
    0xD9: [("fld", 32), None, ("fst", 32), ("fstp", 32), ("fldenv", 0), ("fldcw", 16), ("fnstenv", 0),
           ("fnstcw", 16)],
    0xDA: [("fiadd", 32), ("fimul", 32), ("ficom", 32), ("ficomp", 32), ("fisub", 32), ("fisubr", 32),
           ("fidiv", 32), ("fidivr", 32)],
    0xDB: [("fild", 32), ("fisttp", 32), ("fist", 32), ("fistp", 32), None, ("fld", 80), None, ("fstp", 80)],
    0xDC: [("fadd", 64), ("fmul", 64), ("fcom", 64), ("fcomp", 64), ("fsub", 64), ("fsubr", 64), ("fdiv", 64),
           ("fdivr", 64)],
    0xDD: [("fld", 64), ("fisttp", 64), ("fst", 64), ("fstp", 64), ("frstor", 0), None, ("fnsave", 0),
           ("fnstsw", 16)],
    0xDE: [("fiadd", 16), ("fimul", 16), ("ficom", 16), ("ficomp", 16), ("fisub", 16), ("fisubr", 16),
           ("fidiv", 16), ("fidivr", 16)],
    0xDF: [("fild", 16), ("fisttp", 16), ("fist", 16), ("fistp", 16), ("fbld", 80), ("fild", 64), ("fbstp", 80),
           ("fistp", 64)],
}
D9R = {0xD0: "fnop", 0xE0: "fchs", 0xE1: "fabs", 0xE4: "ftst", 0xE5: "fxam", 0xE8: "fld1", 0xE9: "fldl2t",
       0xEA: "fldl2e", 0xEB: "fldpi", 0xEC: "fldlg2", 0xED: "fldln2", 0xEE: "fldz", 0xF0: "f2xm1",
       0xF1: "fyl2x", 0xF2: "fptan", 0xF3: "fpatan", 0xF4: "fxtract", 0xF5: "fprem1", 0xF6: "fdecstp",
       0xF7: "fincstp", 0xF8: "fprem", 0xF9: "fyl2xp1", 0xFA: "fsqrt", 0xFB: "fsincos", 0xFC: "frndint",
       0xFD: "fscale", 0xFE: "fsin", 0xFF: "fcos"}


def _st(i):
    return Reg("st", i, 80)


ST0 = Reg("st", 0, -1)  # prints as "st"


def _x87_reg(op, m):
    """Register-form x87 (mod==3). Returns (mnem, ops) or None."""
    reg = (m >> 3) & 7
    i = m & 7
    if op == 0xD8:
        names = ["fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"]
        if reg in (2, 3):
            return names[reg], [_st(i)]
        return names[reg], [ST0, _st(i)]
    if op == 0xD9:
        if reg == 0:
            return "fld", [_st(i)]
        if reg == 1:
            return "fxch", [_st(i)]
        if m in D9R:
            return D9R[m], []
        return None
    if op == 0xDA:
        if reg < 4:
            return ["fcmovb", "fcmove", "fcmovbe", "fcmovu"][reg], [ST0, _st(i)]
        if m == 0xE9:
            return "fucompp", []
        return None
    if op == 0xDB:
        if reg < 4:
            return ["fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"][reg], [ST0, _st(i)]
        if m == 0xE2:
            return "fnclex", []
        if m == 0xE3:
            return "fninit", []
        if reg == 5:
            return "fucomi", [ST0, _st(i)]
        if reg == 6:
            return "fcomi", [ST0, _st(i)]
        return None
    if op == 0xDC:
        # Intel: DC E0+i FSUBR st(i),st0; DC E8+i FSUB st(i),st0; F0 FDIVR; F8 FDIV
        names = ["fadd", "fmul", "fcom", "fcomp", "fsubr", "fsub", "fdivr", "fdiv"]
        if reg in (2, 3):
            return names[reg], [_st(i)]
        return names[reg], [_st(i), ST0]
    if op == 0xDD:
        names = ["ffree", "fxch", "fst", "fstp", "fucom", "fucomp", None, None]
        if names[reg] is None:
            return None
        return names[reg], [_st(i)]
    if op == 0xDE:
        if reg == 3:
            return ("fcompp", []) if i == 1 else None
        names = ["faddp", "fmulp", "fcomp", None, "fsubrp", "fsubp", "fdivrp", "fdivp"]
        if reg == 2:
            return "fcomp", [_st(i)]
        return names[reg], [_st(i), ST0]
    if op == 0xDF:
        if m == 0xE0:
            return "fnstsw", [Reg("g", 0, 16)]
        if reg == 0:
            return "ffreep", [_st(i)]
        if reg == 5:
            return "fucomip", [ST0, _st(i)]
        if reg == 6:
            return "fcomip", [ST0, _st(i)]
        return None
    return None


# ---------------------------------------------------------------- decoder
def _s8(b):
    return b - 256 if b > 127 else b


class Decoder:
    """Decodes instructions from an image. read(addr, n) returns bytes."""

    def __init__(self, image, base):
        self.img = image
        self.base = base

    def decode(self, addr):
        img = self.img
        off = addr - self.base
        p = off
        ins = Insn()
        ins.addr = addr
        npfx = 0
        while True:
            b = img[p]
            if b == 0x66:
                ins.opsz = 16
            elif b == 0x67:
                ins.asz = 16
            elif b in SEGPFX:
                ins.seg = SEGPFX[b]
            elif b == 0xF0:
                ins.lock = True
            elif b == 0xF2:
                ins.rep = "repne"
            elif b == 0xF3:
                ins.rep = "rep"
            else:
                break
            p += 1
            npfx += 1
            if npfx > 14:
                raise DecodeError("too many prefixes")
        op = img[p]
        p += 1
        ins.op = op
        st = _State(self, ins, p)
        if op == 0x0F:
            op2 = img[st.p]
            st.p += 1
            ins.op2 = op2
            if op2 == 0x0F:  # 3DNow!: 0F 0F modrm [sib/disp] imm8-suffix
                st.modrm()
                a = st.rm_op("Qq")
                b2 = st.reg_op("Pq")
                suffix = img[st.p]
                st.p += 1
                ins.mnem = f"3dnow_{suffix:02x}"
                ins.ops = [b2, a]
            elif op2 in GRP_TWO:
                names, spec = GRP_TWO[op2]
                m = st.modrm()
                name = names[(m >> 3) & 7]
                if name is None:
                    raise DecodeError(f"bad 0f {op2:02x} /{(m >> 3) & 7}")
                if spec.startswith("M") and (m >> 6) == 3:
                    if op2 == 0xAE and m >= 0xE8:
                        name = {5: "lfence", 6: "mfence", 7: "sfence"}[(m >> 3) & 7]
                        ins.mnem = name
                        ins.size = st.p - off
                        return ins
                    raise DecodeError("mem-only group with reg form")
                ins.mnem = name
                ins.ops = st.operands(spec)
            elif op2 in TWO:
                name, spec = TWO[op2]
                if spec in ("SSE", "SSEI"):
                    st.modrm()
                    a = st.rm_op("Wx")
                    r = st.reg_op("Vx")
                    ins.ops = [r, a]
                    if spec == "SSEI":
                        ins.ops.append(st.imm(8))
                    ins.mnem = name
                else:
                    ins.mnem = name
                    ins.ops = st.operands(spec)
            else:
                raise DecodeError(f"unknown 0f {op2:02x}")
        elif 0xD8 <= op <= 0xDF:
            m = st.modrm()
            if (m >> 6) == 3:
                r = _x87_reg(op, m)
                if r is None:
                    raise DecodeError(f"bad x87 reg {op:02x} {m:02x}")
                ins.mnem, ins.ops = r
            else:
                e = X87M[op][(m >> 3) & 7]
                if e is None:
                    raise DecodeError(f"bad x87 mem {op:02x} /{(m >> 3) & 7}")
                ins.mnem = e[0]
                ins.ops = [st.mem(e[1])]
        elif op in GRP:
            names, spec = GRP[op]
            m = st.modrm()
            name = names[(m >> 3) & 7]
            if name is None:
                raise DecodeError(f"bad group {op:02x} /{(m >> 3) & 7}")
            ins.mnem = name
            ins.ops = st.operands(spec)
        elif op in (0xF6, 0xF7):
            m = st.modrm()
            name, spec = GRP3[(m >> 3) & 7]
            ins.mnem = name
            if op == 0xF6:
                spec = spec.replace("E", "Eb").replace("I", "Ib")
            else:
                spec = spec.replace("E", "Ev").replace("I", "Iz")
            ins.ops = st.operands(spec)
        elif op == 0xFF:
            m = st.modrm()
            e = GRP5[(m >> 3) & 7]
            if e is None:
                raise DecodeError("bad ff /7")
            ins.mnem = e[0]
            ins.ops = st.operands(e[1])
        elif op in ONE:
            name, spec = ONE[op]
            ins.mnem = name
            if spec:
                ins.ops = st.operands(spec)
        else:
            raise DecodeError(f"unknown opcode {op:02x}")
        _fix_mnem(ins)
        ins.size = st.p - off
        if ins.size > 15:
            raise DecodeError("instruction too long")
        return ins


def _fix_mnem(ins):
    m = ins.mnem
    o16 = ins.opsz == 16
    if m in ("movs", "cmps", "stos", "lods", "scas", "ins", "outs"):
        ins.mnem = m + ("w" if o16 else "d")
    elif m == "cwde" and o16:
        ins.mnem = "cbw"
    elif m == "cdq" and o16:
        ins.mnem = "cwd"
    elif m == "pusha":
        ins.mnem = "pushaw" if o16 else "pushal"
    elif m == "popa":
        ins.mnem = "popaw" if o16 else "popal"
    elif m == "pushf":
        ins.mnem = "pushfw" if o16 else "pushfd"
    elif m == "popf":
        ins.mnem = "popfw" if o16 else "popfd"
    elif m == "iret":
        ins.mnem = "iretw" if o16 else "iretd"
    elif m == "jecxz" and ins.asz == 16:
        ins.mnem = "jcxz"
    elif m == "nop" and ins.op == 0x90 and ins.rep == "rep":
        ins.mnem = "pause"
        ins.rep = None


class _State:
    __slots__ = ("d", "ins", "p", "m")

    def __init__(self, d, ins, p):
        self.d, self.ins, self.p, self.m = d, ins, p, None

    def modrm(self):
        if self.m is None:
            self.m = self.d.img[self.p]
            self.p += 1
            self.ins.modrm = self.m
        return self.m

    def imm(self, bits, signed_to=None):
        img = self.d.img
        p = self.p
        if bits == 8:
            v = img[p]
        elif bits == 16:
            v = img[p] | img[p + 1] << 8
        else:
            v = struct.unpack_from("<I", img, p)[0]
        self.p += bits // 8
        if signed_to:
            if v >> (bits - 1):
                v -= 1 << bits
            return Imm(v, signed_to)
        return Imm(v, bits)

    def mem(self, size):
        """Decode the memory operand of the current modrm (mod != 3)."""
        m = self.modrm()
        mod, rm = m >> 6, m & 7
        img = self.d.img
        ins = self.ins
        if ins.asz == 16:
            disp = 0
            pairs = [(3, 6), (3, 7), (5, 6), (5, 7), (6, None), (7, None), (5, None), (3, None)]
            base, index = pairs[rm]
            if mod == 0 and rm == 6:
                base = None
                disp = img[self.p] | img[self.p + 1] << 8
                self.p += 2
            elif mod == 1:
                disp = _s8(img[self.p])
                self.p += 1
            elif mod == 2:
                disp = img[self.p] | img[self.p + 1] << 8
                self.p += 2
            return Mem(size, ins.seg, base, index, 1, disp, 16)
        base = index = None
        scale = 1
        disp = 0
        if rm == 4:
            sib = img[self.p]
            self.p += 1
            ss, idx, b = sib >> 6, (sib >> 3) & 7, sib & 7
            if idx != 4:
                index, scale = idx, 1 << ss
            if b == 5 and mod == 0:
                disp = struct.unpack_from("<i", img, self.p)[0]
                self.p += 4
            else:
                base = b
        elif rm == 5 and mod == 0:
            disp = struct.unpack_from("<i", img, self.p)[0]
            self.p += 4
        else:
            base = rm
        if mod == 1:
            disp = _s8(img[self.p])
            self.p += 1
        elif mod == 2:
            disp = struct.unpack_from("<i", img, self.p)[0]
            self.p += 4
        return Mem(size, ins.seg, base, index, scale, disp, 32)

    def rm_op(self, kind):
        """r/m operand: kind gives size/register file."""
        m = self.modrm()
        o = self.ins.opsz
        size = {"Eb": 8, "Ew": 16, "Ed": 32, "Ev": o, "Esw": 16, "Qq": 64, "Qd": 32, "Nq": 64, "Wx": 128,
                "M": 0, "Mp": 48 if o == 32 else 32, "Mq": 64, "Ma": 64 if o == 32 else 32, "Rd": 32}[kind]
        if (m >> 6) == 3:
            if kind in ("M", "Mp", "Mq", "Ma"):
                raise DecodeError("memory operand required")
            r = m & 7
            if kind in ("Qq", "Qd", "Nq"):
                return Reg("mm", r, 64)
            if kind == "Wx":
                return Reg("xmm", r, 128)
            if kind == "Esw":
                return Reg("g", r, o)
            return Reg("g", r, size)
        if kind == "Nq" or kind == "Rd":
            raise DecodeError("register operand required")
        return self.mem(size)

    def reg_op(self, kind):
        m = self.modrm()
        r = (m >> 3) & 7
        o = self.ins.opsz
        if kind == "Gb":
            return Reg("g", r, 8)
        if kind == "Gw":
            return Reg("g", r, 16)
        if kind == "Gd":
            return Reg("g", r, 32)
        if kind == "Gv":
            return Reg("g", r, o)
        if kind == "Sw":
            if r > 5:
                raise DecodeError("bad segment register")
            return Reg("s", r, 16)
        if kind == "Pq":
            return Reg("mm", r, 64)
        if kind == "Vx":
            return Reg("xmm", r, 128)
        if kind == "Cd":
            return Reg("cr", r, 32)
        if kind == "Dd":
            return Reg("dr", r, 32)
        raise DecodeError(kind)

    def operands(self, spec):
        ins = self.ins
        o = ins.opsz
        out = []
        for s in spec.split(","):
            if not s:
                continue
            if s in ("Eb", "Ew", "Ed", "Ev", "Esw", "Qq", "Qd", "Nq", "M", "Mp", "Mq", "Ma", "Rd"):
                out.append(self.rm_op(s))
            elif s in ("Gb", "Gw", "Gv", "Gd", "Sw", "Pq", "Cd", "Dd"):
                out.append(self.reg_op(s))
            elif s == "Ib":
                out.append(self.imm(8))
            elif s == "Ibs":
                out.append(self.imm(8, signed_to=o))
            elif s == "Iw":
                out.append(self.imm(16))
            elif s == "Iz":
                out.append(self.imm(16 if o == 16 else 32))
            elif s == "Jb":
                d = _s8(self.d.img[self.p])
                self.p += 1
                out.append(Rel(self.d.base + self.p + d))
            elif s == "Jz":
                if o == 16:
                    d = struct.unpack_from("<h", self.d.img, self.p)[0]
                    self.p += 2
                else:
                    d = struct.unpack_from("<i", self.d.img, self.p)[0]
                    self.p += 4
                out.append(Rel(self.d.base + self.p + d))
            elif s in ("Ob", "Ov"):
                if ins.asz == 16:
                    a = self.d.img[self.p] | self.d.img[self.p + 1] << 8
                    self.p += 2
                else:
                    a = struct.unpack_from("<I", self.d.img, self.p)[0]
                    self.p += 4
                out.append(Mem(8 if s == "Ob" else o, ins.seg, None, None, 1, a, ins.asz))
            elif s == "AL":
                out.append(Reg("g", 0, 8))
            elif s == "CL":
                out.append(Reg("g", 1, 8))
            elif s == "DX":
                out.append(Reg("g", 2, 16))
            elif s == "eAX":
                out.append(Reg("g", 0, o))
            elif s == "ONE":
                out.append(Imm(1, 8))
            elif s == "Zb":
                out.append(Reg("g", ins.op & 7, 8))
            elif s == "Zv":
                out.append(Reg("g", (ins.op2 if ins.op == 0x0F else ins.op) & 7, o))
            elif s == "Zd":
                out.append(Reg("g", ins.op2 & 7, 32))
            elif s in ("ES", "CS", "SS", "DS", "FS", "GS"):
                out.append(Reg("s", SEGS.index(s.lower()), 16))
            elif s in ("Xb", "Xv"):
                out.append(Mem(8 if s == "Xb" else o, ins.seg or "ds", 6, None, 1, 0, ins.asz))
            elif s in ("Yb", "Yv"):
                out.append(Mem(8 if s == "Yb" else o, "es", 7, None, 1, 0, ins.asz))
            elif s == "Ap":
                if o == 16:
                    off = self.d.img[self.p] | self.d.img[self.p + 1] << 8
                    self.p += 2
                else:
                    off = struct.unpack_from("<I", self.d.img, self.p)[0]
                    self.p += 4
                seg = self.d.img[self.p] | self.d.img[self.p + 1] << 8
                self.p += 2
                out.append(Far(seg, off))
            else:
                raise DecodeError(f"spec {s}")
        return out


# ---------------------------------------------------------------- formatting (LLVM-like Intel syntax)
_PTR = {8: "byte ptr ", 16: "word ptr ", 32: "dword ptr ", 48: "fword ptr ", 64: "qword ptr ", 80: "tbyte ptr ",
        128: "xmmword ptr ", 0: ""}


def fmt_op(o):
    if isinstance(o, Reg):
        return o.name
    if isinstance(o, Imm):
        return hex(o.val)
    if isinstance(o, Rel):
        return hex(o.target)
    if isinstance(o, Far):
        return f"{o.seg:#x}:{o.off:#x}"
    names = REG16 if o.asz == 16 else REG32
    parts = []
    if o.base is not None:
        parts.append(names[o.base])
    if o.index is not None:
        parts.append(f"{o.scale}*{names[o.index]}" if o.scale != 1 or o.base is None else names[o.index])
    s = " + ".join(parts)
    d = o.disp
    if parts:
        if d > 0:
            s += f" + {d:#x}"
        elif d < 0:
            s += f" - {-d:#x}"
    else:
        s = f"{d & 0xFFFFFFFF:#x}"
    seg = f"{o.seg}:" if o.seg else ""
    return f"{_PTR.get(o.size, '')}{seg}[{s}]"


def fmt_insn(ins):
    pfx = ""
    if ins.lock:
        pfx += "lock "
    if ins.rep:
        pfx += ins.rep + " "
    return pfx + ins.mnem + (" " + ", ".join(fmt_op(o) for o in ins.ops) if ins.ops else "")
