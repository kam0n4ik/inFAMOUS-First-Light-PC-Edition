# args: func... ; prologue (>=14 bytes) with bytes, rip-relative flag, and any reference into [entry+1, entry+cut)
for s in args:
    a=int(s,16); f=F(a); ins=listing.getInstructionAt(A(a)); cut=0; out=[]
    while ins and cut<14:
        b=bytes(x&0xff for x in ins.getBytes()); txt=str(ins)
        out.append("  %x: %-28s %s%s"%(ins.getAddress().getOffset(), b.hex(), txt, "  <RIP>" if "RIP" in txt else ""))
        cut+=len(b); ins=ins.getNext()
    into=[]
    for off in range(1,cut):
        for r in refm.getReferencesTo(A(a+off)): into.append("%s->+%d"%(r.getFromAddress(),off))
    print("%s size=%d cut=%d into=%s"%(s, f.getBody().getNumAddresses() if f else -1, cut, into))
    print("\n".join(out))
