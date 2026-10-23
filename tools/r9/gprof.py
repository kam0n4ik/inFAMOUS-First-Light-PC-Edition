"""R9 gameplay profiler (SSPC_R9_GPROF=1|2): logs/r9_gprof.bin joined with logs/gpu_frames.csv.

  py -I -X utf8 tools/r9/gprof.py report <run dir> [--from S] [--to S] [--phase name=S-S ...] [--marks]
        [--funcs E:/research/r9/ghidra/functions.tsv] [--exe <SecondSonPC.exe>] [--top 30] [--hitch 50]
        [--out <dir>]
The run directory holds r9_gprof.bin and gpu_frames.csv (a stand run's directory or a logs folder). Writes into --out
(default <run dir>/r9):
  frames.csv     one row per presented frame: the frame log's columns plus every thread group's CPU ms in the frame's
                 interval (between the previous present and this one), the counters' deltas, the marks
  phases.txt     per phase (--phase windows, the marks' intervals with --marks, else the whole window): FPS, 1% low,
                 frame time percentiles, CPU ms per frame by thread group, counters per frame
  hot_<phase>.txt (mode 2) the eboot's functions by exclusive and inclusive CPU ms per frame (samples weighted by the
                 cycles their thread ran in the tick), SecondSonPC's functions, the modules (driver, system), per
                 thread group
  hitches.txt    the slowest frames (over --hitch ms): what each thread group and counter did in them, and their samples
Thread groups come from the threads' names (set by the runtime from the game's scePthreadCreate names).
"""
import bisect
import collections
import csv
import math
import os
import re
import struct
import subprocess
import sys

REC_THREAD, REC_TICK, REC_SAMPLE, REC_MARK, REC_COUNTERS, REC_MODULE, REC_END = 1, 2, 3, 4, 5, 6, 7

GROUPS = [  # (group, regex on the thread's name) - first match
    ("game_main", r"^Game:Main"),
    ("jobs", r"^JobWorker"),
    ("fios", r"^SceFios"),
    ("audio", r"^(AK::|shadPS4:Ajm|SDLWASAPI|SecondSonPC:Audio)"),
    ("cp_t1", r"^SecondSonPC:GpuNative"),
    ("cp_front", r"^SecondSonPC:GpuFrontend"),
    ("cp_t2", r"^SecondSonPC:GpuRecord"),
    ("cp_emit", r"^SecondSonPC:GpuEmit"),
    ("present", r"^SecondSonPC:VideoOutPresent"),
    ("shader", r"^SecondSonPC:(Shader|Pipeline|Precomp)"),
    ("profiler", r"^SecondSonPC:R9Gprof"),
    ("game_other", r"^(Game:|Movie|Http|Trophy|Spp|Synergy|GameState|Flip)"),
    ("sspc_other", r"^(SecondSonPC:|shadPS4:)"),
    ("named_other", r"."),
]
GROUP_ORDER = [g for g, _ in GROUPS] + ["unnamed"]


def group_of(name):
    for g, rx in GROUPS:
        if re.search(rx, name):
            return g
    return "unnamed"


