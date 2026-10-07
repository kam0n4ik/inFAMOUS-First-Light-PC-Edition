"""Read-only parser for PS4 (fake-signed) SELF/ELF modules.

Mirrors the logic of shadPS4 src/core/loader/elf.cpp and src/core/module.cpp:
SELF header -> ELF header -> program headers -> PT_DYNAMIC / PT_SCE_DYNLIBDATA ->
DT_SCE_* tags -> modules, libraries, symbols, PLT/RELA relocations.

The input file is opened read-only and never modified.
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

SELF_MAGIC = 0x1D3D154F
ELF_MAGIC = b"\x7fELF"

PT_LOAD = 1
PT_DYNAMIC = 2
PT_TLS = 7
PT_SCE_RELA = 0x60000000
PT_SCE_DYNLIBDATA = 0x61000000
PT_SCE_PROCPARAM = 0x61000001
PT_SCE_MODULE_PARAM = 0x61000002
PT_SCE_RELRO = 0x61000010
PT_GNU_EH_FRAME = 0x6474E550

DT_NULL = 0
DT_NEEDED = 1
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_MODULE_INFO = 0x6100000D
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_IMPORT_LIB_ATTR = 0x61000019
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F
DT_SCE_ORIGINAL_FILENAME = 0x61000009
DT_SCE_PLTGOT = 0x61000027

R_X86_64_64 = 1
R_X86_64_GLOB_DAT = 6
R_X86_64_JUMP_SLOT = 7
R_X86_64_RELATIVE = 8
R_X86_64_DTPMOD64 = 16

STT_NAMES = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 6: "TLS"}
STB_NAMES = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}

_CODES = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def encode_id(value: int) -> str:
    """Same as shadPS4 module.cpp EncodeId()."""
    if value < 0x40:
        return _CODES[value]
    if value < 0x1000:
        return _CODES[(value >> 6) & 0x3F] + _CODES[value & 0x3F]
    return _CODES[(value >> 12) & 0x3F] + _CODES[(value >> 6) & 0x3F] + _CODES[value & 0x3F]


@dataclass
class Phdr:
    index: int
    p_type: int
    p_flags: int
    p_offset: int
    p_vaddr: int
    p_filesz: int
    p_memsz: int
    p_align: int


@dataclass
class ModLib:
    name: str
    id: int
    version: int
    enc_id: str


@dataclass
class Symbol:
    index: int
    raw_name: str
    nid: str
    lib_enc: str
    mod_enc: str
    bind: int
    type: int
    value: int
    size: int
    lib: str = ""
    mod: str = ""
    nid_name: str = ""


@dataclass
class Reloc:
    offset: int
    type: int
    sym_index: int
    addend: int
    plt: bool


@dataclass
class Module:
    path: Path
    is_self: bool
    phdrs: list[Phdr] = field(default_factory=list)
    entry: int = 0
    e_type: int = 0
    import_modules: list[ModLib] = field(default_factory=list)
    import_libs: list[ModLib] = field(default_factory=list)
    export_modules: list[ModLib] = field(default_factory=list)
    export_libs: list[ModLib] = field(default_factory=list)
    symbols: list[Symbol] = field(default_factory=list)
    relocs: list[Reloc] = field(default_factory=list)
    original_filename: str = ""
    sdk_version: int = 0

    # ----------------------------------------------------------------- helpers
    def imports(self) -> list[Symbol]:
        return [s for s in self.symbols if s.value == 0 and s.bind in (1, 2) and s.type in (1, 2) and s.nid]

    def exports(self) -> list[Symbol]:
        return [s for s in self.symbols if s.value != 0 and s.bind in (1, 2) and s.type in (1, 2) and s.nid]

    def plt_slots(self) -> dict[int, Symbol]:
        """GOT slot vaddr (relative to module base) -> imported symbol (JUMP_SLOT/GLOB_DAT/64)."""
        out = {}
        for r in self.relocs:
            if r.type in (R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT, R_X86_64_64) and 0 <= r.sym_index < len(self.symbols):
                out[r.offset] = self.symbols[r.sym_index]
        return out


class Ps4Elf:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.data = self.path.read_bytes()  # read-only
        self.self_segments: list[tuple[int, int, int, int]] = []
        self.elf_off = 0
        self.is_self = False
        self._parse_headers()

    # ------------------------------------------------------------------ SELF/ELF
    def _parse_headers(self) -> None:
        d = self.data
        magic = struct.unpack_from("<I", d, 0)[0]
        if magic == SELF_MAGIC:
            self.is_self = True
            seg_count = struct.unpack_from("<H", d, 0x18)[0]
            for i in range(seg_count):
                flags, foff, fsz, msz = struct.unpack_from("<QQQQ", d, 0x20 + i * 32)
                self.self_segments.append((flags, foff, fsz, msz))
            self.elf_off = 0x20 + seg_count * 32
        elif d[:4] == ELF_MAGIC:
            self.elf_off = 0
        else:
            raise ValueError(f"{self.path}: neither SELF nor ELF (magic {magic:#x})")
        if d[self.elf_off:self.elf_off + 4] != ELF_MAGIC:
            raise ValueError(f"{self.path}: ELF header not found at {self.elf_off:#x}")
        eo = self.elf_off
        (self.e_type, self.e_machine, _ver, self.e_entry, self.e_phoff, _shoff, _flags, _ehsize,
         self.e_phentsize, self.e_phnum) = struct.unpack_from("<HHIQQQIHHH", d, eo + 16)
        self.phdrs = []
        for i in range(self.e_phnum):
            p = struct.unpack_from("<IIQQQQQQ", d, eo + self.e_phoff + i * self.e_phentsize)
            p_type, p_flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_align = p
            self.phdrs.append(Phdr(i, p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz, p_align))

    def read_segment_bytes(self, file_offset: int, size: int) -> bytes:
        """Equivalent of shadPS4 Elf::LoadSegment()."""
        if not self.is_self:
            return self.data[file_offset:file_offset + size]
        for flags, seg_off, _fsz, _msz in self.self_segments:
            if flags & 0x800:  # blocked
                phdr = self.phdrs[(flags >> 20) & 0xFFF]
                if phdr.p_offset <= file_offset < phdr.p_offset + phdr.p_filesz:
                    off = seg_off + (file_offset - phdr.p_offset)
                    return self.data[off:off + size]
        raise ValueError(f"file offset {file_offset:#x} not covered by any SELF segment")

    def phdr_bytes(self, ph: Phdr) -> bytes:
        return self.read_segment_bytes(ph.p_offset, ph.p_filesz) if ph.p_filesz else b""

    def build_image(self) -> tuple[bytearray, int]:
        """Flat memory image of PT_LOAD/RELRO segments, base = 0 (module-relative vaddrs)."""
        loads = [p for p in self.phdrs if p.p_type in (PT_LOAD, PT_SCE_RELRO) and p.p_memsz]
        top = max(p.p_vaddr + p.p_memsz for p in loads)
        img = bytearray(top)
        for p in loads:
            b = self.phdr_bytes(p)
            img[p.p_vaddr:p.p_vaddr + len(b)] = b
        return img, top

    # ---------------------------------------------------------------- dynamic
    def parse_module(self) -> Module:
        m = Module(self.path, self.is_self, self.phdrs, self.e_entry, self.e_type)
        dyn = next((p for p in self.phdrs if p.p_type == PT_DYNAMIC), None)
        dlib = next((p for p in self.phdrs if p.p_type == PT_SCE_DYNLIBDATA), None)
        if dyn is None or dlib is None:
            return m
        dynb = self.phdr_bytes(dyn)
        dl = self.phdr_bytes(dlib)
        tags = []
        for i in range(0, len(dynb), 16):
            tag, val = struct.unpack_from("<qQ", dynb, i)
            if tag == DT_NULL:
                break
            tags.append((tag, val))
        tv = {}
        for tag, val in tags:
            tv.setdefault(tag, val)
        strtab_off = tv.get(DT_SCE_STRTAB, 0)
        strtab = dl[strtab_off:strtab_off + tv.get(DT_SCE_STRSZ, 0)]

        def cstr(off: int) -> str:
            end = strtab.find(b"\0", off)
            return strtab[off:end].decode("ascii", "replace")

        for tag, val in tags:
            name_off = val & 0xFFFFFFFF
            if tag in (DT_SCE_NEEDED_MODULE, DT_SCE_MODULE_INFO):
                ver = (val >> 32) & 0xFFFF
                mid = (val >> 48) & 0xFFFF
                ml = ModLib(cstr(name_off), mid, ver, encode_id(mid))
                (m.import_modules if tag == DT_SCE_NEEDED_MODULE else m.export_modules).append(ml)
            elif tag in (DT_SCE_IMPORT_LIB, DT_SCE_EXPORT_LIB):
                ver = (val >> 32) & 0xFFFF
                lid = (val >> 48) & 0xFFFF
                ml = ModLib(cstr(name_off), lid, ver, encode_id(lid))
                (m.import_libs if tag == DT_SCE_IMPORT_LIB else m.export_libs).append(ml)
            elif tag == DT_SCE_ORIGINAL_FILENAME:
                m.original_filename = cstr(val)

        all_libs = {l.enc_id: l.name for l in m.import_libs + m.export_libs}
        all_mods = {x.enc_id: x.name for x in m.import_modules + m.export_modules}

        symoff = tv.get(DT_SCE_SYMTAB, 0)
        symsz = tv.get(DT_SCE_SYMTABSZ, 0)
        for idx, i in enumerate(range(symoff, symoff + symsz, 24)):
            st_name, st_info, _st_other, _shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", dl, i)
            raw = cstr(st_name) if st_name else ""
            parts = raw.split("#")
            nid, lib_enc, mod_enc = parts if len(parts) == 3 else ("", "", "")
            s = Symbol(idx, raw, nid, lib_enc, mod_enc, st_info >> 4, st_info & 0xF, st_value, st_size)
            s.lib = all_libs.get(lib_enc, "")
            s.mod = all_mods.get(mod_enc, "")
            m.symbols.append(s)

        for tag_off, tag_sz, plt in ((DT_SCE_JMPREL, DT_SCE_PLTRELSZ, True), (DT_SCE_RELA, DT_SCE_RELASZ, False)):
            off = tv.get(tag_off, 0)
            sz = tv.get(tag_sz, 0)
            for i in range(off, off + sz, 24):
                r_offset, r_info, r_addend = struct.unpack_from("<QQq", dl, i)
                m.relocs.append(Reloc(r_offset, r_info & 0xFFFFFFFF, r_info >> 32, r_addend, plt))

        proc = next((p for p in self.phdrs if p.p_type == PT_SCE_PROCPARAM), None)
        if proc is not None and proc.p_filesz >= 0x18:
            pb = self.phdr_bytes(proc)
            # SceProcParam: u64 size; u32 magic 'ORBI'; u32 entry_count; u64 sdk_version
            m.sdk_version = struct.unpack_from("<Q", pb, 0x10)[0]
        return m


_NID_RE = re.compile(r'STUB\(\s*"([^"]+)"\s*,\s*([^)]+?)\s*\)', re.S)


def load_aerolib(aerolib_inl: str | Path) -> dict[str, str]:
    text = Path(aerolib_inl).read_text(encoding="utf-8", errors="replace")
    return {nid: name.strip() for nid, name in _NID_RE.findall(text)}


def resolve_names(module: Module, nids: dict[str, str]) -> None:
    for s in module.symbols:
        if s.nid:
            s.nid_name = nids.get(s.nid, "")
