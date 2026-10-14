"""Find code referencing strings and decompile the referencing functions.

usage: xref_strings.py <out_dir> <string> [<string> ...]
A string matches a defined string whose value equals it exactly (or starts with it if it ends in '*').
Writes <out_dir>/xrefs.txt and <out_dir>/<func_addr>.c for each referencing function.
Also writes <out_dir>/thunks.txt (all thunk functions and their names).
"""
import os
import sys

from gh import program, decompiler

out_dir = sys.argv[1]
wanted = sys.argv[2:]
os.makedirs(out_dir, exist_ok=True)


def match(v):
    for w in wanted:
        if w.endswith("*") and v.startswith(w[:-1]):
            return True
        if v == w:
            return True
    return False


with program() as prog:
    fm = prog.getFunctionManager()
    refm = prog.getReferenceManager()
    dec = decompiler(prog)
    with open(os.path.join(out_dir, "thunks.txt"), "w", encoding="utf-8") as fo:
        for f in fm.getFunctions(True):
            if f.isThunk():
                fo.write(f"{f.getEntryPoint()}\t{f.getName()}\n")
    funcs = {}
    with open(os.path.join(out_dir, "xrefs.txt"), "w", encoding="utf-8") as fo:
        for d in prog.getListing().getDefinedData(True):
            if not d.hasStringValue():
                continue
            v = str(d.getValue())
            if not match(v):
                continue
            for r in refm.getReferencesTo(d.getAddress()):
                src = r.getFromAddress()
                f = fm.getFunctionContaining(src)
                fname = f"{f.getName()}@{f.getEntryPoint()}" if f else "<no function>"
                fo.write(f"{d.getAddress()}\t{v!r}\t{src}\t{fname}\n")
                if f:
                    funcs[str(f.getEntryPoint())] = f
                else:
                    # A pointer table entry: follow references to the table slot.
                    for r2 in refm.getReferencesTo(src):
                        f2 = fm.getFunctionContaining(r2.getFromAddress())
                        if f2:
                            fo.write(f"\t  via ptr {src} <- {r2.getFromAddress()} {f2.getName()}@{f2.getEntryPoint()}\n")
                            funcs[str(f2.getEntryPoint())] = f2
    print(f"referencing functions: {len(funcs)}")
    for ep, f in funcs.items():
        c = dec(f)
        with open(os.path.join(out_dir, f"{ep}.c"), "w", encoding="utf-8") as fo:
            fo.write(c or "// decompile failed\n")
        print(ep, f.getName(), f.getBody().getNumAddresses(), "bytes")
