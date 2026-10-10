"""P3.2 long-session report (docs/PHASE3_INDEPENDENT_RUNTIME.md §10): one run of tools/perf_runs.ps1 -Soak.

    python -I tools/soak_report.py <run dir> [--gameplay-from 100] [--window 300]

Reads <run dir>/samples.csv (tools/perf_sampler.ps1), frametimes.csv (--frame-log) and SecondSonPC.log; writes
<run dir>/soak_report.md and soak_report.json. Per window: FPS, frame time P50/P95/P99, 1 % low, CPU, GPU, private
and system commit, VRAM, threads, handles, access violations per second (the bridge's GPU write tracking). Growth:
least-squares slope of each memory series per 10 minutes over the gameplay (a steady leak shows as a steady
slope; the first window is the warm-up). Thread manager: the created/released lines over time and the session's
end lines. Read-only.
"""
import argparse
import csv
import json
import re
import statistics
from pathlib import Path

TS_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]")
AV_RE = re.compile(r"first-chance stats sample: access violations (\d+) \(\+(\d+) in ([\d.]+) s.*?not handled by the "
                   r"bridge/game (\d+)")
THREAD_RE = re.compile(r'thread (\d+) "([^"]*)" (created|released)(.*?)cached (\d+) \((\d+) KiB\)')
END_RE = re.compile(r"(native threads at exit|native memory at exit|process at exit|FS patcher at exit): (.*)")
BAD_RE = re.compile(r"\[(Critical|Error)\]|unhandled exception|Unhandled Exception|minidump", re.IGNORECASE)


def log_seconds(line: str):
    m = TS_RE.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000


