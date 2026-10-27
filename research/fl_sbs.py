"""Side-by-side listing of an SS function and its FL counterpart (same instruction count assumed; difflib-aligned)."""
import sys, difflib; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
from capstone import x86
ss, fl = load()


def rip(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return hex(i.address + i.size + op.mem.disp)
    return ''


def sbs(a, b, na, nb=None, only=None):
    A = ss.dis(a, na); B = fl.dis(b, nb or na)
    sm = difflib.SequenceMatcher(None, [i.mnemonic for i in A], [i.mnemonic for i in B], autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        for k in range(max(i2 - i1, j2 - j1)):
            x = A[i1 + k] if i1 + k < i2 else None
            y = B[j1 + k] if j1 + k < j2 else None
            sa = f'{x.address:#x} {x.mnemonic} {x.op_str}'[:60] + ' ' + rip(x) if x else ''
            sb = f'{y.address:#x} {y.mnemonic} {y.op_str}'[:60] + ' ' + rip(y) if y else ''
            diff = '' if (x and y and x.op_str == y.op_str) else ' <<'
            line = f'{sa:75s}| {sb}{diff}'
            if only is None or only(x, y, diff):
                print(line)


if __name__ == '__main__':
    a, b, n = (int(v, 16) for v in sys.argv[1:4])
    nb = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
    sbs(a, b, n, nb)
