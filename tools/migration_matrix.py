"""Phase 2.2.3 audit of every BRIDGE import -> docs/PHASE2_NATIVE_MIGRATION_MATRIX.md (+ .csv).

    python -I E:\\SecondSonPC\\tools\\migration_matrix.py --run <run dir> [--startup <calls.json>]
                  [--session <calls.json> ...] [--out FILE]

<run dir>: a run of the current build (SecondSonPC.log lists the NATIVE, OBSERVED, COMMON_STUB and
UNRESOLVED rows of the binding table, every other non-LLE row is BRIDGE; calls.json = calls during the
whole run). --startup: calls.json written at the end of loading in the same run (tools/perf_runs.ps1
-SnapshotAt): "startup" = called by then, "gameplay" = calls grew after it. --session: more runs
(e.g. a long manual session) whose calls also count as gameplay use.
The bridge implementation of each row is found from its LIB_FUNCTION registration in
third_party/shadPS4/src. Tier, complexity, risk, dependencies and priority come from RULES below
(first match wins), reviewed by hand for this game's imports.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(r"E:\SecondSonPC")
SHAD = ROOT / "third_party" / "shadPS4" / "src"

# (name regex, library regex, tier, purpose, shadPS4 dependencies, without own GPU runtime,
#  complexity, risk, oracle, fallback group, priority, note). First match wins.
# Tier 1 low risk / 2 medium / 3 high / 4 GPU-coupled. Priority P1 = this phase, P2 = next kernel
# phase, P3 = later subsystem phase, P4 = after SecondSonPC's own GPU runtime.
RULES = [
    # --- Tier 1: pure functions, no state shared with the bridge -----------------------------
    (r"^scePthreadEqual$", r"libkernel", 1, "compare two pthread handles", "none (pointer compare)",
     "yes", "low", "low", "bridge function", "kernel-util", "P1", "50k calls/100 s (libc, Fios2, eboot)"),
    (r"^(scePthreadYield|sched_yield)$", r"libkernel", 1, "yield the CPU", "none (host yield)",
     "yes", "low", "low", "bridge function (returns)", "kernel-util", "P1", ""),
    (r"^getpagesize$", r"libkernel", 1, "page size (16 KiB on PS4)", "none", "yes", "low", "low",
     "bridge function", "kernel-util", "P1", "imported by libc.prx"),
    (r"^sigfillset$", r"libkernel", 1, "fill a signal set", "none (128-bit set)", "yes", "low", "low",
     "bridge function", "kernel-util", "P1", ""),
    (r"^sceNet(Htons|Ntohs)$", r"libSceNet", 1, "16-bit byte order", "none", "yes", "low", "low",
     "bridge function", "net-util", "P1", ""),
    (r"^sceNetInet(Ntop|Pton)$", r"libSceNet", 2, "IP address text <-> binary",
     "set libSceNet's errno (read by sceNetErrnoLoc) and, through the bridge's kernel errno hook, the "
     "kernel errno", "yes", "low", "medium", "bridge function + net errno", "net", "P2",
     "pure logic, but the error state is the bridge's: moves with libSceNet"),
    (r"^(memset|memcpy)$", r"libSceLibcInternal", 1, "fill / copy memory", "none", "yes", "low", "low",
     "bridge function", "libc-mem", "P1", "imported by Fios2 (memset ~50 calls/s)"),
    # --- Tier 2: state, objects, synchronisation ---------------------------------------------
    (r"^(scePthreadMutex\w*|pthread_mutex\w*|scePthreadMutexattr\w*|pthread_mutexattr\w*)$", r".",
     2, "mutex objects and attributes", "pthread object model, g_curthread (owner), BRIDGE code locking "
     "game mutexes", "yes", "high", "high", "bridge (single thread) + native stress tests",
     "kernel-mutex", "P1/P2", "hottest path (~45k lock+unlock/s); see KERNEL_SYNC.md"),
    (r"^(scePthreadCond\w*|pthread_cond\w*)$", r".", 2, "condition variables",
     "waits on BRIDGE/NATIVE mutexes; g_curthread", "yes", "high", "high",
     "bridge (single thread) + native stress tests", "kernel-condvar", "P2", "moves with/after mutexes"),
    (r"^(scePthreadRwlock\w*|pthread_rwlock\w*)$", r".", 2, "reader/writer locks",
     "pthread object model; g_curthread (writer owner)", "yes", "medium", "medium",
     "bridge (single thread) + native stress tests", "kernel-rwlock", "P2", "imported by Fios2 (~17 calls/s)"),
    (r"^sceKernelConvert(LocaltimeToUtc|UtcToLocaltime)$", r".", 2, "time zone conversion", "none",
     "yes", "low", "medium", "none: the bridge's results are wrong (minutes used as seconds)", "kernel",
     "P3", "not called; needs the PS4 semantics from another source first"),
    (r"^sceKernelGetProcessTime$", r".", 2, "process time in microseconds",
     "the bridge's process clock, shared with VideoOut/AudioOut timestamps (kernel_host ProcessTimeUs)",
     "yes", "low", "medium", "bridge function", "kernel", "P3",
     "a NATIVE wrapper would only call the bridge's clock: moves when the clock itself moves"),
    (r"^sceKernelDebugRaiseException\w*$", r".", 2, "raise a fatal debug exception",
     "process shutdown (log flush, saves)", "yes", "low", "medium", "none (terminates)", "-", "P3",
     "not called in play"),
    (r"^_exit$", r".", 2, "terminate the process", "bridge shutdown (Emulator::Shutdown, log flush)",
     "yes", "low", "medium", "none (terminates)", "-", "P3", "not called in play"),
    (r"^(fflush)$", r"libSceLibcInternal", 2, "flush a C stream", "FILE objects of the bridge's libc",
     "yes", "medium", "medium", "bridge function", "-", "P3", ""),
    (r"^sceLibcHeapGetTraceInfo$", r".", 2, "heap trace information", "bridge libc heap state",
     "yes", "low", "low", "bridge function", "-", "P3", "called once at start"),
    (r"^sceNet(Init|Term|PoolCreate|PoolDestroy|ErrnoLoc)$", r".", 2, "libSceNet start-up, pools, errno",
     "net errno/pools shared with every libSceNet function", "yes", "low", "medium", "bridge function",
     "net", "P2", "moves with the whole libSceNet group"),
    (r"^sceNetCtl\w*$", r".", 2, "network controller state and callbacks", "NetCtl state, callbacks",
     "yes", "medium", "medium", "bridge function", "netctl", "P2", "offline state: needs a defined "
     "'disconnected' contract"),
    (r"^sceNp\w*$", r"libSceNpManager", 2, "PSN state, callbacks, account queries",
     "NpManager state, callbacks polled every frame (sceNpCheckCallback)", "yes", "medium", "medium",
     "bridge function", "np", "P2", "offline PSN: signed-out contract"),
    # --- Tier 3: memory, threads, signals, services with heavy state -------------------------
    (r"^(scePthread(Create|Join|Detach|Exit|Self|Once|Getprio|Setprio|Getaffinity|Setaffinity|Attr\w*)"
     r"|pthread_(create\w*|join|self|attr\w*))$", r".", 3, "guest threads and their attributes",
     "guest TCB/TLS (rtld_tcb), stacks in guest memory, g_curthread, priorities, exception handling",
     "yes", "very high", "very high", "partial (behaviour, not internals)", "kernel-thread", "P3",
     "P2.4 (docs/PHASE2_P23_P24_PLAN.md); after P2.3"),
    (r"^scePthread(KeyCreate|KeyDelete|Getspecific|Setspecific)$", r".", 3, "thread-specific data keys",
     "per-thread slots inside BRIDGE thread objects", "yes", "medium", "high", "bridge function",
     "kernel-thread", "P3", "moves with the threads (P2.4)"),
    (r"^(_?sigprocmask|signal)$", r".", 3, "signal masks and handlers",
     "the bridge's VEH signal emulation", "yes", "high", "high", "partial", "-", "P3", "not called in play"),
    (r"^__error$", r".", 3, "guest errno location", "set by every BRIDGE function that fails",
     "yes", "low", "high", "bridge function", "-", "P3", "moves last (shared by all BRIDGE code)"),
    (r"^sceKernelGetPrtAperture$", r".", 4, "partially resident texture aperture",
     "GPU memory (PRT)", "no", "high", "high", "none", "-", "P4", ""),
    (r"^sceKernel(GetDirectMemorySize|AllocateDirectMemory|ReleaseDirectMemory)$", r".", 3,
     "physical direct memory: budget, allocation, release",
     "release unmaps the views through the bridge's memory manager (patch 0007); the bridge's Map "
     "counts its views in the native map", "yes", "medium", "high", "bridge function + native tests",
     "kernel-dmem", "P1",
     "P2.3.1: NATIVE only with --native kernel-dmem (EXPERIMENTAL, BRIDGE by default; docs/PHASE2_P23_MEMORY.md)"),
    (r"^(sceKernel(\w*DirectMemory\w*|Mprotect|QueryMemoryProtection|\w*Mmap\w*|Munmap|\w*FlexibleMemory\w*"
     r"|Mlock)|mmap|msync)$", r".", 3, "guest address space: direct/flexible memory, protection",
     "Core::Memory address space shared with the GPU buffer/texture cache and write tracking",
     "with a MemoryForGpu adapter", "very high", "very high", "partial", "kernel-memory", "P3",
     "P2.3.2: own virtual memory manager first (docs/PHASE2_P23_MEMORY.md §9)"),
    (r"^sceKernel(\w*Equeue|\w*UserEvent)$", r".", 4, "event queues",
     "VideoOut flip and Gnm EOP events are delivered into the game's queues", "no", "high", "high",
     "bridge function", "kernel-equeue", "P4", "moves with VideoOut/GnmDriver"),
    (r".", r"libSceAudioOut", 3, "audio output ports", "SDL audio backend, output threads, timing",
     "yes", "high", "high", "partial (no audio comparison)", "audioout", "P3", ""),
    (r".", r"libSceAjm", 3, "audio decoder (ATRAC9 etc.)", "decoder instances, batches", "yes", "high",
     "high", "partial", "ajm", "P3", ""),
    (r".", r"libSceVideodec", 3, "video decoder (cut-scenes)", "FFmpeg decoder", "yes", "high", "high",
     "partial", "videodec", "P3", ""),
    (r".", r"libSceSaveData$", 3, "save data mount, search, parameters",
     "save mounts, param.sfo writing, icons (the user's saves)", "yes", "medium", "very high",
     "bridge function on a copy of the saves", "savedata", "P3", "save integrity first"),
    (r".", r"libSceNpTrophy", 3, "trophy contexts and unlocks", "trophy database files, notifications",
     "yes", "medium", "high", "bridge function", "trophy", "P3", ""),
    (r".", r"libSce(SaveDataDialog|ErrorDialog|MsgDialog|CommonDialog)", 3, "system dialogs",
     "the bridge's ImGui dialog UI", "yes", "high", "medium", "none (UI)", "dialog", "P3", ""),
    (r".", r"libSce(Http|Ssl)", 3, "HTTP/TLS client", "network stack, certificates", "yes", "high", "medium",
     "partial (offline)", "http", "P3", "start-up calls only"),
    (r".", r"libSceNet$", 3, "sockets, epoll, resolver", "host sockets, net errno, kernel fd table",
     "yes", "high", "medium", "partial (offline)", "net", "P3", "not called in offline play"),
    # --- Tier 4: GPU-coupled ---------------------------------------------------------------
    (r".", r"libSceGnmDriver", 4, "GPU command buffers, shaders, submission",
     "Vulkan renderer, Liverpool command processor", "no", "very high", "very high", "none", "-", "P4", ""),
    (r".", r"libSceVideoOut", 4, "display output, flips, buffers", "Vulkan presenter, flip events",
     "no", "high", "high", "none", "-", "P4", ""),
]


def classify(name: str, library: str) -> dict:
    for (np, lp, tier, purpose, deps, no_gpu, complexity, risk, oracle, group, prio, note) in RULES:
        if re.search(np, name) and re.search(lp, library):
            return dict(tier=tier, purpose=purpose, deps=deps, no_gpu=no_gpu, complexity=complexity,
                        risk=risk, oracle=oracle, group=group, priority=prio, note=note)
    return dict(tier=0, purpose="?", deps="?", no_gpu="?", complexity="?", risk="?", oracle="?",
                group="?", priority="?", note="UNCLASSIFIED")


def bridge_implementations() -> dict:
    """(nid, library) -> 'file:line symbol' from LIB_FUNCTION registrations."""
    pat = re.compile(r'LIB_FUNCTION\("([^"]+)",\s*"([^"]+)",\s*\d+,\s*"[^"]+",\s*([\w:]+)')
    out = {}
    for f in SHAD.rglob("*.cpp"):
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "LIB_FUNCTION" not in text:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            m = pat.search(line)
            if m:
                out.setdefault((m.group(1), m.group(2)),
                               f"{f.relative_to(SHAD).as_posix()}:{i} {m.group(3)}")
    return out


def load_calls(path: Path | None) -> dict:
    if path is None or not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    return {(e["nid"], e["library"]): e["calls"] for e in data["entries"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", type=Path, required=True)
    ap.add_argument("--startup", type=Path)
    ap.add_argument("--session", type=Path, action="append", default=[])
    ap.add_argument("--after", type=Path, help="run dir of the build after the migration: owner now")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "PHASE2_NATIVE_MIGRATION_MATRIX.md")
    a = ap.parse_args()

    pat = re.compile(r'^\{"([^"]*)", "([^"]*)", "([^"]*)", "([^"]*)", Kind::(\w+), "([^"]*)", ([\w|]+), Phase0::(\w+)')
    rows = []
    for line in (ROOT / "src/loader/generated/import_table.inc").read_text(encoding="utf-8").splitlines():
        m = pat.match(line)
        if m and m.group(8) != "Lle":
            rows.append(dict(nid=m[1], library=m[2], module=m[3], name=m[4], kind=m[5],
                             importers=m[7].replace("k", "").replace("|", "+")))
    def read_owners(run: Path) -> dict:
        out = {}
        for line in (run / "SecondSonPC.log").read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.search(r"\)\s+(NATIVE|OBSERVED|COMMON_STUB|UNRESOLVED)\s+(\S+)\s+(\S+)/(\S+)", line)
            if m:
                out[(m.group(2), m.group(3))] = m.group(1)
        return out

    owners = read_owners(a.run)
    owners_after = read_owners(a.after) if a.after else {}
    full = load_calls(a.run / "calls.json")
    startup = load_calls(a.startup)
    sessions = [load_calls(p) for p in a.session]
    impl = bridge_implementations()

    table = []
    owner_count = Counter()
    for r in rows:
        owner = owners.get((r["name"], r["library"]), "BRIDGE")
        owner_count[owner] += 1
        if owner != "BRIDGE":
            continue
        k = (r["nid"], r["library"])
        n_full, n_start = full.get(k, 0), startup.get(k, 0)
        n_session = max((s.get(k, 0) for s in sessions), default=0)
        c = classify(r["name"], r["library"])
        now = owners_after.get((r["name"], r["library"]), "BRIDGE") if a.after else "-"
        table.append(dict(r, **c, calls=n_full, startup=n_start,
                          gameplay=max(n_full - n_start, 0), session=n_session,
                          impl=impl.get(k, "-"), now=now))
    table.sort(key=lambda t: (t["tier"], t["library"], -t["calls"], t["name"]))

    def used(n: int) -> str:
        return f"yes ({n})" if n else "no"

    by_tier = defaultdict(list)
    for t in table:
        by_tier[t["tier"]].append(t)
    tier_names = {1: "Tier 1 — Low Risk", 2: "Tier 2 — Medium Risk", 3: "Tier 3 — High Risk",
                  4: "Tier 4 — GPU-Coupled", 0: "Unclassified"}
    out = [
        "# Phase 2 — матрица переноса BRIDGE-функций (аудит P2.2.3, состояние после P2.3.1)",
        "",
        f"Сгенерировано `tools\\migration_matrix.py` по прогону `{a.run}`"
        + (f", снимок конца загрузки `{a.startup}`" if a.startup else "")
        + "".join(f", сессия `{p}`" for p in a.session) + ".",
        "Таблица привязок прогона: " + ", ".join(f"{k} {v}" for k, v in sorted(owner_count.items()))
        + f" (без LLE). BRIDGE-строк в матрице: **{len(table)}**.",
        "",
        "Колонки: «Старт» — вызовы до конца загрузки (снимок), «Геймплей» — прирост после снимка в том же "
        "прогоне, «Сессия» — максимум вызовов в дополнительных длинных сессиях (граффити, бой). "
        "«Без GPU» — можно ли реализовать до собственного GPU-рантайма. «Оракул» — с чем сравнивать "
        "(BRIDGE-функция в `--selftest`). «Группа» — имя для `--bridge <группа>` (отдельный fallback). "
        "Приоритет: P1 — эта фаза, P2 — следующая kernel-фаза, P3 — фаза своей подсистемы, P4 — после "
        "собственного GPU-рантайма. Импортёры: Eboot / Libc (libc.prx) / Fios2 (libSceFios2.prx).",
        "",
        "## Сводка",
        "",
        "| Tier | функций | вызваны в прогоне или сессии | вызовов за прогон | NATIVE в прогоне «после» | по библиотекам |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for tier in sorted(by_tier):
        ts = by_tier[tier]
        libs = Counter(t["library"] for t in ts)
        out.append(f"| {tier_names[tier]} | {len(ts)} | {sum(1 for t in ts if t['calls'] or t['session'])} | "
                   f"{sum(t['calls'] for t in ts)} | {sum(1 for t in ts if t['now'] == 'NATIVE')} | "
                   + ", ".join(f"{l} {n}" for l, n in libs.most_common()) + " |")
    out += [
        "",
        "## Решения аудита (Phase 2.2.3)",
        "",
        "- **Tier 1 — 9 функций, все подходящие; переносятся в P2.2.3**: `scePthreadEqual`, `scePthreadYield`, `sched_yield`, "
        "`getpagesize`, `sigfillset` (группа `kernel-util`), `sceNetHtons`, `sceNetNtohs` (`net-util`), "
        "`memset`, `memcpy` libSceLibcInternal (`libc-mem`). Других чистых функций без общего с мостом "
        "состояния среди импортов нет.",
        "- **Не Tier 1, хотя логика простая**: `sceNetInetNtop/Pton` пишут errno libSceNet (его читает "
        "`sceNetErrnoLoc`) и через хук моста — errno ядра; `__error` — errno всех BRIDGE-функций; "
        "`sceKernelGetProcessTime` — часы моста, общие с метками VideoOut/AudioOut (NATIVE-обёртка звала бы "
        "мост); `sceKernelConvert*time` — у эталона ошибка единиц (минуты как секунды), эталона PS4 нет.",
        "- **Синхронизация**: мьютексы (19) и condvar (6) — одна группа `kernel-mutex`: condvar эталона "
        "работает с объектом мьютекса эталона и откладывает пробуждения через его флаг; `libc.prx` пишет "
        "флаг в объект мьютекса по `+0x20` — раскладка сохранена. RW-блокировки (7, только Fios2) — "
        "отдельная группа `kernel-rwlock`. Ни одна другая импортируемая BRIDGE-функция мьютексы/condvar "
        "игры не получает (`docs\\KERNEL_SYNC.md` §4).",
        "- **Блокеры остального**: потоки, TLS-ключи, сигналы — P2.4 (нужен свой менеджер потоков после "
        "P2.3); виртуальная память — P2.3.2 (свой менеджер и адаптер GPU-видимой памяти `MemoryForGpu`); "
        "event queues, VideoOut, GnmDriver — "
        "Tier 4 до собственного GPU-рантайма; аудио/видео-декодеры, сохранения, трофеи, диалоги, сеть — "
        "фазы своих подсистем (сохранения — только с проверкой целостности).",
        "",
        "## Решения P2.3.1 (память)",
        "",
        "- **Перенесено (экспериментально, по умолчанию BRIDGE)**: `sceKernelGetDirectMemorySize`, "
        "`sceKernelAllocateDirectMemory`, `sceKernelReleaseDirectMemory` — группа `kernel-dmem`, включается "
        "`--native kernel-dmem` вместе с учётом отображений моста в нативной карте (патч `0007`). "
        "Колонка «После» показывает прогон с этим ключом.",
        "- **Остаются BRIDGE**: `sceKernelMapDirectMemory`, `sceKernelMunmap`, `mmap`, "
        "`sceKernelMapNamedFlexibleMemory`, `sceKernelMprotect`, `sceKernelQueryMemoryProtection` — "
        "виртуальная память (vma_map, плейсхолдеры, подложка, защита страниц) общая с GPU-мостом; `msync`, "
        "`sceKernelMlock`, `sceKernelGetPrtAperture` — семантика неизвестна, вызовов нет "
        "(`docs\\PHASE2_P23_MEMORY.md`).",
    ]
    for tier in sorted(by_tier):
        out += ["", f"## {tier_names[tier]}", "",
                "| Функция | NID | Библиотека | Импортёры | Назначение | Старт | Геймплей | Сессия | "
                "Реализация моста | Зависимости от shadPS4 | Без GPU | Сложность | Риск | Оракул | Группа | "
                "Приоритет | После | Заметки |",
                "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for t in by_tier[tier]:
            out.append(f"| `{t['name']}` | `{t['nid']}` | {t['library']} | {t['importers']} | {t['purpose']} | "
                       f"{used(t['startup']) if a.startup else '-'} | "
                       f"{used(t['gameplay']) if a.startup else used(t['calls'])} | {used(t['session'])} | "
                       f"{t['impl']} | {t['deps']} | {t['no_gpu']} | {t['complexity']} | {t['risk']} | "
                       f"{t['oracle']} | {t['group']} | {t['priority']} | "
                       f"{'**NATIVE**' if t['now'] == 'NATIVE' else t['now']} | {t['note']} |")
    a.out.write_text("\n".join(out) + "\n", encoding="utf-8")
    with a.out.with_suffix(".csv").open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        cols = ["name", "nid", "library", "module", "importers", "tier", "purpose", "startup", "gameplay",
                "calls", "session", "impl", "deps", "no_gpu", "complexity", "risk", "oracle", "group",
                "priority", "now", "note"]
        w.writerow(cols)
        for t in table:
            w.writerow([t[c] for c in cols])
    print(f"{len(table)} BRIDGE rows -> {a.out}")
    for tier in sorted(by_tier):
        print(f"  {tier_names[tier]}: {len(by_tier[tier])}")
    unclassified = [t["name"] for t in by_tier.get(0, [])]
    if unclassified:
        print("UNCLASSIFIED:", ", ".join(unclassified))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
