"""Phase 3 table of every BRIDGE import -> docs/PHASE3_BRIDGE_DEPENDENCIES.md.

    python -I E:\\SecondSonPC\\tools\\phase3_deps.py --native-run <matrix.csv> --default-run <matrix.csv>
                  [--history docs/PHASE2_NATIVE_MIGRATION_MATRIX.csv] [--out FILE]

Input: CSVs written by tools/migration_matrix.py for two runs of the same build — --native-run with
SecondSonPC's threads and memory (--native threads --native memory: the fewest BRIDGE rows) and
--default-run with the defaults (the thread functions are BRIDGE there too). --history: the P2.2.3
matrix, whose "session" column holds long manual sessions (graffiti, fights); its calls count as use.

Classes (docs/PHASE3_INDEPENDENT_RUNTIME.md §3):
  A  the game's system functions (libkernel, libc): SecondSonPC's runtime must own them;
  B  the graphics bridge (GnmDriver, VideoOut, the event queues their events go through): Phase 4;
  C  subsystem services that stay adapted until their own phase (audio, video decoding, save data,
     NP, network, trophies, dialogs) — called in the recorded runs;
  D  imported, never called in any recorded run or session (and not class A or B).
"""
from __future__ import annotations

import argparse
import csv
from collections import Counter
from pathlib import Path

ROOT = Path(r"E:\SecondSonPC")

GRAPHICS_LIBS = {"libSceGnmDriver", "libSceVideoOut"}
EQUEUE = {"sceKernelCreateEqueue", "sceKernelDeleteEqueue", "sceKernelWaitEqueue", "sceKernelAddUserEvent",
          "sceKernelDeleteUserEvent", "sceKernelTriggerUserEvent"}
SYSTEM_LIBS = {"libkernel", "libScePosix", "libSceLibcInternal", "libSceLibcInternalExt"}

MEMORY_API = "libkernel memory API (BRIDGE code over the selected manager, patch 0012) -> SecondSonPC's"
# Phase 3 decision for class A, by exact name, then by prefix: (need, autonomous, note).
EXACT = {
    "__error": ("P3.5", "yes, with an adapter",
                "errno location: one owner for NATIVE and BRIDGE functions — the bridge's __Error must return "
                "SecondSonPC's"),
    "sceKernelGetDirectMemorySize": ("P3.5", "yes", MEMORY_API + "; native with --native kernel-dmem (P2.3.1)"),
    "sceKernelAllocateDirectMemory": ("P3.5", "yes", MEMORY_API + "; native with --native kernel-dmem (P2.3.1)"),
    "sceKernelReleaseDirectMemory": ("P3.5", "yes", MEMORY_API + "; native with --native kernel-dmem (P2.3.1)"),
    "sceKernelMunmap": ("P3.5", "yes", MEMORY_API),
    "sceKernelMprotect": ("P3.5", "yes", MEMORY_API),
    "sceKernelQueryMemoryProtection": ("P3.5", "yes", MEMORY_API),
    "sceKernelMlock": ("P3.5", "yes", MEMORY_API + "; semantics to confirm"),
    "mmap": ("P3.5", "yes", MEMORY_API),
    "msync": ("P3.5", "yes", MEMORY_API + "; semantics to confirm"),
    "signal": ("P3.4", "yes", "no guest signal delivery in either runtime; the game installs no handler"),
    "sigprocmask": ("P3.4", "yes", "no guest signal delivery in either runtime"),
    "_sigprocmask": ("P3.4", "yes", "no guest signal delivery in either runtime"),
    "sceKernelGetProcessTime": ("P3.5", "yes, with an adapter",
                                "process clock shared with VideoOut/AudioOut timestamps: SecondSonPC's clock, read "
                                "by the bridge"),
    "_exit": ("P3.4", "yes", "process shutdown (crash/exit path)"),
    "fflush": ("later", "yes", "libc FILE of the bridge (Fios2, once at start)"),
    "sceLibcHeapGetTraceInfo": ("later", "yes", "once at start"),
    "sceKernelGetPrtAperture": ("Phase 4", "no", "partially resident textures: GPU memory"),
}
PREFIX = [
    ("scePthread", ("P3.2: own manager by default", "yes",
                    "SecondSonPC's manager exists (--native threads, P2.4); default after the long session")),
    ("pthread_", ("P3.2: own manager by default", "yes", "POSIX names of the thread functions")),
    ("sceKernelMap", ("P3.5", "yes", MEMORY_API)),
    ("sceKernelConvert", ("later", "yes", "the bridge's results are wrong (minutes used as seconds); not called")),
    ("sceKernelDebugRaiseException", ("P3.4", "yes", "fatal path: crash report and shutdown")),
]


