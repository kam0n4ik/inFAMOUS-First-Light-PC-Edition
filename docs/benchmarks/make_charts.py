# Benchmark charts of the README (docs/benchmarks/*.svg, a light and a dark variant each).
#   python docs/benchmarks/make_charts.py docs/benchmarks
# Hand-written SVG: thin marks, rounded data ends, hairline grid, direct value labels, a legend for >= 2 series.
# Palette: the dataviz reference slots 1-3 (validated light + dark, all checks pass; aqua < 3:1 on light -> every bar
# carries a visible value label and the README has the table).
import math

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#898781", grid="#e1e0d9",
                  axis="#c3c2b7", ring="rgba(11,11,11,0.10)",
                  s=["#2a78d6", "#eb6834", "#1baf7a"], before="#b4b2a9"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", muted="#898781", grid="#2c2c2a",
                 axis="#383835", ring="rgba(255,255,255,0.10)",
                 s=["#3987e5", "#d95926", "#199e70"], before="#6b6a65"),
}
FONT = "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"


def esc(t):
    return str(t).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def fmt(v, digits=1):
    return f"{v:.{digits}f}"


def nice_max(v):
    step = 10 ** math.floor(math.log10(v))
    for m in (1, 2, 2.5, 5, 10):
        if m * step >= v:
            return m * step
    return 10 * step


def ticks(vmax, n=6):
    """The smallest nice step (1, 2, 2.5, 5 x 10^k) with at most n steps up to vmax; the axis ends one step above."""
    raw = vmax / n
    base = 10 ** math.floor(math.log10(raw))
    step = next(m * base for m in (1, 2, 2.5, 5, 10) if m * base >= raw)
    count = math.ceil(vmax / step - 1e-9)
    return [step * i for i in range(count + 1)], step * count


class Svg:
    def __init__(self, w, h, theme, title, subtitle, note):
        self.w, self.h, self.t = w, h, THEMES[theme]
        self.parts = []
        t = self.t
        self.parts.append(f'<rect x="0.5" y="0.5" width="{w - 1}" height="{h - 1}" rx="10" fill="{t["surface"]}" '
                          f'stroke="{t["ring"]}"/>')
        self.text(24, 34, title, 18, t["ink"], weight=600)
        if subtitle:
            self.text(24, 56, subtitle, 13, t["ink2"])
        if note:
            self.text(24, h - 16, note, 11, t["muted"])

    def text(self, x, y, s, size, fill, anchor="start", weight=400):
        self.parts.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" font-weight="{weight}" '
                          f'text-anchor="{anchor}">{esc(s)}</text>')

    def legend(self, x, y, items):
        for label, color, kind in items:
            if kind == "dot":
                self.parts.append(f'<circle cx="{x + 5}" cy="{y - 4}" r="5" fill="{color}"/>')
            else:
                self.parts.append(f'<rect x="{x}" y="{y - 9}" width="10" height="10" rx="2" fill="{color}"/>')
            self.text(x + 16, y, label, 12, self.t["ink2"])
            x += 16 + 7.0 * len(label) + 20

    def hbar(self, x0, y, length, thick, color, label):
        """A horizontal bar from the baseline x0, the data end rounded (4 px)."""
        r = min(4, thick / 2, max(length, 0.01))
        x1 = x0 + length
        d = (f"M{x0:.1f},{y:.1f} H{x1 - r:.1f} Q{x1:.1f},{y:.1f} {x1:.1f},{y + r:.1f} V{y + thick - r:.1f} "
             f"Q{x1:.1f},{y + thick:.1f} {x1 - r:.1f},{y + thick:.1f} H{x0:.1f} Z")
        self.parts.append(f'<path d="{d}" fill="{color}"><title>{esc(label)}</title></path>')

    def vbar(self, x, base, height, thick, color, label):
        r = min(4, thick / 2, max(height, 0.01))
        top = base - height
        d = (f"M{x:.1f},{base:.1f} V{top + r:.1f} Q{x:.1f},{top:.1f} {x + r:.1f},{top:.1f} H{x + thick - r:.1f} "
             f"Q{x + thick:.1f},{top:.1f} {x + thick:.1f},{top + r:.1f} V{base:.1f} Z")
        self.parts.append(f'<path d="{d}" fill="{color}"><title>{esc(label)}</title></path>')

    def save(self, path, desc):
        body = "\n".join(self.parts)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
               f'viewBox="0 0 {self.w} {self.h}" font-family="{FONT}" role="img" aria-label="{esc(desc)}">\n'
               f'<title>{esc(desc)}</title>\n{body}\n</svg>\n')
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(svg)


