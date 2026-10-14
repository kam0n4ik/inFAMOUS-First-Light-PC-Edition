# args: outdir addr... ; dumps decompiled C + callers + callees per function
import os
out = args[0]; os.makedirs(out, exist_ok=True)
idx = []
for s in args[1:]:
    f = F(int(s, 16))
    if f is None:
        print("no func", s); continue
    ep = f.getEntryPoint().getOffset()
    callers = sorted({(c.getEntryPoint().getOffset()) for c in f.getCallingFunctions(None)})
    callees = sorted({(c.getEntryPoint().getOffset(), c.getName()) for c in f.getCalledFunctions(None)})
    hdr = "// %s @0x%x size=%d\n// callers: %s\n// callees: %s\n" % (f.getName(), ep, f.getBody().getNumAddresses(),
        " ".join(hex(c) for c in callers), " ".join("%s(%s)" % (hex(a), n) for a, n in callees))
    open(os.path.join(out, "%08x.c" % ep), "w", encoding="utf-8").write(hdr + (dec(f) or "// decompile failed"))
    idx.append(hdr.splitlines()[0] + " callers=%d" % len(callers))
print("\n".join(idx))
