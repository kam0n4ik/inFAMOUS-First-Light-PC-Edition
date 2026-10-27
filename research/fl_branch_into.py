"""Global check: any rel8/rel32 jmp/jcc/call in FL text whose target lies strictly inside a patched range.
Byte-level scan (over-approximates: also counts misaligned decodes); each hit is then decoded to confirm."""
import sys; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
import numpy as np


def scan(b, ranges):
    t = np.frombuffer(b.text, dtype=np.uint8).astype(np.int64)
    n = len(t)
    pos = np.arange(n, dtype=np.int64)
    res = {r: [] for r in ranges}
    # rel32: E8/E9 at p, disp at p+1, end p+5 ; 0F 8x at p, disp p+2, end p+6
    d32 = np.zeros(n, dtype=np.int64)
    d32[:n - 4] = t[:n - 4] | (t[1:n - 3] << 8) | (t[2:n - 2] << 16) | (t[3:n - 1] << 24)
    d32 = np.where(d32 >= 2**31, d32 - 2**32, d32)
    cands = []
    m1 = (t == 0xE8) | (t == 0xE9)
    tgt1 = np.full(n, -1, dtype=np.int64); tgt1[:n - 5] = pos[:n - 5] + 5 + d32[1:n - 4]
    m2 = np.zeros(n, dtype=bool); m2[:n - 1] = (t[:n - 1] == 0x0F) & (t[1:] >= 0x80) & (t[1:] <= 0x8F)
    tgt2 = np.full(n, -1, dtype=np.int64); tgt2[:n - 6] = pos[:n - 6] + 6 + d32[2:n - 4]
    d8 = np.where(t >= 128, t - 256, t)
    m3 = (t == 0xEB) | ((t >= 0x70) & (t <= 0x7F))
    tgt3 = np.full(n, -1, dtype=np.int64); tgt3[:n - 2] = pos[:n - 2] + 2 + d8[1:n - 1]
    for (lo, hi) in ranges:
        for m, tg in ((m1, tgt1), (m2, tgt2), (m3, tgt3)):
            hits = np.nonzero(m & (tg > lo) & (tg < hi))[0]
            for h in hits:
                ins = next(md.disasm(b.text[h:h + 8], int(h)), None)
                if ins is None:
                    continue
                if not (ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL)):
                    continue
                res[(lo, hi)].append(f'{int(h):#x}: {ins.mnemonic} {ins.op_str}')
    return res


if __name__ == '__main__':
    ss, fl = load()
    rs = []
    for a in sys.argv[1:]:
        lo, ln = a.split('+')
        rs.append((int(lo, 16), int(lo, 16) + int(ln, 16)))
    for r, hits in scan(fl, rs).items():
        print(f'{r[0]:#x}..{r[1]:#x}: {len(hits)} raw hits', hits[:12])
