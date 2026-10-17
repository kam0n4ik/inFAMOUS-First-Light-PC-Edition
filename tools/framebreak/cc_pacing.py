"""FrameBreak CC-2: a series' frame latency and pacing per configuration (logs/p53/<tag>/<config>_r<n>/<scene>_1).

For the frames shown in the 62..88 s window: the game's first submission of the frame -> its present (mean, p95), and
|the game's step between its flips - the display's step between presents| (mean, p95): 0 when every shown frame
advances the game's time by what the display advances. Usage: cc_pacing.py <series dir> [scene]"""
import csv, os, re, sys
import numpy as np

root = sys.argv[1]
scene = sys.argv[2] if len(sys.argv) > 2 else "user1022"
by = {}
for d in sorted(os.listdir(root)):
    m = re.fullmatch(r"(.+)_r(\d+)", d)
    f = os.path.join(root, d, f"{scene}_1", "gpu_frames.csv")
    if m and os.path.exists(f):
        by.setdefault(m.group(1), []).append(f)
for cfg, files in sorted(by.items()):
    out = []
    for f in files:
        rows = [x for x in csv.DictReader(open(f)) if 62000 < float(x["present_ms"]) < 88000 and float(x["game_flip_ms"]) > 0]
        lat = np.array([float(x["present_ms"]) - float(x["game_submit_ms"]) for x in rows])
        gf = np.array([float(x["game_flip_ms"]) for x in rows])
        pr = np.array([float(x["present_ms"]) for x in rows])
        j = np.abs(np.diff(gf) - np.diff(pr))
        out.append((lat.mean(), np.percentile(lat, 95), j.mean(), np.percentile(j, 95)))
    o = np.array(out).mean(0)
    print(f"{cfg:12s} n {len(files)}  submit->present mean {o[0]:.2f} p95 {o[1]:.2f} ms | step mismatch mean {o[2]:.2f} "
          f"p95 {o[3]:.2f} ms")
