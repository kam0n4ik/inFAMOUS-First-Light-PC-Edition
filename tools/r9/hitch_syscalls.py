"""R9: a thread group's system time in the slow frames of a window, by export and by export < SecondSonPC callers.

  py -I -X utf8 tools/r9/hitch_syscalls.py <run> <from s> <to s> <frame ms> [group, default cp_t2]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gprof  # noqa: E402

run, t0, t1, slow = sys.argv[1], float(sys.argv[2]) * 1000, float(sys.argv[3]) * 1000, float(sys.argv[4])
group = sys.argv[5] if len(sys.argv) > 5 else "cp_t2"
exe = r"E:\SecondSonPC-r9\build\bin\SecondSonPC.exe"
g = gprof.Gprof(os.path.join(run, "r9_gprof.bin"))
rows = gprof.frame_rows(g, gprof.read_frames(os.path.join(run, "gpu_frames.csv")), t0, t1)
by_exp = collections.Counter()
by_stack = collections.Counter()
nslow = 0
for r in rows:
    if r["ft_ms"] <= slow:
        continue
    nslow += 1
    a, b = r["_ticks"]
    for (tick, idx, flags, cyc, addrs) in g.samples_in(a, b):
        if g.group.get(idx) != group or flags & 2:
            continue
        rip, caller, hosts, chain = g.split(flags, addrs)
        if g.host_lo <= rip < g.host_hi or g.guest_lo <= rip < g.guest_hi:
            continue
        e = g.export_of(rip)
        by_exp[e] += cyc
        by_stack[(e,) + tuple(hosts[:3])] += cyc
names = gprof.symbolize_host(exe, {x for k in by_stack for x in k[1:]}, g.host_lo)
print(f"{nslow} frames over {slow} ms in {t0/1000:.0f}..{t1/1000:.0f} s; {group} system time, ms total")
for e, c in by_exp.most_common(8):
    print(f"  {c * g.cyc_to_ms:8.1f}  {e}")
print("by export < SecondSonPC callers")
for k, c in by_stack.most_common(14):
    print(f"  {c * g.cyc_to_ms:8.1f}  {k[0]} < " + " < ".join(names.get(x, hex(x))[:55] for x in k[1:]))
