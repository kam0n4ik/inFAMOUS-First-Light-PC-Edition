"""SecondSonPC P5.3D.2: strict image comparison of the game's PM4 path and the native render path.

usage: py -I -X utf8 p53_image_compare.py <capture dir> [--names names.json] [--out <dir>]

The capture dir holds what SSPC_DIAG_CAPTURE_IMAGE / SSPC_DIAG_CAPTURE_DIR wrote during an A/B run
(SSPC_P53_NATIVE_AB): image_<address>_<flip>_l<layer>.raw + .txt per capture, and captures.csv - every capture with
the processor's native-operation counters at that flip. A flip whose frame applied no native draw is a PM4 frame; one
that applied the full native share is a native frame; flips in between (the switch fell inside the frame) are left out.

For every target (address) every native frame is compared with every PM4 frame of the same target, and every PM4
frame with every other PM4 frame (the noise floor: a frozen scene still moves - TAA jitter, animated materials). A
native frame is reported with its closest PM4 frame; "exact" means bit for bit. Metrics per pair, over the decoded
channel values (unorm/snorm in 0..1, floats as they are; NaN = NaN when the bits agree):
  max_abs, mae, rms, frac_gt_0 (any channel differs), frac_gt_lsb (differs by more than one unit in the last place of
  the format: 1/255 for 8-bit, 1/1023 for 10-bit, 2^-10 relative for half floats, 2^-23 relative for floats),
  frac_gt_1pct (more than 1% of the channel's range, or 1% relative for floats), psnr (unorm formats).
Writes <out>/report.md, <out>/report.json and a heat map PNG per target for the worst native pair.
"""
import argparse
import csv
import hashlib
import json
import math
import os
import struct
import sys
import zlib
from collections import defaultdict

import numpy as np

# VkFormat -> (numpy dtype, channels, kind, bits) ; kind: unorm | snorm | float | uint | packed
FORMATS = {
    9: (np.uint8, 1, "unorm", 8),        # R8_UNORM
    16: (np.uint8, 2, "unorm", 8),       # R8G8_UNORM
    37: (np.uint8, 4, "unorm", 8),       # R8G8B8A8_UNORM
    38: (np.int8, 4, "snorm", 8),        # R8G8B8A8_SNORM
    43: (np.uint8, 4, "unorm", 8),       # R8G8B8A8_SRGB (compared as stored)
    44: (np.uint8, 4, "unorm", 8),       # B8G8R8A8_UNORM
    50: (np.uint8, 4, "unorm", 8),       # B8G8R8A8_SRGB
    64: (np.uint32, 1, "a2b10g10r10", 10),  # A2B10G10R10_UNORM_PACK32
    70: (np.uint16, 1, "unorm", 16),     # R16_UNORM
    76: (np.float16, 1, "float", 16),    # R16_SFLOAT
    77: (np.uint16, 2, "unorm", 16),     # R16G16_UNORM
    83: (np.float16, 2, "float", 16),    # R16G16_SFLOAT
    91: (np.uint16, 4, "unorm", 16),     # R16G16B16A16_UNORM
    97: (np.float16, 4, "float", 16),    # R16G16B16A16_SFLOAT
    98: (np.uint32, 1, "uint", 32),      # R32_UINT
    100: (np.float32, 1, "float", 32),   # R32_SFLOAT
    103: (np.float32, 2, "float", 32),   # R32G32_SFLOAT
    109: (np.float32, 4, "float", 32),   # R32G32B32A32_SFLOAT
    122: (np.uint32, 1, "b10g11r11", 11),  # B10G11R11_UFLOAT_PACK32
    124: (np.uint16, 1, "unorm", 16),    # D16_UNORM
    126: (np.float32, 1, "float", 32),   # D32_SFLOAT
}


def read_meta(txt):
    meta = {}
    with open(txt, encoding="utf-8") as f:
        for tok in f.read().split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                meta[k] = int(v)
    return meta


def unpack_b10g11r11(u):
    def uf(bits, mbits):
        e = (bits >> mbits) & 0x1F
        m = bits & ((1 << mbits) - 1)
        val = np.where(e == 0, m / (1 << mbits) * 2.0 ** -14,
                       np.where(e == 31, np.where(m == 0, np.inf, np.nan),
                                (1 + m / (1 << mbits)) * np.exp2(e.astype(np.float64) - 15)))
        return val
    r = uf(u & 0x7FF, 6)
    g = uf((u >> 11) & 0x7FF, 6)
    b = uf((u >> 22) & 0x3FF, 5)
    return np.stack([r, g, b], axis=-1)