def grouped_hbars(path, theme, title, subtitle, note, rows, series, unit, digits=1, label_w=170):
    """rows: [(label, [v per series])]; series: [name]. Bars on a zero baseline."""
    t = THEMES[theme]
    thick, gap, group_gap = 14, 2, 16
    group_h = len(series) * thick + (len(series) - 1) * gap
    top = 96 if len(series) > 1 else 76
    w = 880
    plot_bottom = top + len(rows) * group_h + (len(rows) - 1) * group_gap
    h = plot_bottom + 64
    svg = Svg(w, h, theme, title, subtitle, note)
    vmax = max(v for _, vs in rows for v in vs if v is not None)
    tk, tmax = ticks(vmax)
    x0, x1 = 24 + label_w, w - 70
    scale = (x1 - x0) / tmax
    if len(series) > 1:
        svg.legend(24, 80, [(s, t["s"][i], "box") for i, s in enumerate(series)])
    for v in tk:
        x = x0 + v * scale
        svg.parts.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{plot_bottom + 4}" stroke="{t["grid"]}" '
                         f'stroke-width="1"/>')
        svg.text(x, plot_bottom + 20, fmt(v, 0), 11, t["muted"], anchor="middle")
    svg.text(x1 + 22, plot_bottom + 20, unit, 11, t["muted"])
    y = top
    for label, vs in rows:
        svg.text(24 + label_w - 12, y + group_h / 2 + 4, label, 13, t["ink"], anchor="end")
        for i, v in enumerate(vs):
            by = y + i * (thick + gap)
            if v is None:
                continue
            svg.hbar(x0, by, v * scale, thick, t["s"][i], f"{label}, {series[i]}: {fmt(v, digits)} {unit}")
            svg.text(x0 + v * scale + 6, by + thick - 3, fmt(v, digits), 12, t["ink"])
        y += group_h + group_gap
    svg.parts.append(f'<line x1="{x0}" y1="{top - 6}" x2="{x0}" y2="{plot_bottom + 4}" stroke="{t["axis"]}" '
                     f'stroke-width="1"/>')
    svg.save(path, title)


def dumbbell(path, theme, title, subtitle, note, rows, unit, digits=1, label_w=330, before="before", after="after",
             step=10):
    """rows: [(label, scene, v_before, v_after)] - the two sides of one A/B, joined by a line."""
    t = THEMES[theme]
    row_h = 32
    top = 102
    w = 880
    plot_bottom = top + len(rows) * row_h - 12
    h = plot_bottom + 58
    svg = Svg(w, h, theme, title, subtitle, note)
    vals = [v for r in rows for v in r[2:4]]
    lo = math.floor(min(vals) / step) * step
    hi = math.ceil(max(vals) / step) * step
    x0, x1 = 24 + label_w, w - 170
    scale = (x1 - x0) / (hi - lo)
    svg.legend(24, 80, [(before, t["before"], "dot"), (after, t["s"][0], "dot")])
    v = lo
    while v <= hi + 1e-9:
        x = x0 + (v - lo) * scale
        svg.parts.append(f'<line x1="{x:.1f}" y1="{top - 10}" x2="{x:.1f}" y2="{plot_bottom}" stroke="{t["grid"]}" '
                         f'stroke-width="1"/>')
        svg.text(x, plot_bottom + 18, fmt(v, 0), 11, t["muted"], anchor="middle")
        v += step
    svg.text(x1 + 12, plot_bottom + 18, unit, 11, t["muted"])
    y = top
    for label, scene, a, b in rows:
        cy = y
        svg.text(24, cy + 4, label, 13, t["ink"])
        svg.text(24 + label_w - 14, cy + 4, scene, 12, t["ink2"], anchor="end")
        xa, xb = x0 + (a - lo) * scale, x0 + (b - lo) * scale
        svg.parts.append(f'<line x1="{xa:.1f}" y1="{cy}" x2="{xb:.1f}" y2="{cy}" stroke="{t["axis"]}" '
                         f'stroke-width="2"/>')
        for x, c, side, val in ((xa, t["before"], before, a), (xb, t["s"][0], after, b)):
            svg.parts.append(f'<circle cx="{x:.1f}" cy="{cy}" r="5" fill="{c}" stroke="{t["surface"]}" '
                             f'stroke-width="2"><title>{esc(label)} ({esc(scene)}), {side}: {fmt(val, digits)} '
                             f'{unit}</title></circle>')
        pct = (b - a) / a * 100
        svg.text(x1 + 12, cy + 4, f"{fmt(a, digits)} → {fmt(b, digits)}", 12, t["ink"])
        svg.text(x1 + 104, cy + 4, f"{pct:+.0f} %", 12, t["ink2"])
        y += row_h
    svg.save(path, title)


