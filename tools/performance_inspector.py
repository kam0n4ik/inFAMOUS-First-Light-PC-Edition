# SecondSonPC P4.8 Performance Inspector - the report of an inspector session (docs\PHASE4_P48_PERFORMANCE_INSPECTOR.md).
#   py -I tools\performance_inspector.py report <session dir> [--phase name=from-to ...] [--log SecondSonPC.log] [--out x.html]
#   py -I tools\performance_inspector.py compare <session A> <session B> [--phase name=from-to] [--out ab.html]
# A session directory is what the runtime wrote with SSPC_INSPECTOR=light (src\runtime\gpu\inspector.h): frames.csv,
# seconds.csv, threads.csv, cpu_stacks.csv, stacks.csv, modules.csv, markers.csv, pipelines.csv, session.json.
# Output: <session>\report.html (opens offline: inline SVG, no scripts from the network) and summary.json.
#
# What is measured, and how (the report says it next to each figure):
#  - the GPU thread's time between two flip markers is split exactly by its own timers: CP work (PM4, recording,
#    submissions, polls), waits for the GPU (explicit synchronizations, and the previous frame's GPU tail the game
#    waits for), waits for the game's CPU (nothing submitted yet), waits for the display (a VideoOut label);
#  - the GPU's time per frame: the union of the frame's batch intervals (timestamps), per effect category: timestamps
#    at category changes only (all guest queues run on one Vulkan queue here: no overlap to double count);
#  - recording CPU time per category: each draw/dispatch timed from its start to its end (TSC);
#  - CPU time by subsystem: the GPU thread's call stack sampled (250 Hz), symbolized offline - a statistical estimate;
#  - the game's flipping thread: CPU time per frame (cycle counter) and instruction pointer samples.
import argparse
import bisect
import csv
import html
import json
import math
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VBLANK_MS = 1001.0 / 60.0  # 59.94 Hz, the native VideoOut's present grid

CATS = ["Unknown", "Smoke", "Fire", "Particles", "ParticleSimulation", "ParticleSorting", "Foliage", "Shadows", "Water",
        "Reflections", "Lighting", "PostProcessing", "Terrain", "Geometry", "ComputeOther"]
CAT_RU = {
    "Unknown": "Не классифицировано", "Smoke": "Дым", "Fire": "Огонь", "Particles": "Частицы (отрисовка)",
    "ParticleSimulation": "Симуляция частиц (общая)", "ParticleSorting": "Сортировка частиц", "Foliage": "Листва",
    "Shadows": "Тени (карты теней)", "Water": "Вода", "Reflections": "Отражения", "Lighting": "Освещение",
    "PostProcessing": "Постобработка", "Terrain": "Ландшафт", "Geometry": "Геометрия / G-буфер",
    "ComputeOther": "Прочие compute", "gx_transfer": "Копии и write-back (GPU→хост)", "gx_flipcopy": "Копия кадра в презентер",
    "gx_batchstart": "Начало пакетов",
}


# ------------------------------------------------------------------------------------------------ loading

def num(s):
    if s is None or s == "":
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def load_columns(path):
    """CSV -> (header list, dict name -> list of floats)."""
    with open(path, newline="") as f:
        r = csv.reader(f)
        header = next(r)
        cols = [[] for _ in header]
        for row in r:
            for i in range(len(header)):
                cols[i].append(num(row[i]) if i < len(row) else 0.0)
    return header, {h: cols[i] for i, h in enumerate(header)}


def load_dicts(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return list(csv.DictReader(f))


class Session:
    def __init__(self, d):
        self.dir = d
        self.meta = json.load(open(os.path.join(d, "session.json"), encoding="utf-8"))
        self.header, self.c = load_columns(os.path.join(d, "frames.csv"))
        self.n = len(self.c["n"])
        self.cats = [h[2:] for h in self.header if h.startswith("d_")]
        self.seconds = load_dicts(os.path.join(d, "seconds.csv"))
        self.threads = load_dicts(os.path.join(d, "threads.csv"))
        self.pipelines = load_dicts(os.path.join(d, "pipelines.csv"))
        self.markers = load_dicts(os.path.join(d, "markers.csv"))
        self.modules = load_dicts(os.path.join(d, "modules.csv"))
        self.name = os.path.basename(os.path.normpath(d))

    def col(self, k):
        return self.c.get(k, [0.0] * self.n)


# ------------------------------------------------------------------------------------------------ statistics

def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))]


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def fmt(x, d=1):
    if x is None or (isinstance(x, float) and (math.isnan(x) or math.isinf(x))):
        return "—"
    return f"{x:,.{d}f}".replace(",", " ").replace(".", ",")


# ------------------------------------------------------------------------------------------------ frames

