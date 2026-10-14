"""Aggregate SSPC_DIAG_SAMPLE_RAW=1 samples (logs/cpu_samples_raw.txt) per thread and per eboot function.

Step 1 (system Python): py -I -X utf8 cpu_profile.py addrs <raw.txt> <addrs_out.txt>
    -> the distinct eboot-relative addresses (leaf, caller, frame chain) for Ghidra
Step 2 (Ghidra): run.py ghidra/resolve_addrs.py <addrs_out.txt> <funcs_out.tsv>
    -> address<TAB>function entry<TAB>name
Step 3 (system Python): py -I -X utf8 cpu_profile.py report <raw.txt> <funcs_out.tsv> [top]
    -> per thread: samples, share in the eboot / SecondSonPC / system; top eboot functions per thread group by
       exclusive (leaf, or the eboot caller of host code) and inclusive (any frame) samples
"""
import collections
import re
import sys


def load(raw):
    lo = hi = 0
    threads = {}
    samples = []
    with open(raw, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("guest "):
                _, a, b = line.split()
                lo, hi = int(a, 16), int(b, 16)
            elif line.startswith("thread "):
                _, i, name = line.rstrip("\n").split(" ", 2)
                threads[int(i)] = name
            elif line.startswith("s "):
                parts = line.split()
                samples.append((int(parts[1]), [int(x, 16) for x in parts[2:]]))
    return lo, hi, threads, samples


def group(name):
    n = re.sub(r"#\d+$", "", name)
    if n.startswith("JobWorker"):
        return "JobWorker*"
    return n or "?"


def main():
    mode = sys.argv[1]
    lo, hi, threads, samples = load(sys.argv[2])
    if mode == "addrs":
        addrs = set()
        for _, vals in samples:
            for v in vals:
                if lo <= v < hi:
                    addrs.add(v - lo)
        with open(sys.argv[3], "w") as f:
            for a in sorted(addrs):
                f.write(f"{a:x}\n")
        print(f"{len(samples)} samples, {len(threads)} threads, {len(addrs)} distinct eboot addresses")
        return
    funcs = {}
    with open(sys.argv[3], encoding="utf-8") as f:
        for line in f:
            a, ep, name = line.rstrip("\n").split("\t")
            funcs[int(a, 16)] = (int(ep, 16) if ep else None, name)
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 25

    def fn(v):
        r = funcs.get(v - lo)
        return f"{r[1]}@{r[0]:x}" if r and r[0] is not None else f"?@{v - lo:x}"

    per_thread = collections.Counter()
    where = collections.defaultdict(collections.Counter)
    excl = collections.defaultdict(collections.Counter)
    incl = collections.defaultdict(collections.Counter)
    for t, vals in samples:
        g = group(threads.get(t, "?"))
        per_thread[g] += 1
        leaf, caller, chain = vals[0], vals[1] if len(vals) > 1 else 0, vals[2:]
        in_guest = lo <= leaf < hi
        where[g]["eboot" if in_guest else "host"] += 1
        ex = leaf if in_guest else caller
        if ex and lo <= ex < hi:
            excl[g][fn(ex) + ("" if in_guest else " [via host]")] += 1
        seen = set()
        for v in ([leaf] if in_guest else []) + ([caller] if caller else []) + chain:
            if lo <= v < hi:
                k = fn(v)
                if k not in seen:
                    seen.add(k)
                    incl[g][k] += 1
    total = sum(per_thread.values())
    print(f"{total} samples")
    for g, n in per_thread.most_common():
        w = where[g]
        print(f"\n== {g}: {n} samples ({100.0 * n / total:.1f}%); leaf in eboot {100.0 * w['eboot'] / n:.0f}%")
        if n < 200:
            continue
        print("  exclusive (leaf, or the eboot caller of host code):")
        for k, c in excl[g].most_common(top):
            print(f"    {100.0 * c / n:5.1f}%  {k}")
        print("  inclusive:")
        for k, c in incl[g].most_common(top):
            print(f"    {100.0 * c / n:5.1f}%  {k}")


if __name__ == "__main__":
    main()
