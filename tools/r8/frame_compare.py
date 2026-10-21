"""R8-V3: compare captured frames (docs/NATIVE_ENGINE_R8_VERIFICATION.md §5).

Two images (a reference capture and a capture of the change, same scene / camera / input) or two capture series
aligned by the deterministic mode's frame log. Metrics on the whole frame and on tiles (a local error is not averaged
away by a large frame):

  exact     - every pixel equal (the strongest statement; base against base is rarely exact: TAA, particles)
  MAE       - mean |a - b| over RGB, 0..255
  PSNR      - dB (inf when exact)
  SSIM      - structural similarity of the luminance (skimage), 1.0 equal
  changed   - share of pixels whose largest channel difference is over --pixel-threshold
  tiles     - --grid NxM tiles: each tile's MAE and changed share; the worst tiles are listed
  regions   - --region name:x,y,w,h (pixels, or fractions 0..1 of the frame): critical areas checked on their own

A verdict needs the noise of the scene: --noise <json of a base-against-base comparison> (this tool's --json output).
The change passes when the frame and every tile and region stay within the noise (k * the noise's value + slack); a
tile that is worse than every tile of the noise is reported even when the frame's average is fine. Without --noise the
tool only measures.

usage:
  frame_compare.py <a.png> <b.png> [--heatmap diff.png] [--grid 8x6] [--region hud:0,0.9,1,0.1] [--json out.json]
                   [--noise base_vs_base.json] [--k 1.5] [--mask name:x,y,w,h]
  frame_compare.py --series <dirA> <dirB> --frames-a <r8_det_frames.csv> --frames-b <csv> [--at <det frame>,...]
      (series: logs\\p45\\frames\\flip_N.png of SSPC_DIAG_CAPTURE_FRAMES; the det frame log maps a flip to the game
       frame whose dt it ran with; the frames compared are those of equal game frame)
exit: 0 within the noise (or measured only), 1 outside it, 2 bad input
"""
import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

try:
    from skimage.metrics import structural_similarity
except ImportError:  # SSIM is skipped without scikit-image
    structural_similarity = None


def load(path):
    img = Image.open(path).convert("RGB")
    return np.asarray(img, dtype=np.uint8)


def parse_box(spec, w, h):
    name, _, rest = spec.partition(":")
    vals = [float(v) for v in rest.split(",")]
    if len(vals) != 4:
        raise ValueError(f"region '{spec}': name:x,y,w,h")
    if all(v <= 1.0 for v in vals):
        vals = [vals[0] * w, vals[1] * h, vals[2] * w, vals[3] * h]
    x, y, bw, bh = (int(round(v)) for v in vals)
    return name, max(0, x), max(0, y), min(w, x + bw), min(h, y + bh)


def luminance(img):
    f = img.astype(np.float32)
    return 0.299 * f[..., 0] + 0.587 * f[..., 1] + 0.114 * f[..., 2]


def metrics(a, b, thr, mask=None):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    dmax = d.max(axis=2)
    if mask is not None:
        d = d[mask]
        dmax = dmax[mask]
    n = dmax.size
    if n == 0:
        return {"pixels": 0}
    mae = float(d.mean())
    mse = float((d.astype(np.float32) ** 2).mean())
    return {
        "pixels": int(n),
        "exact": bool(dmax.max() == 0),
        "different_pixels": int((dmax > 0).sum()),
        "mae": mae,
        "psnr": float("inf") if mse == 0 else 10 * math.log10(255.0 ** 2 / mse),
        "max": int(dmax.max()),
        "changed": float((dmax > thr).sum()) / n,
    }


def ssim(a, b):
    if structural_similarity is None:
        return None
    la, lb = luminance(a), luminance(b)
    return float(structural_similarity(la, lb, data_range=255.0))


def heatmap(a, b, path, gain=8):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2).astype(np.float32)
    v = np.clip(d * gain, 0, 255) / 255.0
    # black -> red -> yellow -> white, over a dimmed copy of the reference (where it differs is readable)
    r = np.clip(v * 3, 0, 1)
    g = np.clip(v * 3 - 1, 0, 1)
    bl = np.clip(v * 3 - 2, 0, 1)
    base = luminance(a) / 255.0 * 0.25
    out = np.stack([np.maximum(r, base), np.maximum(g, base), np.maximum(bl, base)], axis=2)
    Image.fromarray((out * 255).astype(np.uint8)).save(path)


