# SecondSonPC FrameBreak FB-7: the machine's "slow mode" - each run of a p53_perf_ab series with its CPU clock, context
# switches and core loads (perf.csv, WMI once a second) and the game's CPU (cpu_mem.csv) over the frame window, next to
# its FPS and CP time.
#   py -I tools\framebreak\slow_mode.py logs\p53\<series> [--from 62 --to 88]
import argparse
import csv
import json
import statistics
import sys
from pathlib import Path


def perf_window(path, t0, t1):
    rows = [r for r in csv.DictReader(path.read_text(encoding="ascii").splitlines())
            if t0 <= float(r["t_s"]) <= t1]
    if not rows:
        return None
    cores = [[float(c) for c in r["cores_pct"].split()] for r in rows]
    per_core = [statistics.fmean(c[i] for c in cores) for i in range(min(len(c) for c in cores))]
    return {
        "perf_pct": statistics.fmean(float(r["perf_pct"]) for r in rows),
        "freq_mhz": statistics.fmean(float(r["freq_mhz"]) for r in rows),
        "ctx_per_s": statistics.fmean(float(r["ctx_per_s"]) for r in rows),
        "core_max": max(per_core), "core_min": min(per_core),
        "cores_over_80": sum(1 for c in per_core if c > 80),
    }


def game_cpu(path, t0, t1):
    rows = [r for r in csv.DictReader(path.read_text(encoding="ascii").splitlines()) if t0 <= float(r["t_s"]) <= t1]
    if len(rows) < 2:
        return float("nan")
    dt = float(rows[-1]["t_s"]) - float(rows[0]["t_s"])
    return 100.0 * (float(rows[-1]["cpu_s"]) - float(rows[0]["cpu_s"])) / dt if dt > 0 else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("series")
    ap.add_argument("--from", dest="t0", type=float, default=62)
    ap.add_argument("--to", dest="t1", type=float, default=88)
    a = ap.parse_args()
    root = Path(a.series)
    print("| run | scene | FPS | cp ms | rec ms | GPU ms | CPU perf % | MHz | ctx/s | game % of a core | core max / min % | cores >80 % |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for stats in sorted(root.glob("*/*_1/stats.json")):
        run, scene = stats.parent.parent.name, stats.parent.name[:-2]
        s = json.loads(stats.read_text(encoding="utf-8"))
        perf = stats.parent / "perf.csv"
        p = perf_window(perf, a.t0, a.t1) if perf.exists() else None
        pf = (lambda k: f"{p[k]:.0f}") if p else (lambda k: "-")
        cm = stats.parent / "cpu_mem.csv"
        g = game_cpu(cm, a.t0, a.t1) if cm.exists() else float("nan")
        print(f"| {run} | {scene} | {s.get('avg_fps', 0):.1f} | {s.get('mean_cp_ms', 0):.2f} | "
              f"{s.get('mean_record_ms', 0):.2f} | {s.get('mean_gpu_busy_ms', 0):.2f} | "
              f"{pf('perf_pct')} | {pf('freq_mhz')} | {pf('ctx_per_s')} | {g:.0f} | "
              f"{pf('core_max')} / {pf('core_min')} | {pf('cores_over_80')} |")


if __name__ == "__main__":
    sys.exit(main())
