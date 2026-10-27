"""Second pass of sigmap.py: the functions with no exact masked match, found by their instruction sequence.

First Light's eboot is the same engine rebuilt: functions keep their order, but field offsets and constants inside
them may change. For each unmatched Second Son function its expected First Light RVA is interpolated between the
unique matches of sigmap.tsv (anchors); every 16-aligned call target within a window around it is a candidate, and
the candidate whose mnemonic sequence is the most similar (difflib ratio) wins. A result needs a human check (the
ratio and the runner-up are printed) before a site table pins it.

    python -I tools/sigmap_fuzzy.py <Second Son eboot.bin> <First Light eboot.bin> <sigmap.tsv> <out.tsv> [ss_rva...]
"""
import bisect
import difflib
import os
import struct
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sigmap import text_of  # noqa: E402

WINDOW = 0x60000


def mnemonics(md, code, at, limit):
    out = []
    for ins in md.disasm(code[at:at + limit], at):
        out.append(ins.mnemonic)
    return out


def call_targets(text):
    t = set()
    i = text.find(b'\xe8')
    while i != -1 and i + 5 <= len(text):
        tgt = i + 5 + struct.unpack_from('<i', text, i + 1)[0]
        if 0 <= tgt < len(text) and tgt % 16 == 0:
            t.add(tgt)
        i = text.find(b'\xe8', i + 1)
    return sorted(t)


def main(ss_path, fl_path, sigmap, out, *only):
    ss, fl = text_of(ss_path), text_of(fl_path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    anchors, todo = [], []
    for line in open(sigmap, encoding='utf-8').read().splitlines()[1:]:
        c = line.split('\t')
        rva, size = int(c[0], 16), int(c[1], 16)
        if c[3].startswith('unique'):
            anchors.append((rva, int(c[4], 16)))
        elif not only or c[0] in only:
            todo.append((rva, size, c[2]))
    anchors.sort()
    keys = [a[0] for a in anchors]
    starts = call_targets(fl)
    rows = []
    for rva, size, where in todo:
        i = bisect.bisect_left(keys, rva)
        lo = anchors[max(i - 1, 0)]
        hi = anchors[min(i, len(anchors) - 1)]
        est = lo[1] + (rva - lo[0]) if hi[0] == lo[0] else \
            lo[1] + (rva - lo[0]) * (hi[1] - lo[1]) // (hi[0] - lo[0])
        ref = mnemonics(md, ss, rva, size)
        sm = difflib.SequenceMatcher(autojunk=False)
        sm.set_seq2(ref)
        scored = []
        a, b = bisect.bisect_left(starts, est - WINDOW), bisect.bisect_right(starts, est + WINDOW)
        for s in starts[a:b]:
            cand = mnemonics(md, fl, s, size + size // 4)[:len(ref) + len(ref) // 8]
            sm.set_seq1(cand)
            if sm.real_quick_ratio() < 0.6 or sm.quick_ratio() < 0.6:
                continue
            scored.append((sm.ratio(), s))
        scored.sort(reverse=True)
        best = scored[0] if scored else (0.0, 0)
        second = scored[1] if len(scored) > 1 else (0.0, 0)
        rows.append((rva, size, where, est, best, second))
        print(f'{rva:#x} {where}: est {est:#x} best {best[1]:#x} ({best[0]:.3f}) next {second[1]:#x} '
              f'({second[0]:.3f})', flush=True)
    with open(out, 'w', encoding='utf-8') as o:
        o.write('ss_rva\tsize\tsource\test_fl\tfl_rva\tratio\tnext_rva\tnext_ratio\n')
        for rva, size, where, est, best, second in rows:
            o.write(f'{rva:#x}\t{size:#x}\t{where}\t{est:#x}\t{best[1]:#x}\t{best[0]:.3f}\t{second[1]:#x}\t'
                    f'{second[0]:.3f}\n')


if __name__ == '__main__':
    main(*sys.argv[1:])
