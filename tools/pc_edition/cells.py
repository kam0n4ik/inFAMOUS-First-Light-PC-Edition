"""Print u32 cells of the game's image across snapshots: cells.py <dir> <s,s,...> <rva> [<rva>...]
or search: cells.py <dir> <s,s,...> --seq v0,v1,... (cells whose values across the snapshots are exactly that)."""
import sys
import numpy as np
sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])


def load(d, s):
    regions = [tuple(int(x, 16) for x in l.split()) for l in open(f"{d}/snap_{s}.map")]
    data = np.fromfile(f"{d}/snap_{s}.bin", dtype=np.uint8)
    out, pos = {}, 0
    for a, n in regions:
        out[a] = data[pos:pos + n]
        pos += n
    return out


d, ss = sys.argv[1], sys.argv[2].split(",")
snaps = [load(d, s) for s in ss]
if sys.argv[3] == "--seq":
    want = [int(x, 0) for x in sys.argv[4].split(",")]
    for r in sorted(snaps[0]):
        n = min(len(m[r]) for m in snaps) // 4 * 4
        ok = np.ones(n // 4, bool)
        for m, w in zip(snaps, want):
            if w >= 0:
                ok &= m[r][:n].view(np.uint32) == w
        for i in np.nonzero(ok)[0][:200]:
            print(hex(r + 4 * int(i)))
else:
    for a in sys.argv[3:]:
        a = int(a, 16)
        vals = []
        for m in snaps:
            for r, b in m.items():
                if r <= a < r + len(b):
                    vals.append(hex(int(b[a - r:a - r + 4].view(np.uint32)[0])))
        print(hex(a), vals)
