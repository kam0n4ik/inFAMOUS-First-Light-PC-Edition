# SecondSonPC FrameBreak FB-7: the machine's "slow mode" - each run of a p53_perf_ab series with its CPU clock, context
# switches and core loads (perf.csv, typeperf once a second) over the frame window, next to its FPS and CP time.
#   py -I tools\framebreak\slow_mode.py logs\p53\<series> [--from 62 --to 88]
import argparse
import csv
import json
import statistics
import sys
from pathlib import Path


def perf_window(path, t0, t1):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rows = list(csv.reader(f))
    if len(rows) < 3:
        return None
    head = rows[0]
    cols = {}
    for i, h in enumerate(head):
        cols.setdefault("perf" if h.endswith("(_Total)\\% Processor Performance") else
                        "freq" if h.endswith("(_Total)\\Processor Frequency") else
                        "ctx" if h.endswith("\\Context Switches/sec") else
                        "game" if h.endswith("Process(SecondSonPC)\\% Processor Time") else
                        "core" if "% Processor Time" in h and "Processor Information(" in h and "_Total" not in h
                        else "other", []).append(i)
    data = rows[1 + int(t0):1 + int(t1)]  # (one row a second from the run's start; the game starts ~2 s later)

    def values(i):
        out = []
        for r in data:
            try:
                out.append(float(r[i]))
            except (ValueError, IndexError):
                pass
        return out

    def mean(key):
        vals = [v for i in cols.get(key, []) for v in values(i)]
        return statistics.fmean(vals) if vals else float("nan")

    core_means = [statistics.fmean(v) for v in (values(i) for i in cols.get("core", [])) if v]
    return {
        "perf_pct": mean("perf"), "freq_mhz": mean("freq"), "ctx_per_s": mean("ctx"), "game_pct": mean("game"),
        "core_max": max(core_means) if core_means else float("nan"),
        "core_min": min(core_means) if core_means else float("nan"),
        "cores_over_80": sum(1 for c in core_means if c > 80),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("series")
    ap.add_argument("--from", dest="t0", type=float, default=62)
    ap.add_argument("--to", dest="t1", type=float, default=88)
    a = ap.parse_args()
    root = Path(a.series)
    print("| run | scene | FPS | cp ms | rec ms | GPU ms | CPU perf % | MHz | ctx/s | game % | core max / min % | cores >80 % |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for stats in sorted(root.glob("*/*_1/stats.json")):
        run, scene = stats.parent.parent.name, stats.parent.name[:-2]
        s = json.loads(stats.read_text(encoding="utf-8"))
        perf = stats.parent / "perf.csv"
        p = perf_window(perf, a.t0, a.t1) if perf.exists() else None
        pf = (lambda k: f"{p[k]:.0f}") if p else (lambda k: "-")
        print(f"| {run} | {scene} | {s.get('avg_fps', 0):.1f} | {s.get('mean_cp_ms', 0):.2f} | "
              f"{s.get('mean_record_ms', 0):.2f} | {s.get('mean_gpu_busy_ms', 0):.2f} | "
              f"{pf('perf_pct')} | {pf('freq_mhz')} | {pf('ctx_per_s')} | {pf('game_pct')} | "
              f"{pf('core_max')} / {pf('core_min')} | {pf('cores_over_80')} |")


if __name__ == "__main__":
    sys.exit(main())