def vbars(path, theme, title, subtitle, note, cats, values, unit, digits=1, colors=None, h=300, sublabels=None):
    """One measure, a bar per category (configurations). colors: slot index or theme key per bar."""
    t = THEMES[theme]
    w = 880
    svg = Svg(w, h, theme, title, subtitle, note)
    top, bottom = 92, h - (78 if sublabels else 62)
    x0, x1 = 64, w - 24
    tk, tmax = ticks(max(values), 5)
    scale = (bottom - top) / tmax
    for v in tk:
        y = bottom - v * scale
        svg.parts.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{t["grid"]}" stroke-width="1"/>')
        svg.text(x0 - 8, y + 4, fmt(v, 0), 11, t["muted"], anchor="end")
    svg.text(x0 - 8, top - 14, unit, 11, t["muted"], anchor="end")
    slot = (x1 - x0) / len(cats)
    thick = min(56, slot * 0.5)
    for i, (c, v) in enumerate(zip(cats, values)):
        x = x0 + slot * i + (slot - thick) / 2
        key = colors[i] if colors else 0
        color = t["s"][key] if isinstance(key, int) else t[key]
        svg.vbar(x, bottom, v * scale, thick, color, f"{c}: {fmt(v, digits)} {unit}")
        svg.text(x + thick / 2, bottom - v * scale - 6, fmt(v, digits), 12, t["ink"], anchor="middle")
        svg.text(x + thick / 2, bottom + 18, c, 12, t["ink"], anchor="middle")
        if sublabels:
            svg.text(x + thick / 2, bottom + 34, sublabels[i], 11, t["ink2"], anchor="middle")
    svg.parts.append(f'<line x1="{x0}" y1="{bottom}" x2="{x1}" y2="{bottom}" stroke="{t["axis"]}" stroke-width="1"/>')
    svg.save(path, title)


# ---- data: every number from docs/ reports or the raw series (sources in the README tables) ----

