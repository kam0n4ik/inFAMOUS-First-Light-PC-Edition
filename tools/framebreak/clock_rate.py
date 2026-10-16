# SecondSonPC FrameBreak FB-3: the game's clock rate from logs\game_clock.csv (src/engine/game_clock_probe.*).
#   py -I tools\framebreak\clock_rate.py <game_clock.csv> [--from 62] [--to 88]
# Per clock: game seconds per wall second over the window (a least-squares slope; a clock whose scale changed - a
# pause, a slow-motion effect - shows as a poor fit), and the frames' dt (mean, max, how often at the 1/30 s clamp).
import csv
import json
import sys


def main(argv):
    path, t0, t1 = argv[1], 62.0, 88.0
    i = 2
    while i < len(argv):
        if argv[i] == "--from":
            t0 = float(argv[i + 1]); i += 2
        elif argv[i] == "--to":
            t1 = float(argv[i + 1]); i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    rows = [r for r in csv.DictReader(open(path, newline="")) if t0 <= float(r["t_s"]) <= t1]
    if len(rows) < 3:
        raise SystemExit("too few samples in the window")
    t = [float(r["t_s"]) for r in rows]
    res = {"file": path, "window_s": [t0, t1], "samples": len(rows)}
    mt = sum(t) / len(t)
    for c in ("c0", "c1", "c2", "c3"):
        y = [float(r[c]) for r in rows]
        my = sum(y) / len(y)
        sxx = sum((a - mt) ** 2 for a in t)
        slope = sum((a - mt) * (b - my) for a, b in zip(t, y)) / sxx
        resid = max(abs(b - (my + slope * (a - mt))) for a, b in zip(t, y))
        res[c] = {"game_s_per_s": round(slope, 3), "max_residual": round(resid, 3)}
    dt = [float(r["dt_ms"]) for r in rows]
    res["dt_mean_ms"] = round(sum(dt) / len(dt), 3)
    res["dt_max_ms"] = round(max(dt), 3)
    res["dt_at_clamp_pct"] = round(100.0 * sum(1 for d in dt if d >= 33.3) / len(dt), 1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main(sys.argv)
