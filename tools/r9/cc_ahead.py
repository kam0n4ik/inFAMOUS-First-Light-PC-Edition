"""R9: what a blocked game compute wait waits for on the GPU (lifecycle trace, SSPC_P412_STALL_TRACE=deep +
SSPC_P54_FRAME_TRACE=1): the batches still executing when the label's batch was submitted, split into those submitted
before the game rang the doorbell (one Vulkan queue: nothing the processor can reorder) and after it (the processor
submitted them ahead of the compute request); per label (the engine's two waits).

  py -I -X utf8 tools/r9/cc_ahead.py <p412_trace.bin>
"""
import collections
import sys

import numpy as np

sys.path.insert(0, r"E:\SecondSonPC\tools\p412")
import p412_analyze as pa  # noqa: E402

tr = pa.load_trace(sys.argv[1])
hz = tr["tsc_hz"]
EV = pa.EV
conv, info = pa.calibrate(tr, ([], []))
allev = np.sort(np.concatenate([t["ev"] for t in tr["threads"]]), order="t0")
game = np.sort(np.concatenate([t["ev"] for t in tr["threads"] if t["role"] == "Game"]), order="t0")
waits = game[(game["type"] == EV["GameComputeWait"]) & (game["c"] != 0)]
dd = game[game["type"] == EV["GameDingDong"]]["t0"].astype(np.int64)
fh = allev[allev["type"] == EV["FenceHeld"]]
fw = allev[allev["type"] == EV["FenceWrite"]]
b = np.sort(tr["batches"], order="submit_t0")
bs0 = b["submit_t0"].astype(np.int64)
bs1 = b["submit_t1"].astype(np.int64)
g0 = conv(b["gpu_t0"]).astype(np.int64)
g1 = conv(b["gpu_t1"]).astype(np.int64)
ms = lambda x: x / hz * 1e3
per = collections.defaultdict(lambda: collections.defaultdict(list))
for w in waits:
    a, e = int(w["t0"]), int(w["t1"])
    m = fh[(fh["a"] == w["a"]) & (fh["b"] >= w["b"]) & (fh["t0"].astype(np.int64) >= a - hz // 10)]
    if len(m) == 0:
        continue
    tc = int(m["t0"][0])
    i = int(np.searchsorted(bs0, tc))
    if i >= len(b):
        continue
    d = dd[dd <= a]
    td = int(d[-1]) if len(d) else a
    sub = int(bs1[i])
    # batches before i still on the GPU when i was submitted: their remaining GPU time from i's submission
    before = after = 0.0
    nb = na = 0
    for j in range(i - 1, max(i - 64, -1), -1):
        if g1[j] <= sub:
            continue
        rem = ms(g1[j] - max(g0[j], sub))
        if int(bs1[j]) <= td:
            before += rem
            nb += 1
        else:
            after += rem
            na += 1
    s = per[int(w["a"])]
    s["whole"].append(ms(e - a))
    s["queued"].append(ms(g0[i] - sub))
    s["ahead_before_doorbell"].append(before)
    s["ahead_after_doorbell"].append(after)
    s["n_before"].append(nb)
    s["n_after"].append(na)
    s["exec"].append(ms(g1[i] - g0[i]))
print(info.get("method"), "blocked waits", len(waits))
for lbl, s in sorted(per.items(), key=lambda kv: -len(kv[1]["whole"])):
    n = len(s["whole"])
    print(f"label {lbl:#x}: {n} waits")
    for k, v in s.items():
        v = np.array(v, dtype=float)
        print(f"  {k:24s} mean {v.mean():7.3f}  p50 {np.median(v):7.3f}  p90 {np.percentile(v, 90):7.3f}")
