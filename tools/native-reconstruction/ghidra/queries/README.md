# Ghidra query snippets (read-only)

Snippets for `../run.py`: each runs inside one read-only session with `prog, fm, refm, st, listing, mem, dec, A, F, args`
in scope. They never modify the analysis database.

```
export GHIDRA_INSTALL_DIR='E:\tools\ghidra' JAVA_HOME='E:\tools\jdk21'
export SSPC_GHIDRA_PROJECT_DIR='E:\tools\ghidra-projects\scripts'   # copy of the project while pyghidra-mcp holds the main one
cd tools/native-reconstruction/ghidra
E:/tools/ghidra-mcp/.venv/Scripts/python.exe -X utf8 run.py queries/<snippet>.py <args>
```

| Snippet | Arguments | Output |
|---|---|---|
| `decompile.py` | `outdir addr...` | one `.c` per function: decompiled text, callers, callees |
| `decompile_range.py` | `outdir lo hi` | the same for every function whose entry is in `[lo, hi)` |
| `disasm.py` | `addr` | full disassembly of the function containing `addr` |
| `find_scalar.py` | `value...` | functions whose instructions use each immediate (struct offsets, strides, magic constants) |
| `xrefs.py` | `addr...` | functions referencing each address, by reference type |
| `raw_calls.py` | `addr...` | every `E8`/`E9` rel32 in executable blocks that lands on each address (finds calls Ghidra did not attach) |
| `caller_tree.py` | `depth addr...` | caller tree |
| `find_vtables.py` | `addr...` | data tables holding a pointer to each function, walked back to the table start |
| `qword_table.py` | `addr count stride [disasm-addr...]` | qword table rows with function names; optional raw disassembly at addresses |
| `u32_table.py` | `addr count` | u32 table |

Results feed `docs/native-reconstruction/RENDER_ENGINE_ARCHITECTURE.md`.
