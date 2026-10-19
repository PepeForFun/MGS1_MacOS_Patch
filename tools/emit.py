"""C code generation for recompiled functions.

Registers live in the CPU struct; flags live in per-function locals, and only
flags that are read later (liveness analysis) are computed.
"""
from x86dec import Far, Imm, Mem, Reg, Rel

CF, PF, AF, ZF, SF, OF = 1, 2, 4, 8, 16, 32
ALL = 63
FLAGNAMES = [(CF, "cf"), (PF, "pf"), (AF, "af"), (ZF, "zf"), (SF, "sf"), (OF, "of")]
CC_USE = {"o": OF, "no": OF, "b": CF, "ae": CF, "e": ZF, "ne": ZF, "be": CF | ZF, "a": CF | ZF, "s": SF,
          "ns": SF, "p": PF, "np": PF, "l": SF | OF, "ge": SF | OF, "le": ZF | SF | OF, "g": ZF | SF | OF}
CC_EXPR = {"o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf", "be": "(cf|zf)",
           "a": "!(cf|zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf", "l": "(sf!=of)", "ge": "(sf==of)",
           "le": "(zf|(sf!=of))", "g": "(!zf&&(sf==of))"}
UT = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}
ST_ = {8: "int8_t", 16: "int16_t", 32: "int32_t", 64: "int64_t"}
SHIFTS = {"shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"}
ARITH = {"add", "sub", "cmp", "adc", "sbb"}
LOGIC = {"and", "or", "xor", "test"}
STRING = {"movsb": 8, "movsw": 16, "movsd": 32, "stosb": 8, "stosw": 16, "stosd": 32, "lodsb": 8, "lodsw": 16,
          "lodsd": 32, "scasb": 8, "scasw": 16, "scasd": 32, "cmpsb": 8, "cmpsw": 16, "cmpsd": 32}


MMX_OPS = set("""paddb paddw paddd paddq psubb psubw psubd psubq paddsb paddsw psubsb psubsw paddusb paddusw psubusb
psubusw pmullw pmulhw pmulhuw pmaddwd pand pandn por pxor pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd packsswb
packuswb packssdw punpcklbw punpcklwd punpckldq punpckhbw punpckhwd punpckhdq""".split())


class EmitError(Exception):
    pass


def cc_of(mnem, prefix):
    return mnem[len(prefix):]


# ---------------------------------------------------------------- flag effects
def flag_effects(ins):
    """(uses, defs) bitmasks. defs are flags whose old value is dead afterwards."""
    m = ins.mnem
    ops = ins.ops
    if m in ("add", "sub", "cmp", "neg", "cmpxchg", "xadd"):
        return 0, ALL
    if m in ("adc", "sbb"):
        return CF, ALL
    if m in LOGIC:
        return 0, ALL
    if m in ("inc", "dec"):
        return 0, ALL & ~CF
    if m in SHIFTS or m in ("shld", "shrd"):
        cnt = ops[-1]
        rot = m in ("rol", "ror", "rcl", "rcr")
        d = (CF | OF) if rot else ALL
        u = CF if m in ("rcl", "rcr") else 0
        if isinstance(cnt, Imm):
            if cnt.val & 31 == 0:
                return 0, 0
            return u, d
        return u | d, d  # count may be zero: old flags survive
    if m in ("mul", "imul"):
        return 0, ALL
    if m in ("div", "idiv"):
        return 0, ALL
    if m in ("bt", "bts", "btr", "btc"):
        return 0, CF
    if m in ("bsf", "bsr"):
        return 0, ZF
    if m == "sahf":
        return 0, CF | PF | AF | ZF | SF
    if m == "lahf":
        return CF | PF | AF | ZF | SF, 0
    if m in ("pushfd", "pushfw"):
        return ALL, 0
    if m in ("popfd", "popfw"):
        return 0, ALL
    if m in ("clc", "stc"):
        return 0, CF
    if m == "cmc":
        return CF, CF
    if m.startswith("set") or m.startswith("cmov"):
        cc = cc_of(m, "set" if m.startswith("set") else "cmov")
        return CC_USE[cc], 0
    if m.startswith("j") and m[1:] in CC_USE:
        return CC_USE[m[1:]], 0
    if m in ("loope", "loopne"):
        return ZF, 0
    if m in ("scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd"):
        return (ALL if ins.rep else 0), ALL
    if m == "call":
        return 0, ALL
    if m in ("fcomi", "fucomi", "fcomip", "fucomip"):
        return 0, ALL
    if m == "salc":
        return CF, 0
    return 0, 0


def liveness(func, entries):
    """Per-block live-in flag sets; returns {block_start: live_out}."""
    blocks = func.blocks
    gen, kill = {}, {}
    for s, b in blocks.items():
        g = k = 0
        for ins in reversed(b.insns):
            u, d = flag_effects(ins)
            g = (g & ~d) | u
            k |= d
        gen[s], kill[s] = g, k
    live_in = {s: 0 for s in blocks}

    def succs(b):
        return [x for x in b.succ if x in blocks and not (x in entries and x != func.entry)]

    changed = True
    while changed:
        changed = False
        for s in sorted(blocks, reverse=True):
            b = blocks[s]
            out = 0
            for x in succs(b):
                out |= live_in[x]
            new = gen[s] | (out & ~kill[s])
            if new != live_in[s]:
                live_in[s] = new
                changed = True
    live_out = {}
    for s, b in blocks.items():
        out = 0
        for x in succs(b):
            out |= live_in[x]
        live_out[s] = out
    return live_out


# ---------------------------------------------------------------- operands
def reg(r):
    if r.kind == "g":
        if r.size == 32:
            return f"c->r[{r.num}].d"
        if r.size == 16:
            return f"c->r[{r.num}].w"
        if r.size == 8:
            return f"c->r[{r.num & 3}].b.{'h' if r.num >= 4 else 'l'}"
    if r.kind == "mm":
        return f"c->mm[{r.num}]"
    raise EmitError(f"register {r.name}")


def addr(m):
    if m.asz != 32:
        raise EmitError("16-bit addressing")
    parts = []
    if m.base is not None:
        parts.append(f"c->r[{m.base}].d")
    if m.index is not None:
        parts.append(f"c->r[{m.index}].d" + (f"*{m.scale}u" if m.scale != 1 else ""))
    d = m.disp & 0xFFFFFFFF
    if d or not parts:
        parts.append(f"0x{d:x}u")
    e = " + ".join(parts)
    if m.seg == "fs":
        e = "c->fs_base + " + e
    elif m.seg == "gs":
        raise EmitError("gs segment")
    return f"(uint32_t)({e})" if len(parts) > 1 or m.seg == "fs" else e


def size_of(o, default=32):
    if isinstance(o, Reg):
        return o.size
    if isinstance(o, Mem):
        return o.size or default
    if isinstance(o, Imm):
        return o.size
    return default