def load(path_base):
    meta = read_meta(path_base + ".txt")
    fmt, w, h = meta["format"], meta["width"], meta["height"]
    row = meta.get("row_bytes", 0)
    raw = np.fromfile(path_base + ".raw", dtype=np.uint8)
    if fmt not in FORMATS:
        tb = meta.get("texel_bytes", 4)
        rb = row or w * tb
        data = raw[: rb * h].reshape(h, rb)[:, : w * tb].reshape(h, w, tb)
        return {"fmt": fmt, "kind": "bytes", "bits": 8, "bits_raw": data, "values": data.astype(np.float64) / 255.0}
    dt, ch, kind, bits = FORMATS[fmt]
    tb = np.dtype(dt).itemsize * ch
    rb = row or w * tb
    rows = raw[: rb * h].reshape(h, rb)[:, : w * tb]
    bits_raw = np.ascontiguousarray(rows).view(np.uint8).reshape(h, w, tb)
    arr = np.ascontiguousarray(rows).view(dt).reshape(h, w, ch)
    if kind == "unorm":
        values = arr.astype(np.float64) / float((1 << bits) - 1)
    elif kind == "snorm":
        values = np.maximum(arr.astype(np.float64) / float((1 << (bits - 1)) - 1), -1.0)
    elif kind == "float":
        values = arr.astype(np.float64)
    elif kind == "uint":
        values = arr.astype(np.float64)
    elif kind == "a2b10g10r10":
        u = arr[..., 0]
        values = np.stack([(u & 0x3FF), (u >> 10) & 0x3FF, (u >> 20) & 0x3FF, (u >> 30) & 3], axis=-1).astype(
            np.float64) / np.array([1023.0, 1023.0, 1023.0, 3.0])
    elif kind == "b10g11r11":
        values = unpack_b10g11r11(arr[..., 0].astype(np.uint32))
    else:
        values = arr.astype(np.float64)
    return {"fmt": fmt, "kind": kind, "bits": bits, "bits_raw": bits_raw, "values": values}


def lsb_threshold(img):
    kind, bits = img["kind"], img["bits"]
    if kind in ("unorm", "snorm", "bytes"):
        return None, 1.0 / ((1 << bits) - 1) * 1.0001
    if kind == "a2b10g10r10":
        return None, 1.0 / 1023.0 * 1.0001
    if kind == "b10g11r11":
        return 2.0 ** -5, None
    if kind == "float":
        return (2.0 ** -10 if bits == 16 else 2.0 ** -23), None
    return None, 0.5


def compare(a, b):
    if a["values"].shape != b["values"].shape:
        return {"error": f"shape {a['values'].shape} vs {b['values'].shape}"}
    same_bits = np.all(a["bits_raw"] == b["bits_raw"], axis=-1)
    va, vb = a["values"], b["values"]
    both_nan = np.isnan(va) & np.isnan(vb)
    d = np.abs(va - vb)
    d = np.where(both_nan, 0.0, d)
    d = np.where(np.isnan(d) | np.isinf(d), np.inf, d)
    finite = np.isfinite(d)
    dd = np.where(finite, d, 0.0)
    per_px = np.where(np.all(finite, axis=-1), np.max(dd, axis=-1), np.inf)
    n = per_px.size
    rel, absl = lsb_threshold(a)
    if rel is not None:
        scale = np.maximum(np.maximum(np.abs(va), np.abs(vb)), 2.0 ** -14)
        gt_lsb = np.any(np.where(both_nan, False, d > rel * scale), axis=-1)
        gt_1 = np.any(np.where(both_nan, False, d > 0.01 * scale), axis=-1)
    else:
        gt_lsb = np.any(d > absl, axis=-1)
        gt_1 = np.any(d > 0.01, axis=-1)
    out = {
        "exact": bool(np.all(same_bits)),
        "frac_bits_differ": float(1.0 - np.mean(same_bits)),
        "max_abs": float(np.max(per_px)) if n else 0.0,
        "mae": float(np.mean(dd)),
        "rms": float(math.sqrt(float(np.mean(dd * dd)))),
        "frac_gt_0": float(np.mean(per_px > 0)),
        "frac_gt_lsb": float(np.mean(gt_lsb)),
        "frac_gt_1pct": float(np.mean(gt_1)),
        "nonfinite_diff_px": int(np.sum(~np.all(finite, axis=-1))),
    }
    if a["kind"] in ("unorm", "a2b10g10r10", "bytes"):
        mse = float(np.mean(dd * dd))
        out["psnr_db"] = float("inf") if mse == 0 else 10 * math.log10(1.0 / mse)
    out["_heat"] = per_px
    return out


