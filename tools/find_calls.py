"""Find call sites of imported functions in a PS4 module and disassemble the argument setup.

    python -I E:\\SecondSonPC\\tools\\find_calls.py sceSystemServiceParamGetString [more names...] [--before 64]

Read-only. Addresses are module-relative (eboot base is 0x0 in the file; shadPS4 maps it at 0x55e0000).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "pydeps"))
import capstone  # noqa: E402

from ps4elf import PT_LOAD, Ps4Elf, load_aerolib, resolve_names  # noqa: E402

SHAD = Path(r"E:\SecondSonPC\third_party\shadPS4")


class Image:
    def __init__(self, path: Path):
        self.elf = Ps4Elf(path)
        self.mod = self.elf.parse_module()
        resolve_names(self.mod, load_aerolib(SHAD / "src/core/aerolib/aerolib.inl"))
        self.img, _ = self.elf.build_image()
        text = next(p for p in self.mod.phdrs if p.p_type == PT_LOAD and p.p_flags & 1)
        self.text_lo, self.text_hi = text.p_vaddr, text.p_vaddr + text.p_filesz
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self._plt_cache: dict[int, list[int]] | None = None

    def got_slots(self, name: str) -> list[int]:
        return [off for off, s in self.mod.plt_slots().items() if s.nid_name == name]

    def plt_stubs(self) -> dict[int, list[int]]:
        """GOT slot -> addresses of `jmp qword [rip+disp]` stubs (FF 25 disp32)."""
        if self._plt_cache is not None:
            return self._plt_cache
        out: dict[int, list[int]] = {}
        img = self.img
        i = img.find(b"\xff\x25", self.text_lo)
        while 0 <= i < self.text_hi:
            disp = struct.unpack_from("<i", img, i + 2)[0]
            out.setdefault(i + 6 + disp, []).append(i)
            i = img.find(b"\xff\x25", i + 1)
        self._plt_cache = out
        return out

    def callers(self, target: int) -> list[int]:
        """E8 rel32 calls and E9 rel32 tail jumps to target (byte scan, verified with capstone)."""
        res = []
        img = self.img
        for op in (b"\xe8", b"\xe9"):
            i = img.find(op, self.text_lo)
            while 0 <= i < self.text_hi - 5:
                rel = struct.unpack_from("<i", img, i + 1)[0]
                if i + 5 + rel == target:
                    insn = next(self.cs.disasm(bytes(img[i:i + 5]), i), None)
                    if insn and insn.mnemonic in ("call", "jmp"):
                        res.append(i)
                i = img.find(op, i + 1)
        return sorted(res)

    def got_indirect_calls(self, slot: int) -> list[int]:
        """FF 15 disp32: call qword [rip+disp] straight through the GOT slot."""
        res = []
        img = self.img
        i = img.find(b"\xff\x15", self.text_lo)
        while 0 <= i < self.text_hi:
            disp = struct.unpack_from("<i", img, i + 2)[0]
            if i + 6 + disp == slot:
                res.append(i)
            i = img.find(b"\xff\x15", i + 1)
        return res

    def disasm_window(self, addr: int, before: int, after: int = 24) -> list[str]:
        # Try several start offsets and keep the one that decodes cleanly into `addr`.
        for back in range(before, before - 16, -1):
            start = max(self.text_lo, addr - back)
            code = bytes(self.img[start:addr + after])
            lines, hit = [], False
            for ins in self.cs.disasm(code, start):
                lines.append(f"  {ins.address:#010x}: {ins.mnemonic:<6} {ins.op_str}")
                if ins.address == addr:
                    hit = True
                    lines[-1] += "    <=== CALL"
            if hit:
                return lines
        return [f"  (could not align disassembly at {addr:#x})"]

    def string_at(self, addr: int, limit: int = 120) -> str:
        if not (0 <= addr < len(self.img)):
            return ""
        end = self.img.find(b"\0", addr, addr + limit)
        raw = bytes(self.img[addr:end if end > 0 else addr + limit])
        return raw.decode("ascii", "replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="+")
    ap.add_argument("--module", default=r"E:\shdps4\CUSA00004\eboot.bin")
    ap.add_argument("--before", type=int, default=64)
    a = ap.parse_args()
    im = Image(Path(a.module))
    stubs = im.plt_stubs()
    for name in a.names:
        slots = im.got_slots(name)
        print(f"===== {name}: GOT slots {[hex(s) for s in slots]}")
        for slot in slots:
            for stub in stubs.get(slot, []):
                calls = im.callers(stub)
                print(f"  PLT stub {stub:#x} -> {len(calls)} call sites")
                for c in calls:
                    print(f"--- call site {c:#x}")
                    print("\n".join(im.disasm_window(c, a.before)))
            for c in im.got_indirect_calls(slot):
                print(f"--- indirect call [GOT] at {c:#x}")
                print("\n".join(im.disasm_window(c, a.before)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
