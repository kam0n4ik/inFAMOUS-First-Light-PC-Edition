"""Flicker statistics of a captured frame series (SSPC_DIAG_CAPTURE_FRAMES, P4.5).

Usage: py -I tools/imgcmp/flicker_stats.py <frames dir> [<frames dir> ...]
For each series: the static-camera segments (global mean change < 0.3 for >= 60 frames), and in them per 10x10 block
of the (downsampled) frame the number of frame-to-frame jumps > 8 levels and the "alternation" ratio
|f[n]-f[n+1]| / |f[n]-f[n+2]| (> 1: the block goes back and forth - flicker; < 1: motion).
"""
import glob
import sys

import numpy as np
from PIL import Image


def load(d):
    fs = sorted(glob.glob(d + '/flip_*.png'))
    return [int(f[-10:-4]) for f in fs], np.stack([np.asarray(Image.open(f).convert('RGB')).astype(np.float32).mean(axis=2) for f in fs])


def static_segments(m, min_len=60, eps=0.3):
    segs, start = [], 0
    for t in range(1, len(m) + 1):
        if t == len(m) or abs(m[t] - m[t - 1]) > eps:
            if t - start >= min_len:
                segs.append((start, t))
            start = t
    return segs


for d in sys.argv[1:]:
    ids, F = load(d)
    n, h, w = F.shape
    m = F.mean(axis=(1, 2))
    segs = static_segments(m)
    B = F[:, :h // 10 * 10, :w // 10 * 10].reshape(n, h // 10, 10, w // 10, 10).mean(axis=(2, 4))
    jumps = np.zeros(B.shape[1:])
    frames = 0
    d1s, d2s = [], []
    for a, b in segs:
        S = B[a:b]
        jumps += (np.abs(np.diff(S, axis=0)) > 8).sum(axis=0)
        frames += b - a
        d1s.append(np.abs(np.diff(S, axis=0)).mean(axis=0) * (b - a))
        d2s.append(np.abs(S[2:] - S[:-2]).mean(axis=0) * (b - a))
    d1 = sum(d1s) / max(frames, 1)
    d2 = sum(d2s) / max(frames, 1)
    alt = d1 / (d2 + 0.25)
    top = np.argsort(jumps.flatten())[::-1][:5]
    print(f'{d}: {n} frames, static {frames} in {len(segs)} segments {[(ids[a], ids[b - 1]) for a, b in segs]}')
    print(f'  block jumps > 8 per 100 static frames: total {jumps.sum() / max(frames, 1) * 100:.1f}; top blocks ' +
          ', '.join(f'({k // jumps.shape[1]},{k % jumps.shape[1]}) {jumps.flatten()[k] / max(frames, 1) * 100:.1f}' for k in top))
    print(f'  alternation ratio: blocks > 1.5: {(alt > 1.5).sum()}, max {alt.max():.2f}, mean {alt.mean():.2f}')
