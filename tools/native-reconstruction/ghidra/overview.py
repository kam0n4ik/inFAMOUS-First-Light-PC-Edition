"""First-pass overview of /eboot: blocks, functions, imports/externals, thread/job strings.

usage: overview.py <externals_out.txt> <strings_out.txt>
"""
import collections
import re
import sys

from gh import program

PAT = re.compile(r"job|worker|fiber|task|thread|spu|sched|queue|mutex|semaph|spin|wait|signal", re.I)

with program() as prog:
    print("program:", prog.getName(), prog.getLanguageID(), prog.getCompilerSpec().getCompilerSpecID())
    print("image base:", prog.getImageBase())
    for b in prog.getMemory().getBlocks():
        print(f"  block {b.getName():24} {b.getStart()}-{b.getEnd()} size=0x{b.getSize():x} "
              f"r={int(b.isRead())} w={int(b.isWrite())} x={int(b.isExecute())} init={int(b.isInitialized())}")
    fm = prog.getFunctionManager()
    n = named = thunks = 0
    for f in fm.getFunctions(True):
        n += 1
        thunks += int(f.isThunk())
        named += int(not f.getName().startswith("FUN_"))
    ext = sum(1 for _ in fm.getExternalFunctions())
    print(f"functions={n} non-default-named={named} thunks={thunks} external={ext}")

    libs = collections.Counter()
    ext_names = []
    for s in prog.getSymbolTable().getExternalSymbols():
        parent = s.getParentNamespace().getName()
        libs[parent] += 1
        ext_names.append((parent, s.getName()))
    print("external namespaces:", libs.most_common(40))
    with open(sys.argv[1], "w", encoding="utf-8") as fo:
        for lib, name in sorted(ext_names):
            fo.write(f"{lib}\t{name}\n")

    hits = []
    for d in prog.getListing().getDefinedData(True):
        if not d.hasStringValue():
            continue
        v = str(d.getValue())
        if PAT.search(v) and len(v) < 200:
            hits.append((str(d.getAddress()), v))
    print(f"thread/job-related strings: {len(hits)}")
    with open(sys.argv[2], "w", encoding="utf-8") as fo:
        for a, v in hits:
            fo.write(f"{a}\t{v!r}\n")
