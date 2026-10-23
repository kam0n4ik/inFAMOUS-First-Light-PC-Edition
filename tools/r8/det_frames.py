r"""R8-V3: the deterministic mode's frame log (logs\r8_det_frames.csv, runtime/gpu/r8/r8_verify_frame.h).

usage: py -I -X utf8 det_frames.py <run dir|csv> [<run dir|csv> ...] [--clock K]

Per run: frames, dt values (the mode's premise: every dt the clock's clamp), when each of the four clocks advances
(first frame, frames advanced, its rate in ticks per frame), the world start, the flips per game frame. With two or
more runs: at equal world frames, are the game clocks equal (the game's time follows its frames) and how the flip
numbers differ (the loading's length).
"""
import argparse
import csv
import sys
from collections import Counter
from pathlib import Path


def load(p):
    p = Path(p)
    if p.is_dir():
        p = p / "r8_det_frames.csv"
    with open(p, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k in ("frame", "flip", "c0", "c1", "c2", "c3", "world"):
            r[k] = int(r[k])
        for k in ("wall_ms", "dt_ms", "waited_ms"):
            r[k] = float(r[k])
    return p, rows


def summary(path, rows):
    print(f"{path}: {len(rows)} game frames, flips {rows[0]['flip']}..{rows[-1]['flip']}, "
          f"wall {rows[-1]['wall_ms'] / 1000:.1f} s")
    dts = Counter(round(r["dt_ms"], 4) for r in rows)
    print("  dt (ms): " + ", ".join(f"{k} x{v}" for k, v in dts.most_common(5)))
    for c in range(4):
        key = f"c{c}"
        adv = [i for i in range(1, len(rows)) if rows[i][key] != rows[i - 1][key]]
        if not adv:
            print(f"  clock {c}: never advances")
            continue
        steps = Counter(rows[i][key] - rows[i - 1][key] for i in adv)
        print(f"  clock {c}: first advance at frame {rows[adv[0]]['frame']} (wall {rows[adv[0]]['wall_ms'] / 1000:.1f} s), "
              f"{len(adv)} frames advance; ticks per advancing frame: " +
              ", ".join(f"{k} x{v}" for k, v in steps.most_common(4)))
    w = [r for r in rows if r["world"] >= 0]
    if w:
        print(f"  world start: game frame {w[0]['frame']}, flip {w[0]['flip']}, wall {w[0]['wall_ms'] / 1000:.1f} s; "
              f"world frames {w[-1]['world'] + 1}; frames that waited for the script "
              f"{sum(1 for r in w if r['waited_ms'] > 0.05)} (max {max(r['waited_ms'] for r in w):.1f} ms)")
    per_flip = Counter(r["flip"] for r in rows)
    print("  game frames per flip: " + ", ".join(f"{k} x{v}" for k, v in sorted(Counter(per_flip.values()).items())))


def compare(runs, clock):
    base_path, base = runs[0]
    bw = {r["world"]: r for r in base if r["world"] >= 0}
    for path, rows in runs[1:]:
        rw = {r["world"]: r for r in rows if r["world"] >= 0}
        common = sorted(set(bw) & set(rw))
        if not common:
            print(f"{path} vs {base_path}: no common world frames")
            continue
        same = {c: sum(1 for k in common if bw[k][f"c{c}"] - bw[common[0]][f"c{c}"] ==
                       rw[k][f"c{c}"] - rw[common[0]][f"c{c}"]) for c in range(4)}
        dflip = Counter(rw[k]["flip"] - bw[k]["flip"] for k in common)
        print(f"{Path(path).parent.name} vs {Path(base_path).parent.name}: {len(common)} common world frames; clocks "
              f"advanced alike since world frame {common[0]}: " +
              ", ".join(f"c{c} {same[c]}/{len(common)}" for c in range(4)) +
              "; flip(B) - flip(A) at equal world frame: " +
              ", ".join(f"{k:+d} x{v}" for k, v in dflip.most_common(4)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--clock", type=int, default=0)
    a = ap.parse_args()
    runs = [load(p) for p in a.runs]
    for path, rows in runs:
        summary(path, rows)
    if len(runs) > 1:
        compare(runs, a.clock)
    return 0


if __name__ == "__main__":
    sys.exit(main())
