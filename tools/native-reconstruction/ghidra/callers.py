"""Decompile the callers of named functions (e.g. PS4 imports after label_imports.py).

usage: callers.py <out_dir> <depth> <name> [<name> ...]
depth 1 = direct callers; 2 = also their callers. Writes <out_dir>/calls.txt
(callee, call site, caller) and <out_dir>/<addr>_<name>.c per caller.
"""
import os
import sys

from gh import program, decompiler

out_dir, depth, names = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
os.makedirs(out_dir, exist_ok=True)

with program() as prog:
    fm = prog.getFunctionManager()
    refm = prog.getReferenceManager()
    st = prog.getSymbolTable()
    dec = decompiler(prog)
    targets = []
    for n in names:
        for s in st.getSymbols(n):
            f = fm.getFunctionAt(s.getAddress())
            if f:
                targets.append(f)
    seen = {}
    lines = []
    frontier = targets
    for level in range(depth):
        nxt = []
        for t in frontier:
            for r in refm.getReferencesTo(t.getEntryPoint()):
                if not r.getReferenceType().isCall() and not r.getReferenceType().isJump():
                    continue
                c = fm.getFunctionContaining(r.getFromAddress())
                cname = f"{c.getName()}@{c.getEntryPoint()}" if c else "<none>"
                lines.append(f"L{level + 1}\t{t.getName()}\t{r.getFromAddress()}\t{cname}")
                if c and str(c.getEntryPoint()) not in seen:
                    seen[str(c.getEntryPoint())] = c
                    nxt.append(c)
        frontier = nxt
    with open(os.path.join(out_dir, "calls.txt"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines) + "\n")
    for ep, f in seen.items():
        code = dec(f)
        with open(os.path.join(out_dir, f"{ep}_{f.getName()}.c"), "w", encoding="utf-8") as fo:
            fo.write(code or "// decompile failed\n")
    print(f"targets={len(targets)} callers={len(seen)} call sites={len(lines)}")
