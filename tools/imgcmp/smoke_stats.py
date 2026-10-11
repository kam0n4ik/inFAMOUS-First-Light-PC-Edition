"""Smoke column statistics of a captured frame series (SSPC_DIAG_CAPTURE_FRAMES; Phase 4 smoke research, SSPC-020).

Usage: py -I tools/imgcmp/smoke_stats.py <frames dir> [--csv gpu_frames.csv] [--roi x0,y0,x1,y1] [--ref <frames dir>]
                                          [--json out.json] [--series out.csv]
The ROI (fractions of the frame; default: the smoke column over the long house on the beach route) is dark smoke over a
bright sky. Per pixel the "sky" is the 95th percentile of its luminance over the static frames; the smoke opacity of a
pixel in a frame is (sky - L) / sky, clamped to [0, 1]. Per frame:
  mass      - the mean opacity over the ROI (how much smoke there is),
  coverage  - the fraction of ROI pixels with opacity > 0.10.
Flicker (static-camera frames only; the camera moves for the first frames of a series):
  dips      - a frame whose mass is < 85 % of both neighbours' (a part of the column gone for one frame and back),
  blinks    - per frame, the fraction of ROI pixels that are smoke in frames n-1 and n+1 but not in n (or the reverse):
              puffs that vanish for a single frame; smooth motion of the column gives few blinks,
  jumps     - frame-to-frame |d mass| / mean mass > 0.10.
Every count is given per 100 frames and per second of game time: the game's flip times from gpu_frames.csv (row n is
the n-th flip, the capture's flip id) - the simulation advances with the game's frame time, so a faster run sees less
change per frame; per second the two compare. Without --csv only the per-frame figures are given.
--ref: the same figures of a reference series and the ratio of mean masses (a diagnostic run with draws skipped
against a normal one: mass ratio ~0 - the skipped draws are the smoke).
"""
import csv
import glob
import json
import sys

import numpy as np
from PIL import Image


def load(d, roi):
    fs = sorted(glob.glob(d + '/flip_*.png'))
    if not fs:
        raise SystemExit(f'no flip_*.png in {d}')
    ids, full, crops = [], [], []
    for f in fs:
        im = np.asarray(Image.open(f).convert('L')).astype(np.float32)
        h, w = im.shape
        x0, y0, x1, y1 = int(roi[0] * w), int(roi[1] * h), int(roi[2] * w), int(roi[3] * h)
        ids.append(int(f[-10:-4]))
        full.append(im.mean())
        crops.append(im[y0:y1, x0:x1])
    return np.array(ids), np.array(full), np.stack(crops)


def static_mask(full, ids, eps=0.3, min_len=60):
    # frames of segments where the whole frame's mean changes by < eps between consecutive frames
    keep = np.zeros(len(full), bool)
    start = 0
    for t in range(1, len(full) + 1):
        if t == len(full) or abs(full[t] - full[t - 1]) > eps or ids[t] != ids[t - 1] + 1:
            if t - start >= min_len:
                keep[start:t] = True
            start = t
    return keep


def flip_times(path):
    t = {}
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            if r['game_flip_ms']:
                t[int(r['n'])] = float(r['game_flip_ms'])
    return t


