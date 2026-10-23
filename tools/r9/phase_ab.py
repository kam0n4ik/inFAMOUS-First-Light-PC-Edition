"""R9: a series' runs over one phase of their route (present intervals in [from, to] s of gpu_frames.csv): per
configuration the median [min..max] of FPS and 1% low, p95 / p99 / max, frames over 100 ms, GPU busy, CP, record.

  py -I -X utf8 tools/r9/phase_ab.py <series dir> <from s> <to s>
"""
import csv, os, re, statistics, sys, collections
d, t0, t1 = sys.argv[1], float(sys.argv[2]) * 1000, float(sys.argv[3]) * 1000
res = collections.defaultdict(list)
for run in sorted(os.listdir(d)):
    m = re.fullmatch(r"(.+)_(\d+)", run)
    p = os.path.join(d, run, "gpu_frames.csv")
    if not m or not os.path.exists(p):
        continue
    pres, gpu, cp, rec = [], [], [], []
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("present_ms"):
                t = float(r["present_ms"])
                if t0 <= t <= t1:
                    pres.append(t); gpu.append(float(r["gpu_busy_ms"])); cp.append(float(r["cp_ms"])); rec.append(float(r["record_ms"]))
    pres.sort()
    ft = [b - a for a, b in zip(pres, pres[1:])]
    s = sorted(ft, reverse=True)
    w1 = s[:max(1, len(ft) // 100)]
    ft_s = sorted(ft)
    res[m.group(1)].append((1000 * len(ft) / sum(ft), 1000 * len(w1) / sum(w1), ft_s[int(0.95 * len(ft_s))], ft_s[int(0.99 * len(ft_s))], max(ft), sum(1 for x in ft if x > 100), statistics.mean(gpu), statistics.mean(cp), statistics.mean(rec)))
print(f"window {t0/1000:.1f}..{t1/1000:.1f} s: median [min..max]")
print("config n  FPS                 1% low              p95     p99     max   >100  GPU    CP     record")
for c, v in res.items():
    col = list(zip(*v))
    f = lambda i, fmt="{:.2f}": f"{fmt.format(statistics.median(col[i]))} [{fmt.format(min(col[i]))}..{fmt.format(max(col[i]))}]"
    print(f"{c:6} {len(v)}  {f(0)}  {f(1)}  {statistics.median(col[2]):.2f}  {statistics.median(col[3]):.2f}  {statistics.median(col[4]):.0f}  {statistics.median(col[5]):.0f}  {statistics.median(col[6]):.2f}  {statistics.median(col[7]):.2f}  {statistics.median(col[8]):.2f}")
