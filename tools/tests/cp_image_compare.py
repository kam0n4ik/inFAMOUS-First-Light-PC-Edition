"""SecondSon Native CP: scanout images of command processor modes compared (tools/tests/cp_image_ab.ps1).

usage: py -I -X utf8 cp_image_compare.py <capture dir of the reference run> <capture dir> [...] [--out compare.md]

Every capture dir holds image_<address>_<flip>_l0.png (SSPC_DIAG_CAPTURE_IMAGE). A frozen scene still moves (TAA
jitter, animated materials), so every image of a run is paired with its closest image of the reference run (smallest
mean absolute difference). Per run: the pairs' mean absolute difference (0..255), the share of pixels differing at
all and by more than 8/255 in any channel, PSNR. Two runs of one mode give the noise floor. Reads only.
"""
import glob
import math
import os
import sys

import numpy as np
from PIL import Image


def load(d):
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "image_*_l0.png"))):
        out[os.path.basename(p)] = np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)
    return out


def metrics(a, b):
    if a.shape != b.shape:
        return None
    d = np.abs(a - b)
    mae = float(d.mean())
    mse = float((d.astype(np.float64) ** 2).mean())
    px = d.max(axis=2)
    return {"mae": mae, "psnr": 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse),
            "frac_any": float((px > 0).mean()), "frac_gt8": float((px > 8).mean())}


def main(argv):
    out_md = None
    dirs = []
    i = 1
    while i < len(argv):
        if argv[i] == "--out":
            out_md = argv[i + 1]
            i += 2
        else:
            dirs.append(argv[i])
            i += 1
    if len(dirs) < 2:
        raise SystemExit(__doc__)
    ref = load(dirs[0])
    if not ref:
        raise SystemExit(f"no images in {dirs[0]}")
    lines = ["# CP image comparison", "", f"reference: `{dirs[0]}` ({len(ref)} images)", "",
             "| run | images | mean abs diff (0..255) | pixels differing | pixels > 8/255 | PSNR dB |",
             "|---|---|---|---|---|---|"]
    for d in dirs[1:]:
        imgs = load(d)
        best = []
        for img in imgs.values():
            cands = [m for m in (metrics(img, r) for r in ref.values()) if m is not None]
            if cands:
                best.append(min(cands, key=lambda m: m["mae"]))
        if not best:
            lines.append(f"| `{d}` | {len(imgs)} | - | - | - | - |")
            continue
        mean = lambda k: sum(m[k] for m in best) / len(best)
        lines.append(f"| `{d}` | {len(best)} | {mean('mae'):.3f} | {100 * mean('frac_any'):.2f} % | "
                     f"{100 * mean('frac_gt8'):.3f} % | {mean('psnr'):.1f} |")
    text = "\n".join(lines) + "\n"
    print(text)
    if out_md:
        with open(out_md, "w", encoding="utf-8") as f:
            f.write(text)


if __name__ == "__main__":
    main(sys.argv)
