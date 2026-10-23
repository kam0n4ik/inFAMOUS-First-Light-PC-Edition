"""R9: the eboot's hottest functions over all the game's threads, per phase (docs/R9_HOT_FUNCTIONS.md).

  py -I -X utf8 tools/r9/hot_eboot.py <run dir> [--top 30] [--funcs E:/research/r9/ghidra/functions.tsv]

Phases from the run's marks (as gprof.py report --marks). Per phase: CPU ms per frame by eboot function, exclusive
(the sample's instruction pointer in it) and inclusive (it anywhere in the sample's eboot chain), summed over the game's
thread groups, with the share that ran on Game:Main (the thread whose waits make the frame) and on the job workers.
The game's delay spin 0x53b2b0 (the time Game:Main waits) is listed apart, not ranked.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gprof  # noqa: E402

GAME = ("game_main", "jobs", "game_other", "audio", "fios")
SPIN = 0x53b2b0


def main(argv):
    run = argv[1]
    top, funcs_path = 30, r"E:\research\r9\ghidra\functions.tsv"
    if "--top" in argv:
        top = int(argv[argv.index("--top") + 1])
    if "--funcs" in argv:
        funcs_path = argv[argv.index("--funcs") + 1]
    g = gprof.Gprof(os.path.join(run, "r9_gprof.bin"))
    funcs = gprof.Functions(funcs_path)
    frames = gprof.read_frames(os.path.join(run, "gpu_frames.csv"))
    marks = sorted(g.marks)
    ends = [m[0] for m in marks[1:]] + [g.tick_t[-1] if g.tick_t else 0]
    lo = g.guest_lo
    for (t0, label), t1 in zip(marks, ends):
        rows = gprof.frame_rows(g, frames, t0, t1)
        if not rows:
            continue
        a, b = rows[0]["_ticks"][0], rows[-1]["_ticks"][1]
        k = g.cyc_to_ms / len(rows)
        excl = collections.defaultdict(collections.Counter)  # entry -> group -> cycles
        incl = collections.defaultdict(collections.Counter)
        total = collections.Counter()
        for (tick, idx, flags, cyc, addrs) in g.samples_in(a, b):
            gr = g.group.get(idx, "unnamed")
            if gr not in GAME or flags & 2 or gprof.blocked_at(g, addrs[0]):
                continue
            rip, caller, hosts, chain = g.split(flags, addrs)
            total[gr] += cyc
            seen = set()
            if g.guest_lo <= rip < g.guest_hi:
                e = funcs.of(rip - lo)
                if e is not None:
                    excl[e][gr] += cyc
                    seen.add(e)
            elif caller:
                e = funcs.of(caller - lo)
                if e is not None:
                    seen.add(e)
            for r in chain:
                e = funcs.of(r - lo)
                if e is not None:
                    seen.add(e)
            for e in seen:
                incl[e][gr] += cyc
        spin = sum(excl.pop(SPIN, {}).values()) * k
        print(f"== {label} ({t0 / 1000:.1f}..{t1 / 1000:.1f} s, {len(rows)} frames): game threads sampled "
              + ", ".join(f"{gr} {total[gr] * k:.2f}" for gr in GAME if total[gr]) + f" ms/frame; delay spin {spin:.2f}")
        for title, table in (("exclusive", excl), ("inclusive", incl)):
            print(f"  {title}: entry, name, ms/frame (Game:Main / jobs / other)")
            ranked = sorted(table.items(), key=lambda kv: -sum(kv[1].values()))[:top]
            for e, by in ranked:
                s = sum(by.values())
                other = s - by["game_main"] - by["jobs"]
                print(f"    {e:#9x} {funcs.name(e):<40} {s * k:7.3f}  ({by['game_main'] * k:.3f} / "
                      f"{by['jobs'] * k:.3f} / {other * k:.3f})")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
