"""Find the game's state flags: u32 cells equal across every snapshot of state A, equal across every snapshot of
state B, and different between A and B (SSPC_DIAG_MEMSNAP snapshots, tools/pc_edition/menu_probe.ps1).

usage: memdiff.py <dir> <A seconds,...> <B seconds,...> [--max N]
"""
import sys
import numpy as np


def load(d, s):
    regions = []
    with open(f"{d}/snap_{s}.map") as f:
        for line in f:
            a, n = (int(x, 16) for x in line.split())
            regions.append((a, n))
    data = np.fromfile(f"{d}/snap_{s}.bin", dtype=np.uint8)
    out, pos = {}, 0
    for a, n in regions:
        out[a] = data[pos:pos + n]
        pos += n
    return out


def main():
    d, sa, sb = sys.argv[1], sys.argv[2].split(","), sys.argv[3].split(",")
    mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 200
    A = [load(d, s) for s in sa]
    B = [load(d, s) for s in sb]
    common = set(A[0])
    for m in A[1:] + B:
        common &= set(m)
    hits = []
    for r in sorted(common):
        n = min(len(m[r]) for m in A + B) // 4 * 4
        va = [m[r][:n].view(np.uint32) for m in A]
        vb = [m[r][:n].view(np.uint32) for m in B]
        ok = np.ones(n // 4, bool)
        for v in va[1:]:
            ok &= v == va[0]
        for v in vb[1:]:
            ok &= v == vb[0]
        ok &= va[0] != vb[0]
        for i in np.nonzero(ok)[0]:
            hits.append((r + 4 * int(i), int(va[0][i]), int(vb[0][i])))
    print(f"{len(hits)} cells differ between states A {sa} and B {sb}")
    small = [h for h in hits if h[1] < 256 and h[2] < 256]
    print(f"{len(small)} of them small (both < 256):")
    for a, x, y in small[:mx]:
        print(f"  rva {a:#x}: A {x:#x}  B {y:#x}")
    print("all (first):")
    for a, x, y in hits[:mx]:
        print(f"  rva {a:#x}: A {x:#x}  B {y:#x}")


main()