def build(out):
    import os
    os.makedirs(out, exist_ok=True)
    stand = "Ryzen 5 5500, RTX 2060 6 GB, 1920x1080 internal"
    for th in THEMES:
        sfx = "" if th == "light" else "-dark"
        dumbbell(f"{out}/fps-ab{sfx}.svg", th, "Average FPS: measured A/B steps",
                 "Each row is its own A/B series of one build (median of 2-6 runs); rows are not one timeline",
                 f"{stand}. Sources: docs/REPORTS.md (P4.11, NATIVE_CP CP-7, GPU_OPTIMIZATION, CITY_ENGINE_*, R6, R8_T2, R9).",
                 [("Parallel command processor (P4.11)", "beach", 33.67, 43.91),
                  ("Native command processor (CP-7)", "beach", 33.95, 47.85),
                  ("", "harbour", 30.96, 42.66),
                  ("", "village", 30.27, 37.37),
                  ("GPU optimizations (GPU-1..6)", "beach", 48.09, 55.12),
                  ("", "harbour", 42.63, 49.94),
                  ("", "village", 37.29, 38.55),
                  ("Engine draw runs (CITY-OPT P5)", "city", 25.3, 29.4),
                  ("G-buffer runs (CITY-OPT P6)", "city", 27.9, 31.6),
                  ("Trivial CP polls (R6-P)", "city bench", 28.80, 32.77),
                  ("Frontend + emit threads, ring 128", "live city", 28.64, 36.06),
                  ("Emitter batching (R8 T2)", "city bench", 39.13, 41.54),
                  ("F1 + F3 (R9)", "fight", 44.49, 46.04),
                  ("", "traverse", 47.41, 51.34)],
                 "FPS", digits=1, label_w=300, step=10)
        dumbbell(f"{out}/low1-ab{sfx}.svg", th, "1 % low FPS: smoothness",
                 "The slowest 1 % of frames; higher means fewer stutters",
                 f"{stand}. FB-2: docs/FRAMEBREAK.md; R9: docs/R9_GAMEPLAY_PERFORMANCE.md; R8: docs/R8_T2_OPTIMIZATION.md.",
                 [("Free frame pacing (FB-2)", "beach", 29.4, 48.8),
                  ("", "harbour", 29.4, 46.8),
                  ("", "village", 29.3, 35.3),
                  ("Emitter batching (R8 T2)", "city bench", 31.73, 34.81),
                  ("F1: pipeline cache save off-thread (R9)", "traverse", 7.17, 26.20),
                  ("F1 + F3 (R9)", "fight", 7.5, 25.6),
                  ("", "traverse", 6.1, 27.9)],
                 "FPS", digits=1, label_w=300, step=10)
        grouped_hbars(f"{out}/cp-modes{sfx}.svg", th, "Command processor modes",
                      "Average FPS, the same build, 3 runs per mode (CP-7, 2026-10-16)",
                      f"{stand}. Legacy: one CP thread; Parallel: + Vulkan recording thread; Native: + change journal (play.cmd). docs/NATIVE_CP.md.",
                      [("beach", [33.95, 44.04, 47.85]), ("harbour", [30.96, 39.94, 42.66]),
                       ("village", [30.27, 36.64, 37.37])],
                      ["Legacy", "Parallel", "Native"], "FPS", digits=1, label_w=90)
        grouped_hbars(f"{out}/upscaling{sfx}.svg", th, "Upscaling modes",
                      "Average FPS, 1 run of 90 s per mode (2026-10-15); the frame is bound by the CPU, so the gain is small",
                      f"{stand}. docs/UPSCALING_FRAME_GENERATION.md; frame generation doubles shown frames (beach 43-53 -> 86-105).",
                      [("Native", [35.4, 41.6]), ("DLSS Quality", [36.3, 38.8]), ("DLSS Performance", [35.4, 39.0]),
                       ("FSR 3.1 Quality", [38.7, 44.7]), ("FSR 3.1 Performance", [40.7, 45.3])],
                      ["village", "beach"], "FPS", digits=1, label_w=150)
        grouped_hbars(f"{out}/shader-cache{sfx}.svg", th, "First start against a repeat start",
                      "Frames over 100 ms in a 90 s run (P4.14 shader cache + precompiler, 2026-10-12)",
                      "First start: 26-42 s of pipelines compiled while playing; repeat start: 0. docs/PHASE4_SHADER_PRECOMPILER.md.",
                      [("village", [53, 4]), ("beach", [42, 4]), ("harbour", [36, 3])],
                      ["first start", "repeat start"], "frames", digits=0, label_w=90)
        grouped_hbars(f"{out}/current{sfx}.svg", th, "Where it stands now",
                      "The newest multi-run measurement of each scene (median of 3-6 runs); presets differ slightly, see the table",
                      f"{stand}. Fight / traverse / save: R9 2026-10-23; beach / harbour / village / live city: 2026-10-22.",
                      [("traverse (R9)", [51.34, 27.89]), ("fight (R9)", [46.04, 25.56]),
                       ("beach", [53.14, 35.46]), ("harbour", [43.19, 32.62]), ("village", [37.60, 30.18]),
                       ("live city", [36.06, 24.69]), ("owner's save, rooftop", [29.99, 21.58])],
                      ["average FPS", "1 % low"], "FPS", digits=1, label_w=170)


if __name__ == "__main__":
    import sys
    build(sys.argv[1])
