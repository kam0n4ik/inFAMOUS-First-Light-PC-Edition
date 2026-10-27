"""xrefs to a VA: RIP-relative disp32 in text, and raw u64 in loaded segments."""
import sys, struct; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
from capstone import x86


def rip_xrefs(b, target, lo=0, hi=None):
    t = b.text
    hi = hi or len(t)
    out = []
    # disp32 at position p means instruction ends at p+4+k (imm after disp) ; brute force: for each p, disp = target - (p+4+k)
    for k in (0, 1, 2, 4):
        for p in range(lo, hi - 4):
            pass
    return out


def rip_xrefs_fast(b, target):
    import numpy as np
    t = np.frombuffer(b.text, dtype=np.uint8)
    n = len(t) - 4
    d = (t[:n].astype(np.int64) | (t[1:n+1].astype(np.int64) << 8) | (t[2:n+2].astype(np.int64) << 16) | (t[3:n+3].astype(np.int64) << 24))
    d = np.where(d >= 2**31, d - 2**32, d)
    pos = np.arange(n, dtype=np.int64)
    res = []
    for k in (0, 1, 4):
        hits = np.nonzero(pos + 4 + k + d == target)[0]
        for h in hits:
            for back in range(10, 0, -1):
                st = int(h) - back
                ins = next(md.disasm(b.text[st:st + 16], st), None)
                if ins and ins.disp_offset == back and any(o.type == x86.X86_OP_MEM and o.mem.base == x86.X86_REG_RIP for o in ins.operands) and ins.address + ins.size + ins.disp == target:
                    res.append(ins)
                    break
    return sorted({i.address: i for i in res}.values(), key=lambda i: i.address)


def data_xrefs(b, value):
    out = []
    pk = struct.pack('<Q', value)
    for v, m, d, ph in b.loads:
        i = d.find(pk)
        while i != -1:
            out.append(v + i)
            i = d.find(pk, i + 1)
    return out


if __name__ == '__main__':
    ss, fl = load()
    b = ss if sys.argv[1] == 'ss' else fl
    for a in sys.argv[2:]:
        a = int(a, 16)
        print(f'== {a:#x}')
        for ins in rip_xrefs_fast(b, a):
            print(f'  code {ins.address:#x}: {ins.mnemonic} {ins.op_str}')
        for x in data_xrefs(b, a):
            print(f'  data {x:#x}')
