# Static recompilation PoC, step 1 (decoder + IR): the raw P-code of whole functions, as Ghidra's SLEIGH x86-64
# specification lifts each instruction (docs/NATIVE_ENGINE_RECONSTRUCTION.md §9). Read-only Ghidra snippet for
# ../ghidra/run.py:
#   run.py ../recomp/pcode_dump.py <out.json> <addr>...
# Output: {"functions": [{entry, size, bytes, instructions: [{addr, len, text, mnemonic, pcode: [
#   {op, out: [space, offset, size] | null, in: [[space, offset, size], ...]}]}], callother: {index: name}}],
#  "registers": {"<space offset>:<size>": name}}
import json

from ghidra.program.model.pcode import PcodeOp

out_path = args[0]
lang = prog.getLanguage()
result = {"functions": [], "registers": {}}


def vn(v):
    if v is None:
        return None
    space = v.getAddress().getAddressSpace().getName()
    off = v.getOffset()
    if space == "register":
        reg = lang.getRegister(v.getAddress(), v.getSize())
        if reg is not None:
            result["registers"]["%d:%d" % (off, v.getSize())] = reg.getName()
    return [space, off, v.getSize()]


for s in args[1:]:
    f = F(int(s, 16))
    entry = f.getEntryPoint().getOffset()
    body = f.getBody()
    size = body.getMaxAddress().getOffset() - entry + 1
    insns = []
    callother = {}
    it = listing.getInstructions(body, True)
    for ins in it:
        ops = []
        for op in ins.getPcode():
            code = op.getOpcode()
            rec = {"op": PcodeOp.getMnemonic(code), "out": vn(op.getOutput()),
                   "in": [vn(op.getInput(i)) for i in range(op.getNumInputs())]}
            if code == PcodeOp.CALLOTHER:
                idx = op.getInput(0).getOffset()
                callother[str(idx)] = lang.getUserDefinedOpName(int(idx))
            ops.append(rec)
        insns.append({"addr": ins.getAddress().getOffset(), "len": ins.getLength(), "text": str(ins),
                      "mnemonic": ins.getMnemonicString(), "pcode": ops})
    bs = bytes(mem.getByte(A(entry + i)) & 0xff for i in range(size))
    result["functions"].append({"entry": entry, "size": size, "bytes": bs.hex(), "instructions": insns,
                                "callother": callother})
    print("%x: %d instructions, %d p-code ops, CALLOTHER %s" % (
        entry, len(insns), sum(len(i["pcode"]) for i in insns), sorted(set(callother.values()))))

with open(out_path, "w", encoding="utf-8") as fo:
    json.dump(result, fo)
