# SecondSonPC P4.6-G3: a summary of an NVIDIA Nsight Graphics GPU Trace auto-export (ngfx --auto-export) of a capture
# with SSPC_GPU_MARKERS=1 labels (tools\tests\p46_gputrace.ps1).
#   py -I tools\gputrace_summary.py <...\BASE_UNLOCKED> [--top 25]
# Reads FRAME.xls (GPU frame time), D3DPERF_EVENTS.xls (time per label; column 1 of the first traced frame sums to its
# frame time) and GPUTRACE_REGIMES.xls (92 hardware metrics per label - Nsight fills them only for the first ranges of
# a frame on Turing). Labels: "D p<pipeline> vs<addr> ps<addr> @<PM4 packet>", "X cs<addr> <groups> @<packet>",
# "T ..." transfers, "W ..." batch end, "F ..." frame copy.
# NOTE: under GPU Trace a SecondSonPC frame takes ~7x longer (counters per range) - absolute times are not usable,
# small ranges are inflated most; use SSPC_GPU_PROFILE=1 for the time split.
import argparse
import collections
import csv
import os
import re


def read_tsv(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return list(csv.reader(f, delimiter="\t"))


def num(x):
    try:
        return float(x)
    except ValueError:
        return float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export_dir")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    frame = read_tsv(os.path.join(a.export_dir, "FRAME.xls"))
    if frame:
        print("GPU frame time (ms):", " ".join(frame[0][1:]))
    events = [(r[0], num(r[1])) for r in read_tsv(os.path.join(a.export_dir, "D3DPERF_EVENTS.xls"))[1:] if len(r) > 1]
    total = sum(t for _, t in events if t == t)
    print(f"labels {len(events)}, sum {total:.1f} ms")
    kinds = collections.defaultdict(lambda: [0, 0.0])
    agg = collections.defaultdict(lambda: [0, 0.0])
    for name, t in events:
        k = name.split()[0] if name else "?"
        kinds[k][0] += 1
        kinds[k][1] += t
        key = re.sub(r" \d+x\d+x\d+( indirect)?", "", re.sub(r" @[0-9a-f]+", "", name))
        agg[key][0] += 1
        agg[key][1] += t
    for k, (n, t) in sorted(kinds.items(), key=lambda x: -x[1][1]):
        print(f"  {k}: {n} labels, {t:.1f} ms ({100 * t / max(total, 1e-9):.1f}%)")
    print(f"top {a.top} labels:")
    for name, t in sorted(events, key=lambda x: -x[1])[: a.top]:
        print(f"  {t:8.3f} ms  {name}")
    print(f"top {a.top} by pipeline / shader (all runs):")
    for key, (n, t) in sorted(agg.items(), key=lambda x: -x[1][1])[: a.top]:
        print(f"  {t:8.2f} ms {n:5d} runs  {key}")
    reg_path = os.path.join(a.export_dir, "GPUTRACE_REGIMES.xls")
    if os.path.exists(reg_path):
        rows = read_tsv(reg_path)
        h, data = rows[0], rows[1:]
        cols = [("cycles", "gpu__cycles_elapsed.sum"),
                ("sm%", "SM.TriageA.sm__throughput.avg.pct_of_peak_sustained_elapsed"),
                ("l1%", "SM.TriageA.l1tex__throughput.avg.pct_of_peak_sustained_elapsed"),
                ("l2%", "LTS.TriageA.lts__throughput.avg.pct_of_peak_sustained_elapsed"),
                ("dram%", "FBPA.TriageA.dramc__throughput.avg.pct_of_peak_sustained_elapsed"),
                ("csW%", "TPC.TriageA.tpc__warps_active_shader_cs_realtime.avg.pct_of_peak_sustained_elapsed"),
                ("psW%", "TPC.TriageA.tpc__warps_active_shader_ps_realtime.avg.pct_of_peak_sustained_elapsed"),
                ("idleW%", "TPC.TriageA.tpc__warps_inactive_sm_idle_realtime.avg.pct_of_peak_sustained_elapsed"),
                ("draws", "FE_A.TriageA.fe__draw_count.sum"), ("disp", "HUB.TriageA.gr__dispatch_count.sum")]
        idx = [(k, h.index(n)) for k, n in cols if n in h]
        filled = [r for r in data if num(r[idx[0][1]]) > 0]
        print(f"regimes: {len(data)} rows, {len(filled)} with counters; the largest:")
        print("  " + f"{'range':44s}" + "".join(f"{k:>9s}" for k, _ in idx))
        for r in sorted(filled, key=lambda r: -num(r[idx[0][1]]))[: min(a.top, 15)]:
            print("  " + f"{r[0][:44]:44s}" + "".join(f"{num(r[i]):9.3g}" for _, i in idx))


if __name__ == "__main__":
    main()
