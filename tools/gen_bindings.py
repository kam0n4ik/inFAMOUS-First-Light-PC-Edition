"""Generate SecondSonPC's static binding table (Phase 1).

Inputs (read-only):
  docs/generated/imports.csv, docs/generated/prx_imports.csv  -- every import of eboot.bin,
      libc.prx and libSceFios2.prx with the Phase 0 resolution (tools/audit_imports.py)
  third_party/shadPS4/src                                      -- LIB_FUNCTION/LIB_OBJ registrations

Outputs:
  src/loader/generated/import_table.inc
      One row per unique (library, module, nid) imported by the game modules: name, kind,
      subsystem, importers and how Phase 0 resolved it. No shadPS4 dependency.
  src/bridge/shadps4/generated/bridge_exports.cpp
      TEMPORARY (Phase 1-4): the address of every shadPS4 HLE function/object that can serve one
      of those imports, referenced explicitly by its C++ name (no SymbolsResolver/aerolib).

Usage:  python -I tools/gen_bindings.py [--check]
  --check  only verifies that the outputs are up to date (exit code 1 if not)
"""
from __future__ import annotations

import csv
import re
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHAD = ROOT / "third_party" / "shadPS4"
IMPORTS = [ROOT / "docs" / "generated" / "imports.csv", ROOT / "docs" / "generated" / "prx_imports.csv"]
OUT_TABLE = ROOT / "src" / "loader" / "generated" / "import_table.inc"
OUT_BRIDGE = ROOT / "src" / "bridge" / "shadps4" / "generated" / "bridge_exports.cpp"

IMPORTER_BITS = {"eboot.bin": "kEboot", "libc.prx": "kLibc", "libSceFios2.prx": "kFios2"}
# shadPS4 sources SecondSonPC replaces at build time (cmake/shadps4_bootstrap.cmake): their
# functions are not offered as BRIDGE (SecondSonPC's own implementation is the only one).
REPLACED_FILES = {"src/core/libraries/kernel/threads/tcb.cpp"}
PHASE0 = {"HLE": "Hle", "COMMON_STUB": "CommonStub", "UNRESOLVED_OBJECT": "UnresolvedObject"}


