# args: outdir addr... ; per function (Native Engine Reconstruction, the choice of candidates): size and body range, FNV-1a 64
# prologue (14-byte cut), full disassembly (<= 0x600 bytes), decompiled C. Read-only.
import os
out = args[0]; os.makedirs(out, exist_ok=True)

def fnv(bs):
    h = 0xCBF29CE484222325
    for b in bs:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

summary = []
for s in args[1:]:
    a = int(s, 16)
    f = F(a)
    if f is None:
        summary.append("%s: no function" % s); continue
    ep = f.getEntryPoint().getOffset()
    body = f.getBody()
    hi = body.getMaxAddress().getOffset()
    size = hi - ep + 1
    bs = bytearray(size)
    for i in range(size):
        bs[i] = mem.getByte(A(ep + i)) & 0xff
    callers = []
    for r in refm.getReferencesTo(A(ep)):
        c = fm.getFunctionContaining(r.getFromAddress())
        callers.append("%s %s %s" % (r.getFromAddress(), r.getReferenceType(), ("%s@%x" % (c.getName(), c.getEntryPoint().getOffset())) if c else "?"))
    callees = sorted({"%x" % c.getEntryPoint().getOffset() for c in f.getCalledFunctions(None)})
    lines = ["// %x size=%d (body addrs %d, ranges %d) fnv1a64=%#018x" % (ep, size, body.getNumAddresses(), body.getNumAddressRanges(), fnv(bs)),
             "// callers (%d):" % len(callers)] + ["//   " + c for c in callers[:60]] + ["// callees: " + " ".join(callees)]
    # prologue
    ins = listing.getInstructionAt(A(ep)); cut = 0
    while ins is not None and cut < 14:
        b = bytes(x & 0xff for x in ins.getBytes()); cut += len(b)
        lines.append("// prologue %x: %-24s %s" % (ins.getAddress().getOffset(), b.hex(), ins))
        ins = ins.getNext()
    into = []
    for off in range(1, cut):
        for r in refm.getReferencesTo(A(ep + off)):
            into.append("%s->+%d" % (r.getFromAddress(), off))
    lines.append("// prologue cut=%d refs-into=%s" % (cut, into))
    if size <= 0x600:
        it = listing.getInstructions(body, True)
        for i in it:
            lines.append("//   %x: %-30s %s" % (i.getAddress().getOffset(), bytes(x & 0xff for x in i.getBytes()).hex(), i))
    lines.append(dec(f) or "// decompile failed")
    open(os.path.join(out, "%08x.c" % ep), "w", encoding="utf-8").write("\n".join(lines))
    summary.append("%x size=%d callers=%d callees=%d fnv=%#018x cut=%d" % (ep, size, len(callers), len(callees), fnv(bs), cut))
open(os.path.join(out, "summary.txt"), "w").write("\n".join(summary) + "\n")
print("\n".join(summary))
