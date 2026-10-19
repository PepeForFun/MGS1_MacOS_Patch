"""Function discovery and control-flow graphs for a PE32 image.

Pass 1 finds every function entry (program entry, direct call targets, code
pointers found in data or in instruction immediates). Pass 2 builds a CFG per
function, treating a jump to another function's entry as a tail call. Jump
tables are bounded by the `cmp idx, N; ja` guard that MSVC emits in front.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86dec import DecodeError, Decoder, Imm, Mem, Reg, Rel

NORETURN_IMPORTS = {"ExitProcess", "ExitThread", "FatalAppExitA", "FatalExit", "RaiseException"}
# Instructions that never appear in this game's real code; meeting one means we walked into data.
SUSPECT = {"in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "hlt", "cli", "sti", "lret", "iret",
           "iretd", "int", "into", "int1", "lcall", "ljmp", "les", "lds", "lss", "lfs", "lgs", "bound", "arpl",
           "salc", "aaa", "aas", "daa", "das", "aam", "aad", "sldt", "str", "lldt", "ltr", "verr", "verw", "sgdt",
           "sidt", "lgdt", "lidt", "smsw", "lmsw", "invlpg", "clts", "invd", "wbinvd", "ud2", "rsm", "sysenter",
           "sysexit", "syscall", "sysret", "wrmsr", "rdmsr", "rdpmc", "enter"}
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg",
       "loop", "loope", "loopne", "jecxz", "jcxz"}


class Block:
    __slots__ = ("start", "insns", "succ", "kind", "table")

    def __init__(self, start):
        self.start = start
        self.insns = []
        self.succ = []
        self.kind = None  # fall tailfall jmp jcc ret tail tailimport table ijmp trap noret
        self.table = None


class Function:
    __slots__ = ("entry", "blocks", "calls", "icalls", "notes")

    def __init__(self, entry):
        self.entry = entry
        self.blocks = {}
        self.calls = set()
        self.icalls = 0
        self.notes = []


class Program:
    def __init__(self, pe):
        self.pe = pe
        self.dec = Decoder(pe.image, pe.base)
        self.cache = {}
        text = [s for s in pe.sections if s.executable]
        self.text_lo = pe.base + min(s.va for s in text)
        self.text_hi = pe.base + max(s.va + max(s.vsize, s.raw_size) for s in text)
        self.entries = set()
        self.strong = set()   # program entry and direct call targets: real function starts
        self.funcs = {}
        self.noreturn = set()
        self.log = []

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def insn(self, a):
        i = self.cache.get(a)
        if i is None:
            i = self.dec.decode(a)
            self.cache[a] = i
        return i

    def import_of(self, ins):
        """(dll, name) if ins is `call/jmp dword ptr [IAT slot]`."""
        if ins.ops and isinstance(ins.ops[0], Mem):
            m = ins.ops[0]
            if m.base is None and m.index is None:
                return self.pe.imports.get(m.disp & 0xFFFFFFFF)
        return None

    def is_noreturn_call(self, ins):
        imp = self.import_of(ins)
        if imp:
            return imp[1] in NORETURN_IMPORTS
        if ins.ops and isinstance(ins.ops[0], Rel):
            return ins.ops[0].target in self.noreturn
        return False

    # ------------------------------------------------------------ jump tables
    def jump_table(self, preds, blocks, blk, ins):
        """Resolve `jmp [4*idx + table]` (optionally via a byte table). Returns targets or None."""
        m = ins.ops[0]
        if not (isinstance(m, Mem) and m.index is not None and m.scale == 4 and m.base is None):
            return None
        table = m.disp & 0xFFFFFFFF
        idx = m.index
        bytetable = bt_index = None
        for prev in reversed(blk.insns[:-1]):
            o0 = prev.ops[0] if prev.ops else None
            if prev.mnem in ("mov", "movzx") and isinstance(o0, Reg) and o0.kind == "g" and o0.num == idx and \
                    isinstance(prev.ops[1], Mem) and prev.ops[1].size == 8 and prev.ops[1].index is None and \
                    prev.ops[1].base is not None:
                bytetable = prev.ops[1].disp & 0xFFFFFFFF
                bt_index = prev.ops[1].base
                break
            if isinstance(o0, Reg) and o0.kind == "g" and o0.num == idx and prev.mnem != "xor":
                break
        bound = self.find_bound(preds, blocks, blk, bt_index if bytetable is not None else idx)
        if bytetable is not None:
            if bound is None:
                return None
            o = bytetable - self.pe.base
            n = max(self.pe.image[o:o + bound]) + 1
        else:
            n = bound
        if n is None:
            n = self.and_bound(blk, idx)
        if n is None:
            # Hand-written asm (CRT memcpy) indexes tables with masked or even negative values.
            # The emitted switch reads the real table entry, so a superset of targets is safe:
            # take every nearby dword that points at decodable code close to this block.
            out = []
            for k in range(-32, 64):
                v = self.pe.u32(table + 4 * k)
                if self.in_text(v) and abs(v - blk.start) < 0x4000:
                    try:
                        self.insn(v)
                    except (DecodeError, IndexError):
                        continue
                    out.append(v)
            self.log.append(f"jump table {table:#x} at {ins.addr:#x}: no guard, {len(out)} nearby targets")
            return out or None
        return [self.pe.u32(table + 4 * k) for k in range(n)]

    def and_bound(self, blk, reg):
        """`and reg, 2^k-1` before the jump bounds the index."""
        for prev in reversed(blk.insns[:-1]):
            o0 = prev.ops[0] if prev.ops else None
            if isinstance(o0, Reg) and o0.kind == "g" and o0.num == reg:
                if prev.mnem == "and" and isinstance(prev.ops[1], Imm):
                    v = prev.ops[1].val
                    if v and (v & (v + 1)) == 0:
                        return v + 1
                return None
        return None

    def find_bound(self, preds, blocks, blk, reg):
        """`cmp reg, N` + `ja` in this block or a predecessor. MSVC may schedule instructions that
        leave the flags alone (pop, mov, push, lea) between the two."""
        flagless = {"pop", "push", "mov", "lea", "movzx", "movsx", "nop", "xchg"}
        for b in [blk] + [blocks[p] for p in preds.get(blk.start, ()) if p in blocks]:
            ins = b.insns
            for k in range(len(ins) - 1, 0, -1):
                j = ins[k]
                if j.mnem in ("ja", "jae", "jbe", "jb"):
                    m = k - 1
                    while m >= 0 and ins[m].mnem in flagless:
                        m -= 1
                    if m < 0:
                        break
                    c = ins[m]
                    if c.mnem == "cmp" and isinstance(c.ops[1], Imm):
                        a = c.ops[0]
                        if (isinstance(a, Reg) and a.kind == "g" and a.num == reg) or isinstance(a, Mem):
                            n = c.ops[1].val
                            return n + 1 if j.mnem in ("ja", "jbe") else n
                    break
        return None

    # ------------------------------------------------------------ exploration
    def seh_handlers(self, entry):
        """__except handler blocks of an MSVC SEH3 frame (push ebp; mov ebp, esp; push -1; push
        scopetable; push handler; mov eax, fs:[0]). Only the exception dispatcher reaches them, so they
        are explored as part of the function (the emitter lands there after a fault)."""
        try:
            ins, a = [], entry
            for _ in range(6):
                i = self.insn(a)
                ins.append(i)
                a += i.size
        except Exception:
            return []
        if [i.mnem for i in ins] != ["push", "mov", "push", "push", "push", "mov"]:
            return []
        if not (isinstance(ins[2].ops[0], Imm) and ins[2].ops[0].val == 0xFFFFFFFF and isinstance(ins[3].ops[0], Imm)
                and isinstance(ins[5].ops[1], Mem) and ins[5].ops[1].seg == "fs"):
            return []
        table, out = ins[3].ops[0].val, []
        lo, hi = self.pe.base + 0x1000, self.pe.base + len(self.pe.image)
        for level in range(16):
            try:
                enc, filt, hand = self.pe.u32(table + 12 * level), self.pe.u32(table + 12 * level + 4), \
                    self.pe.u32(table + 12 * level + 8)
            except Exception:
                break
            enc = enc - (1 << 32) if enc & 0x80000000 else enc
            if not (-1 <= enc < level) or not (lo <= hand < hi) or not (entry < hand < entry + 0x10000) or \
                    (filt and not lo <= filt < hi):
                break
            if filt:
                out.append(hand)
        return out

    def explore(self, entry, boundaries, strict=False):
        """Blocks reachable from entry. Jumps to other entries in `boundaries` become tail calls.
        Returns a Function, or None when strict and the code looks bogus."""
        f = Function(entry)
        blocks = f.blocks
        owner = {}  # insn addr -> start of the block holding it
        preds = collections.defaultdict(set)
        work = [entry] + self.seh_handlers(entry)

        def link(b, succ):
            b.succ = succ
            for s_ in succ:
                preds[s_].add(b.start)

        def split(a):
            ob = blocks[owner[a]]
            k = next(i for i, x in enumerate(ob.insns) if x.addr == a)
            nb = Block(a)
            nb.insns = ob.insns[k:]
            nb.kind, nb.table = ob.kind, ob.table
            for s_ in ob.succ:
                preds[s_].discard(ob.start)
            link(nb, ob.succ)
            ob.insns = ob.insns[:k]
            ob.kind, ob.table = "fall", None
            link(ob, [a])
            for x in nb.insns:
                owner[x.addr] = a
            blocks[a] = nb

        while work:
            a = work.pop()
            if a in blocks:
                continue
            if a in owner:
                split(a)
                continue
            b = Block(a)
            blocks[a] = b
            p = a
            while True:
                if p != a and (p in blocks or p in owner or p in boundaries):
                    b.kind = "tailfall" if (p in boundaries and p != entry) else "fall"
                    link(b, [p])
                    if b.kind == "fall" and p not in blocks:
                        work.append(p)
                    break
                if not self.in_text(p):
                    if strict:
                        return None
                    b.kind = "trap"
                    f.notes.append(f"ran out of .text at {p:#x}")
                    break
                try:
                    ins = self.insn(p)
                except (DecodeError, IndexError) as e:
                    if strict:
                        return None
                    b.kind = "trap"
                    f.notes.append(f"decode error at {p:#x}: {e}")
                    break
                if ins.mnem in SUSPECT:
                    if strict:
                        return None
                    b.kind = "trap"
                    f.notes.append(f"suspect {ins.mnem} at {p:#x}")
                    break
                b.insns.append(ins)
                owner[p] = a
                nxt = p + ins.size
                mn = ins.mnem
                if mn == "int3":
                    if strict and p == entry:
                        return None
                    b.kind = "trap"
                    break
                if mn == "ret":
                    b.kind = "ret"
                    break
                if mn == "jmp":
                    o = ins.ops[0]
                    if isinstance(o, Rel):
                        t = o.target
                        if not self.in_text(t):
                            if strict:
                                return None
                            b.kind = "trap"
                        elif t in boundaries and t != entry:
                            b.kind = "tail"
                            link(b, [t])
                        else:
                            b.kind = "jmp"
                            link(b, [t])
                            work.append(t)
                    elif self.import_of(ins):
                        b.kind = "tailimport"
                    else:
                        targets = self.jump_table(preds, blocks, b, ins)
                        if targets is None:
                            b.kind = "ijmp"
                            f.notes.append(f"unresolved indirect jmp at {ins.addr:#x}: {ins}")
                        elif strict and not all(self.in_text(t) for t in targets):
                            return None
                        else:
                            b.kind = "table"
                            b.table = targets
                            link(b, sorted(set(targets)))
                            work.extend(t for t in b.succ if not (t in boundaries and t != entry))
                    break
                if mn in JCC:
                    t = ins.ops[0].target
                    if not self.in_text(t):
                        if strict:
                            return None
                        b.kind = "trap"
                        break
                    b.kind = "jcc"
                    link(b, [nxt, t])
                    for x in (nxt, t):
                        if x in boundaries and x != entry:
                            f.notes.append(f"branch into other function {x:#x} at {p:#x}")
                        else:
                            work.append(x)
                    break
                if mn == "call":
                    o = ins.ops[0]
                    if isinstance(o, Rel):
                        if self.in_text(o.target):
                            f.calls.add(o.target)
                        elif strict:
                            return None
                    elif not self.import_of(ins):
                        f.icalls += 1
                    if self.is_noreturn_call(ins):
                        b.kind = "noret"
                        break
                p = nxt
        return f

    # ------------------------------------------------------------ discovery
    def data_pointer_candidates(self):
        """Aligned 32-bit values in non-code sections that point into .text."""
        pe = self.pe
        out = set()
        for s in pe.sections:
            if s.executable:
                continue
            n = min(s.vsize, s.raw_size) if s.vsize else s.raw_size
            data = pe.image[s.va:s.va + n]
            for k in range(0, len(data) - 3, 4):
                v = struct.unpack_from("<I", data, k)[0]
                if self.in_text(v):
                    out.add(v)
        return out

    def discover(self):
        entries = {self.pe.entry}
        explored = {}
        imm_cands = set()

        def run(queue, strict):
            while queue:
                e = queue.pop()
                if e in explored:
                    continue
                f = self.explore(e, set(), strict=strict)
                explored[e] = f
                if f is None:
                    entries.discard(e)
                    continue
                for blk in f.blocks.values():
                    for ins in blk.insns:
                        if ins.mnem in ("push", "mov"):
                            for o in ins.ops:
                                if isinstance(o, Imm) and o.size == 32 and self.in_text(o.val):
                                    imm_cands.add(o.val)
                new = [t for t in f.calls if t not in entries]
                entries.update(new)
                queue.extend(new)

        run([self.pe.entry], strict=False)
        data_cands = self.data_pointer_candidates()
        while True:  # immediates found in newly accepted code can reveal more code
            todo = sorted(c for c in (data_cands | imm_cands) if c not in explored and c not in entries)
            if not todo:
                break
            for c in todo:
                f = self.explore(c, set(), strict=True)
                explored[c] = f
                if f is not None:
                    entries.add(c)
                    run([c], strict=True) if c not in explored else None
                    for blk in f.blocks.values():
                        for ins in blk.insns:
                            if ins.mnem in ("push", "mov"):
                                for o in ins.ops:
                                    if isinstance(o, Imm) and o.size == 32 and self.in_text(o.val):
                                        imm_cands.add(o.val)
                    new = [t for t in f.calls if t not in entries]
                    entries.update(new)
                    run(new, strict=True)
        self.entries = entries
        strong = {self.pe.entry}
        for f in explored.values():
            if f is not None:
                strong |= f.calls
        self.strong = strong & entries
        return entries

    def find_noreturn(self):
        """Functions whose every exit is a noreturn call or a trap."""
        changed = True
        while changed:
            changed = False
            for e, f in self.funcs.items():
                if e in self.noreturn or not f.blocks:
                    continue
                kinds = {b.kind for b in f.blocks.values()}
                if "noret" in kinds and kinds <= {"noret", "trap", "jmp", "jcc", "fall", "table"}:
                    self.noreturn.add(e)
                    changed = True

    def build(self):
        entries = self.discover()
        # Only real function starts split control flow. Code pointers found in data can point into
        # the middle of a function; jumping or falling into those continues inside the caller.
        for _ in range(3):
            self.funcs = {e: self.explore(e, self.strong) for e in sorted(entries)}
            before = len(self.noreturn)
            self.find_noreturn()
            if len(self.noreturn) == before:
                break
        return self.funcs


if __name__ == "__main__":
    import time
    from pe import PE
    t0 = time.time()
    prog = Program(PE(sys.argv[1]))
    funcs = prog.build()
    kinds = collections.Counter(b.kind for f in funcs.values() for b in f.blocks.values())
    ninsn = sum(len(b.insns) for f in funcs.values() for b in f.blocks.values())
    uniq = len({i.addr for f in funcs.values() for b in f.blocks.values() for i in b.insns})
    notes = collections.Counter(n.split(" at ")[0].split(":")[0] for f in funcs.values() for n in f.notes)
    print(f"{len(funcs)} functions, {sum(len(f.blocks) for f in funcs.values())} blocks, "
          f"{ninsn} insns ({uniq} unique), noreturn {len(prog.noreturn)}, {time.time() - t0:.1f}s")
    print("block kinds:", dict(kinds))
    print("notes:", dict(notes))
    for line in prog.log[:20]:
        print("log:", line)
    if len(sys.argv) > 2:
        for e in sorted(funcs):
            for n in funcs[e].notes:
                if sys.argv[2] in n:
                    print(f"{e:#x}: {n}")
