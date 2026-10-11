"""Native vs native: the same render targets captured by two runs of the same route (P4.5: a change that must not change
the picture, e.g. the forced write-back of GPU-written images, SSPC_DIAG_GPU_EVICT_ALL=1).

Usage: py -I tools/imgcmp/pair_targets.py <dir A> <dir B> <flip>[,<flip>...] [threshold]

<dir>/image_<address>_<flip>_l0.raw + .txt (SSPC_DIAG_CAPTURE_IMAGE). Per address and flip: the mean of both, MAE and the
share of pixels differing by more than `threshold` (0..1 of the display range), the capture times (a file older than the
run would be a leftover). Two runs are not the same game frame (wind, particles): read the numbers against a pair of
plain runs, not against zero.
"""
import glob
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ab_targets import display, load  # noqa: E402


def main():
    a_dir, b_dir, flips = sys.argv[1], sys.argv[2], sys.argv[3].split(',')
    thr = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
    for flip in flips:
        for a in sorted(glob.glob(os.path.join(a_dir, f'image_*_{flip}_l0.txt'))):
            name = os.path.basename(a)
            b = os.path.join(b_dir, name)
            if not os.path.exists(b):
                continue
            A, kind, fmt = load(a[:-4])
            B, _, _ = load(b[:-4])
            if A.shape != B.shape:
                print(f'{name}: shapes differ')
                continue
            lo = hi = None
            if kind == 'depth':
                both = np.concatenate([A.ravel(), B.ravel()])
                lo, hi = np.percentile(both, 1), np.percentile(both, 99)
            da, db = display(A, kind, lo, hi), display(B, kind, lo, hi)
            d = np.abs(da - db)
            ta = time.strftime('%H:%M:%S', time.localtime(os.path.getmtime(a[:-4] + '.raw')))
            tb = time.strftime('%H:%M:%S', time.localtime(os.path.getmtime(b[:-4] + '.raw')))
            print(f'{name[6:-7]} fmt {fmt}: mean A {da.mean():.4f} B {db.mean():.4f} | MAE {d.mean():.4f} | '
                  f'> {thr}: {(d.max(axis=2) > thr).mean() * 100:.2f}% | captured {ta} / {tb}')


if __name__ == '__main__':
    main()