def slope_per_10min(points):
    """Least-squares slope of (t_s, value) in value per 600 s; None with fewer than 10 points."""
    if len(points) < 10:
        return None
    n = len(points)
    mt = sum(t for t, _ in points) / n
    mv = sum(v for _, v in points) / n
    den = sum((t - mt) ** 2 for t, _ in points)
    if den == 0:
        return None
    return round(sum((t - mt) * (v - mv) for t, v in points) / den * 600, 2)


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("--gameplay-from", type=float, default=100.0)
    ap.add_argument("--window", type=float, default=300.0)
    a = ap.parse_args()
    run = a.run_dir

    samples = []
    with (run / "samples.csv").open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                r = {k: float(v) for k, v in row.items()}
            except (TypeError, ValueError):
                continue
            if r.get("threads", 0) > 1:  # the last row is taken while the process exits
                samples.append(r)
    frames = []
    with (run / "frametimes.csv").open(newline="") as f:
        for row in csv.DictReader(f):
            frames.append((float(row["t_s"]), float(row["frame_ms"])))
    end_t = max(t for t, _ in frames) if frames else max(r["t_s"] for r in samples)

    log_lines = (run / "SecondSonPC.log").read_text(encoding="utf-8", errors="replace").splitlines()
    t0 = None
    av = []  # (s since the log's first line, AV/s, not handled)
    threads = []
    end = {}
    bad = []
    for line in log_lines:
        s = log_seconds(line)
        if s is not None and t0 is None:
            t0 = s
        rel = (s - t0) if (s is not None and t0 is not None) else None
        if (m := AV_RE.search(line)) and rel is not None:
            av.append((rel, int(m.group(2)) / float(m.group(3)), int(m.group(4))))
        if (m := THREAD_RE.search(line)) and rel is not None:
            threads.append({"t_s": round(rel, 1), "tid": int(m.group(1)), "name": m.group(2), "event": m.group(3),
                            "cached": int(m.group(5)), "cache_kib": int(m.group(6))})
        if m := END_RE.search(line):
            end[m.group(1)] = m.group(2)
        if BAD_RE.search(line) and "first-chance stats sample" not in line:
            bad.append(line[:300])

    # Windows: the start scene, the foliage (the run's first minutes, as in the matrix), then gameplay windows.
    windows = [("start scene", 30.0, 55.0), ("foliage", 61.0, 98.0)]
    w0 = a.gameplay_from
    i = 0
    while w0 + 1 < end_t:
        w1 = min(w0 + a.window, end_t)
        i += 1
        windows.append((f"gameplay {i}", w0, w1))
        w0 = w1

    def window_stats(name, lo, hi):
        ft = [ms for t, ms in frames if lo <= t < hi]
        sm = [r for r in samples if lo <= r["t_s"] < hi]
        avw = [x for x in av if lo <= x[0] < hi]
        out = {"window": name, "from_s": round(lo), "to_s": round(hi), "frames": len(ft)}
        if ft:
            slow = sorted(ft)[-max(1, len(ft) // 100):]
            out.update({"fps": round(len(ft) / (hi - lo), 2), "p50_ms": round(pct(ft, 50), 2),
                        "p95_ms": round(pct(ft, 95), 2), "p99_ms": round(pct(ft, 99), 2),
                        "low1_fps": round(1000 / statistics.mean(slow), 2), "max_ms": round(max(ft), 2)})
        if sm:
            out.update({"cpu_pct": round(statistics.mean(r["cpu_pct"] for r in sm), 1),
                        "gpu_pct": round(statistics.mean(r["gpu3d_pct"] for r in sm), 1),
                        "private_mib_max": round(max(r["private_mib"] for r in sm)),
                        "sys_commit_mib_max": round(max(r["sys_commit_mib"] for r in sm)),
                        "vram_mib_max": round(max(r["gpu_dedicated_mib"] for r in sm)),
                        "threads_max": round(max(r["threads"] for r in sm)),
                        "handles_max": round(max(r.get("handles", 0) for r in sm))})
        if avw:
            out.update({"av_per_s": round(statistics.mean(x[1] for x in avw)),
                        "av_not_handled": avw[-1][2]})
        return out

    rows = [window_stats(*w) for w in windows]
    game = [r for r in samples if r["t_s"] >= a.gameplay_from + a.window]  # after the first gameplay window
    growth = {k: slope_per_10min([(r["t_s"], r[k]) for r in game])
              for k in ("private_mib", "ws_private_mib", "sys_commit_mib", "gpu_dedicated_mib", "threads", "handles")
              if game and k in game[0]}
    all_frames = [ms for t, ms in frames if t >= a.gameplay_from]
    summary = {
        "run": str(run), "duration_s": round(end_t), "gameplay_from_s": a.gameplay_from,
        "gameplay": {"frames": len(all_frames),
                     "fps": round(len(all_frames) / (end_t - a.gameplay_from), 2) if all_frames else None,
                     "p50_ms": round(pct(all_frames, 50), 2) if all_frames else None,
                     "p95_ms": round(pct(all_frames, 95), 2) if all_frames else None,
                     "p99_ms": round(pct(all_frames, 99), 2) if all_frames else None,
                     "frames_over_50ms": sum(1 for x in all_frames if x > 50),
                     "frames_over_100ms": sum(1 for x in all_frames if x > 100)},
        "growth_per_10min_after_warmup": growth,
        "thread_events": len(threads), "thread_events_list": threads[:200],
        "max_cached_stacks": max((x["cached"] for x in threads), default=0),
        "max_cache_kib": max((x["cache_kib"] for x in threads), default=0),
        "session_end": end, "errors": bad[:50], "error_count": len(bad), "windows": rows,
    }
    (run / "soak_report.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")

    md = [f"# Long session {run.name}", "",
          f"{summary['duration_s']} s; gameplay from {a.gameplay_from:g} s: {summary['gameplay']}", "",
          "| window | s | FPS | P50/P95/P99 ms | 1 % low | CPU % | GPU % | private max | sys commit max | VRAM max | "
          "threads | handles | AV/s |", "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        md.append(f"| {r['window']} | {r['from_s']}-{r['to_s']} | {r.get('fps', '-')} | "
                  f"{r.get('p50_ms', '-')}/{r.get('p95_ms', '-')}/{r.get('p99_ms', '-')} | {r.get('low1_fps', '-')} | "
                  f"{r.get('cpu_pct', '-')} | {r.get('gpu_pct', '-')} | {r.get('private_mib_max', '-')} | "
                  f"{r.get('sys_commit_mib_max', '-')} | {r.get('vram_mib_max', '-')} | {r.get('threads_max', '-')} | "
                  f"{r.get('handles_max', '-')} | {r.get('av_per_s', '-')} |")
    md += ["", f"Growth per 10 min after the warm-up window (least squares): {growth}", "",
           f"Thread manager: {len(threads)} created/released lines, cache at most {summary['max_cached_stacks']} "
           f"stacks / {summary['max_cache_kib']} KiB", ""]
    md += [f"- {k}: {v}" for k, v in end.items()]
    md += ["", f"Error/critical lines: {len(bad)}"] + [f"    {x}" for x in bad[:20]]
    (run / "soak_report.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("\n".join(md))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