class Frames:
    """Per-frame derived values of a session within [t0, t1] seconds (by the flip marker's time)."""

    def __init__(self, s, t0=None, t1=None):
        c = s.col
        cp = c("cp_flip_us")
        idx = [i for i in range(1, s.n) if cp[i] > 0 and cp[i - 1] > 0 and
               (t0 is None or cp[i] >= t0 * 1e6) and (t1 is None or cp[i] <= t1 * 1e6)]
        self.idx = idx
        self.s = s
        ms = lambda k, i: c(k)[i] / 1000.0
        self.t = [cp[i] / 1e6 for i in idx]
        self.cpi = [(cp[i] - cp[i - 1]) / 1000.0 for i in idx]
        pres = c("present_us")
        self.present_ft = [(pres[i] - pres[i - 1]) / 1000.0 for i in idx if pres[i] > 0 and pres[i - 1] > 0]
        gs, gd = c("game_submit_us"), c("gpu_done_us")
        self.rows = []
        for i in idx:
            pre, rnd, poll, flush = ms("pre_us", i), ms("round_us", i), ms("poll_us", i), ms("flush_us", i)
            lab, oth, emp = ms("idle_label_us", i), ms("idle_other_us", i), ms("idle_empty_us", i)
            sync = ms("sync_cp_us", i) + ms("sync_front_us", i) + ms("sync_read_us", i)
            tail_prev = max(0.0, (gd[i - 1] - cp[i - 1]) / 1000.0) if gd[i - 1] > 0 else 0.0
            answer = max(0.0, (gs[i] - gd[i - 1]) / 1000.0) if gs[i] > 0 and gd[i - 1] > 0 else 0.0
            tail_wait = min(emp, tail_prev)
            game_wait = (emp - tail_wait) + oth
            cp_cpu = max(0.0, pre + rnd - sync) + poll + flush
            record = ms("record_us", i)
            shader = ms("shader_us", i)
            pipeline = ms("pipeline_us", i) + ms("module_us", i)
            r = {
                "cpi": (cp[i] - cp[i - 1]) / 1000.0,
                "cp_cpu": cp_cpu, "gpu_wait": sync + tail_wait, "game_wait": game_wait, "output_wait": lab,
                "sync": sync, "tail": tail_prev, "answer": answer,
                "pre": pre, "round": rnd, "poll": poll, "flush": flush, "record": record,
                "pm4": max(0.0, rnd - record - ms("sync_cp_us", i) - ms("sync_read_us", i)),
                "record_other": max(0.0, record - shader - pipeline - ms("sync_front_us", i)),
                "shader": shader, "pipeline": pipeline,
                "gpu": ms("gpu_frame_us", i), "gpu_timed": c("gpu_timed")[i] > 0, "gpu_batches": c("gpu_batches")[i],
                "draws": c("draws")[i], "dispatches": c("dispatches")[i],
                "flip_cpu": ms("flip_thread_us", i),
                "wb_kib": c("wb_kib")[i], "upload_kib": c("buf_up_kib")[i] + c("img_up_kib")[i],
                "stream_kib": c("stream_kib")[i], "barriers": c("barriers")[i], "submit": ms("submit_us", i),
                "file_reads": c("file_reads")[i], "file_kib": c("file_kib")[i], "file_ms": ms("file_us", i),
                "shaders": c("shaders")[i], "pipelines_new": c("pipelines")[i],
            }
            for cat in s.cats:
                r["d_" + cat] = c("d_" + cat)[i]
                r["x_" + cat] = c("x_" + cat)[i]
                r["cpu_" + cat] = ms(f"cpu_{cat}_us", i)
                r["gpu_" + cat] = ms(f"gpu_{cat}_us", i)
            for k in ("gx_transfer", "gx_flipcopy", "gx_batchstart"):
                r["gpu_" + k] = ms(k + "_us", i)
            for k in ("front", "bind", "range", "image", "pipe", "targets", "vertex", "layout", "emit", "dispatch"):
                r["st_" + k] = ms(f"st_{k}_us", i)
            self.rows.append(r)

    def vals(self, k, timed_only=False):
        return [r[k] for r in self.rows if (r["gpu_timed"] or not timed_only)]

    def fps(self):
        ft = self.present_ft
        if len(ft) < 2:
            return {}
        worst = sorted(ft, reverse=True)[: max(1, len(ft) // 100)]
        out = {
            "frames": len(ft), "avg_fps": 1000.0 * len(ft) / sum(ft), "median_fps": 1000.0 / pct(ft, 0.5),
            "low1_fps": 1000.0 / mean(worst), "p95_ms": pct(ft, 0.95), "p99_ms": pct(ft, 0.99), "max_ms": max(ft),
            "mean_ms": mean(ft),
        }
        for lim in (33.4, 50, 100, 200, 500, 1000):
            out[f"over_{lim}"] = sum(1 for x in ft if x >= lim)
        return out


# ------------------------------------------------------------------------------------------------ limiter & model

PARTS = [("cp_cpu", "CPU командного процессора (PM4, подготовка и запись команд)"),
         ("gpu_wait", "Ожидание GPU (синхронизации + хвост GPU прошлого кадра)"),
         ("game_wait", "Ожидание CPU игры (кадр ещё не отправлен)"),
         ("output_wait", "Ожидание вывода (метка VideoOut, VSync)")]
LIMITER_RU = {"cp_cpu": "CPU-bound (командный процессор SecondSonPC)", "gpu_wait": "GPU-bound",
              "game_wait": "CPU игры (логика игры / её ожидания)", "output_wait": "ограничение вывода (VSync)",
              "mixed": "смешанный"}


def limiter_of(r):
    parts = sorted(((r[k], k) for k, _ in PARTS), reverse=True)
    if parts[0][0] - parts[1][0] < 0.1 * max(r["cpi"], 1e-6):
        return "mixed"
    return parts[0][1]


def predict(rows, kind, amount_ms_per_frame):
    """Mean frame time after `amount` ms less of `kind` work per frame (per-frame model, see docs section 6):
    cp: CP CPU work less, the frame cannot go below the GPU's work plus the game's and output waits;
    gpu: GPU work less, only the GPU waits shrink, the frame cannot go below the CP's work plus the waits;
    game: the game's wait shrinks."""
    out = []
    for r in rows:
        cpi = r["cpi"]
        if kind == "cp":
            x = min(amount_ms_per_frame, r["cp_cpu"])
            floor = r["gpu"] + r["game_wait"] + r["output_wait"]
            new = max(cpi - x, floor) if floor < cpi else cpi
        elif kind == "gpu":
            x = min(amount_ms_per_frame, r["gpu_wait"])
            floor = r["cp_cpu"] + r["game_wait"] + r["output_wait"]
            new = max(cpi - x, floor)
        else:
            new = cpi - min(amount_ms_per_frame, r["game_wait"])
        out.append(max(new, VBLANK_MS))
    return mean(out)


# ------------------------------------------------------------------------------------------------ symbols

class Symbols:
    """Module of an address; exe functions by llvm-symbolizer (with the PDB); system DLLs by their exports."""

    def __init__(self, modules, exe_path=None):
        self.mods = []
        self.guest = set()
        for m in modules:
            try:
                self.mods.append((int(m["base"], 16), int(m["size"], 16), m["name"], m.get("path", "").strip('"')))
                if m.get("guest"):
                    self.guest.add(m["name"])
            except (KeyError, ValueError):
                pass
        self.mods.sort()
        self.bases = [m[0] for m in self.mods]
        self.exe = next((m for m in self.mods if m[2].lower() == "secondsonpc.exe"), None)
        self.exe_path = exe_path or (self.exe[3] if self.exe else None)
        self.names = {}
        self.exports = {}

    def module(self, a):
        i = bisect.bisect_right(self.bases, a) - 1
        if i >= 0:
            b, sz, name, path = self.mods[i]
            if a < b + sz:
                return self.mods[i]
        return None

    def _exports(self, path):
        if path in self.exports:
            return self.exports[path]
        table = []
        try:
            data = open(path, "rb").read()
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            nsec = struct.unpack_from("<H", data, pe + 6)[0]
            opt_size = struct.unpack_from("<H", data, pe + 20)[0]
            opt = pe + 24
            exp_rva = struct.unpack_from("<I", data, opt + 112)[0]
            secs = []
            for k in range(nsec):
                so = opt + opt_size + 40 * k
                va, vsz, raw_sz, raw = struct.unpack_from("<IIII", data, so + 8)
                secs.append((va, max(vsz, raw_sz), raw))

            def off(rva):
                for va, sz, raw in secs:
                    if va <= rva < va + sz:
                        return rva - va + raw
                return None
            eo = off(exp_rva)
            if eo is not None:
                nfun, nnames, afun, aname, aord = struct.unpack_from("<IIIII", data, eo + 20)
                fo, no, oo = off(afun), off(aname), off(aord)
                for k in range(nnames):
                    name_rva = struct.unpack_from("<I", data, no + 4 * k)[0]
                    ordi = struct.unpack_from("<H", data, oo + 2 * k)[0]
                    frva = struct.unpack_from("<I", data, fo + 4 * ordi)[0]
                    so = off(name_rva)
                    end = data.index(b"\0", so)
                    table.append((frva, data[so:end].decode("ascii", "replace")))
        except (OSError, struct.error, ValueError, TypeError):
            table = []
        table.sort()
        self.exports[path] = table
        return table

    def resolve(self, addrs):
        exe_addrs = []
        for a in addrs:
            m = self.module(a)
            if m is None:
                self.names[a] = ("guest", f"guest:{a:#x}")
            elif m[2] in self.guest:
                self.names[a] = ("guest", f"{m[2]}+{a - m[0]:#x}")
            elif self.exe and m[0] == self.exe[0]:
                exe_addrs.append(a)
            else:
                table = self._exports(m[3]) if m[3] else []
                rva = a - m[0]
                j = bisect.bisect_right(table, (rva, "￿")) - 1
                nm = table[j][1] if j >= 0 and rva - table[j][0] < 0x2000 else f"+{rva:#x}"
                self.names[a] = (m[2].lower(), f"{m[2]}!{nm}")
        sym = os.path.join(ROOT, "toolchain", "LLVM", "bin", "llvm-symbolizer.exe")
        resolved = {}
        if exe_addrs and self.exe_path and os.path.exists(sym) and os.path.exists(self.exe_path):
            try:
                pref = self._image_base(self.exe_path)
                feed = "\n".join(f"{a - self.exe[0] + pref:#x}" for a in exe_addrs) + "\n"
                out = subprocess.run([sym, f"--obj={self.exe_path}", "--no-inlines", "--functions=linkage"],
                                     input=feed, capture_output=True, text=True, timeout=600).stdout
                blocks = [b for b in out.split("\n\n")]
                for a, blk in zip(exe_addrs, blocks):
                    line = blk.strip().splitlines()[0] if blk.strip() else "??"
                    resolved[a] = line
            except (OSError, subprocess.SubprocessError, ValueError):
                pass
        for a in exe_addrs:
            self.names[a] = ("exe", resolved.get(a, f"SecondSonPC.exe+{a - self.exe[0]:#x}"))

    @staticmethod
    def _image_base(path):
        data = open(path, "rb").read(4096)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        return struct.unpack_from("<Q", data, pe + 24 + 24)[0]

    def name(self, a):
        return self.names.get(a, ("?", f"{a:#x}"))


WAIT_RE = re.compile(r"NtWaitFor|ZwWaitFor|NtDelayExecution|ZwDelayExecution|NtWaitForAlertByThreadId|"
                     r"SleepConditionVariable|_Cnd_timedwait|_Cnd_wait|WaitForSingleObject|WaitForMultipleObjects|"
                     r"NtRemoveIoCompletion|_Thrd_sleep|^Sleep|!Sleep|RtlSleepConditionVariable|NtYieldExecution")
DRIVER_MODS = ("nvoglv64.dll", "nvgpucomp64.dll", "vulkan-1.dll", "nvldumdx.dll", "amdvlk64.dll")

GPU_RULES = [  # (regex on the exe function name, bucket); the first match from the leaf decides
    (re.compile(r"^Shader::|^Sirit::|ShaderTranslator|TranslateProgram|EmitSPIRV|shader_translator_host"),
     "Трансляция шейдеров GCN→SPIR-V"),
    (re.compile(r"PipelineFactory::"), "Создание pipeline (без драйвера)"),
    (re.compile(r"BindStage|PushDescriptors"), "Привязка ресурсов (дескрипторы)"),
    (re.compile(r"GuestRange|BufferCache::|GuestBuffers::|StreamRing::|PrepareShadows|FinishShadows|CopySparse|"
                r"TryWriteBacking|BackingRun"), "Буферы гостя (диапазоны, копии, кольцо)"),
    (re.compile(r"SrtProgram|ShaderFrontend::|ProgramCache::|PipelineStateCache|SpecializationKey|ResolveResources|"
                r"DecodeResources|TimelineReader|Gpu::Shader::|RuntimeParams"), "Фронтенд шейдеров (подготовка стадий)"),
    (re.compile(r"ImageCache::|LayoutFromTSharp|ImageLayout|Detile|image_layout"), "Образы (текстуры, цели)"),
    (re.compile(r"Executor::Draw|SetDynamicState|Executor::Dispatch|SplitMergeLevels|NeedsBarrier|NoteAccesses|"
                r"GlobalBarrier|EndRendering|Executor::InspMark|Executor::Insp"), "Запись draw/dispatch (состояние, барьеры)"),
    (re.compile(r"Executor::Flush|Executor::Poll|Executor::Begin|Timeline::|RecordWriteBack|Executor::Fence|"
                r"Executor::Flip|Presenter::"), "Отправка и опрос пакетов, флип"),
    (re.compile(r"CommandProcessor|Pm4|RegisterFile|Cp::"), "PM4 / командный процессор"),
    (re.compile(r"PageTracker|DrainCpuWrites|::Invalidate"), "Инвалидации (записи CPU игры)"),
    (re.compile(r"Inspector::|FrameLog::"), "Сам профилировщик (Inspector)"),
]
MEMCPY_RE = re.compile(r"^(memmove|memcpy|memset|__memmove|__memcpy|__memset)")


def classify_gpu_stack(frames, sym):
    names = [sym.name(a) for a in frames]
    if not names:
        return "?", ""
    leaf_mod, leaf = names[0]
    exe_names = [n for m, n in names if m == "exe"]
    context = exe_names[0] if exe_names else ""
    first_exe = exe_names[0] if exe_names else ""
    if re.search(r"Timeline::Wait|Executor::WaitOldest", first_exe):
        return "Ожидание GPU (пакет в полёте / Synchronize)", context
    if any(WAIT_RE.search(n) for m, n in names[:4]):
        ctx = " ".join(exe_names[:4])
        if re.search(r"Timeline::Wait|WaitOldest|SyncFrom|Synchronize", ctx):
            return "Ожидание GPU (Synchronize / пакет в полёте)", context
        if re.search(r"Backend::Loop", ctx):
            return "Простой потока GPU (нет работы / ждёт игру)", context
        return "Ожидание (прочее)", context
    if leaf_mod in DRIVER_MODS:
        return "Драйвер Vulkan (NVIDIA)", context
    for m, n in names:
        if m != "exe":
            continue
        if MEMCPY_RE.search(n):
            continue
        for rx, bucket in GPU_RULES:
            if rx.search(n):
                return bucket, context
        if "Backend::Loop" in n:
            return "Цикл потока GPU (прочее)", context
    if leaf_mod == "exe":
        return ("Копирование памяти" if MEMCPY_RE.search(leaf) else "Прочее SecondSonPC"), context
    return "Система Windows", context


def classify_game_stack(frames, sym):
    names = [sym.name(a) for a in frames]
    if not names:
        return "?", ""
    leaf_mod, leaf = names[0]
    ctx = " ".join(n for m, n in names[1:3] if m == "exe")
    if leaf_mod == "guest":
        return "Код игры (гостевой)", leaf
    if WAIT_RE.search(leaf):
        return "Ожидание в ядре Windows (блокировка)", ctx
    if leaf_mod == "exe":
        if re.search(r"Gnm|Gpu::", leaf):
            return "SecondSonPC: Gnm (отправка команд)", leaf
        if re.search(r"Kernel::|Thread|Mutex|Semaphore|EventFlag|Cond", leaf):
            return "SecondSonPC: ядро PS4 (HLE)", leaf
        if re.search(r"Memory|Vmm|PageTracker|Exception|Signal", leaf):
            return "SecondSonPC: память / исключения (HLE)", leaf
        if MEMCPY_RE.search(leaf):
            return "Копирование памяти", ctx
        return "SecondSonPC: прочее HLE", leaf
    return "Система Windows / драйвер", leaf


def load_samples(s, t0=None, t1=None):
    stacks = {}
    p = os.path.join(s.dir, "stacks.csv")
    if not os.path.exists(p):
        return {}, {}
    for r in load_dicts(p):
        try:
            stacks[int(r["id"])] = (r["thread"], [int(x, 16) for x in r["frames"].split(";") if x])
        except (KeyError, ValueError):
            pass
    counts = defaultdict(Counter)  # thread -> stack id -> count
    per_sec = defaultdict(lambda: defaultdict(Counter))
    for r in load_dicts(os.path.join(s.dir, "cpu_stacks.csv")):
        t = num(r["t_s"])
        if (t0 is not None and t < t0) or (t1 is not None and t > t1 + 1):
            continue
        sid, cnt = int(r["stack"]), int(num(r["count"]))
        counts[r["thread"]][sid] += cnt
        per_sec[r["thread"]][int(t)][sid] += cnt
    return stacks, (counts, per_sec)


# ------------------------------------------------------------------------------------------------ SVG charts

PALETTE = ["#4e79a7", "#f28e2b", "#e15759", "#76b7b2", "#59a14f", "#edc948", "#b07aa1", "#ff9da7", "#9c755f",
           "#bab0ac", "#1f77b4", "#8c564b", "#17becf", "#bcbd22", "#7f7f7f", "#d62728", "#2ca02c", "#9467bd"]


def svg_chart(series, title, ylabel, width=980, height=260, stacked=False, ymax=None, hlines=()):
    """series: [(name, xs, ys)]; returns inline SVG."""
    pts = [(x, y) for _, xs, ys in series for x, y in zip(xs, ys)]
    if not pts:
        return f"<p class=muted>{html.escape(title)}: нет данных</p>"
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    if x1 <= x0:
        x1 = x0 + 1
    if stacked:
        acc = defaultdict(float)
        stacked_series = []
        for name, xs, ys in series:
            base = [acc[x] for x in xs]
            top = [b + y for b, y in zip(base, ys)]
            for x, t in zip(xs, top):
                acc[x] = t
            stacked_series.append((name, xs, base, top))
        top_all = max(acc.values()) if acc else 1
    else:
        top_all = max(p[1] for p in pts)
    if ymax is None and not stacked:
        ys_all = sorted(p[1] for p in pts)
        robust = ys_all[min(len(ys_all) - 1, int(0.98 * len(ys_all)))] * 1.35
        if robust < top_all:
            ymax = robust
            title += f" (шкала обрезана; максимум {fmt(top_all, 0)})"
    elif ymax is None and stacked:
        tops = sorted(acc.values())
        robust = tops[min(len(tops) - 1, int(0.98 * len(tops)))] * 1.35 if tops else top_all
        if robust < top_all:
            ymax = robust
            title += f" (шкала обрезана; максимум {fmt(top_all, 0)})"
    y1 = ymax or max(top_all * 1.08, 1e-6)
    L, R, T, B = 52, 12, 26, 30
    W, H = width - L - R, height - T - B
    sx = lambda x: L + (x - x0) / (x1 - x0) * W
    sy = lambda y: T + H - min(y, y1) / y1 * H
    out = [f'<svg viewBox="0 0 {width} {height}" class="chart" role="img" aria-label="{html.escape(title)}">',
           f'<clipPath id="c{id(series)}"><rect x="{L}" y="{T}" width="{W}" height="{H}"/></clipPath>',
           f'<text x="{L}" y="16" class="ct">{html.escape(title)}</text>']
    for k in range(5):
        yv = y1 * k / 4
        y = sy(yv)
        out.append(f'<line x1="{L}" x2="{L + W}" y1="{y:.1f}" y2="{y:.1f}" class="grid"/>'
                   f'<text x="{L - 6}" y="{y + 4:.1f}" class="ax" text-anchor="end">{fmt(yv, 0 if y1 >= 10 else 1)}</text>')
    for k in range(6):
        xv = x0 + (x1 - x0) * k / 5
        out.append(f'<text x="{sx(xv):.1f}" y="{T + H + 18}" class="ax" text-anchor="middle">{fmt(xv, 0)}</text>')
    out.append(f'<text x="{L + W}" y="{T + H + 18}" class="ax" text-anchor="end" dy="10"></text>')
    for hv, label in hlines:
        if hv <= y1:
            y = sy(hv)
            out.append(f'<line x1="{L}" x2="{L + W}" y1="{y:.1f}" y2="{y:.1f}" class="hl"/>'
                       f'<text x="{L + W - 4}" y="{y - 3:.1f}" class="ax" text-anchor="end">{html.escape(label)}</text>')
    if stacked:
        for k, (name, xs, base, top) in enumerate(stacked_series):
            if not xs:
                continue
            col = PALETTE[k % len(PALETTE)]
            path = " ".join(f"{sx(x):.1f},{sy(t):.1f}" for x, t in zip(xs, top))
            back = " ".join(f"{sx(x):.1f},{sy(b):.1f}" for x, b in zip(reversed(xs), reversed(base)))
            out.append(f'<polygon clip-path="url(#c{id(series)})" points="{path} {back}" fill="{col}" fill-opacity="0.85"><title>{html.escape(name)}</title></polygon>')
    else:
        for k, (name, xs, ys) in enumerate(series):
            col = PALETTE[k % len(PALETTE)]
            path = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in zip(xs, ys))
            out.append(f'<polyline clip-path="url(#c{id(series)})" points="{path}" fill="none" stroke="{col}" stroke-width="1.6"><title>{html.escape(name)}</title></polyline>')
    out.append(f'<text x="12" y="{T + H / 2}" class="ax" transform="rotate(-90 12 {T + H / 2})" text-anchor="middle">{html.escape(ylabel)}</text>')
    out.append("</svg>")
    legend = "".join(f'<span class="lg"><i style="background:{PALETTE[k % len(PALETTE)]}"></i>{html.escape(n)}</span>'
                     for k, (n, *_rest) in enumerate(series))
    return "".join(out) + f'<div class="legend">{legend}</div>'


def per_second(t, values):
    acc = defaultdict(list)
    for x, v in zip(t, values):
        acc[int(x)].append(v)
    xs = sorted(acc)
    return xs, acc


# ------------------------------------------------------------------------------------------------ analysis

def analyze(s, t0=None, t1=None, with_samples=True, sym=None):
    fr = Frames(s, t0, t1)
    res = {"window": [t0, t1], "frames": len(fr.rows)}
    if not fr.rows:
        return res, fr
    res["fps"] = fr.fps()
    res["cpi_ms"] = mean(fr.cpi)
    part = {k: mean([r[k] for r in fr.rows]) for k, _ in PARTS}
    res["partition"] = part
    lim = Counter(limiter_of(r) for r in fr.rows)
    res["limiters"] = {k: v / len(fr.rows) for k, v in lim.items()}
    top = max(part.items(), key=lambda kv: kv[1])
    res["limiter"] = top[0] if top[1] >= 0.4 * res["cpi_ms"] else "mixed"
    for k in ("pre", "pm4", "record", "record_other", "shader", "pipeline", "sync", "tail", "answer", "poll", "flush",
              "gpu", "flip_cpu", "draws", "dispatches", "wb_kib", "upload_kib", "stream_kib", "barriers", "submit",
              "file_reads", "file_kib", "file_ms", "shaders", "pipelines_new"):
        v = [r[k] for r in fr.rows]
        res.setdefault("m", {})[k] = {"mean": mean(v), "p95": pct(v, 0.95), "max": max(v)}
    res["gpu_util"] = res["m"]["gpu"]["mean"] / res["cpi_ms"] if res["cpi_ms"] else float("nan")
    timed = [r for r in fr.rows if r["gpu_timed"]]
    res["gpu_timed_frames"] = len(timed)
    cats = {}
    for cat in s.cats:
        d = [r["d_" + cat] for r in fr.rows]
        x = [r["x_" + cat] for r in fr.rows]
        cpu = [r["cpu_" + cat] for r in fr.rows]
        gpu = [r["gpu_" + cat] for r in timed]
        if sum(d) + sum(x) == 0:
            continue
        cats[cat] = {"draws": mean(d), "draws_p95": pct(d, 0.95), "draws_max": max(d),
                     "dispatches": mean(x), "dispatches_p95": pct(x, 0.95), "dispatches_max": max(x),
                     "cpu": mean(cpu), "cpu_p95": pct(cpu, 0.95), "cpu_max": max(cpu),
                     "gpu": mean(gpu) if gpu else float("nan"), "gpu_p95": pct(gpu, 0.95) if gpu else float("nan"),
                     "gpu_max": max(gpu) if gpu else float("nan")}
    for k in ("gx_transfer", "gx_flipcopy", "gx_batchstart"):
        g = [r["gpu_" + k] for r in timed]
        if g and mean(g) >= 0.01:
            cats[k] = {"draws": 0, "dispatches": 0, "cpu": float("nan"), "gpu": mean(g), "gpu_p95": pct(g, 0.95),
                       "gpu_max": max(g)}
    res["categories"] = cats
    # basis per category (pipelines.csv, the run's totals)
    basis = defaultdict(Counter)
    for p in s.pipelines:
        basis[p.get("category", "?")][p.get("basis", "?")] += int(num(p.get("count")))
    res["basis"] = {k: dict(v) for k, v in basis.items()}
    # CPU subsystems (sampled)
    if with_samples and sym is not None:
        stacks, packed = load_samples(s, t0, t1)
        if stacks:
            counts, per_sec = packed
            for thread, classify in (("g", classify_gpu_stack), ("m", classify_game_stack)):
                total = sum(counts[thread].values())
                if total == 0:
                    continue
                buckets, ctxs, top_leaf = Counter(), defaultdict(Counter), Counter()
                for sid, cnt in counts[thread].items():
                    th, frames = stacks.get(sid, ("?", []))
                    b, ctx = classify(frames, sym)
                    buckets[b] += cnt
                    ctxs[b][ctx] += cnt
                    if frames:
                        top_leaf[sym.name(frames[0])[1]] += cnt
                hot = None
                if thread == "m":
                    guest = Counter()
                    for sid, cnt in counts[thread].items():
                        frames = stacks.get(sid, ("?", []))[1]
                        if frames and sym.name(frames[0])[0] == "guest":
                            guest[frames[0] >> 6] += cnt  # 64-byte blocks: one loop
                    if guest:
                        blk, cnt = guest.most_common(1)[0]
                        hot = {"block": sym.name(blk << 6)[1], "share": cnt / total}
                res["samples_" + thread] = {
                    "hot_loop": hot,
                    "total": total,
                    "buckets": {b: c / total for b, c in buckets.items()},
                    "contexts": {b: [(c, n / total) for c, n in ctxs[b].most_common(4)] for b in buckets},
                    "top_leaf": [(n, c / total) for n, c in top_leaf.most_common(15)],
                }
    # recommendations
    res["recommendations"] = recommend(fr, res)
    # long frames (secondary)
    longs = sorted(((r["cpi"], fr.t[i], i) for i, r in enumerate(fr.rows)), reverse=True)[:10]
    res["long_frames"] = [{"t": t, "cpi": c, **{k: fr.rows[i][k] for k in ("cp_cpu", "gpu_wait", "game_wait",
                                                                            "output_wait", "shader", "pipeline", "gpu",
                                                                            "file_ms", "draws")},
                           "limiter": limiter_of(fr.rows[i])} for c, t, i in longs]
    return res, fr


def recommend(fr, res):
    rows = fr.rows
    base = mean([max(r["cpi"], VBLANK_MS) for r in rows])
    items = []

    def add(kind, title, ms, note, share=0.5):
        if not (ms and ms > 0.05):
            return
        new50 = predict(rows, kind, ms * share)
        new100 = predict(rows, kind, ms)
        items.append({"kind": kind, "title": title, "ms": ms, "note": note,
                      "fps_now": 1000 / base, "fps_50": 1000 / new50, "fps_100": 1000 / new100,
                      "gain_ms_50": base - new50})
    for cat, v in res.get("categories", {}).items():
        if not cat.startswith("gx_") and v.get("cpu", 0) == v.get("cpu", 0):
            add("cp", f"CPU записи: {CAT_RU.get(cat, cat)}", v["cpu"],
                f"{fmt(v['draws'], 0)} draw + {fmt(v['dispatches'], 0)} dispatch за кадр")
        if v.get("gpu", float("nan")) == v.get("gpu", float("nan")):
            add("gpu", f"GPU: {CAT_RU.get(cat, cat)}", v["gpu"], "время GPU по меткам смены категорий")
    sg = res.get("samples_g")
    if sg:
        busy = res["partition"]["cp_cpu"] + res["partition"]["gpu_wait"] + res["partition"]["game_wait"] + \
            res["partition"]["output_wait"]
        for b, share in sg["buckets"].items():
            if b.startswith("Ожидание") or b.startswith("Простой"):
                continue
            add("cp", f"CPU подсистема: {b}", share * busy, "доля сэмплов потока GPU × время кадра")
    m = res["m"]
    add("cp", "Компиляция: трансляция шейдеров + создание pipeline", m["shader"]["mean"] + m["pipeline"]["mean"],
        "только в кадрах с новыми шейдерами (P95 {} мс)".format(fmt(m["shader"]["p95"] + m["pipeline"]["p95"])))
    add("game", "CPU игры (ответ после EOP + отправка кадра)", res["partition"]["game_wait"],
        "работа игры между EOP и отправкой кадра; оптимизировать можно только косвенно")
    items.sort(key=lambda x: -x["gain_ms_50"])
    return items[:25]


# ------------------------------------------------------------------------------------------------ HTML

CSS = """
:root{--bg:#ffffff;--fg:#1d2330;--muted:#5d6675;--card:#f5f7fa;--line:#dfe3ea;--accent:#2f6fde;--bad:#c0392b;--good:#2e8b57}
@media (prefers-color-scheme:dark){:root{--bg:#14171c;--fg:#e6e9ef;--muted:#9aa3b2;--card:#1d2128;--line:#2c323c;--accent:#6ea0ff;--bad:#ff7a6b;--good:#5fd38d}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 "Segoe UI",system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:20px 16px 60px}h1{font-size:22px;margin:0 0 4px}h2{font-size:18px;margin:32px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}
h3{font-size:15px;margin:18px 0 6px}.muted{color:var(--muted)}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin:10px 0}
.verdict{font-size:15px}.verdict b{color:var(--accent)}table{border-collapse:collapse;width:100%;margin:6px 0 10px;font-variant-numeric:tabular-nums}
th,td{border-bottom:1px solid var(--line);padding:4px 6px;text-align:right;vertical-align:top}th:first-child,td:first-child{text-align:left}
th{color:var(--muted);font-weight:600;font-size:12px}.chart{width:100%;height:auto;display:block;margin-top:8px}
.ct{font-size:13px;font-weight:600;fill:var(--fg)}.ax{font-size:11px;fill:var(--muted)}.grid{stroke:var(--line);stroke-width:1}.hl{stroke:var(--bad);stroke-dasharray:4 3}
.legend{display:flex;flex-wrap:wrap;gap:4px 14px;font-size:12px;color:var(--muted);margin:4px 0 8px}.lg i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;vertical-align:-1px}
.bar{display:inline-block;height:9px;background:var(--accent);border-radius:2px;vertical-align:middle}code{font-size:12px}.na{color:var(--muted)}
.wrap{overflow-x:auto}
"""


def ms_cell(x, d=2):
    return f"<td>{fmt(x, d)}</td>" if x == x else "<td class=na>не измерено</td>"


def section_phase(name, res, s):
    out = []
    fps = res.get("fps", {})
    part = res.get("partition", {})
    cpi = res.get("cpi_ms", float("nan"))
    lim = res.get("limiter", "mixed")
    out.append(f"<h2>{html.escape(name)}</h2>")
    if not fps:
        out.append("<p class=muted>Нет кадров в этом окне.</p>")
        return "".join(out)
    m = res["m"]
    out.append("<div class='card verdict'>")
    out.append(f"<p><b>{fmt(fps['avg_fps'])} FPS</b> (медиана {fmt(fps['median_fps'])}, 1% low {fmt(fps['low1_fps'])}); "
               f"кадр в среднем <b>{fmt(cpi)} мс</b>. Главный ограничитель: <b>{LIMITER_RU.get(lim, lim)}</b>.</p>")
    lines = []
    for k, label in PARTS:
        lines.append(f"{label}: <b>{fmt(part[k])} мс</b> ({fmt(100 * part[k] / cpi, 0)} %)")
    out.append("<p>Куда уходит время кадра (поток GPU SecondSonPC, точная разбивка его таймерами):<br>" +
               "<br>".join(lines) + "</p>")
    out.append(f"<p>GPU занят <b>{fmt(m['gpu']['mean'])} мс</b> за кадр ({fmt(100 * res['gpu_util'], 0)} % кадра; "
               f"P95 {fmt(m['gpu']['p95'])} мс). Draw/dispatch за кадр: {fmt(m['draws']['mean'], 0)} / "
               f"{fmt(m['dispatches']['mean'], 0)}.</p>")
    recs = res.get("recommendations", [])
    if recs:
        r = recs[0]
        out.append(f"<p>Выгоднее всего: <b>{html.escape(r['title'])}</b> — {fmt(r['ms'], 2)} мс/кадр; "
                   f"−50 % дало бы ≈ {fmt(r['fps_50'])} FPS, устранение целиком — ≈ {fmt(r['fps_100'])} FPS "
                   f"(теоретическая оценка по модели кадра, раздел «Рекомендации»).</p>")
    out.append("</div>")
    # FPS table
    out.append("<h3>Время кадра (интервалы показа)</h3><table><tr><th>Показатель</th><th>Значение</th></tr>")
    for k, label, d in (("avg_fps", "Средний FPS", 1), ("median_fps", "Медианный FPS", 1), ("low1_fps", "1% low FPS", 1),
                        ("mean_ms", "Средний кадр, мс", 1), ("p95_ms", "P95, мс", 1), ("p99_ms", "P99, мс", 1),
                        ("max_ms", "Максимум, мс", 1)):
        out.append(f"<tr><td>{label}</td><td>{fmt(fps[k], d)}</td></tr>")
    out.append(f"<tr><td>Кадров ≥33,4 / 50 / 100 / 200 / 500 / 1000 мс</td><td>{fps['over_33.4']} / {fps['over_50']} / "
               f"{fps['over_100']} / {fps['over_200']} / {fps['over_500']} / {fps['over_1000']} из {fps['frames']}</td></tr>")
    out.append("</table>")
    # limiter mix
    lm = res.get("limiters", {})
    out.append("<h3>Ограничитель по кадрам</h3><table><tr><th>Ограничитель кадра</th><th>Доля кадров</th></tr>")
    for k, v in sorted(lm.items(), key=lambda kv: -kv[1]):
        out.append(f"<tr><td>{LIMITER_RU.get(k, k)}</td><td>{fmt(100 * v, 1)} %</td></tr>")
    out.append("</table><p class=muted>Ограничитель кадра — наибольшая из четырёх частей разбивки; «смешанный» — "
               "две верхние части отличаются менее чем на 10 % кадра.</p>")
    # CP partition detail
    out.append("<h3>Разбивка CPU потока GPU (мс/кадр: среднее · P95 · максимум)</h3><table><tr><th>Часть</th>"
               "<th>Среднее</th><th>P95</th><th>Макс.</th><th>Как измерено</th></tr>")
    rows = [("pm4", "PM4 и прочее CP вне записи команд", "таймеры цикла"),
            ("record_other", "Запись draw/dispatch (фронтенд, привязки, драйвер)", "TSC на каждый draw/dispatch"),
            ("shader", "Трансляция шейдеров GCN→SPIR-V", "таймер трансляции"),
            ("pipeline", "Создание pipeline + модулей (драйвер)", "таймер vkCreate*Pipelines"),
            ("sync", "Ожидания GPU (Synchronize: пакеты, чтения фронтенда)", "таймер ожидания"),
            ("pre", "Приём отправок, инвалидации, записи CPU игры", "таймеры цикла"),
            ("flush", "Отправка пакетов (вне записи)", "таймеры цикла"),
            ("poll", "Опрос завершённых пакетов", "таймеры цикла"),
            ("tail", "Хвост GPU прошлого кадра (CP пуст, игра ждёт EOP)", "метки кадра"),
            ("answer", "Ответ игры после EOP до первой отправки", "метки кадра"),
            ("flip_cpu", "CPU потока игры, отдающего кадры", "QueryThreadCycleTime")]
    for k, label, how in rows:
        v = res["m"][k]
        out.append(f"<tr><td>{label}</td><td>{fmt(v['mean'], 2)}</td><td>{fmt(v['p95'], 2)}</td>"
                   f"<td>{fmt(v['max'], 1)}</td><td class=muted>{how}</td></tr>")
    out.append("</table>")
    out.append("<p class=muted>Вложенность: «Запись draw/dispatch» включает трансляцию, создание pipeline и ожидания "
               "фронтенда — в таблице они вычтены и показаны отдельно. CPU потока игры включает её активное ожидание "
               "EOP (игра держит один кадр в полёте), поэтому близок к длине кадра — см. сэмплы потока игры.</p>")
    # categories
    cats = res.get("categories", {})
    if cats:
        gpu_total = res["m"]["gpu"]["mean"]
        out.append("<h3>Категории эффектов</h3><div class=wrap><table><tr><th>Категория</th><th>draw/кадр</th>"
                   "<th>dispatch/кадр</th><th>CPU мс (ср.)</th><th>CPU P95</th><th>CPU макс.</th><th>GPU мс (ср.)</th>"
                   "<th>GPU P95</th><th>GPU макс.</th><th>Доля GPU</th><th>Основание</th></tr>")
        order = sorted(cats.items(), key=lambda kv: -((kv[1].get("gpu") if kv[1].get("gpu") == kv[1].get("gpu") else 0) +
                                                      (kv[1].get("cpu") if kv[1].get("cpu") == kv[1].get("cpu") else 0)))
        for cat, v in order:
            g = v.get("gpu", float("nan"))
            share = f"<span class=bar style='width:{max(1, min(120, 120 * g / gpu_total)) if g == g and gpu_total else 0:.0f}px'></span> " \
                    f"{fmt(100 * g / gpu_total, 1)} %" if g == g and gpu_total else "—"
            b = res.get("basis", {}).get(cat, {})
            bt = sum(b.values()) or 1
            btxt = ", ".join(f"{k} {fmt(100 * v2 / bt, 0)} %" for k, v2 in sorted(b.items(), key=lambda kv: -kv[1])) \
                if b else ("служебное" if cat.startswith("gx_") else "—")
            out.append(f"<tr><td>{html.escape(CAT_RU.get(cat, cat))}</td><td>{fmt(v.get('draws', 0), 0)}</td>"
                       f"<td>{fmt(v.get('dispatches', 0), 0)}</td>{ms_cell(v.get('cpu', float('nan')))}"
                       f"{ms_cell(v.get('cpu_p95', float('nan')))}{ms_cell(v.get('cpu_max', float('nan')), 1)}"
                       f"{ms_cell(g)}{ms_cell(v.get('gpu_p95', float('nan')))}{ms_cell(v.get('gpu_max', float('nan')), 1)}"
                       f"<td>{share}</td><td class=muted>{html.escape(btxt)}</td></tr>")
        out.append("</table></div>")
        out.append(f"<p class=muted>GPU по категориям: метки времени только на смене категории, в {res['gpu_timed_frames']} "
                   "кадрах из {} ; все очереди игры исполняются на одной очереди Vulkan, поэтому интервалы не "
                   "перекрываются. Барьеры, загрузки и смена render pass перед командой засчитываются её категории. "
                   "Основание: confirmed/probable/shared — каталог tools\\perf_catalog.txt по хешу содержимого шейдера; "
                   "rule — правило по целям (квадратная цель только с глубиной = тени, 3+ цвета или только глубина = "
                   "геометрия, маленький полноэкранный draw = постобработка); none — не классифицировано.</p>"
                   .format(res["frames"]))
    # CPU subsystems
    for thread, title, note in (("g", "CPU потока GPU по подсистемам (сэмплы стека)",
                                 "Статистическая оценка: доля сэмплов (250 Гц) × время кадра. Драйвер NVIDIA без символов — "
                                 "только модуль и вызывающая функция SecondSonPC."),
                                ("m", "Поток игры, отдающий кадры (сэмплы IP)",
                                 "Гостевой код без символов: игровая логика и активное ожидание EOP не различаются; "
                                 "ниже — самые частые адреса.")):
        sm = res.get("samples_" + thread)
        if not sm:
            continue
        out.append(f"<h3>{title}</h3><table><tr><th>Подсистема</th><th>Доля</th><th>мс/кадр</th><th>Где (вызывающая "
                   "функция)</th></tr>")
        for b, share in sorted(sm["buckets"].items(), key=lambda kv: -kv[1]):
            ctx = "; ".join(f"{html.escape(shorten(c))} {fmt(100 * sh, 0)} %" for c, sh in sm["contexts"][b] if c)
            out.append(f"<tr><td>{html.escape(b)}</td><td>{fmt(100 * share, 1)} %</td><td>{fmt(share * cpi, 2)}</td>"
                       f"<td class=muted style='text-align:left'>{ctx}</td></tr>")
        out.append(f"</table><p class=muted>{note} Сэмплов: {sm['total']}.</p>")
        hot = sm.get("hot_loop")
        if hot and hot["share"] >= 0.3:
            out.append(f"<p class=card>Поток игры {fmt(100 * hot['share'], 0)} % времени находится в одном 64-байтном "
                       f"участке кода <code>{html.escape(hot['block'])}</code> — это короткая петля (активное ожидание). "
                       f"Ответ игры после EOP — {fmt(res['m']['answer']['mean'], 2)} мс, ожидание CPU игры — "
                       f"{fmt(res['partition']['game_wait'], 2)} мс за кадр: игра готова раньше, чем SecondSonPC, и ждёт "
                       "конца кадра (EOP). Что именно проверяет петля — без символов игры UNKNOWN (P4.4: метка EOP "
                       "0x248000000).</p>")
        if thread == "m":
            out.append("<details><summary>Самые частые адреса потока игры</summary><table><tr><th>Адрес</th><th>Доля</th></tr>")
            for n, sh in sm["top_leaf"][:12]:
                out.append(f"<tr><td><code>{html.escape(n)}</code></td><td>{fmt(100 * sh, 1)} %</td></tr>")
            out.append("</table></details>")
    return "".join(out)


def shorten(name):
    name = re.sub(r"\(.*", "", name)
    name = name.replace("SecondSon::", "").replace("`anonymous namespace'::", "")
    return name[-70:]


def charts(fr, s):
    out = []
    xs, acc = per_second(fr.t, [r["cpi"] for r in fr.rows])
    out.append(svg_chart([("кадр (среднее за секунду)", xs, [mean(acc[x]) for x in xs]),
                          ("кадр P95 за секунду", xs, [pct(acc[x], 0.95) for x in xs]),
                          ("CPU командного процессора", *(lambda a: (a[0], [mean(a[1][x]) for x in a[0]]))(
                              per_second(fr.t, [r["cp_cpu"] for r in fr.rows]))),
                          ("GPU занят", *(lambda a: (a[0], [mean(a[1][x]) for x in a[0]]))(
                              per_second(fr.t, [r["gpu"] for r in fr.rows])))],
                         "Время кадра, CPU CP и GPU по времени игры", "мс", hlines=((33.37, "30 FPS"), (16.68, "60 FPS"))))
    series = []
    for k, label in PARTS:
        xs2, a = per_second(fr.t, [r[k] for r in fr.rows])
        series.append((label, xs2, [mean(a[x]) for x in xs2]))
    out.append(svg_chart(series, "Разбивка кадра по времени (накопительно)", "мс", stacked=True,
                         hlines=((33.37, "30 FPS"),)))
    cats = [c for c in s.cats if sum(r["gpu_" + c] for r in fr.rows) > 0]
    cats.sort(key=lambda c: -sum(r["gpu_" + c] for r in fr.rows))
    series = []
    for c in cats[:10]:
        xs2, a = per_second(fr.t, [r["gpu_" + c] for r in fr.rows if True])
        series.append((CAT_RU.get(c, c), xs2, [mean(a[x]) for x in xs2]))
    if series:
        out.append(svg_chart(series, "GPU по категориям (накопительно)", "мс", stacked=True))
    cats = [c for c in s.cats if sum(r["cpu_" + c] for r in fr.rows) > 0]
    cats.sort(key=lambda c: -sum(r["cpu_" + c] for r in fr.rows))
    series = []
    for c in cats[:10]:
        xs2, a = per_second(fr.t, [r["cpu_" + c] for r in fr.rows])
        series.append((CAT_RU.get(c, c), xs2, [mean(a[x]) for x in xs2]))
    if series:
        out.append(svg_chart(series, "CPU записи команд по категориям (накопительно)", "мс", stacked=True))
    series = []
    for c in cats[:8]:
        xs2, a = per_second(fr.t, [r["d_" + c] + r["x_" + c] for r in fr.rows])
        series.append((CAT_RU.get(c, c), xs2, [mean(a[x]) for x in xs2]))
    if series:
        out.append(svg_chart(series, "Draw + dispatch за кадр по категориям", "команд"))
    return "".join(out)


def memory_section(s):
    if not s.seconds:
        return ""
    t = [num(r["t_s"]) for r in s.seconds]
    ser = []
    for k, label in (("private_mib", "память процесса (private)"), ("working_set_mib", "рабочий набор процесса"),
                     ("vram_local_mib", "VRAM процесса (DXGI, с драйвером)"), ("own_images_mib", "свои образы Vulkan"),
                     ("own_buffers_mib", "свои буферы Vulkan (резидентно)")):
        ser.append((label, t, [num(r.get(k)) for r in s.seconds]))
    out = ["<h2>Память</h2>", svg_chart(ser, "Память процесса и видеопамять", "МиБ")]
    ser2 = [("системный commit (вся система)", t, [num(r.get("system_commit_mib")) for r in s.seconds]),
            ("предел commit", t, [num(r.get("system_limit_mib")) for r in s.seconds])]
    out.append(svg_chart(ser2, "Системный commit (не процесс!)", "МиБ"))
    out.append("<table><tr><th>Показатель</th><th>Пик, МиБ</th><th>Последнее, МиБ</th></tr>")
    for k, label in (("private_mib", "Private процесса"), ("working_set_mib", "Рабочий набор процесса"),
                     ("process_commit_mib", "Commit процесса"), ("vram_local_mib", "VRAM процесса (DXGI)"),
                     ("vram_nonlocal_mib", "Системная память GPU процесса (DXGI non-local)"),
                     ("own_images_mib", "Свои образы (ImageCache)"), ("own_buffers_mib", "Свои буферы (BufferCache)"),
                     ("system_commit_mib", "Системный commit (все процессы)")):
        v = [num(r.get(k)) for r in s.seconds]
        out.append(f"<tr><td>{label}</td><td>{fmt(max(v), 0)}</td><td>{fmt(v[-1], 0)}</td></tr>")
    out.append("</table><p class=muted>VRAM процесса по DXGI включает выделения драйвера NVIDIA для процесса; "
               "«свои» — только образы и буферы кэшей SecondSonPC. Системный commit — сумма по всей системе.</p>")
    return "".join(out)


def threads_section(s, t0=None, t1=None):
    if not s.threads:
        return ""
    acc = defaultdict(float)
    names = {}
    secs = set()
    for r in s.threads:
        t = num(r["t_s"])
        if (t0 is not None and t < t0) or (t1 is not None and t > t1):
            continue
        secs.add(int(t))
        acc[r["tid"]] += num(r["cpu_ms"])
        names[r["tid"]] = r["name"] or "(без имени)"
    n = max(1, len(secs))
    out = ["<h3>Потоки процесса: процессорное время</h3><table><tr><th>Поток</th><th>CPU мс/с</th><th>Загрузка ядра</th></tr>"]
    for tid, ms in sorted(acc.items(), key=lambda kv: -kv[1])[:15]:
        out.append(f"<tr><td>{html.escape(names[tid])} <span class=muted>({tid})</span></td><td>{fmt(ms / n, 0)}</td>"
                   f"<td>{fmt(ms / n / 10, 0)} %</td></tr>")
    out.append("</table>")
    return "".join(out)


def pipelines_section(s):
    if not s.pipelines:
        return ""
    draws = [p for p in s.pipelines if p["kind"] == "draw"]
    disp = [p for p in s.pipelines if p["kind"] == "dispatch"]
    out = ["<h2>Самые дорогие pipeline (CPU записи, весь прогон)</h2><div class=wrap><table><tr><th>Pipeline</th>"
           "<th>Категория</th><th>Основание</th><th>draw</th><th>экз.</th><th>CPU мс всего</th><th>мкс/draw</th>"
           "<th>Цель</th><th>VS / PS (хеш содержимого)</th></tr>"]
    for p in sorted(draws, key=lambda p: -num(p["cpu_ms"]))[:20]:
        cnt = max(1.0, num(p["count"]))
        tgt = f"{p['colors']} цв.{' + глубина' if p['depth'] == '1' else ''} {p['width']}×{p['height']}"
        out.append(f"<tr><td>p{p['id']}</td><td>{html.escape(CAT_RU.get(p['category'], p['category']))}</td>"
                   f"<td class=muted>{p['basis']}</td><td>{fmt(cnt, 0)}</td><td>{fmt(num(p['instances']), 0)}</td>"
                   f"<td>{fmt(num(p['cpu_ms']), 1)}</td><td>{fmt(1000 * num(p['cpu_ms']) / cnt, 1)}</td><td>{tgt}</td>"
                   f"<td><code>{p['vs']}</code> / <code>{p['ps']}</code></td></tr>")
    out.append("</table></div><p class=muted>Номер pipeline действителен только в этом прогоне; устойчивы хеши "
               "содержимого. GPU-время по отдельным pipeline в лёгком режиме не измеряется (не измерено).</p>")
    out.append("<h3>Compute-программы (весь прогон)</h3><div class=wrap><table><tr><th>CS (адрес / хеш)</th><th>Категория</th>"
               "<th>dispatch</th><th>из них 1×1×1</th><th>групп</th><th>CPU мс всего</th><th>мкс/dispatch</th></tr>")
    for p in sorted(disp, key=lambda p: -num(p["cpu_ms"]))[:20]:
        cnt = max(1.0, num(p["count"]))
        out.append(f"<tr><td><code>{p['cs_addr']}</code> / <code>{p['cs']}</code></td>"
                   f"<td>{html.escape(CAT_RU.get(p['category'], p['category']))}</td><td>{fmt(cnt, 0)}</td>"
                   f"<td>{fmt(num(p['single_group']), 0)}</td><td>{fmt(num(p['groups']), 0)}</td>"
                   f"<td>{fmt(num(p['cpu_ms']), 1)}</td><td>{fmt(1000 * num(p['cpu_ms']) / cnt, 1)}</td></tr>")
    out.append("</table></div>")
    return "".join(out)


def recs_section(res):
    recs = res.get("recommendations", [])
    if not recs:
        return ""
    out = ["<h2>Рекомендации: что выгоднее всего исправить</h2><div class=wrap><table><tr><th>Операция</th><th>мс/кадр</th>"
           "<th>FPS сейчас</th><th>FPS при −50 %</th><th>FPS без неё (предел)</th><th>Пояснение</th></tr>"]
    for r in recs[:15]:
        out.append(f"<tr><td>{html.escape(r['title'])}</td><td>{fmt(r['ms'], 2)}</td><td>{fmt(r['fps_now'])}</td>"
                   f"<td>{fmt(r['fps_50'])}</td><td>{fmt(r['fps_100'])}</td><td class=muted>{html.escape(r['note'])}</td></tr>")
    out.append("</table></div><p class=muted>Модель кадра (раздел 6 документа): для каждого кадра работа CP можно "
               "сократить только до уровня «GPU + ожидания игры и вывода», работу GPU — только в пределах ожиданий GPU; "
               "FPS ограничен сеткой 59,94 Гц. Это верхние оценки: они не учитывают, что ускорение одной части может "
               "изменить поведение игры. Строки по категориям и по подсистемам — два разреза одной и той же работы, "
               "складывать их нельзя.</p>")
    return "".join(out)


def long_section(res):
    lf = res.get("long_frames", [])
    if not lf:
        return ""
    out = ["<h2>Самые длинные кадры (дополнительно)</h2><div class=wrap><table><tr><th>t, с</th><th>Кадр CP, мс</th>"
           "<th>CPU CP</th><th>Ожид. GPU</th><th>Ожид. игры</th><th>Вывод</th><th>Шейдеры</th><th>Pipeline</th>"
           "<th>Файлы, мс</th><th>draw</th><th>Ограничитель</th></tr>"]
    for r in lf:
        out.append(f"<tr><td>{fmt(r['t'], 2)}</td><td>{fmt(r['cpi'])}</td><td>{fmt(r['cp_cpu'])}</td><td>{fmt(r['gpu_wait'])}</td>"
                   f"<td>{fmt(r['game_wait'])}</td><td>{fmt(r['output_wait'])}</td><td>{fmt(r['shader'])}</td>"
                   f"<td>{fmt(r['pipeline'])}</td><td>{fmt(r['file_ms'])}</td><td>{fmt(r['draws'], 0)}</td>"
                   f"<td>{LIMITER_RU.get(r['limiter'], r['limiter'])}</td></tr>")
    out.append("</table></div><p class=muted>«Ожидание игры» в длинном кадре — поток GPU пуст, игра не отдаёт команды: "
               "причина на стороне игры (загрузка, логика) — без сэмплов потоков игры остаётся UNKNOWN.</p>")
    return "".join(out)


def phase_compare_section(phases):
    if len(phases) < 2:
        return ""
    names = [n for n, _ in phases]
    out = ["<h2>Сравнение фаз (например, до и после поворота камеры)</h2><div class=wrap><table><tr><th>Показатель</th>" +
           "".join(f"<th>{html.escape(n)}</th>" for n in names) + "</tr>"]

    def row(label, get, d=1):
        vals = []
        for _, r in phases:
            try:
                vals.append(get(r))
            except (KeyError, TypeError):
                vals.append(float("nan"))
        out.append(f"<tr><td>{label}</td>" + "".join(f"<td>{fmt(v, d)}</td>" for v in vals) + "</tr>")
    row("Средний FPS", lambda r: r["fps"]["avg_fps"])
    row("P95 кадра, мс", lambda r: r["fps"]["p95_ms"])
    row("Кадр CP, мс", lambda r: r["cpi_ms"])
    for k, label in PARTS:
        row(label, lambda r, k=k: r["partition"][k], 2)
    row("GPU занят, мс", lambda r: r["m"]["gpu"]["mean"], 2)
    row("draw за кадр", lambda r: r["m"]["draws"]["mean"], 0)
    row("dispatch за кадр", lambda r: r["m"]["dispatches"]["mean"], 0)
    out.append("</table></div>")
    allcats = []
    for _, r in phases:
        for c in r.get("categories", {}):
            if c not in allcats:
                allcats.append(c)
    a, z = phases[0][1].get("categories", {}), phases[-1][1].get("categories", {})
    out.append(f"<h3>Категории: {html.escape(names[0])} → {html.escape(names[-1])}</h3><div class=wrap><table><tr>"
               "<th>Категория</th><th>команд/кадр</th><th>CPU мс</th><th>Δ CPU</th><th>GPU мс</th><th>Δ GPU</th></tr>")

    def g(d, c, k):
        v = d.get(c, {}).get(k, 0.0)
        return v if v == v else 0.0
    allcats.sort(key=lambda c: -abs(g(z, c, "cpu") - g(a, c, "cpu")) - abs(g(z, c, "gpu") - g(a, c, "gpu")))
    for c in allcats:
        cmd_a = g(a, c, "draws") + g(a, c, "dispatches")
        cmd_z = g(z, c, "draws") + g(z, c, "dispatches")
        out.append(f"<tr><td>{html.escape(CAT_RU.get(c, c))}</td><td>{fmt(cmd_a, 0)} → {fmt(cmd_z, 0)}</td>"
                   f"<td>{fmt(g(a, c, 'cpu'), 2)} → {fmt(g(z, c, 'cpu'), 2)}</td><td>{fmt(g(z, c, 'cpu') - g(a, c, 'cpu'), 2)}</td>"
                   f"<td>{fmt(g(a, c, 'gpu'), 2)} → {fmt(g(z, c, 'gpu'), 2)}</td><td>{fmt(g(z, c, 'gpu') - g(a, c, 'gpu'), 2)}</td></tr>")
    out.append("</table></div>")
    return "".join(out)


def build_html(s, phases, fr_all, title_extra=""):
    meta = s.meta
    parts = [f"<!doctype html><html lang=ru><head><meta charset=utf-8><meta name=viewport content='width=device-width,"
             f"initial-scale=1'><title>Performance Inspector — {html.escape(s.name)}</title><style>{CSS}</style></head>"
             "<body><main>"]
    parts.append(f"<h1>SecondSonPC Performance Inspector — {html.escape(s.name)}{html.escape(title_extra)}</h1>")
    dur = fr_all.t[-1] - fr_all.t[0] if fr_all.t else 0
    parts.append(f"<p class=muted>Сессия {html.escape(meta.get('started', ''))}–{html.escape(meta.get('ended', ''))}, "
                 f"{fmt(dur, 0)} с, кадров {meta.get('frames_written')}; режим {meta.get('mode')}, метки GPU каждые "
                 f"{meta.get('gpu_every')} кадр(а), сэмплы стека {meta.get('sample_hz')} Гц; каталог "
                 f"{meta.get('catalog_entries')} программ; {html.escape(meta.get('device', ''))}.</p>")
    for name, res in phases:
        parts.append(section_phase(name, res, s))
        parts.append(recs_section(res))
    parts.append(phase_compare_section(phases))
    parts.append("<h2>Графики всей сессии</h2>")
    parts.append(charts(fr_all, s))
    parts.append(pipelines_section(s))
    parts.append(memory_section(s))
    parts.append(threads_section(s))
    parts.append(long_section(phases[0][1]))
    if s.markers:
        parts.append("<h2>Отметки игрока (Ctrl+F12)</h2><table><tr><th>№</th><th>t, с</th><th>Кадр</th></tr>")
        for m in s.markers:
            parts.append(f"<tr><td>{m['marker']}</td><td>{fmt(num(m['t_us']) / 1e6, 2)}</td><td>{m['frame']}</td></tr>")
        parts.append("</table>")
    parts.append("<h2>Методика и ограничения</h2><div class=card><ul>"
                 "<li>Время кадра потока GPU разбито его таймерами точно (сумма частей = интервал между метками флипа).</li>"
                 "<li>«Ожидание GPU» = явные синхронизации + хвост GPU прошлого кадра (игра держит один кадр в полёте "
                 "и ждёт его EOP); «ожидание игры» — остаток простоя, пока игра не отдала команды.</li>"
                 "<li>GPU по категориям — метки времени на смене категории; барьеры и загрузки перед командой — её категории; "
                 "метки сами немного замедляют GPU (оценка накладных — в документе).</li>"
                 "<li>Подсистемы CPU — сэмплирование стека (статистика); драйвер NVIDIA без символов.</li>"
                 "<li>Гостевой код игры без символов: логика игры и её активное ожидание не различаются.</li>"
                 "<li>Не измерено в лёгком режиме: GPU-время отдельных pipeline, число частиц (нужен readback).</li></ul></div>")
    parts.append("</main></body></html>")
    return "".join(parts)


def guest_modules(log):
    mods = []
    if log and os.path.exists(log):
        rx = re.compile(r"\) (\S+): base (0x[0-9a-f]+), size (0x[0-9a-f]+)")
        for line in open(log, encoding="utf-8", errors="replace"):
            m = rx.search(line)
            if m:
                mods.append({"name": m.group(1), "base": m.group(2), "size": m.group(3), "path": "", "guest": "1"})
    return mods


def parse_phases(specs, fr_all):
    phases = []
    for spec in specs or []:
        name, _, rng = spec.partition("=")
        a, _, b = rng.partition("-")
        phases.append((name, float(a) if a else None, float(b) if b else None))
    if not phases:
        t0 = fr_all.t[0] if fr_all.t else 0
        phases.append(("Вся сессия (без первых 20 с загрузки)", t0 + 20 if len(fr_all.t) > 600 else None, None))
    return phases


def report(args):
    s = Session(args.session)
    fr_all = Frames(s)
    log = args.log or os.path.join(s.dir, "SecondSonPC.log")
    sym = Symbols(s.modules + guest_modules(log))
    stacks, _ = load_samples(s)
    if stacks:
        sym.resolve(sorted({a for _, fr in stacks.values() for a in fr}))
    phases = []
    for name, a, b in parse_phases(args.phase, fr_all):
        res, _ = analyze(s, a, b, sym=sym)
        res["name"] = name
        phases.append((f"{name}" + (f" ({fmt(a, 0)}–{fmt(b, 0)} с)" if a is not None or b is not None else ""), res))
    out = args.out or os.path.join(s.dir, "report.html")
    open(out, "w", encoding="utf-8").write(build_html(s, phases, fr_all))
    summary = {n: r for n, r in phases}
    json.dump(summary, open(os.path.join(os.path.dirname(out), "summary.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1, default=str)
    for n, r in phases:
        f = r.get("fps", {})
        p = r.get("partition", {})
        print(f"{n}: FPS {fmt(f.get('avg_fps'))}, кадр {fmt(r.get('cpi_ms'))} мс, ограничитель "
              f"{LIMITER_RU.get(r.get('limiter'), r.get('limiter'))}; CP CPU {fmt(p.get('cp_cpu'))}, ожид. GPU "
              f"{fmt(p.get('gpu_wait'))}, ожид. игры {fmt(p.get('game_wait'))}, вывод {fmt(p.get('output_wait'))}; "
              f"GPU занят {fmt(r.get('m', {}).get('gpu', {}).get('mean'))} мс")
    print(f"report: {out}")


def compare(args):
    sa, sb = Session(args.a), Session(args.b)
    spec = (args.phase or [None])[0]
    a0 = b0 = None
    if spec:
        _, _, rng = spec.partition("=")
        x, _, y = rng.partition("-")
        a0, b0 = float(x), float(y)
    ra, _ = analyze(sa, a0, b0, with_samples=False)
    rb, _ = analyze(sb, a0, b0, with_samples=False)
    phases = [(f"A: {sa.name}", ra), (f"B: {sb.name}", rb)]
    html_text = ("<!doctype html><html lang=ru><head><meta charset=utf-8><title>A/B — Performance Inspector</title>"
                 f"<style>{CSS}</style></head><body><main><h1>A/B: {html.escape(sa.name)} → {html.escape(sb.name)}</h1>"
                 f"<p class=muted>Окно {a0}–{b0} с.</p>" + phase_compare_section(phases) +
                 section_phase(phases[0][0], ra, sa) + section_phase(phases[1][0], rb, sb) + "</main></body></html>")
    out = args.out or os.path.join(args.b, "compare.html")
    open(out, "w", encoding="utf-8").write(html_text)
    for n, r in phases:
        print(f"{n}: FPS {fmt(r.get('fps', {}).get('avg_fps'))}, CP CPU {fmt(r.get('partition', {}).get('cp_cpu'))}, "
              f"GPU {fmt(r.get('m', {}).get('gpu', {}).get('mean'))}")
    print(f"compare: {out}")


def main(argv):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description="SecondSonPC Performance Inspector report")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("session")
    r.add_argument("--phase", action="append", help="name=from-to (seconds of the frame log)")
    r.add_argument("--log")
    r.add_argument("--out")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--phase", action="append")
    c.add_argument("--out")
    args = ap.parse_args(argv[1:])
    (report if args.cmd == "report" else compare)(args)


if __name__ == "__main__":
    main(sys.argv)
