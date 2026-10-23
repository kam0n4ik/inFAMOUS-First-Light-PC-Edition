# Every function of the program as TSV (R9 gameplay profiler: offline attribution of sampled eboot addresses without a
# Ghidra session per report): entry, end (exclusive, the body's maximum address + 1), body bytes, name, callers, callees.
# args: <out.tsv>
out = args[0]
n = 0
with open(out, "w", encoding="utf-8") as fo:
    fo.write("entry\tend\tbytes\tname\tcallers\tcallees\n")
    it = fm.getFunctions(True)
    while it.hasNext():
        f = it.next()
        body = f.getBody()
        lo = f.getEntryPoint().getOffset()
        hi = body.getMaxAddress().getOffset() + 1
        size = body.getNumAddresses()
        try:
            callers = len(f.getCallingFunctions(None))
            callees = len(f.getCalledFunctions(None))
        except Exception:
            callers = callees = -1
        fo.write(f"{lo:x}\t{hi:x}\t{size}\t{f.getName()}\t{callers}\t{callees}\n")
        n += 1
print(f"{n} functions -> {out}")
