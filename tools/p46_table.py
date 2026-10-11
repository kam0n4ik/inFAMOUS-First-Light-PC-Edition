# SecondSonPC P4.6: an A/B table of two measurement series (tools\tests\p46_runs.ps1), per scene: the mean of the
# repeats and their range.
#   py -I tools\p46_table.py <series A dir> <series B dir> [scene ...]
# Reads <dir>\<scene>_<n>\stats.json (tools\frame_stats.py) and the VRAM / private / system commit of each run from
# <dir>\summary.txt.
import json
import os
import re
import sys

METRICS = [
    ("avg FPS", "avg_fps", 1), ("median FPS", "median_fps", 1), ("1% low FPS", "low1_fps", 1),
    ("p95 frame, ms", "ft_p95_ms", -1), ("p99 frame, ms", "ft_p99_ms", -1),
    ("GPU thread busy, ms/frame", "mean_cp_ms", -1), ("recording, ms/frame", "mean_record_ms", -1),
    ("waits for the GPU (Synchronize), ms/frame", "mean_sync_ms", -1),
    ("blocked WAIT_REG_MEM, ms/frame", "mean_blocked_other_ms", -1),
    ("GPU busy, ms/frame", "mean_gpu_busy_ms", -1), ("GPU tail after the CP, ms", "gpu_tail_ms", -1),
    ("draws/frame", "mean_draws", 0), ("dispatches/frame", "mean_dispatches", 0), ("batches/frame", "mean_batches", 0),
]


def load(series):
    runs = {}
    for name in sorted(os.listdir(series)):
        p = os.path.join(series, name, "stats.json")
        m = re.match(r"(.+)_(\d+)$", name)
        if m and os.path.exists(p):
            with open(p) as f:
                runs.setdefault(m.group(1), []).append(json.load(f))
    mem = {}
    s = os.path.join(series, "summary.txt")
    if os.path.exists(s):
        for line in open(s, encoding="utf-8-sig"):
            m = re.match(r"(\S+)_(\d+)\s.*VRAM (\d+) MiB, private (\d+) MiB, system commit (\d+) MiB", line)
            if m:
                mem.setdefault(m.group(1), []).append((int(m.group(3)), int(m.group(4)), int(m.group(5))))
    return runs, mem


def fmt(vals):
    vals = [v for v in vals if isinstance(v, (int, float))]
    if not vals:
        return "n/a"
    mean = sum(vals) / len(vals)
    return f"{mean:.1f} ({min(vals):.1f}-{max(vals):.1f})" if len(vals) > 1 else f"{mean:.1f}"


def mean(vals):
    vals = [v for v in vals if isinstance(v, (int, float))]
    return sum(vals) / len(vals) if vals else None


def main(argv):
    a_dir, b_dir = argv[1], argv[2]
    a, am = load(a_dir)
    b, bm = load(b_dir)
    scenes = argv[3:] or sorted(set(a) | set(b))
    for sc in scenes:
        ra, rb = a.get(sc, []), b.get(sc, [])
        print(f"\n### {sc}: A = {os.path.basename(a_dir)} ({len(ra)} runs), B = {os.path.basename(b_dir)} ({len(rb)} runs)")
        print("| Metric | A | B | Change |")
        print("|---|---|---|---|")
        for label, key, better in METRICS:
            va = [r.get(key) for r in ra]
            vb = [r.get(key) for r in rb]
            ma, mb = mean(va), mean(vb)
            ch = ""
            if ma is not None and mb is not None and ma != 0:
                ch = f"{(mb - ma) / ma * 100:+.0f}%"
            print(f"| {label} | {fmt(va)} | {fmt(vb)} | {ch} |")
        for i, label in enumerate(("VRAM, MiB", "process private, MiB", "system commit, MiB")):
            va = [m[i] for m in am.get(sc, [])]
            vb = [m[i] for m in bm.get(sc, [])]
            print(f"| {label} | {fmt(va)} | {fmt(vb)} | |")


if __name__ == "__main__":
    main(sys.argv)