def decision(name: str, cls: str) -> tuple[str, str, str]:
    if cls == "B":
        if name in EQUEUE:
            return ("Phase 4 (explicit adapter in P3.6)", "no",
                    "VideoOut flip and Gnm EOP events are delivered into the game's queues by the graphics bridge")
        if name in EXACT:
            return EXACT[name]
        return ("Phase 4", "no", "graphics bridge")
    if cls == "A":
        if name in EXACT:
            return EXACT[name]
        for prefix, value in PREFIX:
            if name.startswith(prefix):
                return value
        return ("P3.5", "?", "")
    if cls == "C":
        return ("subsystem phase", "yes (own subsystem)", "kept adapted: called, not a runtime mechanism")
    return ("no (stays the bridge's)", "-", "never called in the recorded runs")


def load(path: Path) -> list[dict]:
    with path.open(encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def num(v: str) -> int:
    try:
        return int(v or 0)
    except ValueError:
        return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--native-run", type=Path, required=True)
    ap.add_argument("--default-run", type=Path, required=True)
    ap.add_argument("--history", type=Path, default=ROOT / "docs" / "PHASE2_NATIVE_MIGRATION_MATRIX.csv")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "PHASE3_BRIDGE_DEPENDENCIES.md")
    a = ap.parse_args()

    native = {r["nid"] + "|" + r["library"]: r for r in load(a.native_run)}
    default = {r["nid"] + "|" + r["library"]: r for r in load(a.default_run)}
    history = {r["nid"] + "|" + r["library"]: r for r in load(a.history)} if a.history.exists() else {}

    rows = []
    for key, r in default.items():
        h = history.get(key, {})
        in_native = key in native
        calls_native = num(native[key]["calls"]) if in_native else 0
        calls_default = num(r["calls"])
        calls_hist = max(num(h.get("calls", "")), num(h.get("session", "")))
        used = max(calls_native, calls_default, calls_hist) > 0
        lib, name = r["library"], r["name"]
        if lib in GRAPHICS_LIBS or name in EQUEUE or name == "sceKernelGetPrtAperture":
            cls = "B"
        elif lib in SYSTEM_LIBS:
            cls = "A"
        elif used:
            cls = "C"
        else:
            cls = "D"
        need, auto, note = decision(name, cls)
        if r["note"]:
            note = f"{note}; {r['note']}" if note else r["note"]
        rows.append({
            "cls": cls, "name": name, "nid": r["nid"], "library": lib, "importers": r["importers"],
            "native": str(calls_native) if in_native else "NATIVE", "default": calls_default, "hist": calls_hist,
            "used": used, "purpose": r["purpose"], "deps": r["deps"], "complexity": r["complexity"],
            "risk": r["risk"], "need": need, "auto": auto, "note": note,
        })

    order = {"A": 0, "B": 1, "C": 2, "D": 3}
    rows.sort(key=lambda x: (order[x["cls"]], -max(x["default"], x["hist"]), x["library"], x["name"]))
    by_cls = Counter(x["cls"] for x in rows)
    used_cls = Counter(x["cls"] for x in rows if x["used"])
    calls_cls = Counter()
    for x in rows:
        calls_cls[x["cls"]] += x["default"]

    titles = {
        "A": "A — обязательные системные функции игры",
        "B": "B — графический мост (Phase 4)",
        "C": "C — сервисы подсистем, остаются адаптированными до своей фазы",
        "D": "D — импортируются, но не вызывались",
    }
    out = []
    out.append("# Phase 3 — таблица BRIDGE-зависимостей (гостевые импорты)\n")
    out.append("Сгенерировано `tools\\phase3_deps.py` из матриц `tools\\migration_matrix.py` двух прогонов одной сборки "
               "(`logs\\performance\\p25_final_ab`, HEAD `2d1cafe`, патчи 0001–0020 — эталон Phase 3): «По умолч.» = "
               f"прогон `PO1_1` (BRIDGE {len(default)}), «TM» = прогон `TM` (`--native threads --native memory "
               f"--memory-commit on-demand`, BRIDGE {len(native)}). «История» — максимум вызовов из матрицы P2.2.3 "
               "(`docs\\PHASE2_NATIVE_MIGRATION_MATRIX.csv`: прогон и длинные ручные сессии — граффити, бой). Прогон = "
               "98 с: загрузка «Продолжить» → пляж Salmon Bay → поворот камеры в густую листву.\n")
    out.append("Это таблица **гостевых импортов** (метрика 1). Внутренние зависимости runtime от shadPS4 (слот TCB, патчер "
               "`fs:`, VEH, потоки моста, часы, старт библиотек моста) — метрика 2, `docs\\PHASE3_INDEPENDENT_RUNTIME.md` §4. "
               "P3.3 (свой слот TCB и патчер `fs:`) импорты не меняет: это внутренние механизмы.\n")
    out.append("Классы: **A** — системные функции игры (libkernel, libc): runtime SecondSonPC должен владеть ими; **B** — "
               "графический мост (GnmDriver, VideoOut и очереди событий, в которые мост доставляет флипы и EOP) — Phase 4; "
               "**C** — сервисы подсистем (звук, видео, сохранения, NP, сеть, трофеи, диалоги), вызываются, остаются "
               "адаптированными до своей фазы; **D** — импортируются, ни разу не вызваны в записанных прогонах и сессиях.\n")
    out.append("## Сводка\n")
    out.append("| Класс | функций (по умолч.) | вызваны хоть раз | вызовов за прогон по умолч. |")
    out.append("|---|---:|---:|---:|")
    for c in "ABCD":
        out.append(f"| {titles[c]} | {by_cls[c]} | {used_cls[c]} | {calls_cls[c]} |")
    out.append(f"| **всего BRIDGE по умолчанию** | **{len(rows)}** | {sum(used_cls.values())} | "
               f"{sum(calls_cls.values())} |\n")
    out.append("Колонки: «TM» — вызовов за прогон в режиме TM (NATIVE — там функция уже своя), «По умолч.» — за прогон по "
               "умолчанию, «История» — максимум из прогона/сессий P2.2.3; «Перенос» — подэтап Phase 3 (P3.2–P3.6), "
               "«Phase 4» или «позже»; «Автономно» — можно ли реализовать без графического моста.\n")
    for c in "ABCD":
        out.append(f"## {titles[c]}\n")
        out.append("| Функция | NID | Библиотека | Импортёры | TM | По умолч. | История | Назначение | Зависимости в мосте | "
                   "Сложность | Риск | Перенос | Автономно | Примечание |")
        out.append("|---|---|---|---|---:|---:|---:|---|---|---|---|---|---|---|")
        for x in rows:
            if x["cls"] != c:
                continue
            out.append(f"| `{x['name']}` | `{x['nid']}` | {x['library']} | {x['importers']} | {x['native']} | "
                       f"{x['default']} | {x['hist']} | {x['purpose']} | {x['deps']} | {x['complexity']} | {x['risk']} | "
                       f"{x['need']} | {x['auto']} | {x['note']} |")
        out.append("")
    a.out.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{len(rows)} BRIDGE rows -> {a.out}")
    for c in "ABCD":
        print(f"  {c}: {by_cls[c]} ({used_cls[c]} called, {calls_cls[c]} calls)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
