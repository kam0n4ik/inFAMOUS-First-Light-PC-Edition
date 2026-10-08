"""Phase 2 audit of the game's libkernel imports -> docs/generated/KERNEL_AUDIT.md.

    python -I E:\\SecondSonPC\\tools\\kernel_audit.py --run E:\\SecondSonPC\\logs\\runs\\<tag> [--out FILE]

For every import of module "libkernel" (libkernel + libScePosix functions of the binding table
src/loader/generated/import_table.inc): owner in that run (SecondSonPC.log lists NATIVE, OBSERVED
and COMMON_STUB entries; anything else is BRIDGE), calls during the run (calls.json), the shadPS4
function behind a BRIDGE entry (docs/generated/imports.csv) and a migration class with complexity
and risk.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(r"E:\SecondSonPC")

# (pattern, class, complexity, risk, notes); first match wins.
CLASSES = [
    (r"^(__tls_get_addr|_sceKernelRtld\w*|sceKernelGetProcParam|sceKernelGetModuleInfo\w*|__stack_chk\w*|__progname)$",
     "loader/rtld", "-", "-", "NATIVE since Phase 1 M1"),
    (r"^(sceKernel(Open|Close|Read|Write|Pread|Pwrite|Lseek|Stat|Fstat|Fsync|Ftruncate|Truncate|Mkdir|Rmdir|Rename|Unlink|Getdirentries)|_read|_write)$",
     "file system", "-", "-", "NATIVE since Phase 1 M2"),
    (r"^(clock_gettime|sceKernelClockGettime|gettimeofday|sceKernelGettimeofday|sceKernelGetTscFrequency|sceKernelGetProcessTime\w*|sceKernelReadTsc)$",
     "time", "low", "low", "reads of host clocks; PROCTIME and TSC use the bridge's process clock"),
    (r"^sceKernelConvert\w*time\w*$", "time", "low", "low", "time zone conversion (not called)"),
    (r"^(sceKernelUsleep|sceKernelNanosleep|usleep|nanosleep)$",
     "time (sleep)", "low", "low", "host sleep; must stay a real sleep (no busy wait)"),
    (r"^sceKernelIsNeoMode$", "process", "low", "low", "[Console] neoMode and param.sfo attribute"),
    (r"^__error$", "errno", "low", "medium",
     "guest errno shared with every BRIDGE function that sets it: moves last"),
    (r"^(scePthreadMutex\w*|pthread_mutex\w*)$", "sync: mutex", "medium", "high",
     "hottest path (14.9 M calls / 210 s); objects also used by BRIDGE code; moves with condvars"),
    (r"^(scePthreadCond\w*|pthread_cond\w*)$", "sync: condvar", "medium", "high",
     "waits on BRIDGE mutexes; moves together with the mutexes"),
    (r"^(scePthreadRwlock\w*|pthread_rwlock\w*)$", "sync: rwlock", "medium", "high",
     "shares the pthread object model with mutexes; moves with them"),
    (r"^sceKernel(Create|Delete|Wait|Signal|Poll|Cancel)Sema$", "sync: semaphore", "medium", "medium",
     "self-contained objects; the semaphore table is started by libkernel RegisterLib"),
    (r"^sceKernel\w*EventFlag$", "sync: event flag", "medium", "medium",
     "self-contained objects; AND/OR waits, clear modes, timeouts"),
    (r"^sceKernel(\w*Equeue|\w*UserEvent)$", "sync: event queue", "medium", "high",
     "BRIDGE VideoOut/GnmDriver deliver flip/EOP events into the game's queues: moves with Phase 4"),
    (r"^(scePthread(Create|Join|Self|Equal|Yield|Getprio|Setprio|Getaffinity|Setaffinity|Detach|Exit|Once|Attr\w*)|pthread_(create\w*|join|self|attr\w*)|sched_yield)$",
     "threads", "high", "high", "guest TCB/TLS (rtld_tcb), stacks, priorities, exception handling"),
    (r"^scePthread(KeyCreate|KeyDelete|Getspecific|Setspecific)$", "threads: TLS keys", "medium", "high",
     "per-thread slots live in the BRIDGE thread objects"),
    (r"^(_?sigprocmask|sigfillset|signal|sigreturn|_is_signal_return)$", "signals", "medium", "high",
     "host exception/signal emulation (VEH) of the bridge"),
    (r"^(sceKernel(\w*DirectMemory\w*|Mprotect|QueryMemoryProtection|GetPrtAperture|\w*Mmap\w*|Munmap|\w*FlexibleMemory\w*|Mlock)|mmap|msync|getpagesize)$",
     "memory", "high", "very high", "guest address space shared with the GPU bridge (buffer/texture cache)"),
    (r"^(__elf_phdr_match_addr|__pthread_cxa_finalize|sceKernelInternalMemoryGetModuleSegmentInfo|sceKernelPrintBacktraceWithModuleInfo|sceKernelDebugRaiseException\w*)$",
     "debug/unwind", "low", "low", "not called in normal play"),
    (r"^(_exit|getrusage)$", "process", "low", "low", "not called in normal play"),
]


def classify(name: str) -> tuple[str, str, str, str]:
    for pattern, cls, complexity, risk, notes in CLASSES:
        if re.search(pattern, name):
            return cls, complexity, risk, notes
    return "other", "?", "?", ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "generated" / "KERNEL_AUDIT.md")
    a = ap.parse_args()

    rows = []
    pat = re.compile(r'^\{"([^"]*)", "([^"]*)", "([^"]*)", "([^"]*)", Kind::(\w+),')
    for line in (ROOT / "src/loader/generated/import_table.inc").read_text(encoding="utf-8").splitlines():
        m = pat.match(line)
        if m and m.group(3) == "libkernel":
            rows.append({"nid": m.group(1), "library": m.group(2), "name": m.group(4), "kind": m.group(5)})

    owners = {}
    for line in (a.run / "SecondSonPC.log").read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"\)\s+(NATIVE|OBSERVED|COMMON_STUB|UNRESOLVED)\s+(\S+)\s+(\S+)/(\S+)", line)
        if m:
            owners[(m.group(2), m.group(3))] = m.group(1)

    data = json.loads((a.run / "calls.json").read_text(encoding="utf-8-sig"))
    calls = {(e["nid"], e["library"]): e["calls"] for e in data["entries"]}

    impl = {}
    with open(ROOT / "docs/generated/imports.csv", encoding="utf-8-sig") as fh:
        for r in csv.DictReader(fh):
            impl[(r["nid"], r["library"])] = r["def"]

    table = []
    for r in rows:
        owner = owners.get((r["name"], r["library"]), "BRIDGE")
        cls, complexity, risk, notes = classify(r["name"])
        n = calls.get((r["nid"], r["library"]), 0)
        where = "SecondSonPC" if owner in ("NATIVE", "OBSERVED") else (impl.get((r["nid"], r["library"])) or "-")
        table.append({"cls": cls, "name": r["name"], "library": r["library"], "owner": owner, "calls": n,
                      "where": where, "complexity": complexity, "risk": risk, "notes": notes})
    table.sort(key=lambda t: (t["cls"], -t["calls"], t["name"]))

    by_owner = Counter(t["owner"] for t in table)
    by_class: dict[str, Counter] = defaultdict(Counter)
    for t in table:
        by_class[t["cls"]][t["owner"]] += 1
        by_class[t["cls"]]["_calls"] += t["calls"]
    out = [
        "# libkernel imports of the game — Phase 2 audit",
        "",
        f"Generated by `tools\\kernel_audit.py` from run `{a.run.name}` (calls = during that run).",
        f"{len(table)} imports of module libkernel: "
        + ", ".join(f"{k} {v}" for k, v in sorted(by_owner.items())) + ".",
        "",
        "| Class | NATIVE | BRIDGE | COMMON_STUB | calls |",
        "|---|---:|---:|---:|---:|",
    ]
    for cls in sorted(by_class):
        c = by_class[cls]
        out.append(f"| {cls} | {c['NATIVE']} | {c['BRIDGE']} | {c['COMMON_STUB']} | {c['_calls']} |")
    out += [
        "",
        "| Function | Library | Owner | Calls | Implementation | Class | Complexity | Risk | Notes |",
        "|---|---|---|---:|---|---|---|---|---|",
    ]
    for t in table:
        out.append(f"| `{t['name']}` | {t['library']} | {t['owner']} | {t['calls']} | {t['where']} | "
                   f"{t['cls']} | {t['complexity']} | {t['risk']} | {t['notes']} |")
    a.out.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{len(table)} imports -> {a.out}")
    for k, v in sorted(by_owner.items()):
        print(f"  {k}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
