"""Report of one tools/perf_runs.ps1 session (logs\\performance\\<session>): report.json/.csv/.md.

    python -I tools/perf_report.py <session_dir>

Per mode (sub-folder A/B/C/D or any other with summary.json):
  - FPS from the exact flip count (sceGnmSubmitAndFlipCommandBuffers calls, logged every
    [Trace] dumpIntervalSeconds) per window, split into loading (< --gameplay-from s) and gameplay;
  - per-frame times when the run wrote frametimes.csv (in-process recorder), else marked "n/a";
  - tools/perf_sampler.ps1 samples (samples.csv): CPU, threads, memory, commit, GPU.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
import sys
from pathlib import Path


def windows(series: list[list[float]]) -> list[tuple[float, float, float]]:
    """(start s, end s, fps) of every interval between two frame-count samples."""
    out = []
    for (t0, f0), (t1, f1) in zip(series, series[1:]):
        if t1 > t0:
            out.append((t0, t1, (f1 - f0) / (t1 - t0)))
    return out


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def frame_stats(path: Path, start_s: float) -> dict:
    """frametimes.csv: t_s,frame_ms (one row per presented frame)."""
    if not path.exists():
        return {}
    ms = []
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            if float(row["t_s"]) >= start_s:
                ms.append(float(row["frame_ms"]))
    if not ms:
        return {}
    fps = [1000.0 / m for m in ms if m > 0]
    worst = sorted(ms, reverse=True)[: max(1, len(ms) // 100)]
    return {
        "frames": len(ms),
        "fps_avg": round(len(ms) / (sum(ms) / 1000.0), 2),
        "fps_median": round(statistics.median(fps), 2),
        "fps_1pct_low": round(1000.0 / statistics.mean(worst), 2),
        "frametime_p50_ms": round(percentile(ms, 50), 2),
        "frametime_p95_ms": round(percentile(ms, 95), 2),
        "frametime_p99_ms": round(percentile(ms, 99), 2),
        "frametime_max_ms": round(max(ms), 2),
        "frames_over_33ms": sum(1 for m in ms if m > 1000.0 / 30.0),
        "frames_over_50ms": sum(1 for m in ms if m > 50.0),
        "frames_over_100ms": sum(1 for m in ms if m > 100.0),
    }


# The bridge's compile statistics (patch 0013, vk_pipeline_serialization.cpp LogStats): the last line.
SC_STATS_RE = re.compile(
    r"Shader cache stats \(([^)]*)\): compiled modules (\d+) \+ permutations (\d+), graphics (\d+), "
    r"compute (\d+); preloaded modules (\d+), graphics (\d+), compute (\d+), stale (\d+), used (\d+); "
    r"ms translate ([\d.]+) spirv ([\d.]+) module ([\d.]+) graphics ([\d.]+) compute ([\d.]+); "
    r"stalls (\d+) \(([\d.]+) ms, >5ms (\d+), >20ms (\d+), >100ms (\d+), max ([\d.]+) ms\); "
    r"warm-up ([\d.]+) ms; driver cache (\d+) bytes; persistent cache (\w+)")
SC_SIZE_RE = re.compile(r"Shader cache: (.+) \(generation ([^)]+)\): (\d+) files, ([\d.]+) MiB")
SC_DUMPED_RE = re.compile(r"Cache dumped: written (\d+) blobs \(([\d.]+) MiB")


def shader_cache_stats(log: Path) -> dict:
    if not log.exists():
        return {}
    keys = ["when", "modules", "permutations", "graphics", "compute", "preloaded_modules",
            "preloaded_graphics", "preloaded_compute", "stale", "preloaded_used", "translate_ms",
            "spirv_ms", "module_ms", "graphics_ms", "compute_ms", "stalls", "stall_ms", "stalls_5ms",
            "stalls_20ms", "stalls_100ms", "stall_max_ms", "warmup_ms", "driver_cache_bytes",
            "persistent"]
    out: dict = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = SC_STATS_RE.search(line)
        if m:
            vals = list(m.groups())
            out.update({k: (v if k in ("when", "persistent") else float(v)) for k, v in zip(keys, vals)})
        s = SC_SIZE_RE.search(line)
        if s:
            out.update({"dir": s.group(1), "generation": s.group(2), "files_at_start": int(s.group(3)),
                        "mib_at_start": float(s.group(4))})
        d = SC_DUMPED_RE.search(line)
        if d:
            out.update({"blobs_written": int(d.group(1)), "mib_written": float(d.group(2))})
    return out


def sample_stats(path: Path, start_s: float) -> dict:
    if not path.exists():
        return {}
    rows = []
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                r = {k: float(v) for k, v in row.items()}
            except (TypeError, ValueError):
                continue
            if r["threads"] > 0:  # the process exited during this sample
                rows.append(r)
    game = [r for r in rows if r["t_s"] >= start_s] or rows
    if not game:
        return {}

    def avg(key: str) -> float:
        return round(statistics.mean(r[key] for r in game), 1)

    def peak(key: str) -> float:
        return round(max(r[key] for r in rows), 1)

    return {
        "samples": len(rows),
        "cpu_pct_avg": avg("cpu_pct"),
        "cpu_total_pct_avg": avg("cpu_total_pct"),
        "threads_avg": avg("threads"),
        "threads_max": peak("threads"),
        "private_mib_max": peak("private_mib"),
        "ws_private_mib_max": peak("ws_private_mib"),
        "virtual_mib_max": peak("virtual_mib"),
        "sys_commit_mib_max": peak("sys_commit_mib"),
        "sys_commit_limit_mib": round(rows[-1]["sys_commit_limit_mib"]),
        "avail_mib_min": round(min(r["avail_mib"] for r in rows)),
        "hard_faults_s_avg": avg("hard_faults_s"),
        "ctx_switch_s_avg": avg("ctx_switch_s"),
        "gpu3d_pct_avg": avg("gpu3d_pct"),
        "gpu_dedicated_mib_max": peak("gpu_dedicated_mib"),
        "gpu_shared_mib_max": peak("gpu_shared_mib"),
        # P3.2 (samples.csv has had the column since then): the process's handles at the start of the
        # gameplay window, at its end and the most.
        **({"handles_start": round(game[0]["handles"]), "handles_end": round(game[-1]["handles"]),
            "handles_max": round(max(r["handles"] for r in rows))} if "handles" in game[0] else {}),
    }


TS_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]")
VEH_RE = re.compile(r"first-chance stats sample: access violations (\d+) \(\+(\d+) in ([\d.]+) s")
VEH_QUERY_RE = re.compile(r"VirtualQuery (\d+) \(\+(\d+)\), query time ([\d.]+) ms \(\+([\d.]+) ms")
VEH_LATE_RE = re.compile(r"not handled by the bridge/game (\d+) \(\+(\d+)")
VEH_MAX_RE = re.compile(r"max (\d+) us")


def veh_stats(log: Path, start_s: float) -> dict:
    """Access violations per second and VirtualQuery cost from the 'first-chance stats' lines."""
    if not log.exists():
        return {}
    t0 = None
    rows = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = TS_RE.match(line)
        if not m:
            continue
        h, mi, s, ms = map(int, m.groups())
        t = h * 3600 + mi * 60 + s + ms / 1000.0
        if t0 is None:
            t0 = t
        if "recorder off (--no-fault-recorder)" in line:
            return {"recorder": "off"}
        v = VEH_RE.search(line)
        if not v or float(v.group(3)) <= 0:
            continue
        q = VEH_QUERY_RE.search(line)
        late = VEH_LATE_RE.search(line)
        mx = VEH_MAX_RE.search(line)
        dt = float(v.group(3))
        rows.append({
            "t": (t - t0) % 86400, "total": int(v.group(1)), "rate": int(v.group(2)) / dt,
            "late_rate": int(late.group(2)) / dt if late else None,
            "queries": int(q.group(1)) if q else 0, "query_ms": float(q.group(3)) if q else 0.0,
            "query_pct_core": float(q.group(4)) / (dt * 10.0) if q else 0.0,
            "max_us": int(mx.group(1)) if mx else None,
        })
    if not rows:
        return {}
    play = [r for r in rows if r["t"] >= start_s] or rows
    last = rows[-1]
    return {
        "access_violations_total": last["total"],
        "access_violations_per_s_gameplay_avg": round(statistics.mean(r["rate"] for r in play), 1),
        "access_violations_per_s_max": round(max(r["rate"] for r in rows), 1),
        "not_handled_by_bridge_per_s_gameplay_avg":
            round(statistics.mean(r["late_rate"] for r in play), 2) if play[0]["late_rate"] is not None else None,
        "virtualquery_calls": last["queries"],
        "virtualquery_ms_total": round(last["query_ms"], 1),
        "virtualquery_pct_of_one_core_gameplay_avg": round(statistics.mean(r["query_pct_core"] for r in play), 2),
        "virtualquery_max_us": last["max_us"],
    }


def mode_report(mode_dir: Path, gameplay_from: float) -> dict:
    summary = json.loads((mode_dir / "summary.json").read_text(encoding="utf-8-sig"))
    run = {}
    if (mode_dir / "run.json").exists():
        run = json.loads((mode_dir / "run.json").read_text(encoding="utf-8-sig"))
    w = windows(summary.get("frames_series") or [])
    load = [x for x in w if x[1] <= gameplay_from]
    play = [x for x in w if x[0] >= gameplay_from]
    play_frames = sum((t1 - t0) * fps for t0, t1, fps in play)
    play_time = sum(t1 - t0 for t0, t1, _ in play)
    r = {
        "mode": mode_dir.name,
        "args": run.get("args"),
        "how": run.get("how"),
        "exit_hex": run.get("exit_hex"),
        "frames_total": summary.get("frames"),
        "fps_windows": [[round(t0, 1), round(t1, 1), round(f, 2)] for t0, t1, f in w],
        "fps_loading_avg": round(statistics.mean(f for *_, f in load), 2) if load else None,
        "fps_gameplay_avg": round(play_frames / play_time, 2) if play_time else None,
        "fps_gameplay_window_median": round(statistics.median(f for *_, f in play), 2) if play else None,
        "fps_gameplay_window_min": round(min(f for *_, f in play), 2) if play else None,
        "first_chance_faults": summary.get("first_chance_faults"),
        "crash": summary.get("crash"),
        "alive_at_end": summary.get("alive_at_end"),
        "binding_table": summary.get("binding_table"),
        "commit_max_mib_log": summary.get("commit_max_mib"),
        "process_private_max_mib_log": summary.get("process_private_max_mib"),
        "vram_local_max_mib": summary.get("vram_local_max_mib"),
        "pipelines": summary.get("pipelines"),
        "errors_own": summary.get("errors_own"),
        "errors_bridge": summary.get("errors_bridge"),
    }
    r["frametimes"] = frame_stats(mode_dir / "frametimes.csv", gameplay_from)
    r["frametimes_all"] = frame_stats(mode_dir / "frametimes.csv", 0.0)  # loading included
    r["shader_cache"] = shader_cache_stats(mode_dir / "bridge_shadps4.log")
    r["process"] = sample_stats(mode_dir / "samples.csv", gameplay_from)
    r["veh"] = veh_stats(mode_dir / "SecondSonPC.log", gameplay_from)
    return r


def fmt(v) -> str:
    return "n/a" if v is None or v == {} else str(v)


RUN_SUFFIX_RE = re.compile(r"_\d+$")


def by_mode(modes: list[dict]) -> list[dict]:
    """Runs of the same mode (M0_1, M0_2 -> M0): mean and spread between the runs."""
    groups: dict[str, list[dict]] = {}
    for m in modes:
        groups.setdefault(RUN_SUFFIX_RE.sub("", m["mode"]), []).append(m)
    out = []
    for name, runs in groups.items():
        fps = [r["fps_gameplay_avg"] for r in runs if r["fps_gameplay_avg"] is not None]
        ft = [r["frametimes"] for r in runs if r["frametimes"]]
        pr = [r["process"] for r in runs if r["process"]]

        def mean(values: list[float]) -> float | None:
            return round(statistics.mean(values), 2) if values else None

        def peak(values: list[float]) -> float | None:
            return max(values) if values else None

        out.append({
            "mode": name,
            "runs": [r["mode"] for r in runs],
            "how": [r["how"] for r in runs],
            "fps_gameplay_mean": mean(fps),
            "fps_gameplay_min": min(fps) if fps else None,
            "fps_gameplay_max": max(fps) if fps else None,
            "fps_gameplay_spread": round(max(fps) - min(fps), 2) if fps else None,
            "fps_gameplay_stdev": round(statistics.stdev(fps), 2) if len(fps) > 1 else None,
            "frametime_p50_ms_mean": mean([f["frametime_p50_ms"] for f in ft]),
            "frametime_p95_ms_mean": mean([f["frametime_p95_ms"] for f in ft]),
            "frametime_p99_ms_mean": mean([f["frametime_p99_ms"] for f in ft]),
            "fps_1pct_low_mean": mean([f["fps_1pct_low"] for f in ft]),
            "cpu_pct_mean": mean([p["cpu_pct_avg"] for p in pr]),
            "private_mib_max": peak([p["private_mib_max"] for p in pr]),
            "ws_private_mib_max": peak([p["ws_private_mib_max"] for p in pr]),
            "sys_commit_mib_max": peak([p["sys_commit_mib_max"] for p in pr]),
            "gpu_dedicated_mib_max": peak([p["gpu_dedicated_mib_max"] for p in pr]),
            "first_chance_faults": sum(r["first_chance_faults"] or 0 for r in runs),
        })
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("session_dir", type=Path)
    ap.add_argument("--gameplay-from", type=float, default=30.0,
                    help="seconds after start where gameplay begins (default 30: Continue at 12 s and 22 s)")
    a = ap.parse_args()
    d: Path = a.session_dir
    session = {}
    if (d / "session.json").exists():
        session = json.loads((d / "session.json").read_text(encoding="utf-8-sig"))
    modes = [mode_report(p, a.gameplay_from) for p in sorted(d.iterdir()) if (p / "summary.json").exists()]
    grouped = by_mode(modes)
    report = {"session": session, "gameplay_from_s": a.gameplay_from, "modes": modes, "by_mode": grouped}
    (d / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")

    cols = ["mode", "how", "fps_loading_avg", "fps_gameplay_avg", "fps_gameplay_window_median",
            "fps_gameplay_window_min", "first_chance_faults", "commit_max_mib_log", "vram_local_max_mib"]
    pcols = ["cpu_pct_avg", "cpu_total_pct_avg", "threads_avg", "private_mib_max", "sys_commit_mib_max",
             "sys_commit_limit_mib", "hard_faults_s_avg", "ctx_switch_s_avg", "gpu3d_pct_avg", "gpu_dedicated_mib_max"]
    fcols = ["fps_avg", "fps_median", "fps_1pct_low", "frametime_p50_ms", "frametime_p95_ms", "frametime_p99_ms"]
    vcols = ["access_violations_per_s_gameplay_avg", "not_handled_by_bridge_per_s_gameplay_avg",
             "virtualquery_pct_of_one_core_gameplay_avg", "virtualquery_max_us"]
    with (d / "report.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols + pcols + ["ft_" + c for c in fcols] + ["veh_" + c for c in vcols])
        for m in modes:
            w.writerow([m.get(c) for c in cols] + [m["process"].get(c) for c in pcols] +
                       [m["frametimes"].get(c) for c in fcols] + [m["veh"].get(c) for c in vcols])

    lines = [f"# Performance session {session.get('session', d.name)}", ""]
    if session:
        lines += [f"exe {session.get('exe_time')} ({session.get('git_head')}{' dirty' if session.get('git_dirty') else ''}), "
                  f"{session.get('seconds')} s per run, presses `{session.get('press')}`, "
                  f"free commit at start {session.get('free_commit_gib_at_start')} / {session.get('commit_limit_gib')} GiB", ""]
    lines += ["| mode | args | how | FPS loading | FPS gameplay avg | window median | window min | "
              "CPU % (1 core) | threads | private max MiB | sys commit max MiB | GPU 3D % | VRAM proc MiB | first-chance |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for m in modes:
        p = m["process"]
        args = (m.get("args") or "").split("--press")[-1]
        extra = " ".join(x for x in args.split() if x.startswith("--") or x.startswith("kernel")) or "native"
        lines.append(f"| {m['mode']} | {extra} | {m['how']} | {fmt(m['fps_loading_avg'])} | {fmt(m['fps_gameplay_avg'])} | "
                     f"{fmt(m['fps_gameplay_window_median'])} | {fmt(m['fps_gameplay_window_min'])} | "
                     f"{fmt(p.get('cpu_pct_avg'))} | {fmt(p.get('threads_avg'))} | {fmt(p.get('private_mib_max'))} | "
                     f"{fmt(p.get('sys_commit_mib_max'))} / {fmt(p.get('sys_commit_limit_mib'))} | {fmt(p.get('gpu3d_pct_avg'))} | "
                     f"{fmt(p.get('gpu_dedicated_mib_max'))} | {fmt(m['first_chance_faults'])} |")
    lines += ["", "FPS = exact flip count (sceGnmSubmitAndFlipCommandBuffers) per sampling window; "
              f"gameplay = windows starting at >= {a.gameplay_from:g} s.", ""]
    for m in modes:
        ft = m["frametimes"]
        lines.append(f"- {m['mode']}: windows " + ", ".join(f"{t0:g}-{t1:g}s {f:g}" for t0, t1, f in m["fps_windows"]))
        if ft:
            lines.append(f"  frametimes (gameplay): avg {ft['fps_avg']} FPS, median {ft['fps_median']}, 1% low "
                         f"{ft['fps_1pct_low']}, P50/P95/P99 {ft['frametime_p50_ms']}/{ft['frametime_p95_ms']}/"
                         f"{ft['frametime_p99_ms']} ms, max {ft['frametime_max_ms']} ms, frames >33.3/50/100 ms "
                         f"{ft['frames_over_33ms']}/{ft['frames_over_50ms']}/{ft['frames_over_100ms']}")
        fa = m.get("frametimes_all") or {}
        if fa:
            lines.append(f"  frametimes (whole run, loading included): {fa['frames']} frames, P99 "
                         f"{fa['frametime_p99_ms']} ms, max {fa['frametime_max_ms']} ms, frames >33.3/50/100 ms "
                         f"{fa['frames_over_33ms']}/{fa['frames_over_50ms']}/{fa['frames_over_100ms']}")
        sc = m.get("shader_cache") or {}
        if sc.get("when"):
            lines.append(
                f"  shader cache ({sc['persistent']}, {sc.get('files_at_start', 'n/a')} files / "
                f"{sc.get('mib_at_start', 'n/a')} MiB at start, written {sc.get('blobs_written', 0)} blobs / "
                f"{sc.get('mib_written', 0)} MiB): compiled modules {int(sc['modules'])}+{int(sc['permutations'])} perm, "
                f"pipelines {int(sc['graphics'])} gfx + {int(sc['compute'])} cs (misses); preloaded "
                f"{int(sc['preloaded_graphics'])} gfx + {int(sc['preloaded_compute'])} cs, used {int(sc['preloaded_used'])} "
                f"(hits), stale {int(sc['stale'])}; GPU thread stalled {sc['stall_ms']:.0f} ms in {int(sc['stalls'])} "
                f"compiles (>5/20/100 ms {int(sc['stalls_5ms'])}/{int(sc['stalls_20ms'])}/{int(sc['stalls_100ms'])}, "
                f"max {sc['stall_max_ms']} ms); translate {sc['translate_ms']:.0f} + spirv {sc['spirv_ms']:.0f} + module "
                f"{sc['module_ms']:.0f} + pipelines {sc['graphics_ms'] + sc['compute_ms']:.0f} ms; warm-up "
                f"{sc['warmup_ms']:.0f} ms; driver cache {int(sc['driver_cache_bytes']) // 1024} KiB loaded")
        if m.get("veh"):
            lines.append(f"  VEH: {json.dumps(m['veh'])}")
        if m.get("crash"):
            lines.append(f"  crash/first-chance: {m['crash']}")
    if any(len(g["runs"]) > 1 for g in grouped):
        lines += ["", "## By mode (runs of the same mode together)", "",
                  "| mode | runs | FPS gameplay mean | min..max (spread) | stdev | P50/P95/P99 ms (mean) | "
                  "1% low | CPU % | private max MiB | WS private max MiB | sys commit max MiB | VRAM proc MiB | "
                  "first-chance | how |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for g in grouped:
            lines.append(
                f"| {g['mode']} | {len(g['runs'])} | {fmt(g['fps_gameplay_mean'])} | {fmt(g['fps_gameplay_min'])}.."
                f"{fmt(g['fps_gameplay_max'])} ({fmt(g['fps_gameplay_spread'])}) | {fmt(g['fps_gameplay_stdev'])} | "
                f"{fmt(g['frametime_p50_ms_mean'])}/{fmt(g['frametime_p95_ms_mean'])}/{fmt(g['frametime_p99_ms_mean'])} | "
                f"{fmt(g['fps_1pct_low_mean'])} | {fmt(g['cpu_pct_mean'])} | {fmt(g['private_mib_max'])} | "
                f"{fmt(g['ws_private_mib_max'])} | {fmt(g['sys_commit_mib_max'])} | {fmt(g['gpu_dedicated_mib_max'])} | "
                f"{g['first_chance_faults']} | {', '.join(str(h) for h in g['how'])} |")
    (d / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