def compare(a, b, args):
    if a.shape != b.shape:
        raise ValueError(f"sizes differ: {a.shape} vs {b.shape}")
    h, w = a.shape[:2]
    mask = np.ones((h, w), dtype=bool)
    for spec in args.mask:
        _, x0, y0, x1, y1 = parse_box(spec, w, h)
        mask[y0:y1, x0:x1] = False
    out = {"size": [w, h], "frame": metrics(a, b, args.pixel_threshold, mask)}
    out["frame"]["ssim"] = ssim(a, b)
    gx, gy = (int(v) for v in args.grid.lower().split("x"))
    tiles = []
    for j in range(gy):
        for i in range(gx):
            x0, x1 = w * i // gx, w * (i + 1) // gx
            y0, y1 = h * j // gy, h * (j + 1) // gy
            m = metrics(a[y0:y1, x0:x1], b[y0:y1, x0:x1], args.pixel_threshold, mask[y0:y1, x0:x1])
            if m["pixels"]:
                m["tile"] = [i, j]
                tiles.append(m)
    out["tiles"] = tiles
    out["regions"] = {}
    for spec in args.region:
        name, x0, y0, x1, y1 = parse_box(spec, w, h)
        m = metrics(a[y0:y1, x0:x1], b[y0:y1, x0:x1], args.pixel_threshold)
        m["ssim"] = ssim(a[y0:y1, x0:x1], b[y0:y1, x0:x1]) if min(x1 - x0, y1 - y0) >= 7 else None
        m["box"] = [x0, y0, x1, y1]
        out["regions"][name] = m
    return out


def judge(res, noise, k, slack_mae, slack_changed):
    """Failures against the noise comparison (same grid / regions): a list of strings."""
    fails = []

    def over(what, m, n):
        lim_mae = k * n["mae"] + slack_mae
        lim_ch = k * n["changed"] + slack_changed
        if m["mae"] > lim_mae:
            fails.append(f"{what}: MAE {m['mae']:.3f} > {lim_mae:.3f} (noise {n['mae']:.3f})")
        if m["changed"] > lim_ch:
            fails.append(f"{what}: changed {m['changed'] * 100:.2f} % > {lim_ch * 100:.2f} % "
                         f"(noise {n['changed'] * 100:.2f} %)")

    over("frame", res["frame"], noise["frame"])
    if res["frame"].get("ssim") is not None and noise["frame"].get("ssim") is not None:
        lim = 1.0 - k * (1.0 - noise["frame"]["ssim"]) - 0.002
        if res["frame"]["ssim"] < lim:
            fails.append(f"frame: SSIM {res['frame']['ssim']:.4f} < {lim:.4f} (noise {noise['frame']['ssim']:.4f})")
    ntiles = {tuple(t["tile"]): t for t in noise.get("tiles", [])}
    worst_noise = max((t["mae"] for t in noise.get("tiles", [])), default=0.0)
    for t in res["tiles"]:
        n = ntiles.get(tuple(t["tile"]))
        if n is None:
            continue
        # a tile is compared with its own noise and, so that one noisy area does not set the bar for a quiet one,
        # it fails also when it is worse than the noisiest tile of the noise comparison
        over(f"tile {t['tile']}", t, n)
        if t["mae"] > k * worst_noise + slack_mae:
            fails.append(f"tile {t['tile']}: MAE {t['mae']:.3f} above every tile of the noise (max {worst_noise:.3f})")
    for name, m in res["regions"].items():
        n = noise.get("regions", {}).get(name)
        if n is not None:
            over(f"region {name}", m, n)
    return sorted(set(fails))


def det_frames(path):
    """flip -> game frame (the deterministic mode's frame log: frame,flip,...)."""
    out = {}
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            out.setdefault(int(row["flip"]), int(row["frame"]))
    return out


def series_pairs(args):
    fa, fb = det_frames(args.frames_a), det_frames(args.frames_b)
    shots_a = {int(p.stem.split("_")[1]): p for p in Path(args.series[0]).glob("flip_*.png")}
    shots_b = {int(p.stem.split("_")[1]): p for p in Path(args.series[1]).glob("flip_*.png")}
    by_frame_a = {fa[f]: p for f, p in shots_a.items() if f in fa}
    by_frame_b = {fb[f]: p for f, p in shots_b.items() if f in fb}
    common = sorted(set(by_frame_a) & set(by_frame_b))
    if args.at:
        want = {int(v) for v in args.at.split(",")}
        common = [c for c in common if c in want]
    return [(c, by_frame_a[c], by_frame_b[c]) for c in common]


