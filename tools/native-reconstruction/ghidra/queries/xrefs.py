for s in args:
    a=int(s,16); seen={}
    for r in refm.getReferencesTo(A(a)):
        f=fm.getFunctionContaining(r.getFromAddress())
        k=hex(f.getEntryPoint().getOffset()) if f else "?"
        seen.setdefault(k,[]).append(str(r.getReferenceType()))
    print(s, ":", " ".join("%s(%s)"%(k,",".join(sorted(set(v)))) for k,v in sorted(seen.items())))
