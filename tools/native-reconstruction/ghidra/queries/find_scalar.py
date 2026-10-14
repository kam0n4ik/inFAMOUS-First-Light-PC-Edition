# args: hex scalars ; list functions whose instructions use each scalar
from ghidra.program.model.scalar import Scalar
want = {int(a,16) for a in args}
hits = {w: {} for w in want}
for ins in listing.getInstructions(True):
    for i in range(ins.getNumOperands()):
        for o in ins.getOpObjects(i):
            if isinstance(o, Scalar):
                v = o.getUnsignedValue() & 0xffffffffffffffff
                if v in want:
                    f = fm.getFunctionContaining(ins.getAddress())
                    k = f.getEntryPoint().getOffset() if f else -1
                    hits[v].setdefault(k, []).append(str(ins.getAddress()))
for w in sorted(hits):
    print(hex(w), ":", " ".join("%s[%d]" % (hex(k), len(v)) for k, v in sorted(hits[w].items())))
