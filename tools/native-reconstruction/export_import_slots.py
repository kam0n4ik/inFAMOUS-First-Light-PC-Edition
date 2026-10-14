"""Dump the GOT slots of a PS4 module (eboot.bin) with the imported symbol names, as JSON.

Uses tools/ps4elf.py (the same parser the loader was ported from) and shadPS4's aerolib.inl
for NID -> name. Output feeds ghidra/label_imports.py.

usage: py -I -X utf8 export_import_slots.py <eboot.bin> <out.json>
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import ps4elf  # noqa: E402

elf = ps4elf.Ps4Elf(sys.argv[1])
mod = elf.parse_module()
ps4elf.resolve_names(mod, ps4elf.load_aerolib(ROOT / "third_party/shadPS4/scripts/aerolib.inl"))

slots = []
for r in mod.relocs:
    if not (0 <= r.sym_index < len(mod.symbols)):
        continue
    s = mod.symbols[r.sym_index]
    if not s.nid or s.value != 0:
        continue  # local relocation, not an import
    slots.append({
        "slot": r.offset,
        "plt": r.plt,
        "type": r.type,
        "nid": s.nid,
        "lib": s.lib,
        "module": s.mod,
        "name": s.nid_name or "",
    })
named = sum(1 for s in slots if s["name"])
Path(sys.argv[2]).write_text(json.dumps(slots, indent=0), encoding="utf-8")
print(f"{len(slots)} import slots ({sum(s['plt'] for s in slots)} PLT), {named} with known names")
