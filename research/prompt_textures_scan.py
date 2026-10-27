"""Second Son's button pictures (ui/prompt_textures_table.inc) looked for in First Light's packs: the top level's bytes
of each kind taken from Second Son's pack, searched in every First Light .xpps, each hit checked by the FNV-1a 64 of
the whole top level (KindInfo in ui/prompt_textures.cpp). Prints rows for a First Light table.
    python -I research/prompt_textures_scan.py <ss art\cache> <fl art\cache> <prompt_textures_table.inc> <prompt_textures.cpp>
"""
import mmap
import os
import re
import sys

NEEDLE = 4096


def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def main(ss_dir, fl_dir, table, src):
    info = {m[0]: (int(m[1]), int(m[2]), int(m[3], 16)) for m in
            re.findall(r'case PictureKind::(\w+): return \{(\d+), (\d+), (0x[0-9a-f]+)ull', open(src).read())}
    first = {}
    for pack, off, kind in re.findall(r'\{"([^"]+)", (0x[0-9a-f]+), PictureKind::(\w+)\}', open(table).read()):
        first.setdefault(kind, (pack, int(off, 16)))
    needles = {}
    for kind, (pack, off) in first.items():
        w, h, want = info[kind]
        with open(os.path.join(ss_dir, pack), 'rb') as f:
            f.seek(off)
            top = f.read(w * h)
        ok = fnv(top) == want
        print(f'# {kind}: SS {pack}+{off:#x} {w}x{h} fnv ok {ok}', flush=True)
        if ok:
            needles[kind] = (top[:NEEDLE], w * h, want)
    for name in sorted(os.listdir(fl_dir)):
        if not name.endswith('.xpps'):
            continue
        with open(os.path.join(fl_dir, name), 'rb') as f:
            if os.fstat(f.fileno()).st_size == 0:
                continue
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            for kind, (needle, size, want) in needles.items():
                p = m.find(needle)
                while p != -1:
                    if p + size <= len(m) and fnv(m[p:p + size]) == want:
                        print(f'{{"{name}", {p:#x}, PictureKind::{kind}}},', flush=True)
                    p = m.find(needle, p + 1)
            m.close()


if __name__ == '__main__':
    main(*sys.argv[1:])
