"""R9 stand series summary (tools/r9/series.ps1): per configuration the median [min..max] of each run's frame statistics
over its route's window (stats.json of tools/frame_stats.py) and of the hitches counted in gpu_frames.csv (present
intervals over 50 / 100 / 250 ms), the background CPU while measured; runs <config>_<k> only (not _dirty / _aborted).

  py -I -X utf8 tools/r9/ab_summary.py <series dir>
"""
import csv
import json
import os
import pathlib
import re
import statistics
import sys

METRICS = [  # (column, stats.json key or computed, format)
    ("FPS", "avg_fps", "{:.2f}"),
    ("1% low", "low1_fps", "{:.2f}"),
    ("0.1% low", "present_low01_fps", "{:.2f}"),
    ("p95 ms", "ft_p95_ms", "{:.2f}"),
    ("p99 ms", "ft_p99_ms", "{:.2f}"),
    ("p99.9 ms", "present_p999_ms", "{:.1f}"),
    ("max ms", "ft_max_ms", "{:.0f}"),
    (">50 ms", "h50", "{:.0f}"),
    (">100 ms", "h100", "{:.0f}"),
    (">250 ms", "h250", "{:.0f}"),
    ("CP ms", "mean_cp_ms", "{:.2f}"),
    ("T2 busy ms", "mean_par_busy_ms", "{:.2f}"),
    ("GPU ms", "mean_gpu_busy_ms", "{:.2f}"),
    ("bg %", "bg", "{:.1f}"),
]


def hitches(run, window):
    pres = []
    with open(os.path.join(run, "gpu_frames.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r.get("present_ms"):
                pres.append(float(r["present_ms"]))
    pres.sort()
    lo, hi = window[0] * 1000.0, window[1] * 1000.0
    ft = [b - a for a, b in zip(pres, pres[1:]) if lo <= b <= hi]
    return {"h50": sum(1 for x in ft if x > 50), "h100": sum(1 for x in ft if x > 100),
            "h250": sum(1 for x in ft if x > 250)}


def background(run):
    if not os.path.exists(os.path.join(run, "cpu_bg.csv")):
        return None
    sys.path.insert(0, r"E:\research\deep_perf\tools")
    import summary  # noqa: E402 (the R6-R8 stand's definition of the background)
    return summary.background(pathlib.Path(run))


def main(argv):
    root = argv[1]
    runs = {}
    for d in sorted(os.listdir(root)):
        m = re.fullmatch(r"(.+)_(\d+)", d)
        p = os.path.join(root, d)
        if not m or not os.path.exists(os.path.join(p, "stats.json")):
            continue
        st = json.load(open(os.path.join(p, "stats.json")))
        st.update(hitches(p, st.get("window_s", [0, 1e9])))
        st["bg"] = background(p)
        runs.setdefault(m.group(1), []).append((int(m.group(2)), st))
    lines = [f"series {root}: median [min..max] over the runs of each configuration"]
    head = ["config", "n"] + [c for c, _, _ in METRICS]
    rows = []
    for cfg, items in runs.items():
        items.sort()
        row = [cfg, str(len(items))]
        for _, key, fmt in METRICS:
            v = [s[key] for _, s in items if s.get(key) is not None]
            if not v:
                row.append("-")
                continue
            row.append(f"{fmt.format(statistics.median(v))} [{fmt.format(min(v))}..{fmt.format(max(v))}]")
        rows.append(row)
    w = [max(len(r[i]) for r in rows + [head]) for i in range(len(head))]
    lines.append("  ".join(h.ljust(w[i]) for i, h in enumerate(head)))
    for r in rows:
        lines.append("  ".join(x.ljust(w[i]) for i, x in enumerate(r)))
    lines.append("")
    lines.append("per run:")
    for cfg, items in runs.items():
        for k, s in items:
            lines.append(f"  {cfg}_{k}: FPS {s.get('avg_fps', 0):.2f}, 1% low {s.get('low1_fps', 0):.2f}, max "
                         f"{s.get('ft_max_ms', 0):.0f} ms, >100 ms {s['h100']}, bg {s['bg']}")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
