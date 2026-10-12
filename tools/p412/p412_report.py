# SecondSonPC P4.12: the stall investigation's report - every run under logs\p412 (tools\tests\p412_runs.ps1) together.
#   py -I tools\p412\p412_report.py [--root logs\p412] [--top10 logs\p412\top10.json]
# Reads per run: window.json (tools\frame_stats.py over the scene's A/B window), gpu_frames.csv, p412_frames.csv (light),
# p412_analysis.json (deep: tools\p412\p412_analyze.py). Writes into the root:
#   summary.json        per scene x configuration: the runs' FPS figures, the trace's waits per frame, the overhead of
#                       the trace (light vs off), the deep runs' critical paths
#   stalls.csv          per run: every traced wait/span per frame (light: per-frame totals over the window)
#   critical_path.csv   per deep run: the path's segments (ms/frame, class)
#   frame_outliers.csv  every frame of the windows over 33.3 / 41.7 / 50 ms (presented or emulated) and its class
#   trace.html          the report: the threads' timelines of chosen frames, the paths, S0..S4, the TOP-10 lists
import collections
import csv
import glob
import html
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from p412_analyze import SEG_CLASS  # noqa: E402

SCENES = {"user1022": ("деревня", 25.0, 50.0), "beach": ("пляж", 62.0, 88.0), "early": ("гавань", 62.0, 88.0)}
CONFIGS = {"S0": "однопоточный", "S1": "параллельный", "S2": "параллельный + B", "S3": "параллельный + async",
           "S4": "параллельный + B + async"}
SEG_RU = {
    "GPU executing the frame's batches": "GPU исполняет пакеты кадра",
    "thread 1 busy": "поток 1 (PM4/фронтенд) занят",
    "thread 2 busy": "поток 2 (запись Vulkan) занят",
    "GPU idle with a batch submitted (bind wait / driver)": "GPU простаивает при отправленном пакете",
    "EOP observation (GPU done -> thread 1 saw it)": "наблюдение EOP (GPU закончил → поток 1 увидел)",
    "thread 1 wake-up after a drain": "пробуждение потока 1 после слива",
    "thread 2 wake-up after a push": "пробуждение потока 2 после публикации записи",
    "CPU wake-up after a GPU wait": "пробуждение CPU после ожидания GPU",
    "game answer (EOP seen -> first submission)": "ответ игры (EOP → первый сабмит)",
    "GPU start after the submission": "старт GPU после сабмита",
    "thread 1 waits for the game (submissions)": "поток 1 ждёт игру (сабмиты)",
    "thread 1 wake-up (submission -> processing)": "пробуждение потока 1 (сабмит → разбор)",
    "Vulkan queue lock (another thread submits/presents)": "мьютекс очереди Vulkan",
    "WAIT_REG_MEM on a VideoOut label (flip not shown yet)": "WAIT_REG_MEM на метке VideoOut (ждёт показа)",
    "WAIT_REG_MEM on a flag the game's CPU writes": "WAIT_REG_MEM на флаге CPU игры",
    "flip waits for a presenter frame": "флип ждёт кадр презентера",
    "submission pickup delayed (thread 1 in a GPU wait)": "сабмит ждёт поток 1 (тот в ожидании GPU)",
    "thread 1 idle (GPU wait ended without a completion)": "поток 1 простаивает (ожидание GPU без завершения)",
    "untraced gap": "не размечено",
}
CLASS_RU = {"A": "Idle", "B": "Overhead", "C": "Required", "D": "Unknown", "work": "Работа"}


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))]


def mean(v):
    v = [x for x in v if x is not None and not (isinstance(x, float) and math.isnan(x))]
    return sum(v) / len(v) if v else float("nan")


