"""Run an ad-hoc query snippet against the read-only Ghidra program.

usage: run.py <snippet.py> [args...]
The snippet runs with these names in scope: prog, fm (FunctionManager), refm (ReferenceManager),
st (SymbolTable), listing, mem, dec (decompile(func) -> C text or None), A (int -> Address),
F (int|name -> Function or None), args (list of extra arguments).
"""
import sys

from gh import program, decompiler, addr

with program() as prog:
    fm = prog.getFunctionManager()
    refm = prog.getReferenceManager()
    st = prog.getSymbolTable()
    listing = prog.getListing()
    mem = prog.getMemory()
    dec = decompiler(prog)

    def A(v):
        return addr(prog, v)

    def F(v):
        if isinstance(v, str):
            for s in st.getSymbols(v):
                f = fm.getFunctionAt(s.getAddress())
                if f:
                    return f
            v = int(v, 16)
        return fm.getFunctionContaining(A(v))

    scope = dict(prog=prog, fm=fm, refm=refm, st=st, listing=listing, mem=mem, dec=dec, A=A, F=F,
                 args=sys.argv[2:])
    with open(sys.argv[1], encoding="utf-8") as fi:
        exec(compile(fi.read(), sys.argv[1], "exec"), scope)