# ----------------------------------------------------------------------------- C++ scanning
def strip_code(text: str) -> str:
    """Replaces comments and string/char literals with spaces (keeps offsets and newlines)."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "'" and i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_"):
            i += 1  # digit separator (1'000) or the end of an identifier, not a literal
        elif c == "'" and not re.match(r"'(?:\\.[^']{0,8}|[^'\\\n])'", text[i:i + 12]):
            i += 1  # stray apostrophe
        elif c == '"' or c == "'":
            if c == '"' and i > 0 and text[i - 1] == "R":
                m = re.match(r'"([^(]*)\(', text[i:])
                end = text.find(")" + m.group(1) + '"', i) if m else -1
                j = n if end < 0 else end + len(m.group(1)) + 2
            else:
                j = i + 1
                while j < n and text[j] != c:
                    j += 2 if text[j] == "\\" else 1
                j += 1
            for k in range(i + 1, min(j - 1, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


class Source:
    _cache: dict[Path, "Source"] = {}

    def __init__(self, path: Path):
        self.path = path
        self.text = path.read_text(encoding="utf-8", errors="replace")
        self.code = strip_code(self.text)
        self.line_starts = [0] + [m.end() for m in re.finditer("\n", self.text)]
        self._namespaces = self._scan_namespaces()

    @classmethod
    def get(cls, rel: str) -> "Source":
        path = SHAD / rel.replace("\\", "/")
        if path not in cls._cache:
            cls._cache[path] = Source(path)
        return cls._cache[path]

    def offset(self, line: int) -> int:
        return self.line_starts[line - 1]

    def _scan_namespaces(self) -> list[tuple[int, int, str]]:
        """(start, end, qualified name) of every named namespace block.

        Brace counting is unreliable across #ifdef branches, so blocks are delimited the way
        shadPS4 writes them: `namespace X {` ... `} // namespace X`.
        """
        opens = [(m.start(), m.end(), m.group(1)) for m in
                 re.finditer(r"^[ \t]*namespace[ \t]+([A-Za-z_][\w:]*)[ \t]*\{", self.text, re.M)]
        closes = [(m.start(), m.group(1)) for m in
                  re.finditer(r"^[ \t]*\}[ \t]*//[ \t]*namespace[ \t]+([A-Za-z_][\w:]*)", self.text, re.M)]
        used = set()
        raw = []
        for start, body, name in reversed(opens):  # innermost-last opens claim the nearest close
            for k, (cpos, cname) in enumerate(closes):
                if k not in used and cpos > body and cname == name:
                    used.add(k)
                    raw.append((body, cpos, name))
                    break
        raw.sort()
        blocks = []
        for i, (start, end, name) in enumerate(raw):
            outer = [n for s, e, n in raw[:i] if s < start and end <= e]
            blocks.append((start, end, "::".join(outer + [name])))
        return blocks

    def record_types(self) -> dict[str, str]:
        """name -> "struct"/"class" of the records this file defines at line start."""
        return {name: kw for kw, name in
                re.findall(r"^[ \t]*(struct|class)[ \t]+(\w+)[ \t]*(?:final[ \t]*)?[:{]", self.code, re.M)}

    def namespace_at(self, pos: int) -> str:
        best = ""
        best_start = -1
        for start, end, name in self._namespaces:
            if start <= pos <= end and start > best_start and name:
                best, best_start = name, start
        return best

    def statement_at(self, line: int) -> str:
        """Text of the registration statement starting at `line` (may span lines)."""
        start = self.offset(line)
        end = self.text.find(";", start)
        return " ".join(self.text[start:end].split())

    def is_static_decl(self, name: str) -> bool:
        return re.search(r"\bstatic\b[^;{]*\b" + re.escape(name) + r"\b", self.code) is not None

    def project_includes(self) -> list[str]:
        """src-relative project headers this file includes, in order."""
        out = []
        for m in re.finditer(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', self.text, re.M):
            inc = m.group(1)
            if (SHAD / "src" / inc).exists():
                out.append(inc)
            elif (self.path.parent / inc).exists():
                out.append((self.path.parent / inc).resolve().relative_to((SHAD / "src").resolve()).as_posix())
        return out

    def namespace_usings(self, namespace: str) -> list[str]:
        """`using` declarations/directives written directly inside `namespace` blocks."""
        out = []
        for start, end, name in self._namespaces:
            if name != namespace:
                continue
            depth = 0
            seg = self.code[start:end]
            i = 0
            while i < len(seg):
                c = seg[i]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                elif depth == 0 and seg.startswith("using", i) and (i == 0 or not seg[i - 1].isalnum()):
                    j = seg.find(";", i)
                    out.append(" ".join(self.text[start + i:start + j + 1].split()))
                    i = j
                i += 1
        return out

    def definition_signature(self, line: int, name: str) -> str | None:
        """Declaration text of the function defined at `line` (without the body)."""
        pos = self.offset(line)
        m = re.compile(r"\b" + re.escape(name) + r"\s*\(").search(self.code, pos)
        if not m:
            return None
        # start of the declaration: after the previous ; } { or preprocessor line
        head = self.code[:m.start()]
        cut = max(head.rfind(";"), head.rfind("}"), head.rfind("{"))
        pp = head.rfind("\n#")
        if pp > cut:
            cut = head.find("\n", pp + 1)
        start = cut + 1
        # end: the parenthesis that closes the parameter list
        depth, j = 0, m.end() - 1
        while j < len(self.code):
            if self.code[j] == "(":
                depth += 1
            elif self.code[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        sig = self.code[start:j + 1]
        tail = self.code[j + 1:self.code.find("{", j)]
        if "noexcept" in tail:
            sig += " noexcept"
        sig = " ".join(sig.split())
        sig = re.sub(r"^(?:\[\[[^\]]*\]\]\s*)+", "", sig)
        return sig


# ----------------------------------------------------------------------------- inputs
def load_imports() -> "OrderedDict[tuple[str, str, str], dict]":
    rows: "OrderedDict[tuple[str, str, str], dict]" = OrderedDict()
    for f in IMPORTS:
        for r in csv.DictReader(f.open(encoding="utf-8")):
            importer = r.get("importer") or "eboot.bin"
            key = (r["library"], r["module"], r["nid"])
            if key in rows:
                rows[key]["importers"].add(importer)
                continue
            r = dict(r)
            r["importers"] = {importer}
            rows[key] = r
    return OrderedDict(sorted(rows.items(), key=lambda kv: (kv[0][1], kv[0][0], kv[1]["name"], kv[0][2])))


REG_RE = re.compile(
    r'LIB_(FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\d+\s*,\s*"([^"]+)"\s*,\s*(.+?)\s*\)$')


def c_ident(expr: str) -> str:
    return expr.strip().lstrip("&").strip()


def resolve_bridge(row: dict) -> tuple[str | None, str | None, str | None]:
    """-> (kind, qualified C++ expression, forward declaration) or (None, reason, None)."""
    if row["resolution"] != "HLE" or not row["reg"]:
        return None, "no shadPS4 registration", None
    reg_file, reg_line = row["reg"].rsplit(":", 1)
    if reg_file.replace("\\", "/") in REPLACED_FILES:
        return None, f"defined in {reg_file}, which SecondSonPC replaces at build time", None
    src = Source.get(reg_file)
    stmt = src.statement_at(int(reg_line))
    m = REG_RE.search(stmt)
    if not m:
        return None, f"cannot parse registration: {stmt}", None
    kind, nid, lib, mod, expr = m.groups()
    if nid != row["nid"] or lib != row["library"] or mod != row["module"]:
        return None, f"registration key mismatch: {stmt}", None
    ident = c_ident(expr)
    # ORBIS(f): shadPS4's errno -> ORBIS error wrapper (core/libraries/kernel/kernel.h)
    wrapper = None
    wm = re.fullmatch(r"(ORBIS)\(\s*([\w:]+)\s*\)", ident)
    if wm:
        wrapper, ident = wm.groups()
    if "::" in ident:
        qualified = ident
        short = ident.rsplit("::", 1)[1]
    else:
        ns = src.namespace_at(src.offset(int(reg_line)))
        qualified = f"{ns}::{ident}" if ns else ident
        short = ident
    if src.is_static_decl(short):
        return None, f"{qualified} has internal linkage (static) in {reg_file}", None
    files = [reg_file]
    if kind == "FUNCTION":
        def_file, def_line = (row["def"].rsplit(":", 1) if row["def"] else (reg_file, reg_line))
        dsrc = Source.get(def_file)
        sig = dsrc.definition_signature(int(def_line), short)
        if sig is None:
            return None, f"definition of {qualified} not found", None
        dns = dsrc.namespace_at(dsrc.offset(int(def_line)))
        files.append(def_file)
        decl = {"short": short, "namespace": dns, "signature": sig, "file": def_file, "files": files}
        return "Function", f"{wrapper}({qualified})" if wrapper else qualified, decl
    return "Object", qualified, {"short": short, "namespace": None, "files": files}


# ----------------------------------------------------------------------------- outputs
def cstr(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gen_table(rows) -> str:
    lines = [
        "// GENERATED by tools/gen_bindings.py from docs/generated/{imports,prx_imports}.csv -- do not edit.",
        "// Every import of eboot.bin, libc.prx and libSceFios2.prx, keyed exactly like the game imports it.",
        "// Columns: nid, library, module, name, kind, subsystem, importers, Phase 0 resolution.",
        "// clang-format off",
    ]
    for (lib, mod, nid), r in rows.items():
        kind = "Kind::Object" if r["type"] == "OBJECT" else "Kind::Function"
        importers = "|".join(IMPORTER_BITS[i] for i in sorted(r["importers"]))
        res = r["resolution"]
        phase0 = "Phase0::Lle" if res.startswith("LLE") else "Phase0::" + PHASE0[res]
        lines.append(f"{{{cstr(nid)}, {cstr(lib)}, {cstr(mod)}, {cstr(r['name'])}, {kind}, "
                     f"{cstr(r['subsystem'])}, {importers}, {phase0}}},")
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


def gen_bridge(rows) -> tuple[str, list[str]]:
    entries, problems = [], []
    resolved = []
    for (lib, mod, nid), r in rows.items():
        if r["resolution"] != "HLE":
            continue
        kind, expr, info = resolve_bridge(r)
        if kind is None:
            problems.append(f"{lib}/{r['name']} ({nid}): {expr}")
            continue
        resolved.append((lib, mod, nid, r, kind, expr, info))

    # The same project headers the defining/registering .cpp files include (types, macros).
    headers: "OrderedDict[str, None]" = OrderedDict()
    for *_, info in resolved:
        for f in info["files"]:
            for inc in Source.get(f).project_includes():
                # UI headers (ImGui dialogs) are not needed and clash with <windows.h> macros.
                if inc.endswith(".h") and not inc.endswith("_ui.h") and \
                        not inc.startswith(("imgui", "SDL", "boost")):
                    headers[inc] = None
    corpus = "\n".join((SHAD / "src" / h).read_text(encoding="utf-8", errors="replace") for h in headers)
    corpus = strip_code(corpus)

    # Forward declarations only for functions no included header declares.
    decls: "OrderedDict[str, OrderedDict[str, None]]" = OrderedDict()
    usings: "OrderedDict[str, OrderedDict[str, None]]" = OrderedDict()
    records: "OrderedDict[str, dict[str, str]]" = OrderedDict()
    for lib, mod, nid, r, kind, expr, info in resolved:
        if kind == "Function":
            # declared = a PS4_SYSV_ABI declaration of that name (a mere call does not count)
            if not re.search(r"PS4_SYSV_ABI\s+" + re.escape(info["short"]) + r"\s*\(", corpus):
                ns = info["namespace"]
                dsrc = Source.get(info["file"])
                for u in dsrc.namespace_usings(ns):
                    usings.setdefault(ns, OrderedDict())[u] = None
                records.setdefault(ns, {}).update(dsrc.record_types())
                decls.setdefault(ns, OrderedDict())[info["signature"] + ";"] = None

    # Records defined only in a .cpp and named by a declaration/using: forward-declare them
    # (pointers and aliases of pointers only need an incomplete type).
    forwards: "OrderedDict[str, list[str]]" = OrderedDict()
    for ns, sigs in decls.items():
        text = " ".join(list(sigs.keys()) + list(usings.get(ns, {}).keys()))
        for name, kw in sorted(records.get(ns, {}).items()):
            if re.search(r"\b" + name + r"\b", text) and not re.search(r"\b(?:struct|class)\s+" + name + r"\b", corpus):
                forwards.setdefault(ns, []).append(f"{kw} {name};")

    for lib, mod, nid, r, kind, expr, info in resolved:
        addr = f"HOST_CALL({expr})" if kind == "Function" else f"&{expr}"
        entries.append(f"    {{{cstr(nid)}, {cstr(lib)}, {cstr(mod)}, Addr({addr})}}, // {r['name']}")

    out = [
        "// SPDX-License-Identifier: GPL-2.0-or-later",
        "// GENERATED by tools/gen_bindings.py -- do not edit.",
        "//",
        "// SecondSonPC -- TEMPORARY Phase 1-4 bridge (removed in Phase 5): addresses of the shadPS4 HLE",
        "// functions/objects (third_party/shadPS4 @ 0fe263a) that can serve the game's imports. Each one",
        "// is referenced by its C++ name; shadPS4's SymbolsResolver, InitHLELibs and aerolib are not used.",
        "// Functions are taken through HOST_CALL, exactly as shadPS4's LIB_FUNCTION registers them, so a",
        "// relocated slot holds the same address in the Phase 0 and the Phase 1 loader.",
        "",
        "#include <cstdint>",
        "",
        '#include "bridge/shadps4/bridge_exports.h"',
        '#include "core/libraries/kernel/kernel.h" // ORBIS()',
        '#include "core/tls.h"                     // HOST_CALL()',
    ]
    for h in headers:
        out.append(f'#include "{h}"')
    out.append("")
    out.append("// Declarations of the functions that shadPS4 defines without a header declaration")
    out.append("// (with the using-declarations of their .cpp, for the type names in the signatures).")
    for ns, sigs in decls.items():
        out.append(f"namespace {ns} {{" if ns else "// global namespace")
        out.extend(forwards.get(ns, []))
        out.extend(usings.get(ns, {}).keys())
        out.extend(sigs.keys())
        out.append(f"}} // namespace {ns}" if ns else "")
        out.append("")
    out += [
        "namespace SecondSon::Bridge {",
        "",
        "namespace {",
        "template <typename T>",
        "constexpr const void* Addr(T* p) {",
        "    return reinterpret_cast<const void*>(p);",
        "}",
        "} // namespace",
        "",
        "// clang-format off",
        "const BridgeExport kBridgeExports[] = {",
    ]
    out += entries
    out += [
        "};",
        "// clang-format on",
        "",
        "std::span<const BridgeExport> BridgeExports() {",
        "    return kBridgeExports;",
        "}",
        "",
        "} // namespace SecondSon::Bridge",
        "",
    ]
    return "\n".join(out), problems


def main() -> int:
    check = "--check" in sys.argv
    rows = load_imports()
    table = gen_table(rows)
    bridge, problems = gen_bridge(rows)
    stale = []
    for path, text in ((OUT_TABLE, table), (OUT_BRIDGE, bridge)):
        old = path.read_text(encoding="utf-8") if path.exists() else None
        if old != text:
            stale.append(path)
            if not check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(text.encode("utf-8"))
    counts = {}
    for r in rows.values():
        res = "LLE" if r["resolution"].startswith("LLE") else r["resolution"]
        counts[res] = counts.get(res, 0) + 1
    print(f"imports: {len(rows)} unique {dict(sorted(counts.items()))}")
    n_entries = sum(1 for line in bridge.splitlines() if line.startswith('    {"'))
    print(f"bridge exports: {n_entries} entries")
    for p in problems:
        print(f"  no bridge provider: {p}")
    if check and stale:
        print("stale:", ", ".join(str(p) for p in stale))
        return 1
    for p in stale:
        print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
