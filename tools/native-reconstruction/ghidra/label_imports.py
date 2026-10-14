"""Label PS4 import GOT slots and rename the PLT stubs in the Ghidra project (WRITES the project).

The project was imported without the PS4 loader, so calls to libkernel/libc/Gnm/... go through
unnamed stubs (`jmp [rip+slot]`). This names every slot `got_<name>` and every stub function
that jumps through a slot `<name>` (namespace "ps4import"), so decompiled code shows the PS4 API.

usage: label_imports.py <eboot_imports.json>   (from export_import_slots.py)
Back the project up first (copy E:\\tools\\ghidra-projects\\SecondSon.rep).
"""
import json
import sys

from gh import program, addr

slots = json.load(open(sys.argv[1], encoding="utf-8"))

with program(read_only=False) as prog:
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.program.model.symbol import SourceType
    st = prog.getSymbolTable()
    fm = prog.getFunctionManager()
    refm = prog.getReferenceManager()
    listing = prog.getListing()
    tx = prog.startTransaction("label PS4 imports")
    labeled = renamed = created = refs_total = 0
    no_ref = []
    try:
        ns = st.getNamespace("ps4import", prog.getGlobalNamespace()) or \
            st.createNameSpace(prog.getGlobalNamespace(), "ps4import", SourceType.USER_DEFINED)
        for s in slots:
            name = s["name"] or ("nid_" + s["nid"].replace("+", "_").replace("-", "_"))
            a = addr(prog, s["slot"])
            st.createLabel(a, "got_" + name, SourceType.USER_DEFINED)
            labeled += 1
            refs = list(refm.getReferencesTo(a))
            refs_total += len(refs)
            if not refs:
                no_ref.append(name)
            for r in refs:
                src = r.getFromAddress()
                ins = listing.getInstructionAt(src)
                if ins is None or ins.getMnemonicString().upper() != "JMP":
                    continue
                f = fm.getFunctionContaining(src)
                if f is None:
                    if CreateFunctionCmd(src).applyTo(prog):
                        f = fm.getFunctionAt(src)
                        created += 1
                if f is not None and f.getBody().getNumAddresses() <= 16:
                    f.setName(name, SourceType.USER_DEFINED)
                    f.setParentNamespace(ns)
                    renamed += 1
    finally:
        prog.endTransaction(tx, True)
    print(f"slots labeled={labeled} refs={refs_total} stubs renamed={renamed} stubs created={created} "
          f"slots without refs={len(no_ref)}")
    print("no-ref sample:", no_ref[:20])