def analyse(d, roi, csv_path=None):
    ids, full, C = load(d, roi)
    keep = static_mask(full, ids)
    S = C[keep]
    sid = ids[keep]
    res = {'dir': d, 'frames': int(len(ids)), 'static_frames': int(keep.sum()), 'roi': roi}
    if len(S) < 3:
        res['error'] = 'fewer than 3 static frames'
        return res, None
    sky = np.percentile(S, 95, axis=0)
    op = np.clip((sky[None] - S) / np.maximum(sky[None], 1.0), 0.0, 1.0)
    mass = op.mean(axis=(1, 2))
    smoke = op > 0.10
    cov = smoke.mean(axis=(1, 2))
    # consecutive triples only (a gap in the ids breaks them)
    trip = (sid[2:] == sid[1:-1] + 1) & (sid[1:-1] == sid[:-2] + 1)
    mid = mass[1:-1]
    dips = (mid < 0.85 * mass[:-2]) & (mid < 0.85 * mass[2:]) & trip
    spikes = (mid > mass[:-2] / 0.85) & (mid > mass[2:] / 0.85) & trip
    blink = ((smoke[:-2] & smoke[2:] & ~smoke[1:-1]) | (~smoke[:-2] & ~smoke[2:] & smoke[1:-1])).mean(axis=(1, 2))
    blink = np.where(trip, blink, np.nan)
    pair = sid[1:] == sid[:-1] + 1
    dm = np.abs(np.diff(mass)) / max(mass.mean(), 1e-6)
    jumps = (dm > 0.10) & pair
    n3, n2 = int(trip.sum()), int(pair.sum())
    res.update({
        'mass_mean': float(mass.mean()), 'mass_cv': float(mass.std() / max(mass.mean(), 1e-6)),
        'coverage_mean': float(cov.mean()),
        'dips_per_100': 100.0 * dips.sum() / max(n3, 1), 'spikes_per_100': 100.0 * spikes.sum() / max(n3, 1),
        'blink_mean_pct': float(np.nanmean(blink) * 100.0), 'blink_p95_pct': float(np.nanpercentile(blink, 95) * 100.0),
        'jumps_per_100': 100.0 * jumps.sum() / max(n2, 1),
        'dmass_mean_pct': float(dm[pair].mean() * 100.0),
    })
    series = {'id': sid, 'mass': mass, 'coverage': cov}
    if csv_path:
        t = flip_times(csv_path)
        ft = np.array([t.get(int(i), np.nan) for i in sid])
        dt = np.diff(ft)
        ok = pair & np.isfinite(dt)
        secs = float(np.nansum(np.where(ok, dt, 0.0)) / 1000.0)
        res.update({
            'game_seconds': secs, 'game_frame_ms_mean': float(np.nanmean(dt[ok])) if ok.any() else None,
            'dips_per_s': float(dips.sum() / secs) if secs > 0 else None,
            'jumps_per_s': float(jumps.sum() / secs) if secs > 0 else None,
            # change of the column per ms of game time (motion + flicker; a smooth column changes in proportion)
            'dmass_pct_per_ms': float(np.nanmean(np.where(ok, dm / np.maximum(dt, 1e-3), np.nan)) * 100.0)
            if ok.any() else None,
        })
        series['game_ms'] = ft
    return res, series


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    d = argv[1]
    roi = [0.47, 0.0, 0.56, 0.20]
    csv_path = ref = out_json = out_series = None
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == '--csv':
            csv_path = argv[i + 1]
        elif a == '--roi':
            roi = [float(x) for x in argv[i + 1].split(',')]
        elif a == '--ref':
            ref = argv[i + 1]
        elif a == '--json':
            out_json = argv[i + 1]
        elif a == '--series':
            out_series = argv[i + 1]
        else:
            raise SystemExit(f'unknown argument {a}')
        i += 2
    res, series = analyse(d, roi, csv_path)
    if ref:
        rres, _ = analyse(ref, roi)
        res['ref'] = rres
        if 'mass_mean' in rres and 'mass_mean' in res:
            res['mass_ratio_vs_ref'] = res['mass_mean'] / max(rres['mass_mean'], 1e-6)
    print(json.dumps(res, indent=1))
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(res, f, indent=1)
    if out_series and series is not None:
        with open(out_series, 'w', newline='') as f:
            w = csv.writer(f)
            keys = list(series.keys())
            w.writerow(keys)
            for row in zip(*[series[k] for k in keys]):
                w.writerow([f'{v:.6g}' if isinstance(v, float) or isinstance(v, np.floating) else int(v) for v in row])


if __name__ == '__main__':
    main(sys.argv)
