"""The icon atlas of First Light's language tables against Second Son's (ui/prompt_icons.cpp Locate): its FNV-1a 64 in
every language, the glyph table, and which pixels differ (detiled as the runtime does).
    python -I research/prompt_atlas_check.py <ss lang_*_text.xpps> <fl lang_*_text.xpps>...
"""
import struct
import sys

TEX_ID = bytes([0x29, 0xc1, 0x1c, 0x0c, 0xac, 0x5b, 0x95, 0xed])
DESC = (0x00a00000, 0x007fc1ff, 0x94d00fac, 0x003fe000)
SIDE = 512
SIZE = SIDE * SIDE * 4


def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def locate(f):
    header = struct.unpack_from('<I', f, 0x28)[0]
    p = f.find(TEX_ID)
    while p != -1:
        t = p - 0x20
        if p >= 0x20 and struct.unpack_from('<4I', f, t + 4) == DESC:
            data = header + struct.unpack_from('<I', f, t)[0]
            break
        p = f.find(TEX_ID, p + 1)
    else:
        return None, None
    cells = []
    q = f.find(b'\x80\xf0')
    while q != -1:
        if struct.unpack_from('<H', f, q + 6)[0] == 50 and struct.unpack_from('<H', f, q + 8)[0] == 50 and \
                struct.unpack_from('<H', f, q + 32)[0] == 0xF081:
            last = 0
            r = q
            while r + 32 <= len(f):
                c = struct.unpack_from('<5H', f, r)
                if c[0] < 0xF000 or c[0] >= 0xF200 or c[0] <= last or c[3] == 0 or c[4] == 0 or \
                        c[1] + c[3] > SIDE or c[2] + c[4] > SIDE:
                    break
                cells.append(c)
                last = c[0]
                r += 32
            break
        q = f.find(b'\x80\xf0', q + 1)
    return data, cells


MORTON = [(x, y) for i in range(64) for x, y in [(sum(((i >> (2 * b)) & 1) << b for b in range(3)),
                                                  sum(((i >> (2 * b + 1)) & 1) << b for b in range(3)))]]


def detile(t):
    out = bytearray(SIZE)
    i = 0
    for ty in range(SIDE // 8):
        for tx in range(SIDE // 8):
            for k in range(64):
                mx, my = MORTON[k]
                row = SIDE - 1 - (ty * 8 + my)
                x = tx * 8 + mx
                out[(row * SIDE + x) * 4:(row * SIDE + x) * 4 + 4] = t[i:i + 4]
                i += 4
    return out


def main(ss_path, *fl_paths):
    ss = open(ss_path, 'rb').read()
    sd, sc = locate(ss)
    sa = ss[sd:sd + SIZE]
    print(f'SS {ss_path}: atlas fnv {fnv(sa):016x}, {len(sc)} cells')
    for path in fl_paths:
        f = open(path, 'rb').read()
        d, c = locate(f)
        if d is None:
            print(f'FL {path}: no atlas')
            continue
        a = f[d:d + SIZE]
        print(f'FL {path}: atlas fnv {fnv(a):016x}, {len(c)} cells, glyph table == SS: {c == sc}')
        if c != sc:
            sset = {x[0]: x for x in sc}
            for x in c:
                if sset.get(x[0]) != x:
                    print('   cell differs', [hex(v) if i == 0 else v for i, v in enumerate(x)], 'SS', sset.get(x[0]))
    fl = open(fl_paths[0], 'rb').read()
    d, c = locate(fl)
    A, B = detile(sa), detile(fl[d:d + SIZE])
    diff = {(i // 4) for i in range(0, SIZE, 4) if A[i:i + 4] != B[i:i + 4]}
    print(f'differing pixels: {len(diff)}')
    touched = {}
    for code, x, y, w, h in c:
        n = sum(1 for yy in range(y, y + h) for xx in range(x, x + w) if (yy * SIDE + xx) in diff)
        if n:
            touched[code] = n
    print('cells with differing pixels:', {hex(k): v for k, v in sorted(touched.items())})
    inside = set()
    for code, x, y, w, h in c:
        inside |= {yy * SIDE + xx for yy in range(y, y + h) for xx in range(x, x + w)}
    print('differing pixels outside every cell:', len(diff - inside))


if __name__ == '__main__':
    main(*sys.argv[1:])