def read_csv(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def discover(root):
    runs = []
    for d in sorted(glob.glob(os.path.join(root, "*", "S?_*_*", "*_1"))):
        name = os.path.basename(os.path.dirname(d))
        m = re.match(r"(S\d)_(\w+?)_(\d+)$", name)
        if not m:
            continue
        tag = os.path.basename(os.path.dirname(os.path.dirname(d)))
        if tag.startswith("X_"):
            continue  # special series (ETW, Inspector, GPU profile): their own overhead - not in the A/B figures
        cfg, scene, k = m.group(1), m.group(2), int(m.group(3))
        if scene not in SCENES or not os.path.exists(os.path.join(d, "gpu_frames.csv")):
            continue
        light = os.path.join(d, "p412_light.json")
        mode = "off"
        if os.path.exists(light):
            try:
                mode = json.load(open(light, encoding="utf-8")).get("mode", "light")
            except (OSError, ValueError):
                mode = "light"
        runs.append({"dir": d, "tag": tag, "cfg": cfg, "scene": scene, "k": k, "mode": mode,
                     "id": f"{tag}/{name}"})
    return runs


def frame_rows(path):
    return [{k: num(v) for k, v in r.items()} for r in read_csv(path)]


def window_metrics(run):
    """FPS figures, long frames (presented and emulated) and the trace's per-frame waits over the scene's window."""
    _, a, b = SCENES[run["scene"]]
    lo, hi = a * 1000.0, b * 1000.0
    rows = frame_rows(os.path.join(run["dir"], "gpu_frames.csv"))
    w = {}
    wj = os.path.join(run["dir"], "window.json")
    if os.path.exists(wj):
        w = json.load(open(wj, encoding="utf-8"))
    pres = [(r["n"], r["present_ms"]) for r in rows if r.get("present_ms") and lo <= r["present_ms"] <= hi]
    pint = {int(n2): t2 - t1 for (n1, t1), (n2, t2) in zip(pres, pres[1:])}
    done = [(r["n"], r["gpu_done_ms"]) for r in rows if r.get("gpu_done_ms") and lo <= r["gpu_done_ms"] <= hi]
    eint = {int(n2): t2 - t1 for (n1, t1), (n2, t2) in zip(done, done[1:])}
    by_n = {int(r["n"]): r for r in rows}
    out = {"window": w, "frames": len(pint)}
    for name, ints in (("present", list(pint.values())), ("emulated", list(eint.values()))):
        n = max(1, len(ints))
        out[name] = {"mean_ms": round(mean(ints), 3), "p50_ms": round(pct(ints, 0.5), 2),
                     "p95_ms": round(pct(ints, 0.95), 2), "p99_ms": round(pct(ints, 0.99), 2),
                     "max_ms": round(max(ints), 2) if ints else None,
                     "over_33_3": sum(1 for x in ints if x > 33.4), "over_41_7": sum(1 for x in ints if x > 41.7),
                     "over_50": sum(1 for x in ints if x > 50.1), "n": len(ints)}
        out[name]["share_over_33_3"] = round(out[name]["over_33_3"] / n, 4)
        out[name]["share_over_41_7"] = round(out[name]["over_41_7"] / n, 4)
        out[name]["share_over_50"] = round(out[name]["over_50"] / n, 4)
    # the trace's per-frame totals (frames whose flip marker is in the window)
    frames_in = {int(r["n"]) for r in rows if r.get("cp_flip_ms") and lo <= r["cp_flip_ms"] <= hi}
    per = collections.defaultdict(lambda: collections.defaultdict(float))
    cnt = collections.defaultdict(lambda: collections.defaultdict(float))
    pf = os.path.join(run["dir"], "p412_frames.csv")
    if os.path.exists(pf) and frames_in:
        with open(pf, newline="") as f:
            for r in csv.DictReader(f):
                n = int(r["n"])
                if n in frames_in:
                    key = f"{r['role']}.{r['event']}"
                    per[key][n] += float(r["ms"])
                    cnt[key][n] += float(r["count"])
    stalls = {}
    nf = max(1, len(frames_in))
    for key, d in per.items():
        vals = [d.get(n, 0.0) for n in frames_in]
        stalls[key] = {"ms_per_frame": round(sum(vals) / nf, 4), "p95_frame_ms": round(pct(vals, 0.95), 3),
                       "p99_frame_ms": round(pct(vals, 0.99), 3), "max_frame_ms": round(max(vals), 3),
                       "count_per_frame": round(sum(cnt[key].values()) / nf, 2)}
    out["stalls"] = stalls
    # the long frames of the window, each with its own figures and a class
    med = {k: pct([by_n[n].get(k) or 0.0 for n in frames_in if n in by_n], 0.5)
           for k in ("cp_ms", "gpu_busy_ms", "par_wait_ms", "par_busy_ms")}
    outl = []
    for n in sorted(set(pint) | set(eint)):
        pi_, ei_ = pint.get(n), eint.get(n)
        if (pi_ or 0) <= 33.4 and (ei_ or 0) <= 33.4:
            continue
        r = by_n.get(n, {})
        comp = (r.get("shader_ms") or 0) + (r.get("pipeline_ms") or 0) + (r.get("module_ms") or 0)
        lbl = per.get("CP.IdleLabel", {}).get(n, 0.0)
        flag = per.get("CP.IdleFlag", {}).get(n, 0.0)
        gpu_gap = per.get("GPU.GpuGapInFrame", {}).get(n, 0.0)
        if comp >= 3.0:
            cls = "компиляция шейдеров/pipeline"
        elif ei_ is not None and ei_ <= 33.4 and (pi_ or 0) > 33.4:
            cls = "фаза vblank (эмуляция ≤ 33,3 мс, показ пропустил vblank)"
        elif (r.get("gpu_busy_ms") or 0) - med["gpu_busy_ms"] >= 4.0:
            cls = "GPU-работа кадра (больше обычного)"
        elif lbl >= 4.0 or flag >= 4.0:
            cls = "WAIT_REG_MEM (метка VideoOut / флаг CPU)"
        elif (r.get("cp_ms") or 0) - med["cp_ms"] >= 3.0:
            cls = "CPU командного процессора (больше обычного)"
        else:
            cls = "пограничный (несколько мс сверх 2 vblank, без одной причины)"
        outl.append({"n": n, "t_s": round((r.get("present_ms") or r.get("gpu_done_ms") or 0) / 1000.0, 3),
                     "present_ms": round(pi_, 2) if pi_ else None, "emulated_ms": round(ei_, 2) if ei_ else None,
                     "cp_ms": r.get("cp_ms"), "gpu_busy_ms": r.get("gpu_busy_ms"), "compile_ms": round(comp, 2),
                     "shaders": r.get("shaders"), "pipelines": r.get("pipelines"), "par_wait_ms": r.get("par_wait_ms"),
                     "idle_label_ms": round(lbl, 2), "gpu_gap_ms": round(gpu_gap, 2), "draws": r.get("draws"),
                     "dispatches": r.get("dispatches"), "class": cls})
    out["outliers"] = outl
    out["medians"] = {k: round(v, 3) for k, v in med.items()}
    return out


def agg_runs(ms):
    """Mean and range of a figure over runs."""
    v = [x for x in ms if x is not None and not (isinstance(x, float) and math.isnan(x))]
    if not v:
        return None
    return {"mean": round(sum(v) / len(v), 3), "min": round(min(v), 3), "max": round(max(v), 3), "n": len(v)}


def svg_timeline(tl, width=1180):
    lanes = [("Путь", None), ("CP", "поток 1"), ("Record", "поток 2"), ("GPU", "GPU"), ("Present", "VideoOut")]
    h_lane, top = 22, 18
    span = max(tl["emu_ms"], 1e-3)
    sx = (width - 90) / span
    colors = {"Drain": "#d9534f", "QueueFull": "#a94442", "JournalFull": "#a94442", "RecordWait": "#f0ad4e",
              "IdleGpu": "#5bc0de", "IdleEmpty": "#cccccc", "IdleLabel": "#9b59b6", "IdleFlag": "#8e44ad",
              "GpuSync": "#31708f", "RingWait": "#31708f", "QueueLock": "#e74c3c", "Submit": "#2ecc71",
              "Poll": "#95a5a6", "PreRound": "#7f8c8d", "PipelineCreate": "#e67e22", "ShaderTranslate": "#d35400",
              "FlipAcquire": "#c0392b", "VblankWait": "#ecf0f1", "Present": "#3498db", "PresentSubmit": "#2980b9",
              "PresentFence": "#1abc9c", "PresentAcquire": "#16a085", "batch": "#27ae60"}
    seg_colors = {"GPU": "#27ae60", "CP": "#2c3e50", "Record": "#8e44ad", "Game": "#7f8c8d"}
    parts = [f'<svg viewBox="0 0 {width} {top + h_lane * len(lanes) + 24}" class="tl" role="img">']
    for i, (key, label) in enumerate(lanes):
        y = top + i * h_lane
        parts.append(f'<text x="2" y="{y + 15}" class="lane">{html.escape(label or "критич. путь")}</text>')
        parts.append(f'<line x1="88" y1="{y + h_lane - 1}" x2="{width}" y2="{y + h_lane - 1}" class="grid"/>')
        if key == "Путь":
            for a, b, label2, where in tl["path"]:
                x = 90 + a * sx
                wdt = max(0.6, (b - a) * sx)
                parts.append(f'<rect x="{x:.1f}" y="{y + 4}" width="{wdt:.1f}" height="{h_lane - 8}" '
                             f'fill="{seg_colors.get(where, "#999")}"><title>{html.escape(SEG_RU.get(label2, label2))}'
                             f' {b - a:.3f} мс</title></rect>')
            continue
        for a, b, ev in tl["lanes"].get(key, []):
            if ev == "VblankWait" or b - a < 0.002:
                continue
            x = 90 + a * sx
            wdt = max(0.5, (b - a) * sx)
            parts.append(f'<rect x="{x:.1f}" y="{y + 3}" width="{wdt:.1f}" height="{h_lane - 6}" '
                         f'fill="{colors.get(ev, "#bbb")}"><title>{ev} {b - a:.3f} мс</title></rect>')
    for t in range(0, int(span) + 1, 5):
        x = 90 + t * sx
        parts.append(f'<text x="{x:.1f}" y="{top + h_lane * len(lanes) + 14}" class="tick">{t} мс</text>')
    parts.append("</svg>")
    return "".join(parts)


def fmt(x, d=2):
    if x is None:
        return "—"
    if isinstance(x, dict):
        if x.get("n", 1) > 1 and x["max"] - x["min"] > 0:
            return f"{x['mean']:.{d}f} <span class=r>({x['min']:.{d}f}–{x['max']:.{d}f})</span>"
        return f"{x['mean']:.{d}f}"
    if isinstance(x, float) and math.isnan(x):
        return "—"
    return f"{x:.{d}f}" if isinstance(x, (int, float)) else html.escape(str(x))


def write_html(root, runs, summary, top10):
    css = """
:root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#e5e5ea;--card:#f7f7f9;--acc:#0a5bd3}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#121214;--fg:#ececf0;--mut:#9a9aa2;--line:#2c2c31;--card:#1b1b1f;--acc:#6ea8ff}}
:root[data-theme=dark]{--bg:#121214;--fg:#ececf0;--mut:#9a9aa2;--line:#2c2c31;--card:#1b1b1f;--acc:#6ea8ff}
body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif;margin:0;padding:16px}
main{max-width:1240px;margin:0 auto}h1{font-size:22px}h2{font-size:18px;margin-top:28px;border-bottom:1px solid var(--line)}
h3{font-size:15px}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;margin:8px 0}
th,td{border-bottom:1px solid var(--line);padding:4px 6px;text-align:left;vertical-align:top}th{color:var(--mut);font-weight:600}
.r{color:var(--mut);font-size:12px}.card{background:var(--card);padding:8px 12px;border-radius:8px;margin:8px 0}
.wrap{overflow-x:auto}.tl{width:100%;height:auto;background:var(--card);border-radius:6px}.lane{font-size:11px;fill:var(--mut)}
.tick{font-size:10px;fill:var(--mut)}.grid{stroke:var(--line)}.bar{height:12px;display:inline-block;vertical-align:middle}
"""
    out = ["<!doctype html><html lang=ru><head><meta charset=utf-8><meta name=viewport content='width=device-width,"
           "initial-scale=1'><title>P4.12 Stall Analysis</title><style>" + css + "</style></head><body><main>"]
    out.append("<h1>SecondSonPC P4.12 — простои и критический путь кадра</h1>")
    out.append(f"<p class=r>Прогонов: {summary['runs']}. Окна A/B: деревня 25–50 с, пляж и гавань 62–88 с; глубокая "
               "трасса — деревня 30–42 с, пляж/гавань 70–82 с. Данные: summary.json, stalls.csv, critical_path.csv, "
               "frame_outliers.csv.</p>")
    if top10:
        out.append("<h2>ТОП-10 по сценам (по вкладу в критический путь)</h2>")
        for block in top10.get("blocks", []):
            out.append(f"<h3>{html.escape(block['title'])}</h3><div class=wrap><table><tr>")
            cols = block["columns"]
            out.append("".join(f"<th>{html.escape(c)}</th>" for c in cols) + "</tr>")
            for row in block["rows"]:
                cells = row if isinstance(row, list) else [row.get(c, "") for c in cols]  # (a list or a dict by column)
                out.append("<tr>" + "".join(f"<td>{html.escape(str(v))}</td>" for v in cells) + "</tr>")
            out.append("</table></div>")
            if block.get("note"):
                out.append(f"<p class=r>{html.escape(block['note'])}</p>")
    # S0..S4
    out.append("<h2>Сравнение конфигураций S0–S4 (окна A/B, light-трасса; off — без трассы)</h2><div class=wrap><table>")
    out.append("<tr><th>сцена</th><th>конф.</th><th>трасса</th><th>прогонов</th><th>FPS</th><th>1% low</th>"
               "<th>p95/p99 показа, мс</th><th>CP, мс</th><th>GPU, мс</th><th>ожидание п.1, мс</th>"
               "<th>хвост GPU, мс</th><th>&gt;33,3 / &gt;41,7 / &gt;50 (показ)</th><th>&gt;33,3 (эмуляция)</th></tr>")

    def sh(x):
        return f"{100 * x['mean']:.1f}%" if x else "—"
    for g in summary["groups"]:
        p = g["present"]
        e = g["emulated"]
        out.append(f"<tr><td>{g['scene_ru']}</td><td>{g['cfg']} {html.escape(g['cfg_ru'])}</td><td>{g['trace']}</td>"
                   f"<td>{len(g['runs'])}</td><td>{fmt(g['avg_fps'])}</td><td>{fmt(g['low1_fps'])}</td>"
                   f"<td>{fmt(g['ft_p95_ms'], 1)} / {fmt(g['ft_p99_ms'], 1)}</td><td>{fmt(g['mean_cp_ms'])}</td>"
                   f"<td>{fmt(g['mean_gpu_busy_ms'])}</td><td>{fmt(g['mean_par_wait_ms'])}</td>"
                   f"<td>{fmt(g['gpu_tail_ms'])}</td><td>{sh(p['share_over_33_3'])} / {sh(p['share_over_41_7'])} / "
                   f"{sh(p['share_over_50'])}</td><td>{sh(e['share_over_33_3'])}</td></tr>")
    out.append("</table></div>")
    if summary.get("overhead"):
        out.append("<h3>Накладные расходы light-трассы (S1, light − off)</h3><table><tr><th>сцена</th><th>ΔFPS</th>"
                   "<th>ΔCP, мс</th><th>ΔGPU, мс</th><th>Δожидание п.1, мс</th><th>прогонов off/light</th></tr>")
        for o in summary["overhead"]:
            d = o["light_minus_off"]
            out.append(f"<tr><td>{SCENES[o['scene']][0]}</td><td>{d['avg_fps']:+.2f}</td><td>{d['mean_cp_ms']:+.2f}</td>"
                       f"<td>{d['mean_gpu_busy_ms']:+.2f}</td><td>{d['mean_par_wait_ms']:+.2f}</td>"
                       f"<td>{o['off_runs']}/{o['light_runs']}</td></tr>")
        out.append("</table>")
    # critical paths
    out.append("<h2>Критический путь кадра (глубокая трасса)</h2>")
    for r in runs:
        d = r["deep"]
        if not d:
            continue
        out.append(f"<h3>{SCENES[r['scene']][0]}, {r['cfg']} ({html.escape(CONFIGS.get(r['cfg'], ''))}) — "
                   f"{html.escape(r['id'])}</h3>")
        out.append(f"<p class=r>{d['frames']} кадров, кадр эмуляции {d['emu_ms_mean']} мс (медиана "
                   f"{d['emu_ms_median']}); калибровка: {html.escape(str(d['calibration'].get('method')))}; сэмплов "
                   f"{d.get('samples')} (~{d.get('sample_hz_actual_per_thread')} Гц на поток)</p>")
        out.append("<table><tr><th>отрезок пути</th><th>класс</th><th>мс/кадр</th><th>доля</th><th></th></tr>")
        for k, v in d["path_ms_per_frame"].items():
            if v < 0.005:
                continue
            share = v / d["emu_ms_mean"] if d["emu_ms_mean"] else 0
            out.append(f"<tr><td>{html.escape(SEG_RU.get(k, k))}</td><td>{CLASS_RU.get(SEG_CLASS.get(k, 'D'))}</td>"
                       f"<td>{v:.3f}</td><td>{100 * share:.1f}%</td><td><span class=bar style='width:"
                       f"{int(300 * share)}px;background:var(--acc)'></span></td></tr>")
        out.append("</table>")
        for role, s in d["busy_split"].items():
            out.append(f"<details><summary>Занятость {'потока 1' if role == 'CP' else 'потока 2'} на пути по функциям "
                       f"({s['samples_on_path']} сэмплов)</summary><table><tr><th>корзина</th><th>мс/кадр</th></tr>")
            for k, v in list(s["on_path_ms_per_frame"].items())[:18]:
                out.append(f"<tr><td>{html.escape(k)}</td><td>{v:.3f}</td></tr>")
            out.append("</table></details>")
        g = d.get("gpu") or {}
        if g:
            out.append(f"<p class=r>GPU: занят {g.get('busy_ms_per_frame')} мс/кадр, простаивает "
                       f"{g.get('idle_ms_per_frame')} (ждёт сабмита {g.get('idle_waiting_for_submission_ms_per_frame')}, "
                       f"после сабмита {g.get('idle_after_submission_ms_per_frame')}), пакетов "
                       f"{g.get('batches_per_frame')}/кадр</p>")
        for tl in d.get("timelines", []):
            tag = "медианный" if tl["n"] == d.get("median_frame_n") else "медленный"
            out.append(f"<div class=card><b>кадр {tl['n']}</b> ({tag}), эмуляция {tl['emu_ms']} мс"
                       f"<div class=wrap>{svg_timeline(tl)}</div></div>")
        out.append("<details><summary>Самые медленные кадры окна</summary><table><tr><th>кадр</th><th>t, с</th>"
                   "<th>эмуляция, мс</th><th>путь (главное)</th><th>сэмплы занятости</th><th>журнал кадра</th></tr>")
        for s in d["slowest"][:8]:
            path = "; ".join(f"{SEG_RU.get(k, k)} {v:.1f}" for k, v in list(s["path"].items())[:4])
            bs = "; ".join(f"{k} ×{v}" for k, v in (s.get("busy_samples") or {}).items())
            lg = s.get("log") or {}
            lgs = ", ".join(f"{k} {v:.1f}" for k, v in lg.items() if v is not None)
            out.append(f"<tr><td>{s['n']}</td><td>{s['t_s']}</td><td>{s['emu_ms']}</td><td>{html.escape(path)}</td>"
                       f"<td>{html.escape(bs)}</td><td class=r>{html.escape(lgs)}</td></tr>")
        out.append("</table></details>")
    # outliers by class
    out.append("<h2>Длинные кадры (окна A/B, все прогоны)</h2><table><tr><th>сцена</th><th>конф.</th>"
               "<th>класс</th><th>кадров &gt;41,7 мс</th></tr>")
    cls = collections.Counter()
    for r in runs:
        for o in r["m"]["outliers"]:
            if (o.get("present_ms") or 0) > 41.7 or (o.get("emulated_ms") or 0) > 41.7:
                cls[(SCENES[r["scene"]][0], r["cfg"], o["class"])] += 1
    for (sc, cfg, c), n in sorted(cls.items(), key=lambda kv: (kv[0][0], kv[0][1], -kv[1])):
        out.append(f"<tr><td>{sc}</td><td>{cfg}</td><td>{html.escape(c)}</td><td>{n}</td></tr>")
    out.append("</table><p class=r>Только кадры &gt;41,7 мс (показ или эмуляция); полный список — frame_outliers.csv."
               "</p></main></body></html>")
    with open(os.path.join(root, "trace.html"), "w", encoding="utf-8") as f:
        f.write("".join(out))


def main(argv):
    root = os.path.join(ROOT, "logs", "p412")
    top10_path = None
    i = 1
    while i < len(argv):
        if argv[i] == "--root":
            root = argv[i + 1]
            i += 2
        elif argv[i] == "--top10":
            top10_path = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    sys.stdout.reconfigure(encoding="utf-8")
    runs = discover(root)
    for r in runs:
        r["m"] = window_metrics(r)
        aj = os.path.join(r["dir"], "p412_analysis.json")
        r["deep"] = json.load(open(aj, encoding="utf-8")) if os.path.exists(aj) else None
    groups = collections.defaultdict(list)
    for r in runs:
        groups[(r["scene"], r["cfg"], r["mode"])].append(r)
    summary = {"runs": len(runs), "groups": []}
    for (scene, cfg, mode), rs in sorted(groups.items()):
        g = {"scene": scene, "scene_ru": SCENES[scene][0], "cfg": cfg, "cfg_ru": CONFIGS.get(cfg, cfg), "trace": mode,
             "runs": [r["id"] for r in rs]}
        for k in ("avg_fps", "median_fps", "low1_fps", "ft_p95_ms", "ft_p99_ms", "mean_cp_ms", "mean_gpu_busy_ms",
                  "mean_par_wait_ms", "mean_empty_ms", "gpu_tail_ms", "game_answer_ms", "mean_draws",
                  "mean_dispatches", "mean_record_ms", "mean_par_busy_ms", "mean_blocked_label_ms"):
            g[k] = agg_runs([r["m"]["window"].get(k) for r in rs])
        for name in ("present", "emulated"):
            g[name] = {k: agg_runs([r["m"][name].get(k) for r in rs])
                       for k in ("mean_ms", "p95_ms", "p99_ms", "max_ms", "share_over_33_3", "share_over_41_7",
                                 "share_over_50")}
        if mode != "off":
            keys = set()
            for r in rs:
                keys |= set(r["m"]["stalls"])
            g["stalls"] = {k: agg_runs([r["m"]["stalls"].get(k, {}).get("ms_per_frame") for r in rs])
                           for k in sorted(keys)}
        deep = [r["deep"] for r in rs if r["deep"]]
        if deep:
            keys = set()
            for d in deep:
                keys |= set(d["path_ms_per_frame"])
            g["path"] = {k: agg_runs([d["path_ms_per_frame"].get(k, 0.0) for d in deep]) for k in keys}
            g["emu_ms_mean_deep"] = agg_runs([d["emu_ms_mean"] for d in deep])
            g["gpu_deep"] = {k: agg_runs([d["gpu"].get(k) for d in deep]) for k in deep[0]["gpu"]}
        summary["groups"].append(g)
    # the light trace's overhead: S1 light vs S1 off, per scene
    over = []
    for scene in SCENES:
        off = groups.get((scene, "S1", "off"), [])
        lit = groups.get((scene, "S1", "light"), [])
        if off and lit:
            o = {k: (mean([r["m"]["window"].get(k) for r in lit]) - mean([r["m"]["window"].get(k) for r in off]))
                 for k in ("avg_fps", "mean_cp_ms", "mean_gpu_busy_ms", "mean_par_wait_ms", "low1_fps")}
            over.append({"scene": scene, "light_minus_off": {k: round(v, 3) for k, v in o.items()},
                         "off_runs": len(off), "light_runs": len(lit)})
    summary["overhead"] = over
    with open(os.path.join(root, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1, ensure_ascii=False)
    with open(os.path.join(root, "stalls.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["scene", "config", "trace", "run", "source", "thread", "event", "count_per_frame", "ms_per_frame",
                    "p50_us", "p95_us", "p99_us", "max_us", "p95_frame_ms", "p99_frame_ms", "max_frame_ms"])
        for r in runs:
            for key, s in sorted(r["m"]["stalls"].items(), key=lambda kv: -kv[1]["ms_per_frame"]):
                role, ev = key.split(".", 1)
                w.writerow([r["scene"], r["cfg"], r["mode"], r["id"], "light/frame totals", role, ev,
                            s["count_per_frame"], s["ms_per_frame"], "", "", "", "", s["p95_frame_ms"],
                            s["p99_frame_ms"], s["max_frame_ms"]])
            if r["deep"]:
                for s in r["deep"]["waits"]:
                    w.writerow([r["scene"], r["cfg"], r["mode"], r["id"], "deep/events", s["thread"], s["event"],
                                s["count_per_frame"], s["total_ms_per_frame"], s["p50_us"], s["p95_us"], s["p99_us"],
                                s["max_us"], "", "", ""])
    with open(os.path.join(root, "critical_path.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["scene", "config", "run", "frames", "emu_ms_mean", "segment", "segment_ru", "class", "ms_per_frame",
                    "share"])
        for r in runs:
            d = r["deep"]
            if not d:
                continue
            for k, v in d["path_ms_per_frame"].items():
                w.writerow([r["scene"], r["cfg"], r["id"], d["frames"], d["emu_ms_mean"], k, SEG_RU.get(k, k),
                            CLASS_RU.get(SEG_CLASS.get(k, "D"), "?"), v,
                            round(v / d["emu_ms_mean"], 4) if d["emu_ms_mean"] else ""])
            for role, s in d["busy_split"].items():
                for k, v in s["on_path_ms_per_frame"].items():
                    w.writerow([r["scene"], r["cfg"], r["id"], d["frames"], d["emu_ms_mean"],
                                f"{'thread 1' if role == 'CP' else 'thread 2'} busy: {k}", k, "Работа (сэмплы)", v,
                                round(v / d["emu_ms_mean"], 4) if d["emu_ms_mean"] else ""])
    with open(os.path.join(root, "frame_outliers.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        cols = ["n", "t_s", "present_ms", "emulated_ms", "class", "cp_ms", "gpu_busy_ms", "compile_ms", "shaders",
                "pipelines", "par_wait_ms", "idle_label_ms", "gpu_gap_ms", "draws", "dispatches"]
        w.writerow(["scene", "config", "trace", "run"] + cols)
        for r in runs:
            for o in r["m"]["outliers"]:
                w.writerow([r["scene"], r["cfg"], r["mode"], r["id"]] + [o.get(c) for c in cols])
    top10 = None
    if top10_path and os.path.exists(top10_path):
        top10 = json.load(open(top10_path, encoding="utf-8"))
    write_html(root, runs, summary, top10)
    print(f"{len(runs)} runs -> {root}: summary.json, stalls.csv, critical_path.csv, frame_outliers.csv, trace.html")


if __name__ == "__main__":
    main(sys.argv)
