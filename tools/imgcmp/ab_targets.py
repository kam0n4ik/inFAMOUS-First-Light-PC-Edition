"""A/B of render targets: the bridge's capture against the native backend's (P4.5.3).

Usage: py -I tools/imgcmp/ab_targets.py <bridge dir> <native dir> <flip_bridge> <flip_native> <out dir> [threshold]

Inputs: <dir>/[bridge_]image_<address>_<flip>_l<layer>.raw + .txt ("format=<VkFormat> width= height= row_bytes=
texel_bytes=") written by SSPC_DIAG_BRIDGE_CAPTURE (bridge, patch 0034) and SSPC_DIAG_CAPTURE_IMAGE (native backend).
For every address both have: both decoded to linear floats (8-bit unorm, half floats, 32-bit floats/depth, 10-bit),
shown the same way (HDR: x/(1+x), depth: the 1..99th percentile range of both), per-channel MAE/RMSE and the share of
pixels differing by more than `threshold` (0..1 of the display range), a heatmap of the difference, and a side-by-side
PNG (bridge | native | heatmap). The frames are of the same static scene, not the same game frame: camera, wind,
particles and exposure adaptation differ a little - read the metrics with the pictures, not instead of them.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def load(base):
    meta = dict(kv.split('=') for kv in open(base + '.txt').read().split())
    w, h, fmt, tb = int(meta['width']), int(meta['height']), int(meta['format']), int(meta['texel_bytes'])
    raw = np.fromfile(base + '.raw', dtype=np.uint8)[: w * h * tb]
    if fmt in (37, 43, 44, 50):  # 8-bit RGBA/BGRA
        a = raw.reshape(h, w, 4).astype(np.float32) / 255.0
        if fmt in (44, 50):
            a = a[..., [2, 1, 0, 3]]
        kind = 'unorm'
    elif fmt == 97:  # R16G16B16A16_SFLOAT
        a = raw.view(np.float16).reshape(h, w, 4).astype(np.float32)
        kind = 'hdr'
    elif fmt == 91:  # R16G16B16A16_UNORM
        a = raw.view(np.uint16).reshape(h, w, 4).astype(np.float32) / 65535.0
        kind = 'unorm'
    elif fmt == 83:  # R16G16_SFLOAT
        a = raw.view(np.float16).reshape(h, w, -1).astype(np.float32)
        kind = 'hdr'
    elif fmt in (100, 126):  # R32_SFLOAT / D32_SFLOAT
        a = raw.view(np.float32).reshape(h, w, 1)
        kind = 'depth' if fmt == 126 else 'hdr'
    elif fmt == 124:  # D16
        a = raw.view(np.uint16).reshape(h, w, 1).astype(np.float32) / 65535.0
        kind = 'depth'
    elif fmt in (58, 64):  # A2R10G10B10 / A2B10G10R10
        v = raw.view(np.uint32).reshape(h, w)
        c = np.stack([(v >> s) & 0x3FF for s in (0, 10, 20)], axis=-1).astype(np.float32) / 1023.0
        a = c[..., ::-1] if fmt == 58 else c
        kind = 'unorm'
    elif fmt == 9:  # R8_UNORM
        a = raw.reshape(h, w, 1).astype(np.float32) / 255.0
        kind = 'unorm'
    else:
        a = raw.reshape(h, w, -1).astype(np.float32)
        kind = 'raw'
    return a, kind, fmt


def display(a, kind, lo=None, hi=None):
    if kind == 'hdr':
        x = np.clip(a[..., :3] if a.shape[2] >= 3 else np.repeat(a[..., :1], 3, axis=2), 0, None)
        x = x / (1 + x)
        x = np.clip(x, 0, 1) ** (1 / 2.2)
    elif kind == 'depth':
        x = np.clip((a[..., :1] - lo) / max(hi - lo, 1e-9), 0, 1)
        x = np.repeat(x, 3, axis=2)
    else:
        x = a[..., :3] if a.shape[2] >= 3 else np.repeat(a[..., :1], 3, axis=2)
        x = np.clip(x, 0, 1)
    return x


def main():
    bdir, ndir, fb, fn, out = sys.argv[1:6]
    thr = float(sys.argv[6]) if len(sys.argv) > 6 else 0.1
    os.makedirs(out, exist_ok=True)
    rows = []
    for b in sorted(glob.glob(os.path.join(bdir, f'bridge_image_*_{fb}_l0.txt'))):
        addr = os.path.basename(b).split('_')[2]
        n = os.path.join(ndir, f'image_{addr}_{fn}_l0.txt')
        if not os.path.exists(n):
            continue
        A, kind, fmt = load(b[:-4])
        B, kind_n, fmt_n = load(n[:-4])
        if A.shape != B.shape:
            rows.append(f'{addr}: shapes differ {A.shape} vs {B.shape}')
            continue
        lo = hi = None
        if kind == 'depth':
            both = np.concatenate([A.ravel(), B.ravel()])
            lo, hi = np.percentile(both, 1), np.percentile(both, 99)
        da, db = display(A, kind, lo, hi), display(B, kind, lo, hi)
        d = np.abs(da - db)
        mae, rmse = d.mean(), np.sqrt((d ** 2).mean())
        share = (d.max(axis=2) > thr).mean()
        lum_a, lum_b = da.mean(), db.mean()
        rows.append(f'{addr} fmt {fmt} {A.shape[1]}x{A.shape[0]} ({kind}): mean bridge {lum_a:.4f} native {lum_b:.4f} '
                    f'| MAE {mae:.4f} RMSE {rmse:.4f} | pixels differing > {thr}: {share * 100:.2f}%')
        heat = np.clip(d.max(axis=2) * 4, 0, 1)
        heat_rgb = np.stack([heat, heat * 0.3, 1 - heat], axis=-1) * (heat[..., None] > 0.02)
        w, h = A.shape[1], A.shape[0]
        scale = max(1, w // 640)
        tiles = [Image.fromarray((x * 255).astype(np.uint8)).resize((w // scale, h // scale)) for x in (da, db, heat_rgb)]
        sheet = Image.new('RGB', (3 * (w // scale), h // scale))
        for k, t in enumerate(tiles):
            sheet.paste(t, (k * (w // scale), 0))
        sheet.save(os.path.join(out, f'ab_{addr}.png'))
    report = '\n'.join(rows)
    open(os.path.join(out, 'ab_report.txt'), 'w').write(report + '\n')
    print(report)


if __name__ == '__main__':
    main()
