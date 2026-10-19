"""Recompile a PE32 executable into C.

Usage: python3 -I recomp.py <exe> <outdir> [--funcs-per-file N]
Writes gen_NNN.c (functions), gen.h (prototypes), gen_tables.c (function and
import tables), gen_import_stubs.c (weak stubs for unimplemented imports),
and report.txt (analysis notes).
"""
import argparse
import collections
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import Program  # noqa: E402
from emit import FuncEmitter, flag_effects, liveness  # noqa: E402
from pe import PE  # noqa: E402


def cname_import(dll, name):
    d = dll.lower().replace(".dll", "")
    n = name.lstrip("#")
    return "I_" + re.sub(r"[^A-Za-z0-9_]", "_", f"{d}_{n}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--funcs-per-file", type=int, default=120)
    args = ap.parse_args()
    t0 = time.time()
    pe = PE(args.exe)
    prog = Program(pe)
    funcs = prog.build()
    entries = prog.strong
    fname = {e: f"F_{e:x}" for e in funcs}
    imports = {slot: cname_import(dll, name) for slot, (dll, name) in pe.imports.items()}
    os.makedirs(args.out, exist_ok=True)
    report = []
    # entry-flag sanity: functions reading flags they did not set
    with open(os.path.join(args.out, "gen.h"), "w") as h:
        h.write("#pragma once\n#include \"recomp.h\"\n")
        for e in sorted(funcs):
            h.write(f"void {fname[e]}(CPU *c);\n")
        for n in sorted(set(imports.values())):
            h.write(f"void {n}(CPU *c);\n")
    order = sorted(funcs)
    nfiles = 0
    stats = collections.Counter()
    for k in range(0, len(order), args.funcs_per_file):
        chunk = order[k:k + args.funcs_per_file]
        path = os.path.join(args.out, f"gen_{nfiles:03d}.c")
        with open(path, "w") as fo:
            fo.write("#include \"gen.h\"\n#pragma clang diagnostic ignored \"-Wunused-label\"\n\n")
            for e in chunk:
                f = funcs[e]
                em = FuncEmitter(prog, f, entries, imports, fname)
                code = em.emit()
                stats["traps"] += code.count("rt_trap(c, 0x") - code.count("\"int3\"")
                fo.write(code)
                fo.write("\n\n")
                for n in f.notes:
                    report.append(f"{e:#x}: {n}")
        nfiles += 1
    with open(os.path.join(args.out, "gen_tables.c"), "w") as fo:
        fo.write("#include \"gen.h\"\n#include \"tables.h\"\n\n")
        fo.write(f"const uint32_t g_fn_count = {len(order)};\n")
        fo.write("const uint32_t g_fn_addr[] = {\n")
        for e in order:
            fo.write(f"  0x{e:x}u,\n")
        fo.write("};\nvoid (*const g_fn_ptr[])(CPU *) = {\n")
        for e in order:
            fo.write(f"  {fname[e]},\n")
        fo.write("};\n")
        fo.write(f"const uint32_t g_import_count = {len(pe.imports)};\n")
        fo.write("const ImportSlot g_imports[] = {\n")
        for slot in sorted(pe.imports):
            dll, name = pe.imports[slot]
            fo.write(f"  {{0x{slot:x}u, {imports[slot]}, \"{dll}\", \"{name}\"}},\n")
        fo.write("};\n")
        fo.write(f"const uint32_t g_image_base = 0x{pe.base:x}u;\nconst uint32_t g_entry = 0x{pe.entry:x}u;\n")
        fo.write(f"const char g_exe_name[] = \"{os.path.basename(args.exe).lower()}\";\n")
        fo.write(f"const uint32_t g_exe_size = {len(pe.raw)}u;\n")
    with open(os.path.join(args.out, "gen_import_stubs.c"), "w") as fo:
        fo.write("#include \"gen.h\"\nvoid rt_unimplemented(CPU *c, const char *dll, const char *name);\n\n")
        for slot in sorted(pe.imports):
            dll, name = pe.imports[slot]
            n = imports[slot]
            fo.write(f"__attribute__((weak)) void {n}(CPU *c) {{ rt_unimplemented(c, \"{dll}\", \"{name}\"); }}\n")
    with open(os.path.join(args.out, "report.txt"), "w") as fo:
        fo.write("\n".join(prog.log + report) + "\n")
    print(f"{len(funcs)} functions -> {nfiles} files, {stats['traps']} trap sites, {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