def write_png(path, gray):
    h, w = gray.shape
    raw = b"".join(b"\x00" + gray[y].tobytes() for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def heat(per_px):
    v = np.where(np.isfinite(per_px), per_px, np.nanmax(np.where(np.isfinite(per_px), per_px, 0)) + 1)
    m = float(np.max(v)) if v.size else 0.0
    if m <= 0:
        return np.zeros(per_px.shape, np.uint8)
    return np.clip(np.sqrt(v / m) * 255.0, 0, 255).astype(np.uint8)


def classify(rows):
    """flip -> 'pm4' | 'native' | 'mixed' from the counters' growth between consecutive captured flips."""
    by_flip = {}
    for r in rows:
        by_flip.setdefault(int(r["flip"]), r)
    flips = sorted(by_flip)
    deltas = {}
    for prev, cur in zip(flips, flips[1:]):
        if cur != prev + 1:
            continue
        a, b = by_flip[prev], by_flip[cur]
        deltas[cur] = {k: int(b[k]) - int(a[k]) for k in ("native_draws", "native_state", "verified_draws")}
    native_counts = sorted(d["native_draws"] for d in deltas.values() if d["native_draws"] > 0)
    full = native_counts[len(native_counts) // 2] if native_counts else 0
    out = {}
    for f, d in deltas.items():
        if d["native_draws"] == 0 and d["native_state"] == 0:
            out[f] = "pm4"
        elif full and d["native_draws"] >= 0.98 * full:
            out[f] = "native"
        else:
            out[f] = "mixed"
    return out, deltas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--names", help="json {address hex: name}")
    ap.add_argument("--out")
    ap.add_argument("--max-pairs", type=int, default=40, help="PM4 frames compared per native frame (nearest flips)")
    args = ap.parse_args()
    out = args.out or os.path.join(args.dir, "compare")
    os.makedirs(out, exist_ok=True)
    names = {}
    if args.names:
        with open(args.names, encoding="utf-8") as f:
            names = {k.lower(): v for k, v in json.load(f).items()}
    with open(os.path.join(args.dir, "captures.csv"), encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    flip_rows = [r for r in rows if int(r.get("pass", -1) or -1) < 0]
    kinds, deltas = classify(flip_rows)
    by_addr = defaultdict(dict)    # (pass, address) -> {flip: path}
    by_kind = defaultdict(dict)    # (pass, address) -> {flip: pm4 | native | mixed}
    for r in rows:
        flip, addr, pas = int(r["flip"]), r["address"].lower(), int(r.get("pass", -1) or -1)
        if pas < 0:
            base = os.path.join(args.dir, f"image_{addr}_{flip}_l0")
            kind = kinds.get(flip)
        else:
            # a pass capture says itself which path drew it: every draw of the pass native, or every one from PM4
            base = os.path.join(args.dir, f"p{pas}_{addr}_{flip}_l0")
            n, p = int(r["pass_native_draws"]), int(r["pass_pm4_draws"])
            kind = "native" if n > 0 and p == 0 else "pm4" if p > 0 and n == 0 else "mixed"
        if kind is not None and os.path.exists(base + ".raw"):
            by_addr[(pas, addr)][flip] = base
            by_kind[(pas, addr)][flip] = kind
    report = {"flips": {str(k): v for k, v in sorted(kinds.items())},
              "deltas": {str(k): v for k, v in sorted(deltas.items())}, "targets": []}
    lines = ["# P5.3D.2 image comparison: PM4 path vs native path", "",
             f"capture dir: `{args.dir}`", "",
             f"flips: {sum(1 for v in kinds.values() if v == 'pm4')} PM4, "
             f"{sum(1 for v in kinds.values() if v == 'native')} native, "
             f"{sum(1 for v in kinds.values() if v == 'mixed')} mixed (left out)", ""]
    keys = ["max_abs", "mae", "rms", "frac_gt_0", "frac_gt_lsb", "frac_gt_1pct"]
    for (pas, addr), frames in sorted(by_addr.items()):
        name = (f"after pass {pas}: " if pas >= 0 else "flip end: ") + names.get(addr, "")
        kind_of = by_kind[(pas, addr)]
        pm4 = sorted(f for f in frames if kind_of[f] == "pm4")
        nat = sorted(f for f in frames if kind_of[f] == "native")
        light = {}  # flip -> (digest of the texels, a 1/16 sample of the texel bytes): exact match and candidate choice

        def get(f):
            return load(frames[f])

        def sig(f):
            if f not in light:
                img = load(frames[f])
                raw = np.ascontiguousarray(img["bits_raw"])
                light[f] = (hashlib.blake2b(raw.tobytes(), digest_size=16).hexdigest(),
                            raw[::4, ::4].astype(np.int16), img["fmt"])
            return light[f]

        def closest(f, pool):
            """The pool's flip closest to f: bit-identical if there is one (no decoding), else the smallest byte
            difference on the sample; then the full metrics for that pair only."""
            if not pool:
                return None
            d0, s0, _ = sig(f)
            for g in pool:
                if sig(g)[0] == d0:
                    a = get(f)
                    m = compare(a, a)
                    return g, m
            best = min(pool, key=lambda g: float(np.mean(np.abs(sig(g)[1] - s0))) if sig(g)[1].shape == s0.shape
                       else float("inf"))
            return best, compare(get(f), get(best))
        entry = {"pass": pas, "address": addr, "name": name, "pm4_frames": len(pm4), "native_frames": len(nat),
                 "native_vs_pm4": [], "pm4_vs_pm4": []}
        if not pm4 or not nat:
            entry["note"] = "no PM4 or no native frame"
            report["targets"].append(entry)
            continue
        fmt = sig(pm4[0])[2]
        entry["format"] = fmt
        for f in nat:
            r = closest(f, sorted(pm4, key=lambda g: abs(g - f))[: args.max_pairs])
            if r is None or "error" in r[1]:
                continue
            g, m = r
            entry["native_vs_pm4"].append({"native": f, "pm4": g, **{k: v for k, v in m.items() if k != "_heat"}})
            score = (not m["exact"], m["frac_gt_lsb"], m["rms"])
            if entry.get("_worst") is None or score > entry["_worst"][0]:
                entry["_worst"] = (score, f, g, m["_heat"])
        for f in pm4:
            r = closest(f, sorted((g for g in pm4 if g != f), key=lambda g: abs(g - f))[: args.max_pairs])
            if r is None or "error" in r[1]:
                continue
            entry["pm4_vs_pm4"].append({"a": f, "b": r[0], **{k: v for k, v in r[1].items() if k != "_heat"}})
        worst = entry.pop("_worst", None)
        if worst is not None:
            write_png(os.path.join(out, f"heat_p{pas}_{addr}_{worst[1]}_vs_{worst[2]}.png"), heat(worst[3]))
        report["targets"].append(entry)
        nv, pp = entry["native_vs_pm4"], entry["pm4_vs_pm4"]
        lines.append(f"## {addr} {name} (format {fmt}, {len(pm4)} PM4 / {len(nat)} native frames)")
        lines.append("")
        lines.append("| pairs | exact | " + " | ".join(f"worst {k}" for k in keys) + " |")
        lines.append("|---|---|" + "---|" * len(keys))
        for label, lst in (("native vs closest PM4", nv), ("PM4 vs closest other PM4 (noise floor)", pp)):
            if not lst:
                lines.append(f"| {label} | - |" + " - |" * len(keys))
                continue
            ex = sum(1 for m in lst if m["exact"])
            lines.append(f"| {label} | {ex}/{len(lst)} | " +
                         " | ".join(f"{max(m[k] for m in lst):.3g}" for k in keys) + " |")
        lines.append("")
    # The settled scene: from the first flip after which every PM4 capture (all targets) has a bit-identical PM4 twin.
    # Before it the frozen scene still converges (both paths alike); after it a native capture without a bit-identical
    # PM4 twin is a difference of the native path.
    # (the first PM4 capture after the last one without a twin: the native block between two PM4 blocks may still
    # converge, so the scene counts as settled from a PM4 capture that is itself exact)
    # Per target: settled from the first PM4 capture after its last PM4 capture without a twin; a target whose PM4
    # captures never settle (something in it still moves) is judged against its own PM4 noise instead.
    settle_of = {}
    for t in report["targets"]:
        flips = sorted(m["a"] for m in t["pm4_vs_pm4"])
        bad = [m["a"] for m in t["pm4_vs_pm4"] if not m["exact"]]
        last_bad = max(bad, default=-1)
        after = [f for f in flips if f > last_bad]
        settle_of[(t["pass"], t["address"])] = after[0] if after else None
    settled = [s for s in settle_of.values() if s is not None]
    settle = max(settled, default=0)
    report["settled_from_flip"] = settle
    lines += ["## Settled scene", "",
              f"From flip {settle} (a PM4 capture) every target whose PM4 captures settle has a bit-identical PM4 "
              f"twin for each of them; the native captures from there on are compared. A target that never settles "
              f"(something in it still changes between PM4 frames) is compared with its own PM4 noise over the same "
              f"flips.", "",
              "| target | native captures | bit-identical to a PM4 capture | worst native frac_gt_0 | PM4 noise "
              "(worst frac_gt_0, PM4 vs PM4) | verdict |",
              "|---|---|---|---|---|---|"]
    all_ok = True
    for t in report["targets"]:
        nv = [m for m in t["native_vs_pm4"] if m["native"] >= settle]
        pp = [m for m in t["pm4_vs_pm4"] if m["a"] >= settle]
        ex = [m for m in nv if m["exact"]]
        worst = max((m["frac_gt_0"] for m in nv), default=0.0)
        noise = max((m["frac_gt_0"] for m in pp), default=0.0)
        if settle_of[(t["pass"], t["address"])] is not None:
            ok = len(ex) == len(nv)
            verdict = "bit-identical" if ok else "DIFFERS"
        else:
            # Something in the target keeps changing on the PM4 path too. The native path adds nothing of its own when
            # every pixel where a native capture differs from its PM4 counterpart is one that also changes between
            # PM4 captures (the union of their differences over the window).
            frames = by_addr[(t["pass"], t["address"])]
            lo = min((m["native"] for m in nv), default=settle) - 8 # (the PM4 frames around the compared ones only)
            pm4f = sorted(f for f in frames if by_kind[(t["pass"], t["address"])][f] == "pm4" and f >= lo)
            changing = None
            prev = None
            for f in pm4f:
                cur = load(frames[f])["bits_raw"]
                if prev is not None and prev.shape == cur.shape:
                    d = np.any(prev != cur, axis=-1)
                    changing = d if changing is None else (changing | d)
                prev = cur
            # Time-matched control: each native capture against the PM4 capture nearest in time (distance d), and the
            # PM4 captures of the neighbouring PM4 blocks against each other at the same distance d. The scene drifts
            # (the differences shrink with time), so only equal distances in time compare like with like.
            kinds_t = by_kind[(t["pass"], t["address"])]
            bits = {}

            def b_of(f):
                if f not in bits:
                    bits[f] = load(frames[f])["bits_raw"]
                return bits[f]

            def frac(f, g):
                return float(np.mean(np.any(b_of(f) != b_of(g), axis=-1)))
            all_pm4 = sorted(f for f in frames if kinds_t[f] == "pm4")
            new_px, matched, above = 0, [], 0
            for m in nv:
                n = m["native"]
                p = min(all_pm4, key=lambda g: (abs(g - n), g))
                dist = abs(n - p)
                dn = np.any(b_of(n) != b_of(p), axis=-1)
                new_px = max(new_px, int(np.sum(dn & ~changing)) if changing is not None else int(np.sum(dn)))
                fn = float(np.mean(dn))
                ctrl = [frac(q, q + dist) for q in all_pm4
                        if q + dist in kinds_t and kinds_t[q + dist] == "pm4" and abs(q - n) <= 8]
                c = max(ctrl) if ctrl else None
                matched.append({"native": n, "pm4": p, "distance": dist, "native_frac": fn, "control_max": c,
                                "controls": len(ctrl)})
                if c is None or fn > c:
                    above += 1
            t["native_new_pixels"] = new_px
            t["time_matched"] = matched
            ok = new_px == 0 and above == 0
            verdict = (f"within PM4 noise: no new pixel, {len(matched)}/{len(matched)} at or below the time-matched "
                       f"PM4 control" if ok else
                       f"NOT shown equivalent: {new_px} new pixels, {above}/{len(matched)} above (or without) the "
                       f"time-matched PM4 control")
        all_ok = all_ok and ok
        t["verdict"] = verdict
        lines.append(f"| {t['name']} {t['address']} | {len(nv)} | {len(ex)} | {worst:.3g} | {noise:.3g} | {verdict} |")
    for t in report["targets"]:
        if t.get("time_matched"):
            lines += ["", f"### {t['name']} {t['address']}: time-matched control (fraction of pixels that differ)", "",
                      "| native | nearest PM4 | distance | native vs PM4 | PM4 vs PM4 at that distance (max) |",
                      "|---|---|---|---|---|"]
            for m in t["time_matched"]:
                c = "-" if m["control_max"] is None else f"{m['control_max']:.3g} ({m['controls']})"
                lines.append(f"| {m['native']} | {m['pm4']} | {m['distance']} | {m['native_frac']:.3g} | {c} |")
            lines.append(f"\nnative differences in pixels that never change between PM4 captures: "
                         f"{t['native_new_pixels']}")
    report["settled_all_ok"] = all_ok
    lines += ["", f"**Result: {'no' if all_ok else 'a'} difference of the native path in the settled scene"
              f"{'' if all_ok else ' - see the verdicts'}.**", ""]
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