class Gprof:
    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:8] != b"SSPCR9GP":
            raise SystemExit(f"{path}: not an R9 gameplay profile")
        self.version, self.period_us = struct.unpack_from("<II", data, 8)
        (self.tsc_hz, self.origin, self.guest_lo, self.guest_hi, self.host_lo, self.host_hi,
         self.min_cycles) = struct.unpack_from("<7Q", data, 16)
        off = 16 + 56
        (n,) = struct.unpack_from("<I", data, off)
        off += 4
        self.counter_names = []
        for _ in range(n):
            (ln,) = struct.unpack_from("<H", data, off)
            self.counter_names.append(data[off + 2:off + 2 + ln].decode())
            off += 2 + ln
        self.threads = {}         # idx -> (tid, name)
        self.tick_t = []          # ms since the frame log's start
        self.tick_ran = []        # [(idx, cycles)]
        self.samples = []         # (tick index, thread idx, flags, cycles, addrs tuple)
        self.ctr_t, self.ctr_v = [], []
        self.marks = []           # (ms, label)
        self.modules = []         # (base, size, name)
        self.end = None
        size = len(data)
        to_ms = 1000.0 / self.tsc_hz
        origin = self.origin
        unpack = struct.unpack_from
        nctr = len(self.counter_names)
        while off + 4 <= size:
            (h,) = unpack("<I", data, off)
            typ, ln = h & 0xFF, h >> 8
            p = off + 4
            off = p + ln
            if off > size:
                break  # (a session cut short: the last record incomplete)
            if typ == REC_TICK:
                tsc, cnt = unpack("<QH", data, p)
                self.tick_t.append((tsc - origin) * to_ms)
                self.tick_ran.append([unpack("<HI", data, p + 10 + 6 * k) for k in range(cnt)])
            elif typ == REC_SAMPLE:
                idx, cnt, flags, cyc = unpack("<HBBI", data, p)
                self.samples.append((len(self.tick_t) - 1, idx, flags, cyc, unpack(f"<{cnt}Q", data, p + 8)))
            elif typ == REC_COUNTERS:
                vals = unpack(f"<Q{nctr}Q", data, p)
                self.ctr_t.append((vals[0] - origin) * to_ms)
                self.ctr_v.append(vals[1:])
            elif typ == REC_THREAD:
                idx, tid, nl = unpack("<HIH", data, p)
                self.threads[idx] = (tid, data[p + 8:p + 8 + nl].decode(errors="replace"))
            elif typ == REC_MARK:
                tsc, nl = unpack("<QH", data, p)
                self.marks.append(((tsc - origin) * to_ms, data[p + 10:p + 10 + nl].decode(errors="replace")))
            elif typ == REC_MODULE:
                base, sz, nl = unpack("<QIH", data, p)
                self.modules.append((base, sz, data[p + 14:p + 14 + nl].decode(errors="replace")))
            elif typ == REC_END:
                self.end = unpack("<8Q", data, p)
        self.cyc_to_ms = to_ms
        self.sample_ticks = [smp[0] for smp in self.samples]  # (ascending: samples follow their tick)
        self.group = {i: group_of(name) for i, (_, name) in self.threads.items()}
        if self.guest_hi == 0:  # (the eboot loads after the profiler starts: its range comes as the module "eboot")
            for base, sz, name in self.modules:
                if name == "eboot":
                    self.guest_lo, self.guest_hi = base, base + sz
        self.modules.sort()
        self.mod_bases = [m[0] for m in self.modules]
        self.exports = Exports()

    def samples_in(self, a, b):
        """The samples of ticks [a, b)."""
        return self.samples[bisect.bisect_left(self.sample_ticks, a):bisect.bisect_left(self.sample_ticks, b)]

    def split(self, flags, addrs):
        """A sample's (rip, eboot caller, SecondSonPC return addresses, eboot chain) - versions 1-3."""
        if self.version >= 3:
            nh = flags >> 4
            return addrs[0], addrs[1], addrs[2:2 + nh], addrs[2 + nh:]
        if self.version == 2:
            return addrs[0], addrs[1], (addrs[2],) if addrs[2] else (), addrs[3:]
        return addrs[0], addrs[1], (), addrs[2:]

    def export_of(self, a):
        i = bisect.bisect_right(self.mod_bases, a) - 1
        if i >= 0 and a < self.modules[i][0] + self.modules[i][1]:
            base, _, name = self.modules[i]
            if name == "eboot":
                return name
            return self.exports.name(name, base, a)
        return "?"

    def module_of(self, a):
        i = bisect.bisect_right(self.mod_bases, a) - 1
        if i >= 0 and a < self.modules[i][0] + self.modules[i][1]:
            return self.modules[i][2]
        return "?"


def read_frames(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if v not in ("", None) else None) for k, v in r.items()})
    return rows


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))] if s else float("nan")


