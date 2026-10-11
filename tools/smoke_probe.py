"""Read-backs of the smoke draw's inputs (SSPC_DIAG_PROBE; docs/PHASE4_SMOKE_PIPELINE.md, SSPC-020).

Usage: py -I tools/smoke_probe.py <probe dir> [--series smoke_series.csv] [--list 1] [--words 64]
Files: f<flip>_<binding>_<address>.bin (binding 'a': the indirect arguments). Per flip: the vertex count (args), and for
the --list binding (u32 entries) how many of the first count/4 entries are distinct, out of range or repeated - a
correct sort is a permutation. For every other binding: the 32-bit words that change between flips (as u32 and
float), and with --series (smoke_stats.py's per-frame mass) the correlation of each changing word with the smoke's
mass - a value that tracks the flicker.
"""
import glob
import os
import re
import struct
import sys
from collections import defaultdict

import numpy as np


def load(d):
    by = defaultdict(dict)
    for f in glob.glob(os.path.join(d, 'f*_*_*.bin')):
        m = re.match(r'f(\d+)_(\w+)_([0-9a-f]+)\.bin', os.path.basename(f))
        if m:
            by[int(m.group(1))][m.group(2)] = (int(m.group(3), 16), open(f, 'rb').read())
    return dict(sorted(by.items()))


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    d = argv[1]
    series = None
    list_binding = '1'
    words = 64
    i = 2
    while i < len(argv):
        if argv[i] == '--series':
            series = argv[i + 1]
        elif argv[i] == '--list':
            list_binding = argv[i + 1]
        elif argv[i] == '--words':
            words = int(argv[i + 1])
        i += 2
    by = load(d)
    mass = {}
    if series:
        import csv
        for r in csv.DictReader(open(series)):
            mass[int(r['id'])] = float(r['mass'])
    flips = list(by)
    print(f'{d}: flips {flips[0]}..{flips[-1]} ({len(flips)}), bindings {sorted({k for v in by.values() for k in v})}')
    for fl in flips:
        b = by[fl]
        line = f'f{fl}'
        verts = None
        if 'a' in b:
            a = struct.unpack_from('<4I', b['a'][1])
            verts = a[0]
            line += f' args {a}'
        if list_binding in b and verts:
            addr, raw = b[list_binding]
            u = np.frombuffer(raw, dtype='<u4')
            n = min(verts // 4, len(u))
            head = u[:n]
            uniq = len(np.unique(head))
            line += (f' | list {addr:#x}: {n} entries, distinct {uniq}, repeated {n - uniq}, max {head.max() if n else 0}, '
                     f'first {head[:6].tolist()}')
        if fl in mass:
            line += f' | mass {mass[fl]:.4f}'
        print(line)
    # changing words of the small bindings
    for key in sorted({k for v in by.values() for k in v}):
        if key in ('a', list_binding):
            continue
        sizes = {len(by[fl][key][1]) for fl in flips if key in by[fl]}
        if max(sizes) > 64 * 1024:
            continue
        n = min(sizes) // 4
        rows = [fl for fl in flips if key in by[fl]]
        M = np.stack([np.frombuffer(by[fl][key][1][:n * 4], dtype='<u4') for fl in rows])
        changing = [j for j in range(n) if len(set(M[:, j].tolist())) > 1]
        addrs = sorted({by[fl][key][0] for fl in rows})
        print(f'\nbinding {key} ({len(addrs)} addresses, {n} words): {len(changing)} words change between flips')
        if not changing:
            continue
        F = M.view('<f4')
        ms = np.array([mass.get(fl, np.nan) for fl in rows])
        scored = []
        for j in changing:
            col = F[:, j].astype(np.float64)
            r = np.nan
            ok = np.isfinite(ms) & np.isfinite(col)
            if ok.sum() > 5 and np.std(col[ok]) > 0:
                r = np.corrcoef(col[ok], ms[ok])[0, 1]
            scored.append((abs(r) if np.isfinite(r) else -1, j, r))
        scored.sort(reverse=True)
        for _, j, r in scored[:words]:
            col = F[:, j]
            print(f'  word {j:4} (+{j * 4:#x}): corr with mass {r:+.3f} | float min {np.nanmin(col):.5g} max {np.nanmax(col):.5g} '
                  f'| first u32 {M[:6, j].tolist()}')


if __name__ == '__main__':
    main(sys.argv)
