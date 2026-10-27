"""Second Son's pinned engine functions found in First Light's eboot (docs/PORTING_PLAN.md, stage D).

Every {rva, size, fnv1a64} GuestFunction of core/src (Second Son's eboot) is disassembled from Second Son's eboot;
the bytes that change when code moves (rel8/rel32 branch targets, RIP-relative disp32) are wildcards, everything else
(opcodes, ModRM, immediates, field offsets) must match. The pattern is searched in First Light's text; a unique hit
gives First Light's RVA and the FNV-1a 64 of its body (what a First Light site table pins).

    python -I tools/sigmap.py <Second Son eboot.bin> <First Light eboot.bin> <core/src> <out.tsv>
"""
import os
import re
import sys

import capstone
from capstone import x86

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps4elf import Image  # noqa: E402

TRIPLE = re.compile(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]{16})ull\s*\}')


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def text_of(path):
    img = Image(path)
    ph = img.phdrs[0]
    assert ph['vaddr'] == 0
    return img.seg(ph)


def mask_of(body, rva, md):
    """True for each byte that must match."""
    keep = [True] * len(body)
    off = 0
    for ins in md.disasm(body, rva):
        off = ins.address - rva
        rel = ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL)
        if rel and ins.operands and ins.operands[0].type == x86.X86_OP_IMM:
            n = ins.imm_size or (4 if ins.size >= 5 else 1)
            for i in range(ins.size - n, ins.size):
                keep[off + i] = False
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                d = ins.disp_offset
                for i in range(d, d + ins.disp_size):
                    keep[off + i] = False
        off += ins.size
    decoded = off
    return keep, decoded


def main(ss_path, fl_path, src_root, out):
    ss, fl = text_of(ss_path), text_of(fl_path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    seen = {}
    for root, _, files in os.walk(src_root):
        for f in files:
            if f.endswith(('.cpp', '.h')):
                p = os.path.join(root, f)
                for m in TRIPLE.finditer(open(p, encoding='utf-8', errors='replace').read()):
                    key = (int(m.group(1), 16), int(m.group(2), 16))
                    seen.setdefault(key, (int(m.group(3), 16), os.path.relpath(p, src_root).replace('\\', '/')))
    rows = []
    for (rva, size), (fnv, where) in sorted(seen.items()):
        body = ss[rva:rva + size]
        if len(body) != size or fnv1a64(body) != fnv:
            rows.append((rva, size, where, 'ss-mismatch', '', '', ''))
            continue
        keep, decoded = mask_of(body, rva, md)
        pat = b''.join(re.escape(bytes([b])) if k else b'.' for b, k in zip(body, keep))
        hits = [h.start() for h in re.finditer(pat, fl, re.DOTALL)][:8]
        fixed = sum(keep)
        state = 'unique' if len(hits) == 1 else ('none' if not hits else f'{len(hits)}-hits')
        if decoded < size:
            state += f' (decoded {decoded}/{size})'
        fl_fnv = f'{fnv1a64(fl[hits[0]:hits[0] + size]):016x}' if len(hits) == 1 else ''
        rows.append((rva, size, where, state, ' '.join(hex(h) for h in hits), fl_fnv, f'{fixed}/{size}'))
    with open(out, 'w', encoding='utf-8') as o:
        o.write('ss_rva\tsize\tsource\tresult\tfl_rva\tfl_fnv1a64\tfixed_bytes\n')
        for r in rows:
            o.write('\t'.join([hex(r[0]), hex(r[1])] + list(r[2:])) + '\n')
    from collections import Counter
    c = Counter(r[3].split(' ')[0] for r in rows)
    print(f'{len(rows)} functions: ' + ', '.join(f'{k} {v}' for k, v in c.most_common()))


if __name__ == '__main__':
    main(*sys.argv[1:5])