def rd(o, w=None):
    """C expression reading operand o as an unsigned value of width w."""
    if isinstance(o, Reg):
        return reg(o)
    if isinstance(o, Imm):
        v = o.val
        if w and w > o.size and o.sval < 0:
            v = o.sval & ((1 << w) - 1)
        return f"0x{v:x}u"
    if isinstance(o, Mem):
        w = w or o.size
        return f"R{w}({addr(o)})"
    raise EmitError(f"operand {o}")


def wr(o, expr, w=None):
    if isinstance(o, Reg):
        w = w or o.size
        return f"{reg(o)} = ({UT[w]})({expr});"
    if isinstance(o, Mem):
        w = w or o.size
        return f"W{w}({addr(o)}, ({UT[w]})({expr}));"
    raise EmitError(f"write to {o}")


def sign(w, x):
    return f"((({x}) >> {w - 1}) & 1)"


# ---------------------------------------------------------------- function emitter
class FuncEmitter:
    def __init__(self, prog, func, entries, imports, fname):
        self.prog = prog
        self.f = func
        self.entries = entries
        self.imports = imports  # iat va -> C function name
        self.fname = fname      # entry -> C function name
        self.out = []

    def L(self, a):
        return f"L_{a:x}"

    def target(self, t):
        """Statement transferring control to address t (goto or tail call)."""
        if t in self.f.blocks and not (t in self.entries and t != self.f.entry):
            return f"goto {self.L(t)};"
        if t in self.fname:
            return f"{{ {self.fname[t]}(c); return; }}"
        return f"{{ call_indirect(c, 0x{t:x}u); return; }}"

    def seh_handlers(self):
        """__except handlers of an MSVC SEH3 frame (push ebp; mov ebp, esp; push -1; push scopetable;
        push handler; mov eax, fs:[0]): a fault inside the function resumes there, as on Windows."""
        f = self.f
        if f.entry not in f.blocks:
            return []
        ins = f.blocks[f.entry].insns
        if len(ins) < 6 or ins[0].mnem != "push" or ins[1].mnem != "mov" or ins[2].mnem != "push" \
                or ins[3].mnem != "push" or ins[4].mnem != "push" or ins[5].mnem != "mov":
            return []
        if not (isinstance(ins[2].ops[0], Imm) and (ins[2].ops[0].val & 0xFFFFFFFF) == 0xFFFFFFFF
                and isinstance(ins[3].ops[0], Imm) and isinstance(ins[5].ops[1], Mem) and ins[5].ops[1].seg == "fs"):
            return []
        table = ins[3].ops[0].val & 0xFFFFFFFF
        levels = set()
        for b in f.blocks.values():
            for i in b.insns:
                if i.mnem == "mov" and isinstance(i.ops[0], Mem) and isinstance(i.ops[1], Imm) and \
                        i.ops[0].base == 5 and i.ops[0].index is None and (i.ops[0].disp & 0xFFFFFFFF) == 0xFFFFFFFC:
                    v = i.ops[1].val & 0xFFFFFFFF
                    if v < 64:
                        levels.add(v)
        out = []
        pe = self.prog.pe
        for lv in sorted(levels):
            filt, hand = pe.u32(table + 12 * lv + 4), pe.u32(table + 12 * lv + 8)
            if filt and hand in f.blocks and hand not in out:
                out.append(hand)
        return out

    def emit(self):
        f = self.f
        live_out = liveness(f, self.entries)
        o = self.out
        o.append(f"void {self.fname[f.entry]}(CPU *c) {{")
        o.append("  uint32_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0;")
        o.append("  (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of;")
        handlers = self.seh_handlers()
        if handlers:
            # a fault while this frame is on the SEH chain lands here (seh_dispatch) and goes on in the
            # __except handler; EBP/ESP were set from the frame like _except_handler3 does
            o.append("  SehLanding seh_l;")
            o.append("  seh_l.frame = c->r[4].d - 4;")
            o.append("  if (sigsetjmp(seh_l.jb, 1)) {")
            o.append("    switch (seh_l.target) {")
            for h in handlers:
                o.append(f"    case 0x{h:x}u: goto {self.L(h)};")
            o.append("    }")
            o.append("    rt_trap(c, seh_l.target, \"SEH handler outside the function\");")
            o.append("    return;")
            o.append("  }")
            o.append("  seh_push(&seh_l);")
        if f.entry != min(f.blocks):
            o.append(f"  goto {self.L(f.entry)};")
        order = sorted(f.blocks)
        for k, s in enumerate(order):
            b = f.blocks[s]
            nxt = order[k + 1] if k + 1 < len(order) else None
            o.append(f"{self.L(s)}: BLK(0x{s:x}u);")
            live = live_out[s]
            lives = []
            for ins in reversed(b.insns):
                lives.append(live)
                u, d = flag_effects(ins)
                live = (live & ~d) | u
            lives.reverse()
            for ins, la in zip(b.insns, lives):
                try:
                    self.insn(ins, la, b)
                except EmitError as e:
                    o.append(f"  rt_trap(c, 0x{ins.addr:x}u, \"unsupported: {ins.mnem} ({e})\");")
                    break
            else:
                self.terminate(b, nxt)
        o.append("}")
        return "\n".join(o)

    def terminate(self, b, nxt):
        o = self.out
        k = b.kind
        last = b.insns[-1] if b.insns else None
        if k in ("fall", "tailfall"):
            t = b.succ[0]
            if not (t == nxt and k == "fall" and t in self.f.blocks):
                o.append("  " + self.target(t))
        elif k in ("trap", "noret"):
            if k == "noret":
                o.append(f"  rt_trap(c, 0x{last.addr:x}u, \"returned from noreturn call\");")
            elif not (last and last.mnem in ("int3",)):
                o.append(f"  rt_trap(c, 0x{(last.addr if last else b.start):x}u, \"fell into data\");")
        # jmp/jcc/ret/table/tail are fully emitted by insn()

    # -------------------------------------------------------- flags
    def setflags(self, live, w, kind, a="a", b="b", r="r", extra=None):
        """Emit assignments for live flags. kind: add sub logic inc dec neg adc sbb."""
        out = []
        sb = lambda x: sign(w, x)  # noqa: E731
        if live & CF:
            e = {"add": f"({r} < {a})", "sub": f"({a} < {b})", "logic": "0", "neg": f"({a} != 0)",
                 "adc": extra and extra.get("cf"), "sbb": extra and extra.get("cf")}.get(kind)
            if e is not None:
                out.append(f"cf = {e};")
        if live & ZF:
            out.append(f"zf = ({r} == 0);")
        if live & SF:
            out.append(f"sf = {sb(r)};")
        if live & PF:
            out.append(f"pf = PF8({r});")
        if live & OF:
            e = {"add": sb(f"({a} ^ {r}) & ({b} ^ {r})"), "adc": sb(f"({a} ^ {r}) & ({b} ^ {r})"),
                 "sub": sb(f"({a} ^ {b}) & ({a} ^ {r})"), "sbb": sb(f"({a} ^ {b}) & ({a} ^ {r})"),
                 "logic": "0", "inc": sb(f"~{a} & {r}"), "dec": sb(f"{a} & ~{r}"), "neg": sb(f"{a} & {r}")}[kind]
            out.append(f"of = {e};")
        if live & AF:
            e = {"add": f"((({a} ^ {b} ^ {r}) >> 4) & 1)", "adc": f"((({a} ^ {b} ^ {r}) >> 4) & 1)",
                 "sub": f"((({a} ^ {b} ^ {r}) >> 4) & 1)", "sbb": f"((({a} ^ {b} ^ {r}) >> 4) & 1)",
                 "logic": "0", "inc": f"((({a} ^ {r}) >> 4) & 1)", "dec": f"((({a} ^ {r}) >> 4) & 1)",
                 "neg": f"((({a} ^ {r}) >> 4) & 1)"}[kind]
            out.append(f"af = {e};")
        return " ".join(out)

    # -------------------------------------------------------- instructions
    def insn(self, ins, live, blk):
        m = ins.mnem
        o = self.out
        h = getattr(self, "i_" + m, None)
        if h is None:
            if m.startswith("set"):
                h = self.i_setcc
            elif m.startswith("cmov"):
                h = self.i_cmovcc
            elif m.startswith("j") and m[1:] in CC_USE:
                h = self.i_jcc
            elif m in STRING:
                h = self.i_string
            elif m in SHIFTS:
                h = self.i_shift
            elif m.startswith("f"):
                h = self.i_x87
            elif m.startswith("p") or m in ("movq", "movd", "emms"):
                h = self.i_mmx
            else:
                raise EmitError("no handler")
        if ins.lock and m not in ("inc", "dec", "add", "sub", "and", "or", "xor", "xadd", "cmpxchg", "xchg"):
            raise EmitError("lock prefix")
        if ins.rep and m not in STRING and m not in ("ret", "nop", "pause"):
            raise EmitError("rep prefix on non-string")
        line = h(ins, live, blk)
        if line:
            o.append("  " + line)

    # --- data movement
    def i_mov(self, ins, live, blk):
        d, s = ins.ops
        if isinstance(d, Reg) and d.kind == "s" or isinstance(s, Reg) and s.kind == "s":
            if isinstance(s, Reg) and s.kind == "s":  # reading a segment register: flat model selectors
                return wr(d, {"cs": "0x1b", "ds": "0x23", "es": "0x23", "ss": "0x23", "fs": "0x3b", "gs": "0"}[
                    s.name], w=size_of(d, 16))
            return f"/* mov {d.name}, ... ignored */"
        w = size_of(d)
        return wr(d, rd(s, w), w)

    def i_movzx(self, ins, live, blk):
        d, s = ins.ops
        return wr(d, rd(s))

    def i_movsx(self, ins, live, blk):
        d, s = ins.ops
        return wr(d, f"({UT[d.size]})({ST_[size_of(s)]})({rd(s)})")

    def i_lea(self, ins, live, blk):
        d, s = ins.ops
        return wr(d, addr(s))

    def i_xchg(self, ins, live, blk):
        a, b = ins.ops
        w = size_of(a)
        if isinstance(a, Mem) or isinstance(b, Mem):
            mem, r = (a, b) if isinstance(a, Mem) else (b, a)
            return (f"{{ uint32_t ea = {addr(mem)}; {UT[w]} t = R{w}(ea); W{w}(ea, {reg(r)}); "
                    f"{reg(r)} = t; }}")
        return f"{{ {UT[w]} t = {reg(a)}; {reg(a)} = {reg(b)}; {reg(b)} = t; }}"

    def i_push(self, ins, live, blk):
        s = ins.ops[0]
        if isinstance(s, Reg) and s.kind == "s":
            return "PUSH32(0x23u);"
        if ins.opsz == 16:
            return f"PUSH16({rd(s, 16)});"
        return f"PUSH32({rd(s, 32)});"

    def i_pop(self, ins, live, blk):
        d = ins.ops[0]
        if isinstance(d, Reg) and d.kind == "s":
            return "ESP += 4;"
        if ins.opsz == 16:
            return f"{{ uint16_t v = R16(ESP); ESP += 2; {wr(d, 'v', 16)} }}"
        return f"{{ uint32_t v = R32(ESP); ESP += 4; {wr(d, 'v', 32)} }}"

    def i_pushal(self, ins, live, blk):
        return ("{ uint32_t t = ESP; PUSH32(EAX); PUSH32(ECX); PUSH32(EDX); PUSH32(EBX); PUSH32(t); "
                "PUSH32(EBP); PUSH32(ESI); PUSH32(EDI); }")

    def i_popal(self, ins, live, blk):
        return ("{ EDI = R32(ESP); ESI = R32(ESP + 4); EBP = R32(ESP + 8); EBX = R32(ESP + 16); "
                "EDX = R32(ESP + 20); ECX = R32(ESP + 24); EAX = R32(ESP + 28); ESP += 32; }")

    def flags_value(self):
        return "(cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | 0x200u | (c->df << 10) | (of << 11))"

    def i_pushfd(self, ins, live, blk):
        return f"PUSH32({self.flags_value()});"

    def i_popfd(self, ins, live, blk):
        return ("{ uint32_t v = R32(ESP); ESP += 4; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; "
                "zf = (v >> 6) & 1; sf = (v >> 7) & 1; c->df = (v >> 10) & 1; of = (v >> 11) & 1; }")

    def i_lahf(self, ins, live, blk):
        return "c->r[0].b.h = (uint8_t)(cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7));"

    def i_sahf(self, ins, live, blk):
        return ("{ uint32_t v = c->r[0].b.h; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; "
                "zf = (v >> 6) & 1; sf = (v >> 7) & 1; }")

    def i_cwde(self, ins, live, blk):
        return "EAX = (uint32_t)(int32_t)(int16_t)c->r[0].w;"

    def i_cbw(self, ins, live, blk):
        return "c->r[0].w = (uint16_t)(int16_t)(int8_t)c->r[0].b.l;"

    def i_cdq(self, ins, live, blk):
        return "EDX = (uint32_t)((int32_t)EAX >> 31);"

    def i_cwd(self, ins, live, blk):
        return "c->r[2].w = (uint16_t)((int16_t)c->r[0].w >> 15);"

    def i_bswap(self, ins, live, blk):
        r = reg(ins.ops[0])
        return f"{r} = __builtin_bswap32({r});"

    def i_xlatb(self, ins, live, blk):
        return "c->r[0].b.l = R8(EBX + c->r[0].b.l);"

    def i_nop(self, ins, live, blk):
        return None

    i_pause = i_nop
    i_wait = i_nop

    def i_cld(self, ins, live, blk):
        return "c->df = 0;"

    def i_std(self, ins, live, blk):
        return "c->df = 1;"

    def i_clc(self, ins, live, blk):
        return "cf = 0;" if live & CF else None

    def i_stc(self, ins, live, blk):
        return "cf = 1;" if live & CF else None

    def i_cmc(self, ins, live, blk):
        return "cf = !cf;" if live & CF else None

    def i_cpuid(self, ins, live, blk):
        return "rt_cpuid(c);"

    def i_rdtsc(self, ins, live, blk):
        return "rt_rdtsc(c);"

    def i_int3(self, ins, live, blk):
        return f"rt_trap(c, 0x{ins.addr:x}u, \"int3\");"

    def i_leave(self, ins, live, blk):
        return "ESP = EBP; EBP = R32(ESP); ESP += 4;"

    # --- arithmetic
    def binop(self, ins, live, kind, expr, write=True):
        d, s = ins.ops
        w = size_of(d)
        t = UT[w]
        if isinstance(d, Mem):
            pre = f"uint32_t ea = {addr(d)}; {t} a = R{w}(ea)"
            store = f"W{w}(ea, r);"
        else:
            pre = f"{t} a = {reg(d)}"
            store = f"{reg(d)} = r;"
        extra = None
        if kind in ("adc", "sbb"):
            if w == 32:
                if kind == "adc":
                    extra = {"cf": "(uint32_t)((((uint64_t)a + b + ci) >> 32) & 1)"}
                else:
                    extra = {"cf": "((uint64_t)a < (uint64_t)b + ci)"}
            else:
                if kind == "adc":
                    extra = {"cf": f"((((uint32_t)a + b + ci) >> {w}) & 1)"}
                else:
                    extra = {"cf": "((uint32_t)a < (uint32_t)b + ci)"}
        body = f"{pre}; {t} b = {rd(s, w)};"
        if kind in ("adc", "sbb"):
            body += " uint32_t ci = cf;"
        body += f" {t} r = ({t})({expr});"
        if write:
            body += " " + store
        fl = self.setflags(live, w, kind, extra=extra)
        if ins.lock and write:
            # lock prefix: perform atomically (the result value is recomputed by the atomic op)
            if kind not in ("add", "sub", "logic") or not isinstance(d, Mem):
                raise EmitError("lock form")
            op = {"+": "add", "-": "sub", "&": "and", "|": "or", "^": "xor"}[expr.split()[1]]
            return (f"{{ uint32_t ea = {addr(d)}; {t} b = {rd(s, w)}; {t} a = __atomic_fetch_{op}("
                    f"({t} *)(M + ea), b, __ATOMIC_SEQ_CST); {t} r = ({t})({expr}); {fl} }}")
        return f"{{ {body} {fl} }}"

    def i_add(self, ins, live, blk):
        return self.binop(ins, live, "add", "a + b")

    def i_sub(self, ins, live, blk):
        return self.binop(ins, live, "sub", "a - b")

    def i_cmp(self, ins, live, blk):
        if not live:
            return None
        return self.binop(ins, live, "sub", "a - b", write=False)

    def i_adc(self, ins, live, blk):
        return self.binop(ins, live, "adc", "a + b + ci")

    def i_sbb(self, ins, live, blk):
        return self.binop(ins, live, "sbb", "a - b - ci")

    def i_and(self, ins, live, blk):
        return self.binop(ins, live, "logic", "a & b")

    def i_or(self, ins, live, blk):
        return self.binop(ins, live, "logic", "a | b")

    def i_xor(self, ins, live, blk):
        d, s = ins.ops
        if isinstance(d, Reg) and isinstance(s, Reg) and d.name == s.name and not ins.lock:
            fl = self.setflags(live, d.size, "logic", r="0", a="0", b="0")
            return f"{reg(d)} = 0; {fl}".strip()
        return self.binop(ins, live, "logic", "a ^ b")

    def i_test(self, ins, live, blk):
        if not live:
            return None
        return self.binop(ins, live, "logic", "a & b", write=False)

    def unop(self, ins, live, kind, expr):
        d = ins.ops[0]
        w = size_of(d)
        t = UT[w]
        if ins.lock:
            if kind not in ("inc", "dec") or not isinstance(d, Mem):
                raise EmitError("lock form")
            op = "add" if kind == "inc" else "sub"
            fl = self.setflags(live, w, kind)
            return (f"{{ uint32_t ea = {addr(d)}; {t} a = __atomic_fetch_{op}(({t} *)(M + ea), 1, "
                    f"__ATOMIC_SEQ_CST); {t} r = ({t})({expr}); {fl} }}")
        if isinstance(d, Mem):
            pre = f"uint32_t ea = {addr(d)}; {t} a = R{w}(ea);"
            store = f"W{w}(ea, r);"
        else:
            pre = f"{t} a = {reg(d)};"
            store = f"{reg(d)} = r;"
        fl = self.setflags(live, w, kind) if kind != "not" else ""
        return f"{{ {pre} {t} r = ({t})({expr}); {store} {fl} }}"

    def i_inc(self, ins, live, blk):
        return self.unop(ins, live, "inc", "a + 1")

    def i_dec(self, ins, live, blk):
        return self.unop(ins, live, "dec", "a - 1")

    def i_neg(self, ins, live, blk):
        return self.unop(ins, live, "neg", "0 - a")

    def i_not(self, ins, live, blk):
        return self.unop(ins, live, "not", "~a")

    def i_xadd(self, ins, live, blk):
        d, s = ins.ops
        w = size_of(d)
        t = UT[w]
        fl = self.setflags(live, w, "add")
        if isinstance(d, Mem):
            if ins.lock:
                return (f"{{ uint32_t ea = {addr(d)}; {t} b = {reg(s)}; {t} a = __atomic_fetch_add(({t} *)(M + ea), "
                        f"b, __ATOMIC_SEQ_CST); {t} r = ({t})(a + b); {reg(s)} = a; {fl} }}")
            return (f"{{ uint32_t ea = {addr(d)}; {t} a = R{w}(ea), b = {reg(s)}; {t} r = ({t})(a + b); "
                    f"{reg(s)} = a; W{w}(ea, r); {fl} }}")
        return f"{{ {t} a = {reg(d)}, b = {reg(s)}; {t} r = ({t})(a + b); {reg(s)} = a; {reg(d)} = r; {fl} }}"

    def i_cmpxchg(self, ins, live, blk):
        d, s = ins.ops
        w = size_of(d)
        t = UT[w]
        acc = reg(Reg("g", 0, w))
        fl = self.setflags(live | ZF, w, "sub")
        if isinstance(d, Mem):
            if ins.lock:
                return (f"{{ uint32_t ea = {addr(d)}; {t} a = {acc}, b = a; "
                        f"if (!__atomic_compare_exchange_n(({t} *)(M + ea), &b, {reg(s)}, 0, __ATOMIC_SEQ_CST, "
                        f"__ATOMIC_SEQ_CST)) {acc} = b; {t} r = ({t})(a - b); {fl} }}")
            return (f"{{ uint32_t ea = {addr(d)}; {t} a = {acc}, b = R{w}(ea); {t} r = ({t})(a - b); "
                    f"if (a == b) W{w}(ea, {reg(s)}); else {acc} = b; {fl} }}")
        return (f"{{ {t} a = {acc}, b = {reg(d)}; {t} r = ({t})(a - b); if (a == b) {reg(d)} = {reg(s)}; "
                f"else {acc} = b; {fl} }}")

    def i_mul(self, ins, live, blk):
        s = ins.ops[0]
        w = size_of(s)
        fl = []
        if w == 8:
            core = f"uint16_t p = (uint16_t)(c->r[0].b.l * (uint32_t){rd(s)}); c->r[0].w = p; uint32_t hi = p >> 8;"
            low = "(uint8_t)p"
        elif w == 16:
            core = (f"uint32_t p = (uint32_t)c->r[0].w * {rd(s)}; c->r[0].w = (uint16_t)p; "
                    "c->r[2].w = (uint16_t)(p >> 16); uint32_t hi = p >> 16;")
            low = "(uint16_t)p"
        else:
            core = f"uint64_t p = (uint64_t)EAX * {rd(s)}; EAX = (uint32_t)p; EDX = (uint32_t)(p >> 32); uint32_t hi = EDX;"
            low = "(uint32_t)p"
        if live & (CF | OF):
            fl.append("cf = of = (hi != 0);")
        if live & ZF:
            fl.append(f"zf = ({low} == 0);")
        if live & SF:
            fl.append(f"sf = {sign(w, low)};")
        if live & PF:
            fl.append(f"pf = PF8({low});")
        if live & AF:
            fl.append("af = 0;")
        return "{ " + core + " " + " ".join(fl) + " }"

    def i_imul(self, ins, live, blk):
        ops = ins.ops
        fl = []
        if len(ops) == 1:
            s = ops[0]
            w = size_of(s)
            if w == 8:
                core = (f"int32_t p = (int32_t)(int8_t)c->r[0].b.l * (int8_t){rd(s)}; c->r[0].w = (uint16_t)p; "
                        "uint32_t ovf = (p != (int8_t)p);")
                low = "(uint8_t)p"
            elif w == 16:
                core = (f"int32_t p = (int32_t)(int16_t)c->r[0].w * (int16_t){rd(s)}; c->r[0].w = (uint16_t)p; "
                        "c->r[2].w = (uint16_t)((uint32_t)p >> 16); uint32_t ovf = (p != (int16_t)p);")
                low = "(uint16_t)p"
            else:
                core = (f"int64_t p = (int64_t)(int32_t)EAX * (int32_t){rd(s)}; EAX = (uint32_t)p; "
                        "EDX = (uint32_t)((uint64_t)p >> 32); uint32_t ovf = (p != (int32_t)p);")
                low = "(uint32_t)p"
        else:
            d = ops[0]
            a, b = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
            w = size_of(d)
            if w == 32:
                core = (f"int64_t p = (int64_t)(int32_t){rd(a, 32)} * (int32_t){rd(b, 32)}; "
                        f"uint32_t ovf = (p != (int32_t)p); {reg(d)} = (uint32_t)p;")
                low = "(uint32_t)p"
            else:
                core = (f"int32_t p = (int32_t)(int16_t){rd(a, 16)} * (int16_t){rd(b, 16)}; "
                        f"uint32_t ovf = (p != (int16_t)p); {reg(d)} = (uint16_t)p;")
                low = "(uint16_t)p"
        if live & (CF | OF):
            fl.append("cf = of = ovf;")
        if live & ZF:
            fl.append(f"zf = ({low} == 0);")
        if live & SF:
            fl.append(f"sf = {sign(w, low)};")
        if live & PF:
            fl.append(f"pf = PF8({low});")
        if live & AF:
            fl.append("af = 0;")
        return "{ " + core + " (void)ovf; " + " ".join(fl) + " }"

    def i_div(self, ins, live, blk):
        s = ins.ops[0]
        w = size_of(s)
        a = ins.addr
        if w == 8:
            return (f"{{ uint32_t d = {rd(s)}; uint32_t n = c->r[0].w; if (!d || n / d > 0xFF) rt_div_fault(c, 0x{a:x}u); "
                    "c->r[0].b.l = (uint8_t)(n / d); c->r[0].b.h = (uint8_t)(n % d); }")
        if w == 16:
            return (f"{{ uint32_t d = {rd(s)}; uint32_t n = ((uint32_t)c->r[2].w << 16) | c->r[0].w; "
                    f"if (!d || n / d > 0xFFFF) rt_div_fault(c, 0x{a:x}u); c->r[0].w = (uint16_t)(n / d); "
                    "c->r[2].w = (uint16_t)(n % d); }")
        return (f"{{ uint64_t d = {rd(s)}; uint64_t n = ((uint64_t)EDX << 32) | EAX; "
                f"if (!d || n / d > 0xFFFFFFFFull) rt_div_fault(c, 0x{a:x}u); EAX = (uint32_t)(n / d); "
                "EDX = (uint32_t)(n % d); }")

    def i_idiv(self, ins, live, blk):
        s = ins.ops[0]
        w = size_of(s)
        a = ins.addr
        if w == 8:
            return (f"{{ int32_t d = (int8_t){rd(s)}; int32_t n = (int16_t)c->r[0].w; "
                    f"if (!d || n / d > 127 || n / d < -128) rt_div_fault(c, 0x{a:x}u); "
                    "c->r[0].b.l = (uint8_t)(n / d); c->r[0].b.h = (uint8_t)(n % d); }")
        if w == 16:
            return (f"{{ int32_t d = (int16_t){rd(s)}; int32_t n = (int32_t)(((uint32_t)c->r[2].w << 16) | c->r[0].w); "
                    f"if (!d || (n == INT32_MIN && d == -1) || n / d > 32767 || n / d < -32768) rt_div_fault(c, 0x{a:x}u); "
                    "c->r[0].w = (uint16_t)(n / d); c->r[2].w = (uint16_t)(n % d); }")
        return (f"{{ int64_t d = (int32_t){rd(s)}; int64_t n = (int64_t)(((uint64_t)EDX << 32) | EAX); "
                f"if (!d || (n == INT64_MIN && d == -1) || n / d > 2147483647LL || n / d < -2147483648LL) "
                f"rt_div_fault(c, 0x{a:x}u); EAX = (uint32_t)(n / d); EDX = (uint32_t)(n % d); }}")

    # --- shifts and rotates
    def i_shift(self, ins, live, blk):
        m = ins.mnem
        d, cnt = ins.ops
        w = size_of(d)
        t = UT[w]
        if isinstance(d, Mem):
            pre = f"uint32_t ea = {addr(d)}; {t} a = R{w}(ea);"
            store = f"W{w}(ea, r);"
        else:
            pre = f"{t} a = {reg(d)};"
            store = f"{reg(d)} = r;"
        if isinstance(cnt, Imm):
            n = cnt.val & 31
            if n == 0:
                return None
            nexpr = str(n)
        else:
            nexpr = "(c->r[1].b.l & 31)"
        fl = []
        if m in ("shl", "sal"):
            core = f"uint64_t t = (uint64_t)a << n; {t} r = ({t})t;"
            if live & CF:
                fl.append(f"cf = (uint32_t)((t >> {w}) & 1);")
            if live & OF:
                fl.append(f"of = {sign(w, 'r')} ^ (uint32_t)((t >> {w}) & 1);")
            kind = "shift"
        elif m == "shr":
            core = f"{t} r = ({t})((uint32_t)a >> n);"
            if live & CF:
                fl.append("cf = (uint32_t)((a >> (n - 1)) & 1);")
            if live & OF:
                fl.append(f"of = {sign(w, 'a')};")
            kind = "shift"
        elif m == "sar":
            core = f"{t} r = ({t})((int32_t)({ST_[w]})a >> (n > {w - 1} ? {w - 1} : n));"
            if live & CF:
                fl.append(f"cf = (uint32_t)(((int32_t)({ST_[w]})a >> (n - 1 > {w - 1} ? {w - 1} : n - 1)) & 1);")
            if live & OF:
                fl.append("of = 0;")
            kind = "shift"
        elif m in ("rol", "ror"):
            if m == "rol":
                core = f"uint32_t k = n % {w}; {t} r = ({t})(k ? (({t})(a << k) | (a >> ({w} - k))) : a);"
                if live & CF:
                    fl.append("cf = r & 1;")
                if live & OF:
                    fl.append(f"of = {sign(w, 'r')} ^ (r & 1);")
            else:
                core = f"uint32_t k = n % {w}; {t} r = ({t})(k ? ((a >> k) | ({t})(a << ({w} - k))) : a);"
                if live & CF:
                    fl.append(f"cf = {sign(w, 'r')};")
                if live & OF:
                    fl.append(f"of = {sign(w, 'r')} ^ ((r >> {w - 2}) & 1);")
            kind = "rot"
        else:  # rcl / rcr: bit-serial through carry
            step = (f"for (uint32_t i = 0; i < n % {w + 1}; i++) {{ uint32_t o = {sign(w, 'r')}; "
                    f"r = ({t})((r << 1) | cy); cy = o; }}") if m == "rcl" else \
                   (f"for (uint32_t i = 0; i < n % {w + 1}; i++) {{ uint32_t o = r & 1; "
                    f"r = ({t})((r >> 1) | (cy << {w - 1})); cy = o; }}")
            core = f"{t} r = a; uint32_t cy = cf; {step}"
            fl.append("cf = cy;")
            if m == "rcl":
                fl.append(f"of = {sign(w, 'r')} ^ cy;")
            else:
                fl.append(f"of = {sign(w, 'r')} ^ ((r >> {w - 2}) & 1);")
            kind = "rot"
        if kind == "shift":
            if live & ZF:
                fl.append("zf = (r == 0);")
            if live & SF:
                fl.append(f"sf = {sign(w, 'r')};")
            if live & PF:
                fl.append("pf = PF8(r);")
            if live & AF:
                fl.append("af = 0;")
        body = f"{pre} uint32_t n = {nexpr}; {core} {store} {' '.join(fl)}"
        if not isinstance(cnt, Imm):
            body = f"{pre} uint32_t n = {nexpr}; if (n) {{ {core} {store} {' '.join(fl)} }}"
        return "{ " + body + " }"

    def i_shld(self, ins, live, blk):
        return self.dshift(ins, live, left=True)

    def i_shrd(self, ins, live, blk):
        return self.dshift(ins, live, left=False)

    def dshift(self, ins, live, left):
        d, s, cnt = ins.ops
        w = size_of(d)
        if w != 32:
            raise EmitError("16-bit shld/shrd")
        if isinstance(d, Mem):
            pre = f"uint32_t ea = {addr(d)}; uint32_t a = R32(ea);"
            store = "W32(ea, r);"
        else:
            pre = f"uint32_t a = {reg(d)};"
            store = f"{reg(d)} = r;"
        nexpr = str(cnt.val & 31) if isinstance(cnt, Imm) else "(c->r[1].b.l & 31)"
        if left:
            core = (f"uint32_t b = {reg(s)}; uint32_t r = (a << n) | (b >> (32 - n)); "
                    "uint32_t co = (a >> (32 - n)) & 1;")
        else:
            core = (f"uint32_t b = {reg(s)}; uint32_t r = (a >> n) | (b << (32 - n)); "
                    "uint32_t co = (a >> (n - 1)) & 1;")
        fl = []
        if live & CF:
            fl.append("cf = co;")
        if live & OF:
            fl.append("of = ((r ^ a) >> 31) & 1;")
        if live & ZF:
            fl.append("zf = (r == 0);")
        if live & SF:
            fl.append("sf = r >> 31;")
        if live & PF:
            fl.append("pf = PF8(r);")
        return f"{{ {pre} uint32_t n = {nexpr}; if (n) {{ {core} {store} {' '.join(fl)} }} }}"

    # --- bit operations
    def bitop(self, ins, live, kind):
        d, s = ins.ops
        w = size_of(d)
        t = UT[w]
        if isinstance(d, Mem):
            if isinstance(s, Imm):
                pre = f"uint32_t ea = {addr(d)}; uint32_t bit = {s.val & (w - 1)};"
            else:
                pre = (f"int32_t off = (int32_t)({ST_[w]}){reg(s)}; uint32_t ea = {addr(d)} + "
                       f"(uint32_t)((off >> {5 if w == 32 else 4}) * {w // 8}); uint32_t bit = (uint32_t)off & {w - 1};")
            get = f"{t} a = R{w}(ea);"
            put = f"W{w}(ea, r);"
        else:
            pre = f"uint32_t bit = ({rd(s)}) & {w - 1};"
            get = f"{t} a = {reg(d)};"
            put = f"{reg(d)} = r;"
        core = {"bt": "", "bts": f"{t} r = ({t})(a | (1u << bit)); {put}",
                "btr": f"{t} r = ({t})(a & ~(1u << bit)); {put}", "btc": f"{t} r = ({t})(a ^ (1u << bit)); {put}"}[kind]
        fl = "cf = (a >> bit) & 1;" if live & CF else ""
        return f"{{ {pre} {get} {fl} {core} }}"

    def i_bt(self, ins, live, blk):
        return self.bitop(ins, live, "bt")

    def i_bts(self, ins, live, blk):
        return self.bitop(ins, live, "bts")

    def i_btr(self, ins, live, blk):
        return self.bitop(ins, live, "btr")

    def i_btc(self, ins, live, blk):
        return self.bitop(ins, live, "btc")

    def i_bsf(self, ins, live, blk):
        d, s = ins.ops
        return f"{{ uint32_t v = {rd(s)}; zf = (v == 0); if (v) {wr(d, '__builtin_ctz(v)')} }}"

    def i_bsr(self, ins, live, blk):
        d, s = ins.ops
        return f"{{ uint32_t v = {rd(s)}; zf = (v == 0); if (v) {wr(d, '31 - __builtin_clz(v)')} }}"

    # --- conditional ops
    def i_setcc(self, ins, live, blk):
        cc = cc_of(ins.mnem, "set")
        return wr(ins.ops[0], f"{CC_EXPR[cc]} ? 1 : 0", 8)

    def i_cmovcc(self, ins, live, blk):
        cc = cc_of(ins.mnem, "cmov")
        d, s = ins.ops
        return f"if ({CC_EXPR[cc]}) {wr(d, rd(s, d.size))}"

    def i_salc(self, ins, live, blk):
        return "c->r[0].b.l = cf ? 0xFF : 0;"

    # --- control flow
    def i_jmp(self, ins, live, blk):
        o = ins.ops[0]
        if isinstance(o, Rel):
            return self.target(o.target)
        imp = self.prog.import_of(ins)
        if imp:
            return f"{{ {self.imports[ins.ops[0].disp & 0xFFFFFFFF]}(c); return; }}"
        if blk.kind == "table":
            m = o
            idx = f"c->r[{m.index}].d"
            lines = [f"switch (R32(0x{m.disp & 0xFFFFFFFF:x}u + {idx}*4u)) {{"]
            for t in blk.succ:
                lines.append(f"    case 0x{t:x}u: {self.target(t)}")
            lines.append(f"    default: rt_trap(c, 0x{ins.addr:x}u, \"jump table target\");")
            lines.append("  }")
            return "\n  ".join(lines)
        return f"{{ call_indirect(c, {rd(o, 32)}); return; }}"

    def i_jcc(self, ins, live, blk):
        cc = ins.mnem[1:]
        t = ins.ops[0].target
        fall = ins.addr + ins.size
        return f"if ({CC_EXPR[cc]}) {self.target(t)} {self.target(fall)}"

    def i_jecxz(self, ins, live, blk):
        t = ins.ops[0].target
        return f"if (ECX == 0) {self.target(t)} {self.target(ins.addr + ins.size)}"

    def i_loop(self, ins, live, blk):
        t = ins.ops[0].target
        cond = {"loop": "ECX != 0", "loope": "ECX != 0 && zf", "loopne": "ECX != 0 && !zf"}[ins.mnem]
        return f"ECX -= 1; if ({cond}) {self.target(t)} {self.target(ins.addr + ins.size)}"

    i_loope = i_loop
    i_loopne = i_loop

    def i_call(self, ins, live, blk):
        o = ins.ops[0]
        ret = ins.addr + ins.size
        push = f"PUSH32(0x{ret:x}u);"
        if isinstance(o, Rel):
            t = o.target
            fn = self.fname.get(t)
            if fn:
                return f"{push} {fn}(c);"
            return f"{push} call_indirect(c, 0x{t:x}u);"
        imp = self.prog.import_of(ins)
        if imp:
            return f"{push} {self.imports[o.disp & 0xFFFFFFFF]}(c);"
        return f"{{ uint32_t t = {rd(o, 32)}; {push} call_indirect(c, t); }}"

    def i_ret(self, ins, live, blk):
        n = 4 + (ins.ops[0].val if ins.ops else 0)
        return f"ESP += {n}; return;"

    # --- string instructions
    def i_string(self, ins, live, blk):
        m = ins.mnem
        w = STRING[m]
        t = UT[w]
        sz = w // 8
        step = f"(c->df ? -{sz} : {sz})"
        src_seg = "c->fs_base + " if ins.seg == "fs" else ""
        if m.startswith("movs"):
            one = f"W{w}(EDI, R{w}({src_seg}ESI)); ESI += {step}; EDI += {step};"
            if ins.rep:
                fast = ""
                if not src_seg:
                    fast = (f"if (!c->df && (EDI - ESI >= ECX * {sz}u || EDI <= ESI)) {{ "
                            f"memmove(M + EDI, M + ESI, (size_t)ECX * {sz}); ESI += ECX * {sz}; EDI += ECX * {sz}; ECX = 0; }} ")
                return f"{{ {fast}while (ECX) {{ {one} ECX--; }} }}"
            return one
        if m.startswith("stos"):
            acc = reg(Reg("g", 0, w))
            one = f"W{w}(EDI, {acc}); EDI += {step};"
            if ins.rep:
                fast = ""
                if w == 8:
                    fast = "if (!c->df) { memset(M + EDI, c->r[0].b.l, ECX); EDI += ECX; ECX = 0; } "
                return f"{{ {fast}while (ECX) {{ {one} ECX--; }} }}"
            return one
        if m.startswith("lods"):
            acc = reg(Reg("g", 0, w))
            one = f"{acc} = R{w}({src_seg}ESI); ESI += {step};"
            if ins.rep:
                return f"{{ while (ECX) {{ {one} ECX--; }} }}"
            return one
        # scas / cmps: compare and set flags like cmp
        if m.startswith("scas"):
            acc = reg(Reg("g", 0, w))
            cmp = f"{t} a = {acc}, b = R{w}(EDI); EDI += {step};"
        else:
            cmp = f"{t} a = R{w}({src_seg}ESI), b = R{w}(EDI); ESI += {step}; EDI += {step};"
        fl = self.setflags(ALL, w, "sub")
        body = f"{cmp} {t} r = ({t})(a - b); {fl}"
        if ins.rep:
            stop = "if (!zf) break;" if ins.rep == "rep" else "if (zf) break;"
            return f"while (ECX) {{ {body} ECX--; {stop} }}"
        return f"{{ {body} }}"

    # --- x87
    def i_x87(self, ins, live, blk):
        m = ins.mnem
        ops = ins.ops

        def stn(r):
            return f"ST({r.num})"

        def memval(o):
            if o.size == 32:
                return f"(double)RF32({addr(o)})"
            if o.size == 64:
                return f"RF64({addr(o)})"
            if o.size == 80:
                return f"rt_f80_load({addr(o)})"
            raise EmitError("x87 mem size")

        def intval(o):
            if o.size == 16:
                return f"(double)(int16_t)R16({addr(o)})"
            if o.size == 32:
                return f"(double)(int32_t)R32({addr(o)})"
            if o.size == 64:
                return f"(double)(int64_t)R64({addr(o)})"
            raise EmitError("x87 int size")

        arith = {"fadd": "+", "fmul": "*", "fsub": "-", "fdiv": "/", "fsubr": "r-", "fdivr": "r/"}
        base = m.rstrip("p") if m not in ("fcompp", "fucompp") else m
        if m == "fld":
            o = ops[0]
            if isinstance(o, Reg):
                return f"{{ double v = {stn(o)}; fpush(c, v); }}"
            return f"fpush(c, {memval(o)});"
        if m == "fild":
            return f"fpush(c, {intval(ops[0])});"
        consts = {"fld1": "1.0", "fldz": "0.0", "fldpi": "M_PI", "fldl2e": "M_LOG2E", "fldl2t": "3.321928094887362347",
                  "fldlg2": "0.301029995663981195", "fldln2": "M_LN2"}
        if m in consts:
            return f"fpush(c, {consts[m]});"
        if m in ("fst", "fstp"):
            o = ops[0]
            pop = " fpop(c);" if m == "fstp" else ""
            if isinstance(o, Reg):
                return f"{stn(o)} = ST(0);{pop}"
            if o.size == 32:
                return f"WF32({addr(o)}, (float)ST(0));{pop}"
            if o.size == 64:
                return f"WF64({addr(o)}, ST(0));{pop}"
            if o.size == 80:
                return f"rt_f80_store({addr(o)}, ST(0));{pop}"
            raise EmitError("fst size")
        if m in ("fist", "fistp", "fisttp"):
            o = ops[0]
            pop = " fpop(c);" if m != "fist" else ""
            v = "trunc(ST(0))" if m == "fisttp" else "ST(0)"
            return f"W{o.size}({addr(o)}, fist{o.size}(c, {v}));{pop}"
        if base in arith:
            op = arith[base]
            pop = m.endswith("p") and m not in arith
            if len(ops) == 1 and isinstance(ops[0], Mem):
                dst, src = "ST(0)", memval(ops[0])
            elif len(ops) == 2:
                dst, src = stn(ops[0]) if ops[0].num or ops[0].size != -1 else "ST(0)", stn(ops[1])
                if ops[0].size == -1:
                    dst = "ST(0)"
                if ops[1].size == -1:
                    src = "ST(0)"
            elif not ops:  # faddp with no operands = faddp st(1), st
                dst, src = "ST(1)", "ST(0)"
            else:
                raise EmitError("x87 arith form")
            if op.startswith("r"):
                e = f"{dst} = {src} {op[1]} {dst};"
            else:
                e = f"{dst} = {dst} {op} {src};"
            return "{ " + e + (" fpop(c);" if pop else "") + " }"
        iarith = {"fiadd": "+", "fimul": "*", "fisub": "-", "fidiv": "/", "fisubr": "r-", "fidivr": "r/"}
        if m in iarith:
            op = iarith[m]
            src = intval(ops[0])
            if op.startswith("r"):
                return f"ST(0) = {src} {op[1]} ST(0);"
            return f"ST(0) = ST(0) {op} {src};"
        if m in ("fcom", "fcomp", "fucom", "fucomp", "ficom", "ficomp"):
            pop = " fpop(c);" if m.endswith("p") else ""
            o = ops[0] if ops else Reg("st", 1, 80)
            if isinstance(o, Mem):
                src = intval(o) if m.startswith("fi") else memval(o)
            else:
                src = stn(o)
            return f"fcom_sw(c, ST(0), {src});{pop}"
        if m in ("fcompp", "fucompp"):
            return "fcom_sw(c, ST(0), ST(1)); fpop(c); fpop(c);"
        if m in ("fcomi", "fucomi", "fcomip", "fucomip"):
            pop = " fpop(c);" if m.endswith("p") else ""
            src = stn(ops[1])
            return (f"{{ double a = ST(0), b = {src}; int un = (a != a || b != b); zf = un || a == b; pf = un; "
                    f"cf = un || a < b; of = sf = af = 0; }}{pop}")
        if m == "ftst":
            return "fcom_sw(c, ST(0), 0.0);"
        if m == "fxch":
            o = ops[0] if ops else Reg("st", 1, 80)
            return f"{{ double t = ST(0); ST(0) = {stn(o)}; {stn(o)} = t; }}"
        simple = {"fchs": "ST(0) = -ST(0);", "fabs": "ST(0) = fabs(ST(0));", "fsqrt": "ST(0) = sqrt(ST(0));",
                  "fsin": "ST(0) = sin(ST(0)); c->fsw &= ~0x400;", "fcos": "ST(0) = cos(ST(0)); c->fsw &= ~0x400;",
                  "frndint": "ST(0) = fround(c, ST(0));",
                  "f2xm1": "ST(0) = expm1(ST(0) * M_LN2);",
                  "fscale": "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
                  "fyl2x": "{ double v = ST(1) * log2(ST(0)); fpop(c); ST(0) = v; }",
                  "fyl2xp1": "{ double v = ST(1) * log2(ST(0) + 1.0); fpop(c); ST(0) = v; }",
                  "fpatan": "{ double v = atan2(ST(1), ST(0)); fpop(c); ST(0) = v; }",
                  "fptan": "ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~0x400;",
                  "fsincos": "{ double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->fsw &= ~0x400; }",
                  "fprem": "rt_fprem(c, 0);", "fprem1": "rt_fprem(c, 1);", "fxam": "rt_fxam(c);",
                  "fxtract": "rt_fxtract(c);",
                  "fninit": "fninit(c);", "fnclex": "c->fsw &= 0x7F00;", "fnop": None,
                  "fdecstp": "c->ftop = (c->ftop - 1) & 7;", "fincstp": "c->ftop = (c->ftop + 1) & 7;"}
        if m in simple:
            return simple[m]
        if m == "fnstsw":
            o = ops[0]
            if isinstance(o, Reg):
                return "c->r[0].w = fstsw(c);"
            return f"W16({addr(o)}, fstsw(c));"
        if m == "fnstcw":
            return f"W16({addr(ops[0])}, c->fcw);"
        if m == "fldcw":
            return f"c->fcw = R16({addr(ops[0])});"
        if m in ("ffree", "ffreep"):
            return "fpop(c);" if m == "ffreep" else None
        if m == "fnstenv":
            return f"rt_fnstenv(c, {addr(ops[0])});"
        if m == "fldenv":
            return f"rt_fldenv(c, {addr(ops[0])});"
        if m == "fnsave":
            return f"rt_fnsave(c, {addr(ops[0])});"
        if m == "frstor":
            return f"rt_frstor(c, {addr(ops[0])});"
        if m.startswith("fcmov"):
            cc = {"fcmovb": "cf", "fcmove": "zf", "fcmovbe": "(cf|zf)", "fcmovu": "pf", "fcmovnb": "!cf",
                  "fcmovne": "!zf", "fcmovnbe": "!(cf|zf)", "fcmovnu": "!pf"}[m]
            return f"if ({cc}) ST(0) = {stn(ops[1])};"
        raise EmitError("x87 op")

    # --- MMX
    def i_mmx(self, ins, live, blk):
        m = ins.mnem
        ops = ins.ops
        if m == "emms":
            return "c->ftag = 0xFFFF;"
        if m == "movd":
            d, s = ops
            if isinstance(d, Reg) and d.kind == "mm":
                return f"{reg(d)} = (uint64_t)({rd(s, 32)});"
            return wr(d, f"(uint32_t){reg(s)}", 32)
        if m == "movq":
            d, s = ops
            src = reg(s) if isinstance(s, Reg) else f"R64({addr(s)})"
            if isinstance(d, Reg):
                return f"{reg(d)} = {src};"
            return f"W64({addr(d)}, {src});"
        if m in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"):
            d, s = ops
            if isinstance(s, Imm):
                n = f"{s.val}u"
            elif isinstance(s, Reg):
                n = reg(s)
            else:
                n = f"R64({addr(s)})"
            return f"{reg(d)} = mmx_{m}({reg(d)}, {n});"
        if m not in MMX_OPS:
            raise EmitError("mmx op")
        d, s = ops
        if isinstance(s, Reg):
            src = reg(s)
        elif m.startswith("punpckl"):
            src = f"(uint64_t)R32({addr(s)})"
        else:
            src = f"R64({addr(s)})"
        return f"{reg(d)} = mmx_{m}({reg(d)}, {src});"
