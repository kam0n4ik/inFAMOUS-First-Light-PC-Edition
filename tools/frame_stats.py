# SecondSonPC P4.6: frame statistics of one native-backend run from its logs\gpu_frames.csv (src/runtime/gpu/frame_log.*).
#   py -I tools\frame_stats.py <gpu_frames.csv> [--from 62] [--to 88] [--json out.json]
# The window is in seconds of the backend's time (it starts with the process). Presented frames whose present time is
# inside the window give the FPS figures (intervals between consecutive presents, vblank-quantized as the player sees
# them); frames whose flip marker is inside the window give the GPU thread's and the GPU's per-frame figures.
# Definitions: average FPS = (frames - 1) / span; median FPS = 1000 / median frame time; 1% low FPS = 1000 / mean of the
# slowest 1% frame times (at least one frame); p95/p99 = frame time percentiles (nearest rank).
# FrameBreak (docs/FRAMEBREAK.md): every stream is reported under both 1% low definitions - "low1_fps" (mean of the
# worst 1 %) and "low1_p99_fps" (1000 / p99) - and 0.1 % low, p99.9 and the shares of frames under / over fixed budgets.
# Streams: "present" = intervals between presents (what the display was given; vblank-quantized in the console
# pacing), "engine" = intervals between the game's flip requests (its frame loop), "render" = intervals between the
# command processor's flip markers (a frame's commands processed).
import csv
import json
import math
import sys


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))]


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def stream(ft, prefix):
    """Frame time statistics of one stream of intervals (ms) under `prefix`_ keys."""
    if len(ft) < 2:
        return {}
    s = sorted(ft, reverse=True)
    w1 = s[: max(1, len(ft) // 100)]
    w01 = s[: max(1, len(ft) // 1000)]
    n = len(ft)
    return {
        prefix + "avg_fps": 1000.0 / mean(ft),
        prefix + "low1_fps": 1000.0 / mean(w1),
        prefix + "low1_p99_fps": 1000.0 / pct(ft, 0.99),
        prefix + "low01_fps": 1000.0 / mean(w01) if n >= 1000 else float("nan"),
        prefix + "p50_ms": pct(ft, 0.5), prefix + "p95_ms": pct(ft, 0.95), prefix + "p99_ms": pct(ft, 0.99),
        prefix + "p999_ms": pct(ft, 0.999) if n >= 1000 else float("nan"), prefix + "max_ms": max(ft),
        prefix + "share": {  # % of frames: faster than 120/90/60 FPS, slower than 40/30/20 FPS
            "<8.33": 100.0 * sum(1 for d in ft if d < 8.33) / n, "<11.11": 100.0 * sum(1 for d in ft if d < 11.11) / n,
            "<16.67": 100.0 * sum(1 for d in ft if d < 16.67) / n, ">25": 100.0 * sum(1 for d in ft if d > 25.0) / n,
            ">33.33": 100.0 * sum(1 for d in ft if d > 33.33) / n, ">50": 100.0 * sum(1 for d in ft if d > 50.0) / n},
    }


def main(argv):
    path = argv[1]
    t0, t1, out_json = 62.0, 88.0, None
    i = 2
    while i < len(argv):
        if argv[i] == "--from":
            t0 = float(argv[i + 1]); i += 2
        elif argv[i] == "--to":
            t1 = float(argv[i + 1]); i += 2
        elif argv[i] == "--json":
            out_json = argv[i + 1]; i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if v not in ("", None) else None) for k, v in r.items()})
    lo, hi = t0 * 1000.0, t1 * 1000.0

    # presented frames
    pres = [r["present_ms"] for r in rows if r["present_ms"] and lo <= r["present_ms"] <= hi]
    ft = [b - a for a, b in zip(pres, pres[1:])]
    res = {"file": path, "window_s": [t0, t1], "frames": len(pres)}
    if len(ft) >= 2:
        span = pres[-1] - pres[0]
        worst = sorted(ft, reverse=True)[: max(1, len(ft) // 100)]
        res.update({
            "avg_fps": (len(pres) - 1) * 1000.0 / span,
            "median_fps": 1000.0 / pct(ft, 0.5),
            "low1_fps": 1000.0 / mean(worst),
            "ft_mean_ms": mean(ft), "ft_p50_ms": pct(ft, 0.5), "ft_p95_ms": pct(ft, 0.95), "ft_p99_ms": pct(ft, 0.99),
            "ft_max_ms": max(ft),
            "ft_bins": {  # vblank multiples (16.68 ms)
                "<=1v": sum(1 for d in ft if d < 25.0), "2v": sum(1 for d in ft if 25.0 <= d < 41.7),
                "3v": sum(1 for d in ft if 41.7 <= d < 58.4), "4v": sum(1 for d in ft if 58.4 <= d < 75.1),
                ">=5v": sum(1 for d in ft if d >= 75.1)},
        })

    # the GPU thread and the GPU, per frame (flip marker inside the window)
    fr = [r for r in rows if r["cp_flip_ms"] and lo <= r["cp_flip_ms"] <= hi and r["cp_ms"] is not None]
    for k in ("cp_ms", "record_ms", "sync_ms", "blocked_label_ms", "blocked_other_ms", "empty_ms", "gpu_busy_ms",
              "draws", "dispatches", "batches", "blocks"):
        res["mean_" + k] = mean([r[k] for r in fr])
    # P4.10 (older logs have no such columns): the processor's other callbacks; the parallel command processor
    for k in ("callbacks_ms", "par_wait_ms", "par_busy_ms", "par_idle_ms", "par_drains", "par_records",
              "par_depth_mean"):
        v = [r[k] for r in fr if r.get(k) is not None]
        if v:
            res["mean_" + k] = mean(v)

    # the frame's path (pairs by row: the n-th game flip, flip marker, completion, present)
    def lat(a, b, rr=rows):
        return [r[b] - r[a] for r in rr if r[a] and r[b] and lo <= r[b] <= hi]

    res["game_span_ms"] = mean(lat("game_submit_ms", "game_flip_ms"))      # first submission -> flip request
    res["cp_after_game_ms"] = mean(lat("game_flip_ms", "cp_flip_ms"))      # the game done -> the processor done
    res["gpu_tail_ms"] = mean(lat("cp_flip_ms", "gpu_done_ms"))            # the processor done -> the GPU done (EOP)
    res["gpu_tail_p95_ms"] = pct(lat("cp_flip_ms", "gpu_done_ms"), 0.95)
    res["to_vblank_ms"] = mean(lat("gpu_done_ms", "present_ms"))           # the GPU done -> shown at a vblank
    # the game's answer: the previous frame's EOP -> this frame's first submission
    ans = []
    for a, b in zip(rows, rows[1:]):
        if a["gpu_done_ms"] and b["game_submit_ms"] and lo <= b["game_submit_ms"] <= hi:
            ans.append(b["game_submit_ms"] - a["gpu_done_ms"])
    res["game_answer_ms"] = mean(ans)
    gf = [r["game_flip_ms"] for r in rows if r["game_flip_ms"] and lo <= r["game_flip_ms"] <= hi]
    gft = [b - a for a, b in zip(gf, gf[1:])]
    res["game_ft_mean_ms"] = mean(gft)
    res["game_ft_p95_ms"] = pct(gft, 0.95)
    # FrameBreak: the three streams under both 1 % low definitions
    cf = [r["cp_flip_ms"] for r in rows if r["cp_flip_ms"] and lo <= r["cp_flip_ms"] <= hi]
    res.update(stream(ft, "present_"))
    res.update(stream(gft, "engine_"))
    res.update(stream([b - a for a, b in zip(cf, cf[1:])], "render_"))

    for k, v in res.items():
        if isinstance(v, float):
            res[k] = round(v, 3)
        elif isinstance(v, dict):
            res[k] = {a: round(x, 2) if isinstance(x, float) else x for a, x in v.items()}
    if out_json:
        with open(out_json, "w") as f:
            json.dump(res, f, indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main(sys.argv)
