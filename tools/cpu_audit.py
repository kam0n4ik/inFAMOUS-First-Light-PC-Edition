"""CPU feature audit of the game's modules (P3.4): which instruction sets the code needs.

Decodes every function the way SecondSonPC's loader-time patcher does: exact ranges from the FDE
search table in .eh_frame_hdr; when decoding of a range stops before its end (a jump table after
the code), instructions are trusted only up to the last unconditional transfer before the stop.
Reports the instruction-set groups (capstone) of all trusted instructions, and every place of
the instructions a host CPU may lack or execute differently (SSE4a, reciprocal estimates, ...).

The game files are opened read-only.

    python -I tools/cpu_audit.py E:/shdps4/CUSA00004 [--csv out.csv]
"""
from __future__ import annotations

import argparse
import collections
import csv
import struct
import sys
from pathlib import Path

import capstone
from capstone import x86_const

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ps4elf import PT_GNU_EH_FRAME, Ps4Elf  # noqa: E402

# Instructions whose presence decides what a host CPU needs (or must emulate).
WATCH = {
    # SSE4a: AMD only (Jaguar has it; no Intel CPU does).
    "extrq", "insertq", "movntss", "movntsd",
    # Reciprocal estimates: the result bits differ between AMD and Intel.
    "rcpps", "rsqrtps", "rcpss", "rsqrtss", "vrcpps", "vrsqrtps", "vrcpss", "vrsqrtss",
    # Timing, identification, traps, system.
    "rdtsc", "rdtscp", "cpuid", "xgetbv", "int", "int1", "int3", "ud2", "hlt", "syscall",
    "rdrand", "rdseed", "rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase",
    # Feature-sensitive scalar ops.
    "popcnt", "lzcnt", "tzcnt", "movbe", "crc32", "andn", "bextr", "blsi", "blsmsk", "blsr",
    "prefetchw", "monitorx", "mwaitx", "clzero",
}

GROUP_NAMES = {
    v: k[len("X86_GRP_"):] for k, v in vars(x86_const).items() if k.startswith("X86_GRP_")
}
# Groups that are properties of the encoding, not CPU features.
IGNORED_GROUPS = {"MODE64", "MODE32", "NOT64BITMODE", "JUMP", "CALL", "RET", "INT", "IRET",
                  "BRANCH_RELATIVE", "PRIVILEGE", "INVALID", "16BITMODE", "NOVLX"}

UNCONDITIONAL = {"ret", "jmp", "ud2", "int3", "hlt"}


def read_encoded(img: bytes, pos: int, enc: int, base_pc: int, datarel: int = 0) -> tuple[int, int]:
    """DW_EH_PE_* value at img[pos]; returns (value, size)."""
    fmt = enc & 0x0F
    app = enc & 0x70
    if fmt == 0x0B:  # sdata4
        v, n = struct.unpack_from("<i", img, pos)[0], 4
    elif fmt == 0x03:  # udata4
        v, n = struct.unpack_from("<I", img, pos)[0], 4
    elif fmt == 0x0C:  # sdata8
        v, n = struct.unpack_from("<q", img, pos)[0], 8
    elif fmt in (0x04, 0x00):  # udata8 / absptr
        v, n = struct.unpack_from("<Q", img, pos)[0], 8
    else:
        raise ValueError(f"unsupported pointer encoding {enc:#x}")
    if app == 0x10:  # pcrel
        v += base_pc
    elif app == 0x30:  # datarel
        v += datarel
    return v, n


