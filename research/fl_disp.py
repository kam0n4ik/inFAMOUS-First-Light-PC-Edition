"""Instructions in [lo,hi) using memory displacement `disp` (non-RIP), by linear decode of the range."""
import sys; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
from capstone import x86


def disp_uses(b, lo, hi, disps):
    out = []
    for ins in md.disasm(b.text[lo:hi], lo):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base != x86.X86_REG_RIP and op.mem.disp in disps:
                out.append(ins)
    return out


if __name__ == '__main__':
    ss, fl = load()
    b = ss if sys.argv[1] == 'ss' else fl
    lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
    ds = {int(x, 16) for x in sys.argv[4:]}
    for i in disp_uses(b, lo, hi, ds):
        print(f'{i.address:#x}: {i.mnemonic} {i.op_str}')
