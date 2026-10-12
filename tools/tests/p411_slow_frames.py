# SecondSonPC P4.11: what made the slow frames slow - one native-backend run's logs\gpu_frames.csv.
#   py -I tools\tests\p411_slow_frames.py <gpu_frames.csv> [--from 25] [--to 50] [--slow 41.7] [--json out.json]
# A presented frame is slow when its present interval is at least --slow ms (3 vblanks at 60 Hz: 41.7). Its chain from
# the previous frame's GPU completion to its own: the game's answer (previous EOP -> first submission), the game's
# submitting (-> its flip), the processor after the game (-> its flip marker), the GPU's tail (-> completion), then the
# vblank. Each part is compared with its median over the window's frames; the slow frame is put in the class of the part
# that grew most, refined by the processor's own per-frame figures (shader translation and pipeline creation, waits for
# the GPU, blocked WAIT_REG_MEM, the parallel processor's thread 1 waiting) and the GPU's busy time.
import csv
import json
import math
import sys


def med(v):
    s = sorted(x for x in v if x is not None)
    return s[len(s) // 2] if s else 0.0


def main(argv):
    path = argv[1]
    t0, t1, slow, out_json = 25.0, 50.0, 41.7, None
    i = 2
    while i < len(argv):
        a = argv[i]
        if a in ("--from", "--to", "--slow", "--json"):
            v = argv[i + 1]
            if a == "--from":
                t0 = float(v)
            elif a == "--to":
                t1 = float(v)
            elif a == "--slow":
                slow = float(v)
            else:
                out_json = v
            i += 2
        else:
            raise SystemExit(f"unknown argument {a}")
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if v not in ("", None) else None) for k, v in r.items()})
    lo, hi = t0 * 1000.0, t1 * 1000.0
    frames = []
    for prev, r in zip(rows, rows[1:]):
        need = ("present_ms", "gpu_done_ms", "cp_flip_ms", "game_flip_ms", "game_submit_ms")
        if any(r.get(k) is None for k in need) or prev.get("present_ms") is None or prev.get("gpu_done_ms") is None:
            continue
        if not (lo <= r["present_ms"] <= hi):
            continue
        f = {
            "n": int(r["n"]),
            "t_s": round(r["present_ms"] / 1000.0, 3),
            "ft": r["present_ms"] - prev["present_ms"],
            "answer": r["game_submit_ms"] - prev["gpu_done_ms"],
            "span": r["game_flip_ms"] - r["game_submit_ms"],
            "cp_after": r["cp_flip_ms"] - r["game_flip_ms"],
            "tail": r["gpu_done_ms"] - r["cp_flip_ms"],
            "cp": r.get("cp_ms") or 0.0,
            "gpu": r.get("gpu_busy_ms") or 0.0,
            "compile": (r.get("shader_ms") or 0.0) + (r.get("pipeline_ms") or 0.0) + (r.get("module_ms") or 0.0),
            "shaders": r.get("shaders") or 0.0,
            "pipelines": r.get("pipelines") or 0.0,
            "sync": r.get("sync_ms") or 0.0,
            "blocked": (r.get("blocked_label_ms") or 0.0) + (r.get("blocked_other_ms") or 0.0),
            "par_wait": r.get("par_wait_ms") or 0.0,
            "par_busy": r.get("par_busy_ms") or 0.0,
            "draws": r.get("draws") or 0.0,
            "dispatches": r.get("dispatches") or 0.0,
        }
        frames.append(f)
    if not frames:
        raise SystemExit("no frames in the window")
    keys = ("ft", "answer", "span", "cp_after", "tail", "cp", "gpu", "compile", "sync", "blocked", "par_wait",
            "par_busy", "draws", "dispatches")
    base = {k: med([f[k] for f in frames]) for k in keys}
    slow_frames = [f for f in frames if f["ft"] >= slow]
    classes = {}
    for f in slow_frames:
        d = {k: f[k] - base[k] for k in ("answer", "span", "cp_after", "tail")}
        part = max(d, key=d.get)
        # the processor's and the GPU's own figures name the mechanism
        if f["compile"] >= 4.0 and f["compile"] >= 0.3 * max(d[part], 1e-9):
            cls = "shader translation / pipeline creation"
        elif part == "tail" or f["gpu"] - base["gpu"] >= 6.0:
            cls = "GPU work (busy/tail)"
        elif f["blocked"] - base["blocked"] >= 4.0:
            cls = "WAIT_REG_MEM blocked (memory the CPU writes / labels)"
        elif f["sync"] - base["sync"] >= 4.0:
            cls = "processor waited for the GPU (Synchronize/EOP)"
        elif part in ("cp_after", "span") and f["par_wait"] - base["par_wait"] >= 4.0:
            cls = "thread 1 waited for thread 2 (drains)"
        elif part in ("cp_after", "span") and f["cp"] - base["cp"] >= 4.0:
            cls = "processor CPU work (more commands)" if f["draws"] + f["dispatches"] > 1.1 * (
                base["draws"] + base["dispatches"]) else "processor CPU work (same commands)"
        elif part == "answer":
            cls = "the game's answer (its CPU / scheduling)"
        else:
            cls = "borderline (a few ms over 2 vblanks, no single cause)"
        f["class"] = cls
        f["grew"] = part
        classes.setdefault(cls, []).append(f)
    res = {"file": path, "window_s": [t0, t1], "frames": len(frames), "slow_ms": slow, "slow_frames": len(slow_frames),
           "median": {k: round(v, 2) for k, v in base.items()},
           "classes": {c: {"count": len(v), "examples": [
               {k: (round(x, 2) if isinstance(x, float) else x) for k, x in e.items()} for e in v[:4]]}
               for c, v in sorted(classes.items(), key=lambda kv: -len(kv[1]))}}
    if out_json:
        with open(out_json, "w") as fo:
            json.dump(res, fo, indent=1)
    print(f"{path}: window {t0}..{t1} s, {len(frames)} frames, {len(slow_frames)} at >= {slow} ms")
    print("median: " + ", ".join(f"{k} {v:.1f}" for k, v in base.items()))
    for c, v in sorted(classes.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(v):4d}  {c}")
        for e in v[:3]:
            print(f"        n {e['n']} t {e['t_s']} s: ft {e['ft']:.1f} | answer {e['answer']:.1f} span {e['span']:.1f} "
                  f"cp_after {e['cp_after']:.1f} tail {e['tail']:.1f} | cp {e['cp']:.1f} gpu {e['gpu']:.1f} compile "
                  f"{e['compile']:.1f} ({e['shaders']:.0f}/{e['pipelines']:.0f}) blocked {e['blocked']:.1f} sync "
                  f"{e['sync']:.1f} par_wait {e['par_wait']:.1f} draws {e['draws']:.0f}")


if __name__ == "__main__":
    main(sys.argv)