def uleb(img: bytes, pos: int) -> tuple[int, int]:
    v = shift = 0
    start = pos
    while True:
        b = img[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return v, pos - start


def cie_fde_encoding(img: bytes, cie: int) -> int:
    """Pointer encoding ('R' augmentation) of FDEs that use this CIE."""
    length = struct.unpack_from("<I", img, cie)[0]
    assert length != 0xFFFFFFFF
    pos = cie + 4 + 4  # length, CIE id
    pos += 1  # version
    end = img.index(0, pos)
    aug = img[pos:end].decode("ascii")
    pos = end + 1
    _, n = uleb(img, pos)  # code alignment
    pos += n
    while img[pos] & 0x80:  # data alignment (sleb)
        pos += 1
    pos += 1
    _, n = uleb(img, pos)  # return address register
    pos += n
    enc = 0x00
    if aug.startswith("z"):
        _, n = uleb(img, pos)
        pos += n
        for ch in aug[1:]:
            if ch == "R":
                enc = img[pos]
                pos += 1
            elif ch == "L":
                pos += 1
            elif ch == "P":
                penc = img[pos]
                pos += 1
                _, sz = read_encoded(img, pos, penc, pos)
                pos += sz
    return enc


def functions(elf: Ps4Elf, img: bytes) -> list[tuple[int, int]]:
    hdr_ph = next(p for p in elf.phdrs if p.p_type == PT_GNU_EH_FRAME)
    hdr = hdr_ph.p_vaddr
    version, ptr_enc, cnt_enc, tbl_enc = img[hdr:hdr + 4]
    assert version == 1
    pos = hdr + 4
    _eh_frame, n = read_encoded(img, pos, ptr_enc, pos, hdr)
    pos += n
    count, n = read_encoded(img, pos, cnt_enc, pos, hdr)
    pos += n
    out = []
    cie_enc: dict[int, int] = {}
    for _ in range(count):
        start, n = read_encoded(img, pos, tbl_enc, pos, hdr)
        pos += n
        fde, n = read_encoded(img, pos, tbl_enc, pos, hdr)
        pos += n
        length = struct.unpack_from("<I", img, fde)[0]
        assert length != 0xFFFFFFFF
        cie_ptr = struct.unpack_from("<I", img, fde + 4)[0]
        cie = fde + 4 - cie_ptr
        if cie not in cie_enc:
            cie_enc[cie] = cie_fde_encoding(img, cie)
        enc = cie_enc[cie]
        p = fde + 8
        begin, sz = read_encoded(img, p, enc, p)
        p += sz
        rng, _ = read_encoded(img, p, enc & 0x0F, p)  # range: value only
        assert begin == start, (hex(begin), hex(start))
        out.append((begin, rng))
    return out


def audit_module(path: Path, md: capstone.Cs, rows: list) -> dict:
    elf = Ps4Elf(path)
    img, _top = elf.build_image()
    img = bytes(img)
    funcs = functions(elf, img)
    stats = collections.Counter()
    groups = collections.Counter()
    watched = collections.Counter()
    for begin, size in funcs:
        if size == 0:
            continue
        code = img[begin:begin + size]
        insns = list(md.disasm(code, begin))
        end = insns[-1].address + insns[-1].size if insns else begin
        trusted = len(insns)
        if end != begin + size:
            stats["functions stopping early"] += 1
            trusted = 0
            for i, ins in enumerate(insns):
                if ins.mnemonic in UNCONDITIONAL:
                    trusted = i + 1
        stats["functions"] += 1
        for ins in insns[:trusted]:
            stats["instructions"] += 1
            for g in ins.groups:
                name = GROUP_NAMES.get(g, str(g))
                if name not in IGNORED_GROUPS:
                    groups[name] += 1
            if ins.mnemonic in WATCH:
                watched[ins.mnemonic] += 1
                rows.append({
                    "module": path.name, "offset": f"{ins.address:#x}", "function": f"{begin:#x}",
                    "mnemonic": ins.mnemonic, "operands": ins.op_str, "bytes": ins.bytes.hex(),
                })
        stats["untrusted tail instructions"] += len(insns) - trusted
    return {"stats": stats, "groups": groups, "watched": watched}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("--csv", help="every watched instruction, one row each")
    args = ap.parse_args()
    game = Path(args.game_dir)
    modules = [game / "eboot.bin"] + sorted((game / "sce_module").glob("*.prx"))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    rows: list[dict] = []
    for m in modules:
        r = audit_module(m, md, rows)
        print(f"== {m.name}")
        print("   " + ", ".join(f"{k} {v}" for k, v in r["stats"].items()))
        print("   groups: " + ", ".join(f"{k} {v}" for k, v in sorted(r["groups"].items(), key=lambda kv: -kv[1])))
        print("   watched: " + (", ".join(f"{k} {v}" for k, v in sorted(r["watched"].items())) or "none"))
    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=["module", "offset", "function", "mnemonic", "operands", "bytes"])
            w.writeheader()
            w.writerows(rows)
        print(f"rows: {len(rows)} -> {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
