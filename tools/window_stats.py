"""Frame statistics of time windows of one run (P2.5: the same run, the camera turned between windows).

    python -I tools\\window_stats.py <frametimes.csv> <name=t0-t1>[,<name=t0-t1>...] [--json out.json]

frametimes.csv is written by --frame-log (t_s, frame_ms, and with --track-stats the per-frame counters of
tools/perf_report.py). For each window [t0, t1) in seconds of the run: frames, FPS (frames / time),
P50/P95/P99 frame time, 1 % low (mean of the worst 1 % of frames), frames over 33/50/100 ms and, when the
file has them, the mean per frame of every counter column.
"""
from __future__ import annotations

import csv
import json
import statistics
import sys
from pathlib import Path


def percentile(values: list[float], p: float) -> float:
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def parse_windows(spec: str) -> list[tuple[str, float, float]]:
    out = []
    for item in spec.split(","):
        name, rng = item.split("=", 1)
        t0, t1 = rng.split("-", 1)
        out.append((name.strip(), float(t0), float(t1)))
    return out


def window_stats(rows: list[dict], counters: list[str], t0: float, t1: float) -> dict:
    sel = [r for r in rows if t0 <= float(r["t_s"]) < t1]
    if not sel:
        return {"frames": 0}
    ms = [float(r["frame_ms"]) for r in sel]
    worst = sorted(ms, reverse=True)[: max(1, len(ms) // 100)]
    out = {
        "frames": len(ms),
        "fps": round(len(ms) / (sum(ms) / 1000.0), 2),
        "p50_ms": round(percentile(ms, 50), 1),
        "p95_ms": round(percentile(ms, 95), 1),
        "p99_ms": round(percentile(ms, 99), 1),
        "low1_fps": round(1000.0 / statistics.mean(worst), 1),
        "over33": sum(1 for m in ms if m > 1000.0 / 30.0 + 1.0),
        "over50": sum(1 for m in ms if m > 51.0),
        "over100": sum(1 for m in ms if m > 100.0),
    }
    for c in counters:
        vals = [float(r[c]) for r in sel if r.get(c) not in (None, "")]
        if vals:
            out[c] = round(statistics.mean(vals), 1)
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path = Path(sys.argv[1])
    windows = parse_windows(sys.argv[2])
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    counters = [c for c in (rows[0].keys() if rows else []) if c not in ("t_s", "frame_ms")]
    result = {name: window_stats(rows, counters, t0, t1) for name, t0, t1 in windows}
    cols = ["frames", "fps", "p50_ms", "p95_ms", "p99_ms", "low1_fps", "over33", "over50", "over100"]
    print("window  " + "  ".join(f"{c:>8}" for c in cols))
    for name, st in result.items():
        print(f"{name:7} " + "  ".join(f"{st.get(c, ''):>8}" for c in cols))
    if counters:
        print("\nper frame (mean): " + ", ".join(counters))
        for name, st in result.items():
            print(f"{name:7} " + "  ".join(f"{c}={st.get(c, '')}" for c in counters if c in st))
    if "--json" in sys.argv:
        Path(sys.argv[sys.argv.index("--json") + 1]).write_text(json.dumps(result, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
