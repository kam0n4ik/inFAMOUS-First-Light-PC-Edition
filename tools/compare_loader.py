"""Compare a SecondSonPC run against the Phase 0 oracle run (Phase 1 migration checks).

    python -I tools/compare_loader.py <oracle_run_dir> <candidate_run_dir> [--opens]

Each run dir is logs\\runs\\<tag> as written by tools/run_phase0.ps1. Compared (when present):
  * module layout   -- "loading module X to <base>" (ours) / "Loading module X to <base>" (shadPS4)
  * reloc_snapshot.txt -- every relocation slot of every module (--reloc-snapshot)
  * call_order.txt  -- the first N import calls (--call-order N); COMMON_STUB entries are ignored
                       (the oracle's generic stubs are not counted)
  * calls.json      -- functions called at least once, per owner
  * --opens         -- the sequence of files the game opened (FileSystem logs), see M2
Exit code 0 when nothing unexpected differs.
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path

LOAD_RE = re.compile(r"[Ll]oading module (\S+) to (0x[0-9a-fA-F]+)")


def module_layout(run: Path) -> list[tuple[str, int]]:
    out = []
    for name in ("SecondSonPC.log", "bridge_shadps4.log"):
        p = run / name
        if p.exists():
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
                m = LOAD_RE.search(line)
                if m:
                    out.append((m.group(1), int(m.group(2), 16)))
    return out


def read_lines(p: Path) -> list[str]:
    return p.read_text(encoding="utf-8", errors="replace").splitlines() if p.exists() else []


def compare_snapshots(a: Path, b: Path) -> int:
    la, lb = read_lines(a), read_lines(b)
    if not la or not lb:
        print(f"  reloc snapshot: missing ({'oracle' if not la else 'candidate'})")
        return 0
    if len(la) != len(lb):
        print(f"  reloc snapshot: {len(la)} vs {len(lb)} lines")
    kinds = Counter()
    examples: dict[str, list[str]] = {}
    for x, y in zip(la, lb):
        if x == y:
            continue
        cx = x.split("\t")[-1]
        cy = y.split("\t")[-1]
        if x.startswith("#") and y.startswith("#"):
            kind = "info: module base differs (varies between oracle runs too; slots are module-relative)"
        elif cx.startswith("raw ") and cy.startswith("COMMON_STUB "):
            kind = "expected: oracle generic stub -> SecondSonPC COMMON_STUB"
        elif cy.startswith(("NATIVE ", "OBSERVED ")) and not cx.startswith(("NATIVE ", "OBSERVED ")):
            kind = "expected: owner moved to SecondSonPC (planned migration)"
        elif cx.startswith("raw ") and cy.startswith("raw "):
            kind = "raw value differs (data written by the game before the snapshot?)"
        else:
            kind = "UNEXPECTED"
        kinds[kind] += 1
        examples.setdefault(kind, [])
        if len(examples[kind]) < 8:
            examples[kind].append(f"      oracle    {x}\n      candidate {y}")
    same = sum(1 for x, y in zip(la, lb) if x == y)
    print(f"  reloc snapshot: {same}/{min(len(la), len(lb))} slots identical")
    for kind, n in kinds.most_common():
        print(f"    {n:6} {kind}")
        for e in examples[kind]:
            print(e)
    return kinds["UNEXPECTED"] + (len(la) != len(lb))


def compare_call_order(a: Path, b: Path) -> int:
    def seq(p: Path) -> list[str]:
        out = []
        for line in read_lines(p):
            if line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 4 and parts[3] != "COMMON_STUB":
                out.append(f"{parts[1]} ({parts[2]})")
        return out

    sa, sb = seq(a), seq(b)
    if not sa or not sb:
        print(f"  call order: missing ({'oracle' if not sa else 'candidate'})")
        return 0
    n = min(len(sa), len(sb))
    prefix = next((i for i in range(n) if sa[i] != sb[i]), n)
    threads = sum(1 for c in sa[:prefix] if c.startswith("scePthreadCreate "))
    print(f"  call order: first {prefix} of {n} import calls identical "
          f"({threads} guest threads created before the first difference)")
    if prefix < n:
        lo = max(0, prefix - 3)
        for i in range(lo, min(n, prefix + 4)):
            mark = ">>" if i == prefix else "  "
            print(f"    {mark} {i:7} oracle {sa[i]:<55} candidate {sb[i]}")
        # Multi-threaded interleaving makes later order nondeterministic; report the multiset too.
        ca, cb = Counter(sa[:n]), Counter(sb[:n])
        diff = (ca - cb) + (cb - ca)
        print(f"    same calls in a different order? multiset difference = {sum(diff.values())} calls")
    return 0


def compare_calls(a: Path, b: Path) -> int:
    def load(p: Path) -> dict[str, tuple[str, int]]:
        if not p.exists():
            return {}
        data = json.loads(p.read_text(encoding="utf-8"))
        return {f"{e['library']}:{e['name']}": (e["owner"], e["calls"]) for e in data["entries"]}

    ca, cb = load(a), load(b)
    if not ca or not cb:
        print("  calls.json: missing")
        return 0
    called_a = {k for k, (_, c) in ca.items() if c}
    called_b = {k for k, (_, c) in cb.items() if c}
    print(f"  calls.json: {len(called_a)} functions called (oracle), {len(called_b)} (candidate)")
    only_a = sorted(called_a - called_b)
    only_b = sorted(called_b - called_a)
    if only_a:
        print(f"    called only by the oracle: {', '.join(only_a)}")
    if only_b:
        print(f"    called only by the candidate: {', '.join(only_b)}")
    owners = Counter(o for k, (o, c) in cb.items())
    print(f"    candidate owners (listed): {dict(owners)}")
    return 0


# shadPS4 FileSystem (oracle, bridge_shadps4.log) and SecondSonPC FileSystem (SecondSonPC.log)
OPEN_SHAD_RE = re.compile(r"\(([^)]*)\) file_system\.cpp:\d+ open: path = (\S+) flags = (0x[0-9a-f]+)")
OPEN_SSPC_RE = re.compile(r"\[Filesystem\] \(tid \d+\) open \[([^\]]*)\] (\S+) flags (0x[0-9a-f]+) mode")


def per_thread(run: Path) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for name, rx in (("SecondSonPC.log", OPEN_SSPC_RE), ("bridge_shadps4.log", OPEN_SHAD_RE)):
        for line in read_lines(run / name):
            m = rx.search(line)
            if m:
                out.setdefault(m.group(1), []).append(f"{m.group(2)} {m.group(3)}")
    return out
KEY_RE = re.compile(r"(russian|english|lang_|delete_me|savedata0|playgo|psarc|\.bsf)")


def compare_opens(a: Path, b: Path) -> int:
    def opens(run: Path) -> list[str]:
        out = []
        for name, rx in (("SecondSonPC.log", OPEN_SSPC_RE), ("bridge_shadps4.log", OPEN_SHAD_RE)):
            for line in read_lines(run / name):
                m = rx.search(line)
                if m:
                    out.append(f"{m.group(2)} {m.group(3)}")
        return out

    # The decisive check: the order of opens inside every guest thread.
    ta, tb = per_thread(a), per_thread(b)
    thread_problems = 0
    for t in sorted(set(ta) | set(tb)):
        sa, sb = ta.get(t, []), tb.get(t, [])
        same = sa == sb
        thread_problems += not same
        print(f"  opens on thread {t}: {len(sa)} / {len(sb)} -> {'order identical' if same else 'DIFFERS'}")
        if not same:
            k = next((i for i in range(min(len(sa), len(sb))) if sa[i] != sb[i]), min(len(sa), len(sb)))
            print(f"      first difference at #{k}: oracle {sa[k] if k < len(sa) else '-'} / "
                  f"candidate {sb[k] if k < len(sb) else '-'}")

    oa, ob = opens(a), opens(b)
    n = min(len(oa), len(ob))
    prefix = next((i for i in range(n) if oa[i] != ob[i]), n)
    print(f"  opened files: {len(oa)} (oracle) / {len(ob)} (candidate); first {prefix} identical "
          f"(several threads open files: later order depends on scheduling)")
    for i in range(prefix, min(n, prefix + 4)):
        print(f"    >> {i:5} oracle {oa[i]}\n             candidate {ob[i]}")
    ca, cb = Counter(oa[:n]), Counter(ob[:n])
    only_a, only_b = ca - cb, cb - ca
    print(f"    same opens in the first {n}? differences: {sum(only_a.values())} oracle-only, "
          f"{sum(only_b.values())} candidate-only")
    for k, v in list(only_a.items())[:6]:
        print(f"      oracle only:    {k} x{v}")
    for k, v in list(only_b.items())[:6]:
        print(f"      candidate only: {k} x{v}")
    # key files: first occurrence order
    ka = list(dict.fromkeys(x for x in oa if KEY_RE.search(x)))
    kb = list(dict.fromkeys(x for x in ob if KEY_RE.search(x)))
    same_keys = ka == kb[:len(ka)] or kb == ka[:len(kb)]
    print(f"    key files ({len(ka)} / {len(kb)}): first-open order {'identical' if same_keys else 'DIFFERS'}")
    if not same_keys:
        for i, (x, y) in enumerate(zip(ka, kb)):
            if x != y:
                print(f"      #{i}: oracle {x} / candidate {y}")
                break
    return 0 if thread_problems == 0 and not only_a and not only_b else 1


def main() -> int:
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    a, b = Path(args[0]), Path(args[1])
    print(f"oracle    {a}\ncandidate {b}")
    problems = 0
    la, lb = module_layout(a), module_layout(b)
    print("  module layout:")
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else ("-", 0)
        y = lb[i] if i < len(lb) else ("-", 0)
        # The order/names must match; a base address may differ (the eboot's base also varies
        # between two oracle runs: it depends on host allocations made before it is mapped).
        ok = x[0] == y[0]
        problems += not ok
        mark = "!!" if not ok else ("ok" if x[1] == y[1] else "~ ")
        print(f"    {mark} {x[0]:<18} {x[1]:#014x}   {y[0]:<18} {y[1]:#014x}")
    problems += compare_snapshots(a / "reloc_snapshot.txt", b / "reloc_snapshot.txt")
    problems += compare_call_order(a / "call_order.txt", b / "call_order.txt")
    problems += compare_calls(a / "calls.json", b / "calls.json")
    if "--opens" in sys.argv:
        problems += compare_opens(a, b)
    print(f"result: {'OK' if problems == 0 else f'{problems} unexpected difference(s)'}")
    return 0 if problems == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
