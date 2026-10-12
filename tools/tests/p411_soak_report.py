# SecondSonPC P4.11: the report of one soak run (tools\tests\p411_soak.ps1) from its folder.
#   py -I tools\tests\p411_soak_report.py <run folder> [--window 120] [--from 40]
# gpu_frames.csv: per window of --window s from --from s (the backend's time) - presented frames' FPS (average, 1% low,
# p95/p99 frame time, frames over 75 ms), the GPU thread's and the GPU's per-frame means (as tools\frame_stats.py).
# SecondSonPC.log: VRAM and commit samples (--vram-log), the backend's "soak:" lines - first/last/max and a least-squares
# slope per minute over the play; errors. run.json: how the process ended. gpu_native.txt: STATUS. Writes soak.json too.
import csv
import json
import math
import os
import re
import sys


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))]


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def slope(points):
    """Least-squares slope (per minute) of [(t_s, value)]."""
    if len(points) < 3:
        return float("nan")
    n = len(points)
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mx) ** 2 for p in points)
    if sxx == 0:
        return float("nan")
    return sum((p[0] - mx) * (p[1] - my) for p in points) / sxx * 60.0


def frames_window(rows, lo, hi):
    pres = [r["present_ms"] for r in rows if r["present_ms"] and lo <= r["present_ms"] <= hi]
    ft = [b - a for a, b in zip(pres, pres[1:])]
    res = {"frames": len(pres)}
    if len(ft) >= 2:
        span = pres[-1] - pres[0]
        worst = sorted(ft, reverse=True)[: max(1, len(ft) // 100)]
        res.update({"avg_fps": (len(pres) - 1) * 1000.0 / span, "low1_fps": 1000.0 / mean(worst),
                    "p95_ms": pct(ft, 0.95), "p99_ms": pct(ft, 0.99), "max_ms": max(ft),
                    "over75": sum(1 for d in ft if d >= 75.0), "v3": sum(1 for d in ft if 41.7 <= d < 58.4)})
    fr = [r for r in rows if r["cp_flip_ms"] and lo <= r["cp_flip_ms"] <= hi and r["cp_ms"] is not None]
    for k in ("cp_ms", "gpu_busy_ms", "par_wait_ms", "par_busy_ms", "draws", "dispatches"):
        v = [r[k] for r in fr if r.get(k) is not None]
        res[k] = mean(v)
    for k in ("shaders", "pipelines"):
        res[k] = sum(r[k] or 0 for r in fr)
    return res


def main(argv):
    folder = argv[1]
    window, start = 120.0, 40.0
    i = 2
    while i < len(argv):
        if argv[i] == "--window":
            window = float(argv[i + 1]); i += 2
        elif argv[i] == "--from":
            start = float(argv[i + 1]); i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    out = {"folder": folder}
    rows = []
    path = os.path.join(folder, "gpu_frames.csv")
    if os.path.exists(path):
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                rows.append({k: (float(v) if v not in ("", None) else None) for k, v in r.items()})
    end_ms = max((r["present_ms"] for r in rows if r["present_ms"]), default=0.0)
    out["play_s"] = [start, round(end_ms / 1000.0, 1)]
    out["all"] = frames_window(rows, start * 1000.0, end_ms)
    wins = []
    t = start
    while t * 1000.0 < end_ms:
        w = frames_window(rows, t * 1000.0, min((t + window) * 1000.0, end_ms))
        w["from_s"] = t
        wins.append(w)
        t += window
    out["windows"] = wins

    # the log: samples over the play
    log = os.path.join(folder, "SecondSonPC.log")
    series = {k: [] for k in ("private_mib", "working_set_mib", "vram_local_mib", "vram_nonlocal_mib", "handles",
                              "batches", "images", "image_mib", "buffers_mib", "guest_windows", "guest_mib",
                              "pipelines", "modules", "frontend_mib", "tw_pending", "gw_now", "depth_mean",
                              "drains_frame", "t1_wait_ms", "t2_exec_ms", "cpu_t1_pct", "cpu_t2_pct",
                              "cpu_process_pct")}
    errors, lost, first_ts = [], 0, None

    def ts(line):
        m = re.match(r"\[(\d+):(\d+):(\d+)\.(\d+)\]", line)
        if not m:
            return None
        h, mi, s, ms = (int(x) for x in m.groups())
        return h * 3600 + mi * 60 + s + ms / 1000.0

    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            for line in f:
                t = ts(line)
                if t is None:
                    continue
                if first_ts is None:
                    first_ts = t
                rel = t - first_ts
                if rel < 0:
                    rel += 86400.0  # (past midnight)
                if "[Error]" in line and len(errors) < 50:
                    errors.append(line.strip()[:300])
                if "DEVICE_LOST" in line or "device is lost" in line:
                    lost += 1
                m = re.search(r"VRAM sample: local usage (\d+) MiB.*non-local usage (\d+) MiB", line)
                if m:
                    series["vram_local_mib"].append((rel, int(m.group(1))))
                    series["vram_nonlocal_mib"].append((rel, int(m.group(2))))
                m = re.search(r"commit sample: process private (\d+) MiB.*working set (\d+) MiB", line)
                if m:
                    series["private_mib"].append((rel, int(m.group(1))))
                    series["working_set_mib"].append((rel, int(m.group(2))))
                m = re.search(r"soak: handles (\d+); batches (\d+), images (\d+) \((\d+) MiB, created \d+\), buffers "
                              r"resident (\d+) MiB, guest windows (\d+) \((\d+) MiB\), pipelines (\d+) graphics \+ (\d+) "
                              r"compute, modules (\d+), frontend (\d+) MiB, timeline writes pending (\d+)", line)
                if m:
                    g = [int(x) for x in m.groups()]
                    for k, v in zip(("handles", "batches", "images", "image_mib", "buffers_mib", "guest_windows",
                                     "guest_mib"), g[:7]):
                        series[k].append((rel, v))
                    series["pipelines"].append((rel, g[7] + g[8]))
                    series["modules"].append((rel, g[9]))
                    series["frontend_mib"].append((rel, g[10]))
                    series["tw_pending"].append((rel, g[11]))
                    m2 = re.search(r"depth mean ([\d.]+) .*drains that waited ([\d.]+)/frame, thread 1 waited ([\d.]+) "
                                   r"ms/frame, thread 2 executing ([\d.]+) ms/frame, GPU write ranges (\d+)", line)
                    if m2:
                        series["depth_mean"].append((rel, float(m2.group(1))))
                        series["drains_frame"].append((rel, float(m2.group(2))))
                        series["t1_wait_ms"].append((rel, float(m2.group(3))))
                        series["t2_exec_ms"].append((rel, float(m2.group(4))))
                        series["gw_now"].append((rel, int(m2.group(5))))
                    m3 = re.search(r"GPU thread (\d+), recording thread (\d+), process (\d+)", line)
                    if m3:
                        series["cpu_t1_pct"].append((rel, int(m3.group(1))))
                        series["cpu_t2_pct"].append((rel, int(m3.group(2))))
                        series["cpu_process_pct"].append((rel, int(m3.group(3))))
    trend = {}
    for k, pts in series.items():
        play = [p for p in pts if p[0] >= start + 20.0]  # (the level loaded)
        if not play:
            continue
        trend[k] = {"first": play[0][1], "last": play[-1][1], "min": min(p[1] for p in play),
                    "max": max(p[1] for p in play), "per_min": round(slope(play), 3), "samples": len(play)}
    out["trend"] = trend
    out["errors"] = errors
    out["device_lost_lines"] = lost
    rj = os.path.join(folder, "run.json")
    if os.path.exists(rj):
        with open(rj, encoding="utf-8-sig") as f:
            run = json.load(f)
        out["run"] = {k: run.get(k) for k in ("how", "exit_code", "exit_hex", "lifetime_s", "action")}
    gn = os.path.join(folder, "gpu_native.txt")
    if os.path.exists(gn):
        with open(gn, encoding="utf-8", errors="replace") as f:
            out["status"] = next((l.strip() for l in f if l.startswith("STATUS")), None)
    vv = os.path.join(folder, "vk_validation.log")
    if os.path.exists(vv):
        with open(vv, encoding="utf-8", errors="replace") as f:
            out["validation_lines"] = sum(1 for _ in f)

    def rnd(o):
        if isinstance(o, float):
            return round(o, 2)
        if isinstance(o, dict):
            return {k: rnd(v) for k, v in o.items()}
        if isinstance(o, list):
            return [rnd(v) for v in o]
        return o

    out = rnd(out)
    with open(os.path.join(folder, "soak.json"), "w") as f:
        json.dump(out, f, indent=1)
    a = out["all"]
    print(f"play {out['play_s'][0]}..{out['play_s'][1]} s: frames {a.get('frames')}, avg {a.get('avg_fps')} FPS, 1% low "
          f"{a.get('low1_fps')}, p95 {a.get('p95_ms')} ms, p99 {a.get('p99_ms')} ms, max {a.get('max_ms')} ms, frames "
          f">=75 ms {a.get('over75')}; CP {a.get('cp_ms')} ms, GPU {a.get('gpu_busy_ms')} ms, thread 1 waited "
          f"{a.get('par_wait_ms')} ms; shaders {a.get('shaders')}, pipelines {a.get('pipelines')}")
    print("windows (from s: avg FPS / 1% low / p99 ms / CP / GPU / >=75 ms):")
    for w in out["windows"]:
        print(f"  {w['from_s']:>6}: {w.get('avg_fps')} / {w.get('low1_fps')} / {w.get('p99_ms')} / {w.get('cp_ms')} / "
              f"{w.get('gpu_busy_ms')} / {w.get('over75')}")
    print("trend (first -> last, max, slope per minute):")
    for k, v in trend.items():
        print(f"  {k}: {v['first']} -> {v['last']} (max {v['max']}, {v['per_min']}/min, {v['samples']} samples)")
    print(f"run: {out.get('run')}; status: {out.get('status')}; [Error] lines {len(errors)}; device lost lines {lost}; "
          f"validation lines {out.get('validation_lines', 'n/a')}")
    for e in errors[:10]:
        print("  " + e)


if __name__ == "__main__":
    main(sys.argv)
