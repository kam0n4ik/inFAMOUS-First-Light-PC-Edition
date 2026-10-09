"""P2.5: where the CPU time of one SecondSonPC.exe run goes, from a kernel ETW trace.

    xperf -i trace.etl -o dump.txt -a dumper          (tools\\etw_capture.ps1 records trace.etl)
    python -I tools\\etw_profile.py <dir with dump.txt and capture.json> [--log <SecondSonPC.log>] [--top 25]
        [--exe <SecondSonPC.exe of the traced run>]

Frames xperf left as Image!0x<address> (a dump without -symbols, which is much faster) are resolved
here with llvm-symbolizer for SecondSonPC.exe (--exe, with its PDB next to it), ntdll.dll,
KernelBase.dll and ntoskrnl.exe (copies with their PDBs in E:\\symcache\\llvm\\<name>\\, see
docs\\PHASE2_P25_PERFORMANCE.md); other modules stay Image!0x<address>.

Reads xperf's text dump of one trace (CPU samples with call stacks, context switches, ready-thread
events, thread names) and writes <dir>\\profile.md and <dir>\\profile.json:
  * per thread of the game process: CPU time from context switches (exact: time between switch-in and
    switch-out) and from samples, kernel share, number of switch-ins, why it stopped running (wait
    reasons), how long it waited for a CPU after being readied;
  * per thread and for the whole process: CPU samples by category, decided from the call stack:
      exception dispatch  - under ntdll!KiUserExceptionDispatcher (vectored handlers and what they call)
                            or the kernel half of an access violation (KiPageFault -> KiDispatchException),
                            with the VirtualProtect calls made inside handlers counted separately;
      page fault          - a page fault that is not an exception (demand-zero, soft fault);
      VirtualProtect      - NtProtectVirtualMemory outside exception handling;
      guest               - instruction pointer outside every loaded image (the game's code: eboot and
                            its PRX modules are mapped by SecondSonPC, not loaded as images);
      host                - SecondSonPC.exe (the runtime and the shadPS4 bridge);
      driver / system     - the GPU driver's user-mode DLLs, other system DLLs, the kernel;
  * the hottest leaf functions and, for exception handling, the hottest functions on the stack.
One sample = 1/profile_hz s of one CPU. Samples are statistical; context-switch times are exact.
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from pathlib import Path

EXC_USER = "ntdll.dll!KiUserExceptionDispatcher"
KERNEL_IMAGES = ("ntoskrnl.exe", "hal.dll")
PROTECT = ("NtProtectVirtualMemory", "ZwProtectVirtualMemory")
DRIVER_DLLS = ("nvoglv64.dll", "nvgpucomp64.dll", "nvwgf2umx.dll", "nvldumdx.dll", "vulkan-1.dll",
               "nvspcap64.dll", "nvd3dumx.dll")
WAIT_STATES = {"Waiting"}


def pid_of(field: str) -> int:
    m = re.search(r"\(\s*(-?\d+)\)\s*$", field)
    return int(m.group(1)) if m else -1


def split(line: str, n: int) -> list[str]:
    return [p.strip() for p in line.split(",", n)]


def module_of(sym: str) -> str:
    return sym.split("!", 1)[0].lower() if "!" in sym else sym.lower()


def is_kernel(sym: str) -> bool:
    mod = module_of(sym)
    return mod in KERNEL_IMAGES or mod.endswith(".sys")


def short(sym: str) -> str:
    """Image!Function without long template arguments and parameter lists."""
    sym = re.sub(r"\(.*\)$", "", sym)
    if len(sym) > 140:
        sym = sym[:137] + "..."
    return sym


class Thread:
    def __init__(self, tid: int):
        self.tid = tid
        self.name = ""
        self.start = ""
        self.run_us = 0
        self.switch_ins = 0
        self.samples = 0
        self.kernel_samples = 0
        self.cats: collections.Counter[str] = collections.Counter()
        self.leaf: collections.Counter[str] = collections.Counter()
        self.wait_reasons: collections.Counter[str] = collections.Counter()
        self.wait_reason_us: collections.Counter[str] = collections.Counter()
        self.ready_wait_us = 0
        self.ready_count = 0
        self.ready_by: collections.Counter[int] = collections.Counter()
        self.inclusive: collections.Counter[str] = collections.Counter()
        self.chains: collections.Counter[str] = collections.Counter()
        self.switched_out_at: int | None = None
        self.out_reason = ""
        self.ready_at: int | None = None


def classify(frames: list[str], guest_ranges: list[tuple[int, int, str]], ip: int) -> tuple[str, str]:
    """(category, detail) of one sample; frames[0] is the leaf."""
    funcs = [f.split("!", 1)[1] if "!" in f else f for f in frames]
    in_exc_user = EXC_USER in frames
    kernel = [f for f in frames if is_kernel(f)]
    kfuncs = {f.split("!", 1)[1] for f in kernel if "!" in f}
    protect = any(p in fn for fn in funcs for p in PROTECT)
    if in_exc_user:
        if protect:
            return "exception: VirtualProtect in handler", ""
        idx = frames.index(EXC_USER)
        handler = [f for f in frames[:idx] if module_of(f) == "secondsonpc.exe"]
        if handler:
            return "exception: handler code", short(handler[0])
        return "exception: user-mode dispatch", short(frames[0])
    if "KiPageFault" in kfuncs and ({"KiDispatchException", "KiExceptionDispatch"} & kfuncs):
        return "exception: kernel trap + dispatch", short(frames[0])
    if "KiPageFault" in kfuncs:
        return "page fault (not an exception)", short(frames[0])
    if protect:
        return "VirtualProtect (outside handlers)", ""
    leaf = frames[0] if frames else ""
    if kernel and is_kernel(leaf):
        return "kernel (other)", short(leaf)
    mod = module_of(leaf)
    if mod == "secondsonpc.exe":
        return "host (SecondSonPC.exe)", short(leaf)
    if mod in DRIVER_DLLS or mod.startswith("nv"):
        return "GPU driver (user mode)", mod
    if mod in ("unknown", "") or leaf.startswith("Unknown") or "!" not in leaf:
        for lo, hi, name in guest_ranges:
            if lo <= ip < hi:
                return "guest code", name
        return "guest code", "outside known modules"
    return "system DLL", mod


def load_guest_modules(log: Path | None) -> list[tuple[int, int, str]]:
    """Guest modules from SecondSonPC.log lines 'loading module <name> to 0x<base>' (size unknown:
    each runs to the next module's base, the last one 256 MiB)."""
    if not log or not log.exists():
        return []
    bases = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"loading module (\S+).*? to (0x[0-9a-fA-F]+)", line)
        if m:
            bases.append((int(m.group(2), 16), Path(m.group(1)).name))
    bases.sort()
    out = []
    for i, (base, name) in enumerate(bases):
        end = bases[i + 1][0] if i + 1 < len(bases) else base + (256 << 20)
        out.append((base, end, name))
    return out


