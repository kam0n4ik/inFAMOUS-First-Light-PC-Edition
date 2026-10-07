"""Build docs/generated/CALLS.md from a SecondSonPC run (logs/calls.json) + the import audit CSVs.

    python -I E:\\SecondSonPC\\tools\\make_calls_md.py --calls E:\\SecondSonPC\\logs\\calls.json [--run-note "..."]

Ownership of the 417 host-side functions the game needs:
  NATIVE       implemented by SecondSonPC
  OBSERVED     SecondSonPC observer (logging) in front of a BRIDGE implementation
  BRIDGE       temporary shadPS4 implementation
  COMMON_STUB  nobody implements it; shadPS4 binds a generic stub returning 0
  LLE          satisfied by the game's own sce_module/*.prx (not part of the 417)
"""
from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from pathlib import Path

ROOT = Path(r"E:\SecondSonPC")
GEN = ROOT / "docs" / "generated"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--calls", type=Path, default=ROOT / "logs" / "calls.json")
    ap.add_argument("--out", type=Path, default=GEN / "CALLS.md")
    ap.add_argument("--run-note", default="")
    a = ap.parse_args()

    data = json.loads(a.calls.read_text(encoding="utf-8"))
    # calls.json lists every SecondSonPC export (any count) and every called BRIDGE function.
    calls = {(e["nid"], e["library"], e["module"]): e for e in data["entries"]}

    host: dict[tuple, dict] = {}
    lle: set[tuple] = set()
    for name in ("imports.csv", "prx_imports.csv"):
        with open(GEN / name, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                key = (r["nid"], r["library"], r["module"])
                if r["status"] == "LLE":
                    lle.add(key)
                else:
                    host.setdefault(key, {"name": r["name"], "status": r["status"]})

    for key, info in host.items():
        e = calls.get(key)
        info["calls"] = e["calls"] if e else 0
        if e and e["owner"] in ("NATIVE", "OBSERVED"):
            info["owner"] = e["owner"]
        elif info["status"] == "MISSING":
            info["owner"] = "COMMON_STUB"
        else:
            info["owner"] = "BRIDGE"

    by_owner = Counter(i["owner"] for i in host.values())
    called_by_owner = Counter(i["owner"] for i in host.values() if i["calls"] > 0)
    extra = [e for k, e in calls.items() if k not in host]
    called = sorted(((k, i) for k, i in host.items() if i["calls"] > 0), key=lambda x: -x[1]["calls"])

    w = data["wrapped"]
    out = ["# CALLS — реальные вызовы импортов (Phase 0)", "",
           f"Источник: `{a.calls}` (снимок {data.get('generated', '?')}). Счётчиком обёрнуто "
           f"{data.get('wrapped_total')} функций: native {w['native']}, observed {w['observed']}, "
           f"bridge {w['bridge']} (весь HLE-реестр shadPS4)."]
    if a.run_note:
        out += ["", f"Прогон: {a.run_note}"]
    out += ["", "## Владельцы 417 функций, которые должен предоставить рантайм", "",
            "| Владелец | Всего | Вызваны в прогоне |", "|---|---:|---:|"]
    for o in ("NATIVE", "OBSERVED", "BRIDGE", "COMMON_STUB"):
        out.append(f"| {o} | {by_owner.get(o, 0)} | {called_by_owner.get(o, 0)} |")
    out.append(f"| **Итого** | **{len(host)}** | **{sum(called_by_owner.values())}** |")
    out.append(f"| LLE (модули игры, не входят в 417) | {len(lle)} | не считаются |")
    out += ["", "OBSERVED — наблюдатель SecondSonPC перед реализацией shadPS4; для миграции это всё ещё BRIDGE.",
            "COMMON_STUB-вызовы счётчик не видит (shadPS4 пишет их в `logs\\bridge_shadps4.log`: `CommonStub`).",
            "", "## Вызванные функции (по убыванию)", "",
            "| # | Функция | Библиотека | Владелец | Вызовов |", "|---:|---|---|---|---:|"]
    for n, (k, i) in enumerate(called, 1):
        out.append(f"| {n} | `{i['name']}` | {k[1]} | {i['owner']} | {i['calls']} |")
    if extra:
        out += ["", "## Вызваны, но отсутствуют в списке импортов аудита", ""]
        out += [f"- `{e['name']}` {e['library']}/{e['module']} {e['owner']}: {e['calls']}" for e in extra]
    not_called = Counter(k[2] for k, i in host.items() if i["calls"] == 0)
    out += ["", "## Не вызывались в этом прогоне (по модулям)", "", "| Модуль | Функций |", "|---|---:|"]
    out += [f"| {mod} | {n} |" for mod, n in sorted(not_called.items())]
    a.out.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"NATIVE={by_owner.get('NATIVE', 0)} OBSERVED={by_owner.get('OBSERVED', 0)} "
          f"BRIDGE={by_owner.get('BRIDGE', 0)} COMMON_STUB={by_owner.get('COMMON_STUB', 0)} "
          f"LLE={len(lle)} called={len(called)} -> {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
