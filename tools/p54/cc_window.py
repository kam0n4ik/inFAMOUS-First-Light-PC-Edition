"""Each game compute wait that waited: from its start to the processor reaching the label's RELEASE_MEM (FenceHeld),
what thread 1 did (graphics / compute slices, drains inside them, idle states)."""
import sys, numpy as np
sys.path.insert(0, r"E:\SecondSonPC\tools\p412")
import p412_analyze as pa
tr = pa.load_trace(sys.argv[1]); hz = tr["tsc_hz"]; EV = pa.EV
cp = np.sort(next(t for t in tr["threads"] if t["role"] == "CP")["ev"], order="t0")
game = np.sort(np.concatenate([t["ev"] for t in tr["threads"] if t["role"] == "Game"]), order="t0")
allev = np.sort(np.concatenate([t["ev"] for t in tr["threads"]]), order="t0")
waits = game[(game["type"] == EV["GameComputeWait"])]
dd = game[game["type"] == EV["GameDingDong"]]
fh = allev[allev["type"] == EV["FenceHeld"]]
fw = allev[allev["type"] == EV["FenceWrite"]]
def overlap(sel, a, b):
    t0 = np.maximum(sel["t0"].astype(np.int64), int(a)); t1 = np.minimum(sel["t1"].astype(np.int64), int(b))
    return np.clip(t1 - t0, 0, None).sum() / hz * 1e3
kinds = {"gfx slice": (cp["type"] == EV["CpQueueSlice"]) & (cp["a"] == 0),
         "compute slice": (cp["type"] == EV["CpQueueSlice"]) & (cp["a"] == 1),
         "drain": cp["type"] == EV["Drain"]}
for n in ("IdleGpu", "IdleEmpty", "Poll", "PreRound"):
    kinds[n] = cp["type"] == EV[n]
rows = []
for w in waits:
    if w["c"] == 0: continue
    a, b = w["t0"], w["t1"]
    m = fh[(fh["a"] == w["a"]) & (fh["b"] >= w["b"]) & (fh["t0"] <= b + hz)]
    m = m[m["t0"] >= a - hz // 10]
    if len(m) == 0: continue
    reach = m["t0"][0]
    d = dd[dd["t0"] <= a]
    last_dd = d["t0"][-1] if len(d) else a
    r = {"wait": (b - a) / hz * 1e3, "to_fence": (reach - a) / hz * 1e3, "dd_before": (a - last_dd) / hz * 1e3,
         "fence_before_wait": reach < a}
    end = max(reach, a)
    for k, sel in kinds.items():
        r[k] = overlap(cp[sel], a, end)
    rows.append(r)
print("waits that waited, matched:", len(rows), "of", int((waits["c"] != 0).sum()))
keys = list(rows[0].keys())
for k in keys:
    v = np.array([r[k] for r in rows], dtype=float)
    print(f"  {k:18s} mean {v.mean():.3f}  p50 {np.median(v):.3f}  p90 {np.percentile(v,90):.3f}")
# drains by reason in the windows, and the packets of slices in them
names = ["RoundEnd","CacheAction","Fence","Dma","Flip","TimelineWrite","TimelineCopy","TimelineGds","MemoryAcquired","WaitMet","Synchronize","MemoryRead","MemoryWrite","FrontendRead","Stop"]
dr = cp[cp["type"] == EV["Drain"]]
sl = cp[cp["type"] == EV["CpQueueSlice"]]
acc = {}; cnt = {}; pk = {0: 0, 1: 0}; nsl = {0: 0, 1: 0}; nw = 0
for w in waits:
    if w["c"] == 0: continue
    a = int(w["t0"])
    m = fh[(fh["a"] == w["a"]) & (fh["b"] >= w["b"]) & (fh["t0"] >= a - hz // 10)]
    if len(m) == 0: continue
    e = max(int(m["t0"][0]), a); nw += 1
    for why in set(dr["c"].tolist()):
        s = dr[dr["c"] == why]
        acc[why] = acc.get(why, 0) + overlap(s, a, e)
        cnt[why] = cnt.get(why, 0) + int(((s["t0"].astype(np.int64) < e) & (s["t1"].astype(np.int64) > a)).sum())
    for q in (0, 1):
        s = sl[(sl["a"] == q) & (sl["t0"].astype(np.int64) >= a) & (sl["t1"].astype(np.int64) <= e)]
        pk[q] += int(s["b"].sum()); nsl[q] += len(s)
print("drains in the windows, per wait:")
for why, v in sorted(acc.items(), key=lambda kv: -kv[1]):
    print(f"  {names[why] if why < len(names) else why:15s} {v / nw:.3f} ms, {cnt[why] / nw:.1f} drains")
print("slices fully inside, per wait: graphics", nsl[0] / nw, "packets", pk[0] / nw, "; compute", nsl[1] / nw, "packets", pk[1] / nw)
# slow vs typical windows
tf = np.array([r["to_fence"] for r in rows])
cut = np.percentile(tf, 90)
for name, sel in (("slowest 10 %", tf >= cut), ("the rest", tf < cut)):
    print(name, "n", int(sel.sum()), " ".join(f"{k} {np.mean([r[k] for r, s in zip(rows, sel) if s]):.2f}" for k in ("to_fence", "gfx slice", "compute slice", "drain", "IdleGpu")))
