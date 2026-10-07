"""Cross-reference Second Son imports with the shadPS4 HLE implementation.

Usage (read-only for the game and shadPS4 sources):
    python -I E:\\SecondSonPC\\tools\\audit_imports.py [--log shad_log.txt] --out E:\\SecondSonPC\\docs\\generated

Outputs:
    imports.csv             every import of eboot.bin with resolution + implementation status
    prx_imports.csv         imports of the LLE modules shipped in sce_module/
    summary.json            counters per module / status
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ps4elf import Ps4Elf, load_aerolib, resolve_names  # noqa: E402

GAME = Path(r"E:\shdps4\CUSA00004")
SHAD = Path(r"E:\SecondSonPC\third_party\shadPS4")

LIB_RE = re.compile(
    r'LIB_(FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*(.+?)\)\s*;',
    re.S,
)
DEF_RE_TMPL = r'PS4_SYSV_ABI\s+(?:[\w:]+::)?{name}\s*\('


def strip_func(expr: str) -> str:
    expr = expr.strip()
    m = re.fullmatch(r"ORBIS\((.+)\)", expr)  # errno translation wrapper
    if m:
        expr = m.group(1).strip()
    expr = expr.lstrip("&")
    return expr.split("::")[-1].strip()


def extract_body(text: str, start: int) -> str:
    i = text.find("{", start)
    semi = text.find(";", start)
    if i < 0 or (0 <= semi < i):
        return ""  # declaration only
    depth = 0
    for j in range(i, len(text)):
        c = text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return text[i:]


def code_statements(body: str) -> list[str]:
    inner = body[1:-1]
    inner = re.sub(r"//.*", "", inner)
    inner = re.sub(r"/\*.*?\*/", "", inner, flags=re.S)
    inner = re.sub(r"LOG_\w+\((?:[^;]|\n)*?\);", "", inner)
    return [s.strip() for s in inner.split(";") if s.strip()]


def classify(body: str) -> str:
    if not body:
        return "NO_BODY_FOUND"
    stmts = code_statements(body)
    labelled_stub = "(STUBBED)" in body
    labelled_dummy = re.search(r"\((DUMMY|dummy)\)", body) is not None
    # A "(STUBBED)" label inside an otherwise real body (e.g. sceAudioOutOutput) is only a stale log tag.
    if labelled_stub and len(stmts) <= 2:
        return "STUBBED"
    if labelled_dummy and len(stmts) <= 2:
        return "DUMMY"
    if ("UNREACHABLE" in body or "UNIMPLEMENTED" in body) and len(stmts) <= 3:
        return "UNIMPLEMENTED"
    if len(stmts) <= 1 and all(re.fullmatch(r"return\s*(ORBIS_OK|0|-?\d+|0x[0-9a-fA-F]+|nullptr|true|false|ORBIS_\w+)?", s) for s in stmts):
        return "TRIVIAL_RETURN"
    if labelled_stub or labelled_dummy:
        return "PARTIAL"  # real code but still flagged STUBBED/DUMMY by shadPS4 authors
    return "IMPLEMENTED"


def scan_shadps4(src: Path):
    regs: dict[tuple[str, str, str], dict] = {}
    files = list(src.rglob("*.cpp"))
    texts = {f: f.read_text(encoding="utf-8", errors="replace") for f in files}
    for f, t in texts.items():
        for m in LIB_RE.finditer(t):
            kind, nid, lib, _ver, mod, fexpr = m.groups()
            line = t.count("\n", 0, m.start()) + 1
            regs[(nid, lib, mod)] = {
                "kind": kind,
                "func": strip_func(" ".join(fexpr.split())),
                "reg_file": str(f.relative_to(SHAD)),
                "reg_line": line,
            }
    # function definitions
    cache: dict[str, tuple[str, int, str]] = {}

    def find_def(name: str, prefer: Path | None):
        if name in cache:
            return cache[name]
        pat = re.compile(DEF_RE_TMPL.format(name=re.escape(name)))
        order = ([prefer] if prefer else []) + [f for f in files if f != prefer]
        for f in order:
            t = texts.get(f)
            if t is None:
                continue
            for m in pat.finditer(t):
                body = extract_body(t, m.end())
                if body:
                    res = (str(f.relative_to(SHAD)), t.count("\n", 0, m.start()) + 1, body)
                    cache[name] = res
                    return res
        cache[name] = ("", 0, "")
        return cache[name]

    for key, r in regs.items():
        if r["kind"] == "OBJ":
            r.update(def_file="", def_line=0, status="OBJECT", body_lines=0)
            continue
        df, dl, body = find_def(r["func"], SHAD / r["reg_file"])
        r.update(def_file=df, def_line=dl, status=classify(body), body_lines=body.count("\n") + 1 if body else 0)
    return regs


def parse_log(log: Path | None) -> tuple[Counter, Counter]:
    """Function names seen in a shadPS4 log + names resolved to CommonStub."""
    seen: Counter = Counter()
    stubbed_calls: Counter = Counter()
    if not log or not log.exists():
        return seen, stubbed_calls
    rx = re.compile(r"^\[[^\]]+\] <\w+> \([^)]*\) \S+:\d+ (\w+):")
    rx_common = re.compile(r"CommonStub: Stub: (\S+) \(nid: ([^)]+)\)")
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = rx.match(line)
        if m:
            seen[m.group(1)] += 1
        m = rx_common.search(line)
        if m:
            stubbed_calls[m.group(1)] += 1
    return seen, stubbed_calls


SUBSYSTEM = [
    ("libkernel", "Kernel"),
    ("libc", "libc (LLE prx)"),
    ("libSceFios2", "FileIO / Fios2 (LLE prx)"),
    ("libSceGnmDriver", "GPU (Gnm)"),
    ("libSceVideoOut", "Display / VideoOut"),
    ("libSceAudioOut", "Audio out"),
    ("libSceAjm", "Audio decode (Ajm)"),
    ("libSceVideodec", "Video decode (cutscenes)"),
    ("libScePad", "Input: pad"),
    ("libSceMouse", "Input: mouse"),
    ("libSceUserService", "Users"),
    ("libSceSystemService", "System params / events"),
    ("libSceSaveDataDialog", "Save data UI"),
    ("libSceSaveData", "Save data"),
    ("libSceMsgDialog", "Dialogs"),
    ("libSceCommonDialog", "Dialogs"),
    ("libSceErrorDialog", "Dialogs"),
    ("libScePlayGo", "PlayGo / install chunks"),
    ("libSceAppContent", "DLC / app content"),
    ("libSceSysmodule", "Module loader"),
    ("libSceNpManager", "PSN"),
    ("libSceNpCommon", "PSN"),
    ("libSceNpTrophy", "Trophies"),
    ("libSceNetCtl", "Network"),
    ("libSceNet", "Network"),
    ("libSceHttp", "Network (HTTP)"),
    ("libSceSsl", "Network (SSL)"),
    ("libSceGameLiveStreaming", "Streaming / share"),
]


def subsystem(mod: str) -> str:
    for prefix, name in SUBSYSTEM:
        if mod == prefix or mod.startswith(prefix):
            return name
    return "Other"


STATUS_ORDER = ["IMPLEMENTED", "LLE", "PARTIAL", "TRIVIAL_RETURN", "DUMMY", "STUBBED", "UNIMPLEMENTED",
                "NO_BODY_FOUND", "OBJECT", "MISSING"]


def write_markdown(path: Path, eboot_rows: list[dict], prx_rows: list[dict], log: Path | None) -> None:
    out = ["# Second Son — импорты eboot.bin (сгенерировано tools/audit_imports.py)", ""]
    out.append(f"Лог рантайма: `{log}`" if log else "Лог рантайма: не указан")
    out.append("")
    out.append("`seen` — сколько раз имя функции встретилось в логе shadPS4 (0 ≠ «не вызывается»: "
               "многие HLE-функции ничего не логируют).")
    out.append("")
    per_mod: dict[str, list[dict]] = defaultdict(list)
    for r in eboot_rows:
        per_mod[r["module"]].append(r)
    out.append("| Модуль | Всего | " + " | ".join(STATUS_ORDER) + " | seen>0 |")
    out.append("|---|---:|" + "---:|" * len(STATUS_ORDER) + "---:|")
    for mod, rows in sorted(per_mod.items()):
        c = Counter(r["status"] for r in rows)
        seen = sum(1 for r in rows if int(r["seen_in_log"]) > 0)
        out.append(f"| {mod} | {len(rows)} | " + " | ".join(str(c.get(s, "")) for s in STATUS_ORDER) + f" | {seen} |")
    c = Counter(r["status"] for r in eboot_rows)
    out.append(f"| **Итого** | **{len(eboot_rows)}** | " + " | ".join(f"**{c.get(s, '')}**" for s in STATUS_ORDER)
               + f" | **{sum(1 for r in eboot_rows if int(r['seen_in_log']) > 0)}** |")
    out.append("")
    for mod, rows in sorted(per_mod.items()):
        out.append(f"## {mod} ({len(rows)})")
        out.append("")
        out.append("| Функция | NID | Статус | seen | Реализация shadPS4 |")
        out.append("|---|---|---|---:|---|")
        for r in rows:
            impl = r["def"] or r["resolution"]
            out.append(f"| `{r['name']}` | `{r['nid']}` | {r['status']} | {r['seen_in_log']} | `{impl}` |")
        out.append("")
    out.append("## Импорты LLE-модулей из sce_module")
    out.append("")
    out.append("| Модуль | Функция | Источник | Статус | Реализация |")
    out.append("|---|---|---|---|---|")
    for r in prx_rows:
        out.append(f"| {r['importer']} | `{r['name']}` | {r['module']}/{r['library']} | {r['status']} | "
                   f"`{r['def'] or r['resolution']}` |")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", type=Path, default=None)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    nids = load_aerolib(SHAD / "src/core/aerolib/aerolib.inl")
    regs = scan_shadps4(SHAD / "src")
    seen, common = parse_log(a.log)

    mods = {}
    for p in [GAME / "eboot.bin", *sorted((GAME / "sce_module").glob("*.prx"))]:
        e = Ps4Elf(p)
        m = e.parse_module()
        resolve_names(m, nids)
        mods[p.name] = m

    lle_exports: dict[tuple[str, str, str], str] = {}
    for name, m in mods.items():
        if name == "eboot.bin":
            continue
        mod_name = m.export_modules[0].name if m.export_modules else name
        for s in m.exports():
            lle_exports[(s.nid, s.lib, mod_name)] = name

    def resolve(s) -> tuple[str, dict]:
        key = (s.nid, s.lib, s.mod)
        if key in regs:
            return "HLE", regs[key]
        if key in lle_exports:
            return "LLE:" + lle_exports[key], {}
        return "COMMON_STUB" if s.type == 2 else "UNRESOLVED_OBJECT", {}

    def rows_for(m):
        out = []
        for s in m.imports():
            how, r = resolve(s)
            name = s.nid_name or "?"
            status = r.get("status", "") if how == "HLE" else ("LLE" if how.startswith("LLE") else "MISSING")
            out.append({
                "module": s.mod,
                "library": s.lib,
                "nid": s.nid,
                "name": name,
                "type": "FUNC" if s.type == 2 else "OBJECT",
                "subsystem": subsystem(s.mod),
                "resolution": how,
                "status": status,
                "hle_func": r.get("func", ""),
                "def": f'{r.get("def_file", "")}:{r.get("def_line", "")}' if r.get("def_file") else "",
                "reg": f'{r.get("reg_file", "")}:{r.get("reg_line", "")}' if r else "",
                "body_lines": r.get("body_lines", ""),
                "seen_in_log": sum(seen.get(n, 0) for n in {name, r.get("func", "")} if n) + common.get(name, 0),
            })
        out.sort(key=lambda x: (x["module"], x["name"]))
        return out

    fields = ["module", "library", "nid", "name", "type", "subsystem", "resolution", "status", "hle_func",
              "def", "reg", "body_lines", "seen_in_log"]
    eboot_rows = rows_for(mods["eboot.bin"])
    with open(a.out / "imports.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(eboot_rows)
    prx_rows = []
    for name, m in mods.items():
        if name == "eboot.bin":
            continue
        for r in rows_for(m):
            r = {"importer": name, **r}
            prx_rows.append(r)
    with open(a.out / "prx_imports.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=["importer", *fields])
        w.writeheader()
        w.writerows(prx_rows)

    per_mod = defaultdict(Counter)
    for r in eboot_rows:
        per_mod[r["module"]][r["status"]] += 1
    write_markdown(a.out / "IMPORTS.md", eboot_rows, prx_rows, a.log)
    summary = {
        "eboot_imports": len(eboot_rows),
        "status_total": Counter(r["status"] for r in eboot_rows),
        "per_module": {k: dict(v) for k, v in sorted(per_mod.items())},
        "prx_status_total": Counter(f'{r["importer"]}:{r["status"]}' for r in prx_rows),
        "log_common_stub_calls": dict(common),
        "hle_registrations_total": len(regs),
    }
    (a.out / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