def ft_stats(ft):
    if len(ft) < 2:
        return {}
    s = sorted(ft, reverse=True)
    w1 = s[: max(1, len(ft) // 100)]
    w01 = s[: max(1, len(ft) // 1000)]
    return {"frames": len(ft), "avg_fps": 1000.0 * len(ft) / sum(ft), "low1_fps": 1000.0 * len(w1) / sum(w1),
            "low01_fps": 1000.0 * len(w01) / sum(w01), "p50": pct(ft, 0.5), "p95": pct(ft, 0.95),
            "p99": pct(ft, 0.99), "p999": pct(ft, 0.999), "max": max(ft),
            "over50": sum(1 for d in ft if d > 50), "over100": sum(1 for d in ft if d > 100)}


class Functions:
    """The eboot's functions (export_functions.py TSV): eboot-relative address -> (entry, name)."""

    def __init__(self, path):
        self.lo, self.hi, self.names = [], [], []
        if not path or not os.path.exists(path):
            return
        with open(path, encoding="utf-8") as f:
            next(f)
            for line in f:
                e, end, _, name = line.rstrip("\n").split("\t")[:4]
                self.lo.append(int(e, 16))
                self.hi.append(int(end, 16))
                self.names.append(name)

    def of(self, rel):
        i = bisect.bisect_right(self.lo, rel) - 1
        if i >= 0 and rel < self.hi[i]:
            return self.lo[i]
        return None

    def name(self, entry):
        i = bisect.bisect_left(self.lo, entry)
        return self.names[i] if i < len(self.lo) and self.lo[i] == entry else "?"


class Exports:
    """A system DLL's exports (its file in System32): the nearest export at or below an address names it ("~name": a
    function between two exports takes the lower one's name)."""

    def __init__(self):
        self.cache = {}

    def table(self, name):
        if name in self.cache:
            return self.cache[name]
        rvas, names = [], []
        path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", name)
        try:
            data = open(path, "rb").read()
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            nsec = struct.unpack_from("<H", data, pe + 6)[0]
            opt = pe + 24
            exp_rva = struct.unpack_from("<I", data, opt + 112)[0]  # (PE32+: data directory 0)
            secs = []
            sh = opt + struct.unpack_from("<H", data, pe + 20)[0]
            for i in range(nsec):
                vsz, va, raw_sz, raw = struct.unpack_from("<IIII", data, sh + 40 * i + 8)
                secs.append((va, max(vsz, raw_sz), raw))

            def off(rva):
                for va, sz, raw in secs:
                    if va <= rva < va + sz:
                        return rva - va + raw
                raise ValueError(rva)

            e = off(exp_rva)
            nfun, nnames, afun, aname, aord = struct.unpack_from("<IIIII", data, e + 20)
            funcs = struct.unpack_from(f"<{nfun}I", data, off(afun))
            pairs = []
            for k in range(nnames):
                nrva = struct.unpack_from("<I", data, off(aname) + 4 * k)[0]
                o = struct.unpack_from("<H", data, off(aord) + 2 * k)[0]
                no = off(nrva)
                nm = data[no:data.index(b"\x00", no)].decode(errors="replace")
                pairs.append((funcs[o], nm))
            pairs.sort()
            rvas = [r for r, _ in pairs]
            names = [n for _, n in pairs]
        except (OSError, ValueError, struct.error):
            pass
        self.cache[name] = (rvas, names)
        return self.cache[name]

    def name(self, module, base, addr):
        rvas, names = self.table(module)
        i = bisect.bisect_right(rvas, addr - base) - 1
        return f"{module}!~{names[i]}" if i >= 0 else module


def symbolize_host(exe, addrs, host_lo):
    """SecondSonPC.exe addresses -> function names (llvm-symbolizer with the PDB beside the exe)."""
    if not exe or not addrs:
        return {}
    sym = r"E:\SecondSonPC\toolchain\LLVM\bin\llvm-symbolizer.exe"
    if not os.path.exists(sym):
        return {}
    with open(exe, "rb") as f:
        head = f.read(4096)
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    image_base = struct.unpack_from("<Q", head, pe + 24 + 24)[0]
    addrs = sorted(addrs)
    inp = "\n".join(f"0x{a - host_lo + image_base:x}" for a in addrs) + "\n"
    out = subprocess.run([sym, f"--obj={exe}", "--functions=short", "--no-inlines", "--demangle"], input=inp,
                         capture_output=True, text=True, timeout=600).stdout.split("\n")
    names = {}
    k = 0
    for a in addrs:
        # each address answers with: function, file:line, empty line
        while k < len(out) and out[k] == "":
            k += 1
        names[a] = out[k] if k < len(out) else "?"
        k += 3
    return names


# Game:Main's waits (docs/FRAMEBREAK.md, src/engine/render/frame_trace.cpp): the eboot's delay loop 0x53b2b0 under
# the async compute wait 0x1695e0 (from EndFrame 0x3edef0: the CPU reads the particles' emitter state; from 0x3ee470,
# a tail call returning into the main loop 0x29c8d0: the particles' light DMA) or under SubmitFrame 0x1f1e30 (the EOP of
# the frame before: one frame in flight on the GPU)
WAIT_KINDS = ("cw_emitters", "cw_light", "eop", "spin_other")
K_DELAY, K_CWAIT, K_ENDFRAME, K_MAINLOOP, K_SUBMIT = 0x53b2b0, 0x1695e0, 0x3edef0, 0x29c8d0, 0x1f1e30


def wait_kind(g, funcs, flags, addrs):
    """A Game:Main sample's wait (WAIT_KINDS), None when the game works."""
    rip = addrs[0]
    if not (g.guest_lo <= rip < g.guest_hi) or funcs.of(rip - g.guest_lo) != K_DELAY:
        return None
    chain = g.split(flags, addrs)[3]
    f = [funcs.of(r - g.guest_lo) for r in chain[:2]]
    if f and f[0] == K_CWAIT:
        if len(f) > 1 and f[1] == K_ENDFRAME:
            return "cw_emitters"
        if len(f) > 1 and f[1] == K_MAINLOOP:
            return "cw_light"
    if f and f[0] == K_SUBMIT:
        return "eop"
    return "spin_other"


# a running sample whose thread sits in one of these when it is suspended ran in the tick and then blocked: its cycles
# were spent elsewhere (the job workers run a short job and wait) - counted apart, not as the wait's CPU time
WAIT_SYSCALLS = {"ZwWaitForAlertByThreadId", "ZwWaitForSingleObject", "ZwWaitForMultipleObjects", "ZwDelayExecution",
                 "ZwRemoveIoCompletion", "ZwRemoveIoCompletionEx", "ZwWaitForWorkViaWorkerFactory",
                 "ZwSignalAndWaitForSingleObject", "ZwWaitForKeyedEvent", "ZwUserMsgWaitForMultipleObjectsEx"}


def blocked_at(g, rip):
    """The instruction pointer is a wait system call's (outside the eboot and SecondSonPC)."""
    if g.guest_lo <= rip < g.guest_hi or g.host_lo <= rip < g.host_hi:
        return False
    name = g.export_of(rip)
    return name.split("!~", 1)[-1] in WAIT_SYSCALLS if "!~" in name else False


def main_waits(g, funcs, a, b):
    """Game:Main's cycles in ticks [a, b) by wait kind, plus "work"."""
    c = collections.Counter()
    for (tick, idx, flags, cyc, addrs) in g.samples_in(a, b):
        if g.group.get(idx) == "game_main" and not flags & 2:
            c[wait_kind(g, funcs, flags, addrs) or "work"] += cyc
    return c


def frame_rows(g, frames, t0, t1):
    """Presented frames inside [t0, t1] ms with every group's CPU ms and the counters' deltas in their interval."""
    pres = [(r["present_ms"], r) for r in frames if r.get("present_ms")]
    pres.sort(key=lambda x: x[0])
    out = []
    ti = 0
    ci = 0
    nt = len(g.tick_t)
    for k in range(1, len(pres)):
        a, b = pres[k - 1][0], pres[k][0]
        if b < t0 or b > t1:
            continue
        while ti < nt and g.tick_t[ti] <= a:
            ti += 1
        cpu = collections.Counter()
        j = ti
        while j < nt and g.tick_t[j] <= b:
            for idx, cyc in g.tick_ran[j]:
                cpu[g.group.get(idx, "unnamed")] += cyc
            j += 1
        while ci < len(g.ctr_t) and g.ctr_t[ci] <= a:
            ci += 1
        c0 = g.ctr_v[ci - 1] if ci > 0 else None
        cj = ci
        while cj < len(g.ctr_t) and g.ctr_t[cj] <= b:
            cj += 1
        c1 = g.ctr_v[cj - 1] if cj > 0 else None
        row = dict(pres[k][1])
        row["t_s"] = b / 1000.0
        row["ft_ms"] = b - a
        for gr in GROUP_ORDER:
            row["cpu_" + gr] = cpu.get(gr, 0) * g.cyc_to_ms
        if c0 is not None and c1 is not None:
            for name, v0, v1 in zip(g.counter_names, c0, c1):
                row["d_" + name] = v1 - v0
        row["marks"] = ";".join(lbl for t, lbl in g.marks if a < t <= b)
        row["_ticks"] = (ti, j)
        out.append(row)
    return out


def phases_from_marks(g, t0, t1):
    """The marks' intervals: each mark starts a phase that lasts until the next mark (or the window's end)."""
    ms = [(t, lbl) for t, lbl in g.marks if t0 <= t <= t1]
    out = []
    for i, (t, lbl) in enumerate(ms):
        end = ms[i + 1][0] if i + 1 < len(ms) else t1
        out.append((f"{i:02d}_{lbl}", t, end))
    return out


def hot(g, funcs, exe, ticks_range, nframes, top):
    """The code samples of ticks [a, b): CPU ms per frame by eboot function (exclusive / inclusive), by SecondSonPC
    function, by module, per thread group."""
    a, b = ticks_range
    lo = g.guest_lo
    excl = collections.defaultdict(collections.Counter)   # group -> entry -> cycles
    incl = collections.defaultdict(collections.Counter)
    host = collections.defaultdict(collections.Counter)   # group -> host rip -> cycles
    mods = collections.defaultdict(collections.Counter)
    host_caller = collections.defaultdict(collections.Counter)  # group -> eboot entry of the caller of host code
    sys_by_host = collections.defaultdict(collections.Counter)  # group -> SecondSonPC return address -> cycles
    sys_fn = collections.defaultdict(collections.Counter)       # group -> "module!~export" -> cycles
    waits = collections.defaultdict(collections.Counter)  # group -> (SecondSonPC stack) -> waiting samples
    nwait = collections.Counter()
    blocked = collections.Counter()  # group -> cycles of running samples taken in a wait system call (see blocked_at)
    total = collections.Counter()
    sampled = collections.Counter()
    for (tick, idx, flags, cyc, addrs) in g.samples_in(a, b):
        gr = g.group.get(idx, "unnamed")
        rip, caller, hosts, chain = g.split(flags, addrs)
        if flags & 2:  # a waiting sample (version 3): what a watched thread that did not run is blocked in
            waits[gr][(rip,) + tuple(hosts[:4])] += 1
            nwait[gr] += 1
            continue
        sampled[gr] += cyc
        if blocked_at(g, rip):
            blocked[gr] += cyc
            continue
        hret = hosts[0] if hosts else 0
        seen = set()
        if g.guest_lo <= rip < g.guest_hi:
            e = funcs.of(rip - lo)
            excl[gr][e if e is not None else ("?", rip - lo)] += cyc
            if e is not None:
                seen.add(e)
        else:
            mods[gr][g.module_of(rip)] += cyc
            if g.host_lo <= rip < g.host_hi:
                host[gr][rip] += cyc
            else:
                sys_fn[gr][g.export_of(rip)] += cyc
                if hret:
                    sys_by_host[gr][hret] += cyc
            if caller:
                e = funcs.of(caller - lo)
                host_caller[gr][e] += cyc
                if e is not None:
                    seen.add(e)
        for r in chain:
            e = funcs.of(r - lo)
            if e is not None:
                seen.add(e)
        for e in seen:
            incl[gr][e] += cyc
    for ran in g.tick_ran[a:b]:
        for idx, cyc in ran:
            total[g.group.get(idx, "unnamed")] += cyc
    all_host = set()
    for gr in host:
        all_host |= set(host[gr])
    for gr in sys_by_host:
        all_host |= set(sys_by_host[gr])
    for gr in waits:
        for key in waits[gr]:
            all_host |= {x for x in key if g.host_lo <= x < g.host_hi}
    names = symbolize_host(exe, all_host, g.host_lo)
    k = g.cyc_to_ms / max(nframes, 1)
    lines = []
    def where(x):
        if g.host_lo <= x < g.host_hi:
            return names.get(x, f"exe+{x - g.host_lo:#x}")
        if g.guest_lo <= x < g.guest_hi:
            e = funcs.of(x - lo)
            return f"eboot {e:#x} {funcs.name(e)}" if e is not None else "eboot ?"
        return g.export_of(x)

    for gr in GROUP_ORDER:
        if total.get(gr, 0) == 0 and not nwait.get(gr):
            continue
        lines.append(f"== {gr}: CPU {total[gr] * k:.2f} ms/frame, sampled {sampled[gr] * k:.2f} ms/frame "
                     f"({100.0 * sampled[gr] / max(total[gr], 1):.0f}% of it in ticks that ran >= threshold)")
        if blocked.get(gr):
            lines.append(f"  ran in the tick, blocked in a wait when sampled (where these cycles went is not seen): "
                         f"{blocked[gr] * k:.2f} ms/frame ({100.0 * blocked[gr] / max(sampled[gr], 1):.1f}%)")
        if excl[gr]:
            lines.append("  eboot, exclusive (leaf in the eboot)            ms/frame   share")
            for e, c in excl[gr].most_common(top):
                nm = funcs.name(e) if not isinstance(e, tuple) else f"? +{e[1]:x}"
                ent = f"{e:#x}" if not isinstance(e, tuple) else "?"
                lines.append(f"    {ent:>10} {nm:<36} {c * k:8.3f}   {100.0 * c / max(sampled[gr], 1):5.1f}%")
        if incl[gr]:
            lines.append("  eboot, inclusive (any frame of the chain)")
            for e, c in incl[gr].most_common(top):
                lines.append(f"    {e:#10x} {funcs.name(e):<36} {c * k:8.3f}   {100.0 * c / max(sampled[gr], 1):5.1f}%")
        if mods[gr]:
            lines.append("  outside the eboot, by module")
            for m, c in mods[gr].most_common(12):
                lines.append(f"    {m:<47} {c * k:8.3f}   {100.0 * c / max(sampled[gr], 1):5.1f}%")
        if host[gr]:
            byname = collections.Counter()
            for rip, c in host[gr].items():
                byname[names.get(rip, f"{rip - g.host_lo:#x}")] += c
            lines.append("  SecondSonPC.exe, by function")
            for nm, c in byname.most_common(top):
                lines.append(f"    {nm[:80]:<80} {c * k:8.3f}   {100.0 * c / max(sampled[gr], 1):5.1f}%")
        if sys_fn[gr]:
            lines.append("  driver/system code, by nearest export")
            for nm, c in sys_fn[gr].most_common(12):
                lines.append(f"    {nm[:80]:<80} {c * k:8.3f}   {100.0 * c / max(sampled[gr], 1):5.1f}%")
        if sys_by_host[gr]:
            byname = collections.Counter()
            for a, c in sys_by_host[gr].items():
                byname[names.get(a, f"{a - g.host_lo:#x}")] += c
            lines.append("  driver/system time by its SecondSonPC caller")
            for nm, c in byname.most_common(15):
                lines.append(f"    {nm[:80]:<80} {c * k:8.3f}   {100.0 * c / max(sampled[gr], 1):5.1f}%")
        if host_caller[gr]:
            lines.append("  host/driver/system time by its eboot caller (the game's call into SecondSonPC)")
            for e, c in host_caller[gr].most_common(15):
                ent = f"{e:#x}" if e is not None else "?"
                lines.append(f"    {ent:>10} {funcs.name(e) if e is not None else '?':<36} {c * k:8.3f}")
        if nwait.get(gr):
            byw = collections.Counter()
            for key, c in waits[gr].items():
                byw[" < ".join(where(x)[:60] for x in key)] += c
            lines.append(f"  waiting (not running) - {nwait[gr]} samples of the watched threads, share of them; "
                         f"instruction pointer < SecondSonPC callers")
            for key, c in byw.most_common(12):
                lines.append(f"    {100.0 * c / nwait[gr]:5.1f}%  {key[:230]}")
        lines.append("")
    return "\n".join(lines)


def main(argv):
    if len(argv) < 3 or argv[1] != "report":
        print(__doc__)
        return 2
    run = argv[2]
    opts = {"from": None, "to": None, "funcs": r"E:\research\r9\ghidra\functions.tsv", "exe": None, "top": "30",
            "hitch": "50", "out": None}
    phases = []
    use_marks = False
    i = 3
    while i < len(argv):
        a = argv[i]
        if a == "--phase":
            name, span = argv[i + 1].split("=", 1)
            s0, s1 = span.split("-", 1)
            phases.append((name, float(s0) * 1000.0, float(s1) * 1000.0))
            i += 2
        elif a == "--marks":
            use_marks = True
            i += 1
        elif a.startswith("--") and a[2:] in opts:
            opts[a[2:]] = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {a}")
    out = opts["out"] or os.path.join(run, "r9")
    os.makedirs(out, exist_ok=True)
    g = Gprof(os.path.join(run, "r9_gprof.bin"))
    frames = read_frames(os.path.join(run, "gpu_frames.csv"))
    t0 = float(opts["from"]) * 1000.0 if opts["from"] else 0.0
    t1 = float(opts["to"]) * 1000.0 if opts["to"] else 1e12
    rows = frame_rows(g, frames, t0, t1)
    if not rows:
        raise SystemExit("no presented frame in the window")
    funcs = Functions(opts["funcs"])
    if g.samples:  # (mode 2) Game:Main's waits per frame
        for r in rows:
            mw = main_waits(g, funcs, *r["_ticks"])
            for kind in WAIT_KINDS + ("work",):
                r["main_" + kind] = mw.get(kind, 0) * g.cyc_to_ms
    # frames.csv
    cols = [c for c in rows[0].keys() if not c.startswith("_")]
    with open(os.path.join(out, "frames.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for r in rows:
            w.writerow([(f"{r[c]:.3f}" if isinstance(r.get(c), float) else r.get(c, "")) for c in cols])
    if use_marks:
        phases += phases_from_marks(g, t0, min(t1, rows[-1]["present_ms"]))
    if not phases:
        phases = [("window", rows[0]["present_ms"] - rows[0]["ft_ms"], rows[-1]["present_ms"])]
    exe = opts["exe"]
    top = int(opts["top"])
    # phases.txt
    lines = [f"R9 gameplay profile {run}: period {g.period_us} us, threads {len(g.threads)}, ticks {len(g.tick_t)}, "
             f"samples {len(g.samples)}, marks {len(g.marks)}"]
    if g.end:
        ticks, samples, failed, dropped, susp, selfc, late, mdrop = g.end
        lines.append(f"profiler: its thread {selfc * g.cyc_to_ms / 1000:.2f} s CPU, threads suspended "
                     f"{susp * g.cyc_to_ms / 1000:.3f} s, late ticks {late}, dropped records {dropped}, "
                     f"failed samples {failed}")
    lines.append("marks: " + ", ".join(f"{t / 1000:.1f}s {lbl}" for t, lbl in g.marks))
    for name, p0, p1 in phases:
        pr = [r for r in rows if p0 < r["present_ms"] <= p1]
        if len(pr) < 2:
            continue
        st = ft_stats([r["ft_ms"] for r in pr])
        lines.append(f"\n== phase {name}: {p0 / 1000:.1f}..{p1 / 1000:.1f} s, {st['frames']} frames")
        lines.append(f"  FPS {st['avg_fps']:.2f}, 1% low {st['low1_fps']:.2f}, 0.1% low {st['low01_fps']:.2f}, "
                     f"p50 {st['p50']:.2f} p95 {st['p95']:.2f} p99 {st['p99']:.2f} p99.9 {st['p999']:.2f} "
                     f"max {st['max']:.1f} ms; frames > 50 ms {st['over50']}, > 100 ms {st['over100']}")
        n = len(pr)
        lines.append("  CPU ms/frame by thread group: " + ", ".join(
            f"{gr} {sum(r['cpu_' + gr] for r in pr) / n:.2f}" for gr in GROUP_ORDER
            if sum(r['cpu_' + gr] for r in pr) / n >= 0.05))
        fl = ["cp_ms", "record_ms", "gpu_busy_ms", "par_busy_ms", "par_idle_ms", "par_wait_ms", "draws", "dispatches",
              "pipelines", "pipeline_ms", "shaders", "shader_ms"]
        lines.append("  frame log per frame: " + ", ".join(
            f"{c} {sum((r.get(c) or 0) for r in pr) / n:.2f}" for c in fl if c in pr[0]))
        lines.append("  counters per frame: " + ", ".join(
            f"{c} {sum((r.get('d_' + c) or 0) for r in pr) / n:.2f}" for c in g.counter_names
            if c not in ("game_flips", "cp_flips", "presents", "marks")))
        if g.samples:
            lines.append("  Game:Main per frame (ms): " + ", ".join(
                f"{kind} {sum(r['main_' + kind] for r in pr) / n:.2f}" for kind in ("work",) + WAIT_KINDS)
                + "  (cw_emitters: async compute wait from EndFrame 0x3edef0; cw_light: from 0x3ee470; eop: SubmitFrame "
                "0x1f1e30)")
            ta, tb = pr[0]["_ticks"][0], pr[-1]["_ticks"][1]
            txt = hot(g, funcs, exe, (ta, tb), n, top)
            with open(os.path.join(out, f"hot_{name}.txt"), "w", encoding="utf-8") as f:
                f.write(f"R9 hot code, phase {name} ({p0 / 1000:.1f}..{p1 / 1000:.1f} s, {n} frames); ms per frame are "
                        f"CPU time (samples weighted by their thread's cycles in the tick)\n\n" + txt)
    with open(os.path.join(out, "phases.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    # hitches.txt
    hitch = float(opts["hitch"])
    slow = sorted([r for r in rows if r["ft_ms"] > hitch], key=lambda r: -r["ft_ms"])[:40]
    hl = [f"frames over {hitch} ms: {sum(1 for r in rows if r['ft_ms'] > hitch)} of {len(rows)}"]
    for r in slow:
        top_g = sorted(((r["cpu_" + gr], gr) for gr in GROUP_ORDER), reverse=True)[:6]
        hl.append(f"\n{r['t_s']:.3f}s frame {r['ft_ms']:.1f} ms: cp {r.get('cp_ms') or 0:.1f}, record "
                  f"{r.get('record_ms') or 0:.1f}, gpu {r.get('gpu_busy_ms') or 0:.1f}, pipelines "
                  f"{r.get('pipelines') or 0:.0f} ({r.get('pipeline_ms') or 0:.1f} ms), shaders {r.get('shaders') or 0:.0f} "
                  f"({r.get('shader_ms') or 0:.1f} ms), file reads {r.get('d_file_reads', 0):.0f} "
                  f"({r.get('d_file_read_bytes', 0) / 1048576:.1f} MiB, {r.get('d_file_read_ns', 0) / 1e6:.1f} ms), maps "
                  f"{r.get('d_guest_maps', 0):.0f}/{r.get('d_guest_unmaps', 0):.0f}, faults "
                  f"{r.get('d_tracker_faults', 0):.0f} {r['marks']}")
        hl.append("  CPU ms: " + ", ".join(f"{gr} {v:.1f}" for v, gr in top_g if v > 0.05))
        if g.samples:
            a, b = r["_ticks"]
            leaf = collections.Counter()
            wt = collections.Counter()
            for (tick, idx, flags, cyc, addrs) in g.samples_in(a, b):
                gr = g.group.get(idx, "unnamed")
                if flags & 2:
                    rip, caller, hosts, chain = g.split(flags, addrs)
                    wt[(gr, (rip,) + tuple(hosts[:4]))] += 1
                    continue
                if g.guest_lo <= addrs[0] < g.guest_hi:
                    e = funcs.of(addrs[0] - g.guest_lo)
                    key = f"{gr}: eboot {e:#x} {funcs.name(e)}" if e is not None else f"{gr}: eboot ?"
                elif blocked_at(g, addrs[0]):
                    key = f"{gr}: (ran, then blocked in a wait when sampled)"
                else:
                    key = f"{gr}: {g.module_of(addrs[0])}"
                leaf[key] += cyc
            for key, c in leaf.most_common(8):
                hl.append(f"    {c * g.cyc_to_ms:7.2f} ms  {key}")
            if wt:
                hosts_all = {x for (_, key) in wt for x in key if g.host_lo <= x < g.host_hi}
                nm = symbolize_host(exe, hosts_all, g.host_lo)

                def w(x):
                    if g.host_lo <= x < g.host_hi:
                        return nm.get(x, f"exe+{x - g.host_lo:#x}")[:60]
                    if g.guest_lo <= x < g.guest_hi:
                        e = funcs.of(x - g.guest_lo)
                        return f"eboot {e:#x}" if e is not None else "eboot ?"
                    return g.export_of(x)
                hl.append("  waiting (watched threads, samples every 10 ticks):")
                for (gr, key), c in wt.most_common(10):
                    hl.append(f"    {c:4d}x {gr}: " + " < ".join(w(x) for x in key))
    with open(os.path.join(out, "hitches.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(hl) + "\n")
    print("\n".join(lines))
    print(f"-> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