LLVM_SYMBOLIZER = Path(r"E:\SecondSonPC\toolchain\LLVM\bin\llvm-symbolizer.exe")
SYSTEM_OBJS = {
    "ntdll.dll": Path(r"E:\symcache\llvm\ntdll\ntdll.dll"),
    "kernelbase.dll": Path(r"E:\symcache\llvm\kernelbase\KernelBase.dll"),
    "ntoskrnl.exe": Path(r"E:\symcache\llvm\ntoskrnl\ntoskrnl.exe"),
}
RAW = re.compile(r'^"?([^"!]+)"?!0x([0-9a-fA-F]+)$')


def symbolize(frames: set[str], bases: dict[str, int], objs: dict[str, Path]) -> dict[str, str]:
    """Image!0x<address> -> Image!function for the modules in objs (llvm-symbolizer, JSON output)."""
    by_mod: dict[str, list[tuple[str, int]]] = collections.defaultdict(list)
    for f in frames:
        m = RAW.match(f)
        if not m:
            continue
        mod = m.group(1).lower()
        if mod in objs and mod in bases and objs[mod].exists():
            by_mod[mod].append((f, int(m.group(2), 16) - bases[mod]))
    out: dict[str, str] = {}
    for mod, items in by_mod.items():
        text = "".join(f"0x{rva:x}\n" for _, rva in items)
        res = subprocess.run([str(LLVM_SYMBOLIZER), f"--obj={objs[mod]}", "--relative-address", "--no-inlines",
                              "--demangle", "--output-style=JSON"], input=text, capture_output=True, text=True)
        lines = [l for l in res.stdout.splitlines() if l.strip()]
        for (frame, _), line in zip(items, lines):
            try:
                sym = json.loads(line).get("Symbol") or [{}]
                name = sym[0].get("FunctionName") or ""
            except (ValueError, AttributeError, IndexError):
                name = ""
            if name and name != "??":
                out[frame] = f"{frame.split('!', 1)[0].strip(chr(34))}!{name}"
        print(f"symbolized {mod}: {len(items)} addresses, {sum(1 for f, _ in items if f in out)} named", file=sys.stderr)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--dump", type=Path, default=None)
    ap.add_argument("--log", type=Path, default=None)
    ap.add_argument("--exe", type=Path, default=Path(r"E:\SecondSonPC\build\bin\SecondSonPC.exe"))
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    cap = json.loads((args.dir / "capture.json").read_text(encoding="utf-8-sig"))
    pid = int(cap["pid"])
    hz = int(cap.get("profile_hz", 1000))
    dump = args.dump or (args.dir / "dump.txt")
    guest = load_guest_modules(args.log)

    threads: dict[int, Thread] = {}

    def th(tid: int) -> Thread:
        t = threads.get(tid)
        if t is None:
            t = threads[tid] = Thread(tid)
        return t

    bases: dict[str, int] = {}  # image file name (lower case) -> base address in the game / the kernel
    samples: dict[tuple[int, int], tuple[int, int]] = {}  # (ts, tid) -> (ip, cpu)
    stacks: dict[tuple[int, int], list[str]] = collections.defaultdict(list)
    cpu_cur: dict[int, tuple[int, int, int]] = {}  # cpu -> (tid, pid, since)
    first_ts = last_ts = None
    sample_ts_min = sample_ts_max = None

    with dump.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.lstrip()
            if not s or s[0] not in "SCRTI":
                continue
            if s.startswith("Stack,"):
                p = split(s, 5)
                if len(p) < 6 or not p[1].isdigit():
                    continue
                key = (int(p[1]), int(p[2]))
                if key in samples:
                    stacks[key].append(sys.intern(p[5]))
                continue
            if s.startswith(("I-DCStart,", "I-Start,")):
                p = [x.strip() for x in s.split(",")]
                if len(p) > 8 and p[3].startswith("0x"):
                    owner = pid_of(p[2])
                    name = p[8].strip('"').replace("\\", "/").rsplit("/", 1)[-1].lower()
                    if owner == pid:
                        bases[name] = int(p[3], 16)
                    elif owner in (0, 4):
                        bases.setdefault(name, int(p[3], 16))
                continue
            if s.startswith("SampledProfile,"):
                p = split(s, 6)
                if len(p) < 6 or not p[1].isdigit() or pid_of(p[2]) != pid:
                    continue
                ts, tid = int(p[1]), int(p[3])
                samples[(ts, tid)] = (int(p[4], 16), int(p[5]))
                sample_ts_min = ts if sample_ts_min is None else min(sample_ts_min, ts)
                sample_ts_max = ts if sample_ts_max is None else max(sample_ts_max, ts)
                continue
            if s.startswith("CSwitch,"):
                p = split(s, 17)
                if len(p) < 17 or not p[1].isdigit():
                    continue
                ts = int(p[1])
                first_ts = ts if first_ts is None else first_ts
                last_ts = ts
                new_pid, new_tid = pid_of(p[2]), int(p[3])
                old_pid, old_tid = pid_of(p[8]), int(p[9])
                old_state, reason, cpu = p[12], p[13], int(p[16])
                prev = cpu_cur.get(cpu)
                if prev and prev[1] == pid and prev[0] == old_tid:
                    t = th(old_tid)
                    t.run_us += ts - prev[2]
                if old_pid == pid:
                    t = th(old_tid)
                    t.switched_out_at = ts
                    t.out_reason = reason if old_state in WAIT_STATES else f"({old_state})"
                    t.wait_reasons[t.out_reason] += 1
                if new_pid == pid:
                    t = th(new_tid)
                    t.switch_ins += 1
                    if t.switched_out_at is not None:
                        t.wait_reason_us[t.out_reason] += ts - t.switched_out_at
                        t.switched_out_at = None
                    if t.ready_at is not None:
                        t.ready_wait_us += ts - t.ready_at
                        t.ready_count += 1
                        t.ready_at = None
                cpu_cur[cpu] = (new_tid, new_pid, ts)
                continue
            if s.startswith("ReadyThread,"):
                p = split(s, 9)
                if len(p) < 6 or not p[1].isdigit():
                    continue
                if pid_of(p[4]) == pid:
                    t = th(int(p[5]))
                    t.ready_at = int(p[1])
                    if pid_of(p[2]) == pid:
                        t.ready_by[int(p[3])] += 1
                continue
            if s.startswith("ThreadName,"):
                p = split(s, 4)
                if len(p) >= 5 and pid_of(p[2]) == pid:
                    th(int(p[3])).name = p[4].strip('"')
                continue
            if s.startswith(("T-DCStart,", "T-Start,")):
                p = [x.strip() for x in s.split(",")]
                if len(p) > 4 and pid_of(p[2]) == pid:
                    th(int(p[3])).start = short(p[-1].strip('"'))
                continue

    window_s = (sample_ts_max - sample_ts_min) / 1e6 if samples else 0.0
    cs_window_s = (last_ts - first_ts) / 1e6 if first_ts is not None else 0.0

    proc_cats: collections.Counter[str] = collections.Counter()
    proc_detail: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    leaf_all: collections.Counter[str] = collections.Counter()
    exc_inclusive: collections.Counter[str] = collections.Counter()
    unique: set[str] = set()
    for key in samples:
        unique.update(stacks.get(key, ()))
    names = symbolize(unique, bases, {**SYSTEM_OBJS, "secondsonpc.exe": args.exe})
    for (ts, tid), (ip, _cpu) in samples.items():
        frames = [names.get(f) or f.replace('"', "") for f in stacks.get((ts, tid), [])]
        # Kernel stack events come first; a user stack recorded separately follows them.
        frames = [f for f in frames if is_kernel(f)] + [f for f in frames if not is_kernel(f)]
        if not frames:
            frames = [f"Unknown!{ip:#x}"]
        cat, detail = classify(frames, guest, ip)
        t = th(tid)
        t.samples += 1
        t.kernel_samples += 1 if ip >= 0xFFFF800000000000 else 0
        t.cats[cat] += 1
        leaf = short(frames[0])
        if leaf.startswith("Unknown") or "!" not in leaf:
            leaf = f"guest:{detail}" if cat == "guest code" else leaf
        t.leaf[leaf] += 1
        leaf_all[leaf] += 1
        proc_cats[cat] += 1
        if detail:
            proc_detail[cat][detail] += 1
        if cat.startswith("exception"):
            for fn in set(short(f) for f in frames):
                exc_inclusive[fn] += 1
        # Inclusive time per function on this thread (each function once per sample), and the
        # caller chains (leaf + 4 callers) of the samples.
        seen = set()
        for fn in frames:
            s_fn = short(fn)
            if s_fn not in seen:
                seen.add(s_fn)
                t.inclusive[s_fn] += 1
        t.chains[" <- ".join(short(f) for f in frames[:5])] += 1

    ms_per_sample = 1000.0 / hz
    total = sum(proc_cats.values())
    live = sorted((t for t in threads.values() if t.samples or t.run_us), key=lambda t: -(t.run_us or t.samples))
    out = {
        "pid": pid, "profile_hz": hz, "sample_window_s": round(window_s, 3), "cswitch_window_s": round(cs_window_s, 3),
        "samples": total, "cpu_ms_from_samples": round(total * ms_per_sample, 1),
        "categories_ms": {k: round(v * ms_per_sample, 1) for k, v in proc_cats.most_common()},
        "threads": [],
    }
    for t in live:
        out["threads"].append({
            "tid": t.tid, "name": t.name, "start": t.start, "run_ms": round(t.run_us / 1000, 1),
            "run_pct_of_window": round(100 * t.run_us / 1e6 / cs_window_s, 1) if cs_window_s else None,
            "samples": t.samples, "kernel_pct": round(100 * t.kernel_samples / t.samples, 1) if t.samples else None,
            "switch_ins": t.switch_ins,
            "ready_wait_ms_avg": round(t.ready_wait_us / t.ready_count / 1000, 3) if t.ready_count else None,
            "categories_ms": {k: round(v * ms_per_sample, 1) for k, v in t.cats.most_common()},
            "top_leaf": [[k, v] for k, v in t.leaf.most_common(8)],
            "wait_reasons": dict(t.wait_reasons.most_common(6)),
            "off_cpu_ms_by_reason": {k: round(v / 1000, 1) for k, v in t.wait_reason_us.most_common(6)},
            "readied_by": [[threads[r].name if r in threads else str(r), n] for r, n in t.ready_by.most_common(4)],
        })
    out["top_leaf"] = [[k, v] for k, v in leaf_all.most_common(args.top)]
    out["exception_inclusive"] = [[k, v] for k, v in exc_inclusive.most_common(args.top)]
    out["category_detail"] = {k: [[d, n] for d, n in c.most_common(10)] for k, c in proc_detail.items()}
    (args.dir / "profile.json").write_text(json.dumps(out, indent=1, ensure_ascii=False), encoding="utf-8")

    md = [f"# ETW profile, pid {pid}", "",
          f"Samples {total} at {hz} Hz over {window_s:.2f} s (= {total * ms_per_sample / 1000 / window_s if window_s else 0:.2f} "
          f"cores on average); context switches over {cs_window_s:.2f} s.", "",
          "## Process CPU by category", "", "| category | CPU ms | % | ms per second |", "|---|---|---|---|"]
    for k, v in proc_cats.most_common():
        md.append(f"| {k} | {v * ms_per_sample:.0f} | {100 * v / total:.1f} | {v * ms_per_sample / window_s:.1f} |")
    md += ["", "## Threads (run time from context switches; categories from samples)", "",
           "| tid | name | run % of window | samples | kernel % | switch-ins | ready wait avg ms | top categories |",
           "|---|---|---|---|---|---|---|---|"]
    for e in out["threads"][:40]:
        cats = "; ".join(f"{k} {v:.0f}" for k, v in list(e["categories_ms"].items())[:4])
        md.append(f"| {e['tid']} | {e['name'] or e['start']} | {e['run_pct_of_window']} | {e['samples']} | {e['kernel_pct']} | "
                  f"{e['switch_ins']} | {e['ready_wait_ms_avg']} | {cats} |")
    md += ["", "## Hottest leaf functions (process)", ""]
    md += [f"- {v} ({100 * v / total:.1f} %) {k}" for k, v in leaf_all.most_common(args.top)]
    md += ["", "## Exception handling: functions on the stack (inclusive samples)", ""]
    md += [f"- {v} {k}" for k, v in exc_inclusive.most_common(args.top)]
    md += ["", "## Category details", ""]
    for k, c in proc_detail.items():
        md.append(f"- {k}: " + ", ".join(f"{d} {n}" for d, n in c.most_common(8)))
    md += ["", "## Busiest threads: wait reasons and off-CPU time", ""]
    for e in out["threads"][:12]:
        md.append(f"- {e['tid']} {e['name']}: off-CPU ms by reason {e['off_cpu_ms_by_reason']}; readied by {e['readied_by']}; "
                  f"top leaf {e['top_leaf'][:4]}")
    busiest = sorted((t for t in threads.values() if t.samples), key=lambda t: -t.samples)[:3]
    for t in busiest:
        md += ["", f"## {t.name or t.tid}: inclusive samples (of {t.samples})", ""]
        md += [f"- {v} ({100 * v / t.samples:.1f} %) {k}" for k, v in t.inclusive.most_common(args.top + 20)]
        md += ["", f"## {t.name or t.tid}: hottest call chains (leaf <- callers)", ""]
        md += [f"- {v} {k}" for k, v in t.chains.most_common(args.top)]
    (args.dir / "profile.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("\n".join(md[:60]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
