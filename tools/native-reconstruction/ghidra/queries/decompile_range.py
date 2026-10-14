# args: outdir lo hi ; dump every function in [lo,hi)
import os
out=args[0]; os.makedirs(out, exist_ok=True)
lo=int(args[1],16); hi=int(args[2],16)
it = fm.getFunctions(A(lo), True)
for f in it:
    ep=f.getEntryPoint().getOffset()
    if ep>=hi: break
    callers = sorted({c.getEntryPoint().getOffset() for c in f.getCallingFunctions(None)})
    callees = sorted({(c.getEntryPoint().getOffset(), c.getName()) for c in f.getCalledFunctions(None)})
    hdr = "// %s @0x%x size=%d ncallers=%d\n// callers: %s\n// callees: %s\n" % (f.getName(), ep, f.getBody().getNumAddresses(), len(callers),
        " ".join(hex(c) for c in callers[:40]), " ".join("%s(%s)" % (hex(a), n) for a, n in callees))
    open(os.path.join(out, "%08x.c" % ep), "w", encoding="utf-8").write(hdr + (dec(f) or "// decompile failed"))
