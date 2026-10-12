# SecondSonPC P4.12: what ran on the SMT sibling of each logical CPU while chosen threads ran - from an ETW trace's
# context switches (xperf -a dumper text, tools\etw_capture.ps1). Hypothesis tested: the "two CPU speed modes" of P4.11
# (CP ~25.5 vs ~29 ms in the village) come from threads 1 and 2 sharing a physical core with the game's main thread
# (a spin on rdtsc without pause) or with each other.
#   py -I tools\p412\p412_smt.py <dump.txt> <pid> name=tid [name=tid ...] [--siblings=adjacent|half]
# Siblings: Windows numbers a Zen 3 core's two threads adjacently (0-1, 2-3, ...) - "adjacent" (default).
# Out (stdout, JSON): per named thread: run ms, and of it the ms its sibling CPU ran: idle, each named thread, another
# thread of the process, another process.
import collections
import json
import sys


def pid_of(field):
    i, j = field.rfind("("), field.rfind(")")
    try:
        return int(field[i + 1:j]) if i >= 0 and j > i else -1
    except ValueError:
        return -1


def main(argv):
    dump, pid = argv[1], int(argv[2])
    names = {}
    mode = "adjacent"
    for a in argv[3:]:
        if a.startswith("--siblings="):
            mode = a.split("=", 1)[1]
        elif "=" in a:
            k, v = a.split("=", 1)
            names[int(v)] = k
    # per CPU: list of (t0, t1, pid, tid) - what ran
    runs = collections.defaultdict(list)
    cur = {}
    first = last = None
    with open(dump, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.lstrip()
            if not line.startswith("CSwitch,"):
                continue
            p = [x.strip() for x in line.split(",", 17)]
            if len(p) < 17 or not p[1].isdigit():
                continue
            ts, cpu = int(p[1]), int(p[16])
            first = ts if first is None else first
            last = ts
            prev = cur.get(cpu)
            if prev is not None:
                runs[cpu].append((prev[2], ts, prev[0], prev[1]))
            cur[cpu] = (pid_of(p[2]), int(p[3]), ts)
    ncpu = max(runs) + 1 if runs else 0

    def sibling(c):
        return c ^ 1 if mode == "adjacent" else (c + ncpu // 2) % ncpu

    def label(rpid, rtid):
        if rtid == 0:
            return "idle"
        if rpid == pid:
            return names.get(rtid, "other thread of the process")
        return "another process"
    out = {"window_ms": round((last - first) / 1000.0, 1) if first is not None else 0, "cpus": ncpu, "siblings": mode,
           "threads": {}}
    for tid, nm in names.items():
        tot = 0
        by = collections.Counter()
        by_cpu = collections.Counter()
        for cpu, lst in runs.items():
            mine = [(a, b) for a, b, rp, rt in lst if rp == pid and rt == tid]
            if not mine:
                continue
            sib = runs.get(sibling(cpu), [])
            j = 0
            for a, b in mine:
                tot += b - a
                by_cpu[cpu] += b - a
                # the sibling's runs overlapping [a, b)
                while j < len(sib) and sib[j][1] <= a:
                    j += 1
                k = j
                covered = 0
                while k < len(sib) and sib[k][0] < b:
                    o = min(b, sib[k][1]) - max(a, sib[k][0])
                    if o > 0:
                        by[label(sib[k][2], sib[k][3])] += o
                        covered += o
                    k += 1
                if b - a > covered:
                    by["unknown (no switch seen)"] += b - a - covered
        out["threads"][nm] = {"tid": tid, "run_ms": round(tot / 1000.0, 1),
                              "sibling_ran_ms": {k: round(v / 1000.0, 1) for k, v in by.most_common()},
                              "sibling_ran_share": {k: round(v / max(1, tot), 3) for k, v in by.most_common()},
                              "run_ms_by_cpu": {str(c): round(v / 1000.0, 1) for c, v in sorted(by_cpu.items())}}
    print(json.dumps(out, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main(sys.argv)
