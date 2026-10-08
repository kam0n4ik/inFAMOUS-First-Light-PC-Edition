"""Summary of one SecondSonPC test run (logs\\runs\\<tag>), as one JSON line.

    python -I tools/analyze_run.py <run_dir> [--exit N] [--how HOW]
                                   [--save-before FILE --save-after FILE]

--how / --exit default to run_dir\\run.json (tools/run_phase0.ps1: closed-by-test, crashed,
killed-close-timeout, ...; see tools/game_process.psm1 Get-RunOutcome).

Reads SecondSonPC.log (own log: VRAM/commit samples, import-call time series, GPU allocation
failures, crash handler), bridge_shadps4.log (pipelines, shadPS4 errors) and calls.json.
"Alive" means the presented-frame counter was still growing in the last two samples of the run,
so a process that merely existed for the whole run but hung is not counted as a pass.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timedelta
from pathlib import Path

TS_RE = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d{3})\]")
LOADER_RE = re.compile(r"loader: .*\((sspc|shadps4)")
VRAM_RE = re.compile(r"VRAM (?:sample|crash): local usage (\d+) MiB / budget (\d+) MiB.*non-local usage (\d+) MiB")
COMMIT_RE = re.compile(r"commit (?:sample|crash): process private (\d+) MiB.*system committed (\d+) / limit (\d+) MiB")
CALLS_RE = re.compile(r"import calls: NATIVE (\d+) OBSERVED (\d+) BRIDGE (\d+) COMMON_STUB (\d+); frames \S+ (\d+)")
BINDING_RE = re.compile(r"binding table: (.*)$")
PIPELINE_RE = re.compile(r"Get(?:Graphics|Compute)Pipeline: Compiling")
OPEN_SSPC_RE = re.compile(r"\[Filesystem\] .* open \[[^\]]*\] (\S+) flags")
OPEN_BRIDGE_RE = re.compile(r"file_system\.cpp:\d+ open: path = (\S+) flags")
LANG_RE = re.compile(r"game opened language file: (\S+) \(tag=(\w+)")
CRASH_PATTERNS = [
    ("gpu-alloc", re.compile(r"SSPC-001 allocation failure: (.*)$")),
    ("unhandled", re.compile(r"unhandled exception (0x[0-9a-f]+ at 0x[0-9a-f]+ \S+)")),
]
BRIDGE_CRASH_PATTERNS = [
    ("shadps4-assert", re.compile(r"(Failed allocating \w+ with error \w+)")),
    ("shadps4-signal", re.compile(r"SignalHandler: (Unhandled Exception code 0x[0-9a-f]+ at 0x[0-9a-f]+)")),
    ("shadps4-assert", re.compile(r"<Critical> .*(Assertion Failed!?.*)$")),
]


def read_lines(path: Path) -> list[str]:
    if not path.exists():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def parse_time(text: str) -> datetime:
    return datetime.strptime(text, "%H:%M:%S.%f")


def seconds_between(a: datetime, b: datetime) -> float:
    delta = b - a
    if delta < timedelta(0):  # run crossed midnight
        delta += timedelta(days=1)
    return round(delta.total_seconds(), 1)


def read_json(path: Path) -> dict:
    # utf-8-sig: Windows PowerShell's Set-Content -Encoding UTF8 writes a BOM.
    return json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {}


def file_hashes(path: str | None) -> dict[str, str]:
    return read_json(Path(path)) if path else {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--exit", type=int, default=None)
    ap.add_argument("--how", default="")
    ap.add_argument("--save-before")
    ap.add_argument("--save-after")
    args = ap.parse_args()

    run = Path(args.run_dir)
    own = read_lines(run / "SecondSonPC.log")
    bridge = read_lines(run / "bridge_shadps4.log")
    # run.json: how tools/run_phase0.ps1 ended the process (absent for manual sessions).
    stop = read_json(run / "run.json")
    window = stop.get("window") or {}

    stamps = [parse_time(m.group(1)) for line in own if (m := TS_RE.match(line))]
    start = stamps[0] if stamps else None
    duration = seconds_between(stamps[0], stamps[-1]) if len(stamps) > 1 else 0.0

    loader = next((m.group(1) for line in own if (m := LOADER_RE.search(line))), "?")
    binding = next((m.group(1) for line in own if (m := BINDING_RE.search(line))), None)

    vram = [tuple(map(int, m.groups())) for line in own if (m := VRAM_RE.search(line))]
    commit = [tuple(map(int, m.groups())) for line in own if (m := COMMIT_RE.search(line))]

    series = []  # (seconds since start, frames, calls by owner)
    for line in own:
        m = CALLS_RE.search(line)
        t = TS_RE.match(line)
        if m and t and start:
            n, o, b, s, frames = map(int, m.groups())
            series.append((seconds_between(start, parse_time(t.group(1))), frames, (n, o, b, s)))
    alive = len(series) >= 2 and series[-1][1] > series[-2][1] and duration - series[-1][0] <= 15

    crash = None
    for line in own:
        for kind, rx in CRASH_PATTERNS:
            if (m := rx.search(line)) and crash is None:
                crash = f"{kind}: {m.group(1)}"
    for line in bridge:
        for kind, rx in BRIDGE_CRASH_PATTERNS:
            if (m := rx.search(line)) and crash is None:
                crash = f"{kind}: {m.group(1)}"

    if loader == "sspc":
        opens = [m.group(1) for line in own if (m := OPEN_SSPC_RE.search(line))]
    else:
        opens = [m.group(1) for line in bridge if (m := OPEN_BRIDGE_RE.search(line))]
    lang = [(m.group(1), m.group(2)) for line in own if (m := LANG_RE.search(line))]

    before, after = file_hashes(args.save_before), file_hashes(args.save_after)
    save_changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))

    own_errors = sum(1 for line in own if "] [Error] [" in line or "] [Critical] [" in line)
    bridge_errors = sum(1 for line in bridge if "<Error>" in line or "<Critical>" in line)

    summary = {
        "tag": run.name,
        "loader": loader,
        "start": start.strftime("%H:%M:%S") if start else None,
        "duration_s": duration,
        "exit": args.exit if args.exit is not None else stop.get("exit_code"),
        "exit_hex": f"0x{args.exit & 0xFFFFFFFF:08X}" if args.exit is not None else stop.get("exit_hex"),
        "how": args.how or stop.get("how", ""),
        "exit_note": stop.get("note"),
        "window_class": window.get("class"),
        "window_reason": stop.get("window_reason"),
        "crash": crash,
        "alive_at_end": alive,
        "frames": series[-1][1] if series else None,
        "frames_series": [(t, f) for t, f, _ in series],
        "calls": dict(zip(("native", "observed", "bridge", "common_stub"), series[-1][2])) if series else None,
        "binding_table": binding,
        "pipelines": sum(1 for line in bridge if PIPELINE_RE.search(line)),
        "vram_local_max_mib": max((v[0] for v in vram), default=None),
        "vram_budget_mib": vram[-1][1] if vram else None,
        "vram_nonlocal_max_mib": max((v[2] for v in vram), default=None),
        "process_private_max_mib": max((c[0] for c in commit), default=None),
        "commit_max_mib": max((c[1] for c in commit), default=None),
        "commit_limit_mib": commit[-1][2] if commit else None,
        "opens": len(opens),
        "opens_unique": len(set(opens)),
        "lang_files": [f"{Path(p).name}:{tag}" for p, tag in lang],
        "errors_own": own_errors,
        "errors_bridge": bridge_errors,
        "save_files": len(after) if after else len(before),
        "save_changed": save_changed,
    }
    print(json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
