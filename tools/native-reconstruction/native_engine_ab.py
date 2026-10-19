r"""Native Engine Reconstruction: the summary of interleaved A/B city runs (run_native_engine.ps1 output dirs).

usage: py -I -X utf8 native_engine_ab.py <runs root> <tagA1,tagA2,...> <tagB1,tagB2,...> [--from 70] [--to 148]

Each run's gpu_frames.csv goes through tools/frame_stats.py over the window (the stand's 70..148 s); per configuration
the mean and the range of every figure, then B - A with A's own range next to it: a difference inside the ranges is
not a measured change.
"""
import json
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FRAME_STATS = os.path.join(HERE, "..", "frame_stats.py")
FIELDS = [("avg_fps", "avg FPS"), ("low1_fps", "1% low FPS"), ("ft_p99_ms", "frame p99 ms"),
          ("mean_cp_ms", "CP (thread 1) ms"), ("mean_par_busy_ms", "recording thread ms"),
          ("mean_gpu_busy_ms", "GPU busy ms"), ("mean_draws", "draws / frame")]


def stats(run, lo, hi):
    out = os.path.join(run, "stats.json")
    subprocess.run([sys.executable, "-I", FRAME_STATS, os.path.join(run, "gpu_frames.csv"), "--from", str(lo), "--to",
                    str(hi), "--json", out], check=True, stdout=subprocess.DEVNULL)
    return json.load(open(out, encoding="utf-8"))


def main():
    root, a_tags, b_tags = sys.argv[1], sys.argv[2].split(","), sys.argv[3].split(",")
    lo = float(sys.argv[sys.argv.index("--from") + 1]) if "--from" in sys.argv else 70
    hi = float(sys.argv[sys.argv.index("--to") + 1]) if "--to" in sys.argv else 148
    res = {}
    for name, tags in (("A", a_tags), ("B", b_tags)):
        res[name] = [stats(os.path.join(root, t), lo, hi) for t in tags]
    print("window %g..%g s; A = %s; B = %s" % (lo, hi, ",".join(a_tags), ",".join(b_tags)))
    print("%-24s %-28s %-28s %s" % ("", "A mean (min..max)", "B mean (min..max)", "B - A"))
    for key, label in FIELDS:
        va = [r.get(key) for r in res["A"] if isinstance(r.get(key), (int, float))]
        vb = [r.get(key) for r in res["B"] if isinstance(r.get(key), (int, float))]
        if not va or not vb:
            continue
        ma, mb = statistics.mean(va), statistics.mean(vb)
        print("%-24s %-28s %-28s %+.2f" % (label, "%.2f (%.2f..%.2f)" % (ma, min(va), max(va)),
                                           "%.2f (%.2f..%.2f)" % (mb, min(vb), max(vb)), mb - ma))
    for name in ("A", "B"):
        print("%s per run avg FPS: %s" % (name, " ".join("%.2f" % r.get("avg_fps", float("nan")) for r in res[name])))


if __name__ == "__main__":
    main()