def report(res, label):
    f = res["frame"]
    s = f"{label}: {res['size'][0]}x{res['size'][1]} exact {f['exact']}, MAE {f['mae']:.3f}, PSNR {f['psnr']:.2f} dB, " \
        f"max {f['max']}, changed {f['changed'] * 100:.2f} %" + \
        (f", SSIM {f['ssim']:.4f}" if f.get("ssim") is not None else "")
    print(s)
    worst = sorted(res["tiles"], key=lambda t: -t["mae"])[:3]
    print("  worst tiles: " + ", ".join(f"{t['tile']} MAE {t['mae']:.2f} changed {t['changed'] * 100:.1f} %"
                                         for t in worst))
    for name, m in res["regions"].items():
        print(f"  region {name} {m['box']}: exact {m['exact']}, MAE {m['mae']:.3f}, changed {m['changed'] * 100:.2f} %"
              + (f", SSIM {m['ssim']:.4f}" if m.get("ssim") is not None else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", nargs="?")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--series", nargs=2, metavar=("DIR_A", "DIR_B"))
    ap.add_argument("--frames-a")
    ap.add_argument("--frames-b")
    ap.add_argument("--at", help="series: only these game frames")
    ap.add_argument("--heatmap", help="the difference as an image (series: a directory)")
    ap.add_argument("--grid", default="8x6")
    ap.add_argument("--region", action="append", default=[])
    ap.add_argument("--mask", action="append", default=[], help="left out of the frame and tiles (not regions)")
    ap.add_argument("--pixel-threshold", type=int, default=8)
    ap.add_argument("--noise", help="a base-against-base result (--json) to judge against")
    ap.add_argument("--k", type=float, default=1.5)
    ap.add_argument("--slack-mae", type=float, default=0.25)
    ap.add_argument("--slack-changed", type=float, default=0.002)
    ap.add_argument("--json")
    args = ap.parse_args()
    noise = json.loads(Path(args.noise).read_text(encoding="utf-8")) if args.noise else None
    if noise is not None and "pairs" in noise:  # a series' noise: the mean per pair is the bar (its worst pair too)
        noise = noise["pairs"][-1]["result"] if len(noise["pairs"]) == 1 else noise["worst"]
    results = []
    try:
        if args.series:
            pairs = series_pairs(args)
            if not pairs:
                print("no game frame captured in both series", file=sys.stderr)
                return 2
            for frame, pa, pb in pairs:
                res = compare(load(pa), load(pb), args)
                res["game_frame"], res["a"], res["b"] = frame, str(pa), str(pb)
                if args.heatmap:
                    Path(args.heatmap).mkdir(parents=True, exist_ok=True)
                    heatmap(load(pa), load(pb), Path(args.heatmap) / f"diff_{frame:06}.png")
                results.append(res)
        else:
            if not args.a or not args.b:
                ap.error("two images, or --series")
            a, b = load(args.a), load(args.b)
            res = compare(a, b, args)
            res["a"], res["b"] = args.a, args.b
            if args.heatmap:
                heatmap(a, b, args.heatmap)
            results.append(res)
    except (OSError, ValueError) as e:
        print(f"bad input: {e}", file=sys.stderr)
        return 2
    failed = 0
    for res in results:
        report(res, f"game frame {res['game_frame']}" if "game_frame" in res else "frame")
        if noise is not None:
            fails = judge(res, noise, args.k, args.slack_mae, args.slack_changed)
            res["verdict"] = "OUTSIDE NOISE" if fails else "within noise"
            res["failures"] = fails
            print(f"  verdict: {res['verdict']}" + "".join(f"\n    {x}" for x in fails[:12]))
            failed += bool(fails)
    if args.json:
        doc = results[0] if len(results) == 1 else {
            "pairs": [{"game_frame": r["game_frame"], "result": r} for r in results],
            # the bar for a later comparison: per tile/region the largest value over the pairs
            "worst": worst_of(results),
        }
        Path(args.json).write_text(json.dumps(doc, indent=1), encoding="utf-8")
    return 1 if failed else 0


def worst_of(results):
    w = json.loads(json.dumps(results[0]))
    for r in results[1:]:
        for key in ("mae", "changed", "max"):
            w["frame"][key] = max(w["frame"][key], r["frame"][key])
        if r["frame"].get("ssim") is not None and w["frame"].get("ssim") is not None:
            w["frame"]["ssim"] = min(w["frame"]["ssim"], r["frame"]["ssim"])
        tiles = {tuple(t["tile"]): t for t in r["tiles"]}
        for t in w["tiles"]:
            o = tiles.get(tuple(t["tile"]))
            if o:
                t["mae"], t["changed"] = max(t["mae"], o["mae"]), max(t["changed"], o["changed"])
        for name, m in w["regions"].items():
            o = r["regions"].get(name)
            if o:
                m["mae"], m["changed"] = max(m["mae"], o["mae"]), max(m["changed"], o["changed"])
    return w


if __name__ == "__main__":
    sys.exit(main())
