"""Static material binding database of every KCAP pack (no game run), as the engine builds it at load (eboot 0x4468e0 ->
0x447190, bound per draw by 0x448640):

program object X (type fca0d3a4 ef031461 at +0x18):
  +0x2c ntex, +0x30 nbuf, +0x80 passes (stride 0xb8), +0x88 npass, +0xc0 buffer records (0x20: +0 u64 name, +8 api),
  +0xd0 texture records (0x10: +0 u64 name, +0xb api), +0xe0 material texture ids (u8 x ntex), +0xf0 buffer ids
pass: +0x20 type, +0x38 VS struct, +0x40 PS struct (Gnmx struct, code link at +8), +0x88/+0x90 buffer pairs
  {u8 stage, u8 id}, +0x98/+0xa0 texture pairs
material instance D (geometry +0x10): +0x20 -> X, +0x30 -> texture bindings (0x30: +0 u64 texture id, +0x18 category),
  +0x38 count (= X ntex; binding i = material texture i)
texture object (0x60): +0x28 T# (8 dwords), +0x48 u64 id

usage: py -I -X utf8 material_db.py <out.json> <game dir>
out: {"textures": n, "programs": [{pack, x, passes: [{type, vs, ps, tex: [[stage, api, id, name, mat_index|-1]],
       buf: [[stage, api, id, name, mat_index|-1]]}]}], "instances": [{pack, d, x, tex: [[texid, category, fmt|null]]}]}
fmt = [dfmt, nfmt, type, dst_sel] of the T#
"""
import json
import os
import struct
import sys

sys.path.insert(0, r"E:\SecondSonPC-rebirth\native\tools\research")
import kcap  # noqa: E402

PROGRAM_TYPE = (0xfca0d3a4, 0xef031461)


def load(path):
    data = open(path, 'rb').read()
    h = kcap.parse(data)
    hs = h['header_size']
    segs = kcap.segments(data, h)
    links, dic = [], {}
    for s in segs:
        if s['pool'] != 2:
            continue
        st = hs + s['offset']
        for tag, p, size in kcap.chunks(data, st, st + s['size']):
            if tag == kcap.TAG_KNLI:
                links += kcap.knli(data, p)
            elif tag == kcap.TAG_DIC:
                for j in range(kcap.u32(data, p)):
                    dic[kcap.u32(data, p + 8 + j * 16)] = kcap.u64(data, p + 16 + j * 16)
    return data, hs, segs, links, dic


def footer(data, hs, code):
    a = hs + code
    if a + 8 > len(data) or struct.unpack_from('<I', data, a)[0] != 0xBEEB03FF:
        return None
    fo = a + (struct.unpack_from('<I', data, a + 4)[0] + 1) * 8
    if fo + 28 > len(data) or data[fo:fo + 7] != b'OrbShdr':
        return None
    bits = struct.unpack_from('<I', data, fo + 8)[0]
    return (bits >> 2) & 0xF, struct.unpack_from('<Q', data, fo + 16)[0]


def scan(path, textures, programs, instances):
    data, hs, segs, links, dic = load(path)
    name = os.path.basename(path)
    ptr, k4 = {}, {}
    for k, loc in links:
        if hs + loc + 4 > len(data):
            continue
        v = struct.unpack_from('<I', data, hs + loc)[0]
        if k in (0, 3):
            ptr[loc] = v
        elif k == 4:
            k4[loc] = v
    end = len(data) - hs

    def u32(o):
        return struct.unpack_from('<I', data, hs + o)[0]

    def u64(o):
        return struct.unpack_from('<Q', data, hs + o)[0]

    def u8(o):
        return data[hs + o]

    # textures: a kind-4 link at obj+0x28 (T# dword0) with an image type in dword3
    for loc in k4:
        if hs + loc + 0x28 > len(data):
            continue
        dw = struct.unpack_from('<8I', data, hs + loc)
        ty = dw[3] >> 28
        if ty < 8:
            continue
        tid = u64(loc + 0x20)       # texture object +0x48 = T# + 0x20
        if tid and tid not in textures:
            textures[tid] = [(dw[1] >> 20) & 0x3f, (dw[1] >> 26) & 0xf, ty, dw[3] & 0xfff]

    def shader(sp):
        if sp is None:
            return None
        for o in range(0, 0x40, 4):
            if sp + o in k4:
                f = footer(data, hs, k4[sp + o])
                if f:
                    return f"{f[1]:x}"
        return None

    def pairs(e, lo, co):
        p = ptr.get(e + lo)
        if p is None:
            return []
        n = u32(e + co)
        if n > 256 or p + n * 2 > end:
            return []
        return [(u8(p + i * 2), u8(p + i * 2 + 1)) for i in range(n)]

    # program objects: owners of pass arrays. A pass element E carries a DIC entry at E+0x20 (the runtime pass class
    # index, eboot 0x4468e0 DAT_00ce56b8[E+0x20]) and a PS struct at E+0x40; X+0x80 -> first pass, X+0x88 npass.
    # The u64 at X+0x18 is a per-program hash, not a class (the character programs share fca0d3a4 ef031461).
    pe = {loc - 0x20 for loc in dic if loc - 0x20 + 0x40 in ptr and ptr[loc - 0x20 + 0x40] + 8 in k4}
    xs = set()
    for loc, t in ptr.items():
        if t in pe:
            n = u32(loc + 8)
            if 0 < n <= 64 and all(t + i * 0xb8 in pe for i in range(n)):
                xs.add(loc - 0x80)
    for s in segs:
        if s['pool'] != 0:
            continue
        o = s['offset'] + 0x18
        hi = s['offset'] + s['size']
        while o + 8 <= hi:
            if struct.unpack_from('<II', data, hs + o) == PROGRAM_TYPE:
                xs.add(o - 0x18)
            o += 8
    for X in sorted(xs):
        try:
            ntex, nbuf, npass = u32(X + 0x2c), u32(X + 0x30), u32(X + 0x88)
            TR, BR, A = ptr.get(X + 0xd0), ptr.get(X + 0xc0), ptr.get(X + 0x80)
            if A is None or npass > 64 or ntex > 64 or nbuf > 64:
                continue
            tids = [u8(ptr[X + 0xe0] + i) for i in range(ntex)] if ntex and X + 0xe0 in ptr else []
            bids = [u8(ptr[X + 0xf0] + i) for i in range(nbuf)] if nbuf and X + 0xf0 in ptr else []
            passes = []
            for p in range(npass):
                E = A + p * 0xb8
                tex = []
                for st, rid in pairs(E, 0x98, 0xa0):
                    api = u8(TR + rid * 0x10 + 0xb) if TR is not None else -1
                    nm = f"{u64(TR + rid * 0x10):016x}" if TR is not None else ''
                    tex.append([st, api, rid, nm, tids.index(rid) if rid in tids else -1])
                buf = []
                for st, rid in pairs(E, 0x88, 0x90):
                    api = u8(BR + rid * 0x20 + 8) if BR is not None else -1
                    nm = f"{u64(BR + rid * 0x20):016x}" if BR is not None else ''
                    buf.append([st, api, rid, nm, bids.index(rid) if rid in bids else -1])
                passes.append({'type': u32(E + 0x20), 'cls': f"{dic.get(E + 0x20, 0):016x}", 'vs': shader(ptr.get(E + 0x38)), 'ps': shader(ptr.get(E + 0x40)),
                               'tex': tex, 'buf': buf})
            programs.append({'pack': name, 'x': X, 'hash': f"{u64(X + 0x18):016x}", 'ntex': ntex, 'nbuf': nbuf, 'passes': passes})
        except (struct.error, IndexError, KeyError):
            continue
    # material instances: +0x20 -> a program object, +0x30 -> bindings, +0x38 = that program's ntex
    for loc, t in ptr.items():
        if t not in xs:
            continue
        D = loc - 0x20
        B = ptr.get(D + 0x30)
        if B is None:
            continue
        try:
            n = u32(D + 0x38)
            if n == 0 or n > 64 or n != u32(t + 0x2c):
                continue
            tex = [[f"{u64(B + i * 0x30):016x}", u64(B + i * 0x30 + 0x18)] for i in range(n)]
            instances.append({'pack': name, 'd': D, 'x': t, 'tex': tex})
        except struct.error:
            continue


def main():
    paths = []
    for r, _, fs in os.walk(sys.argv[2]):
        paths += [os.path.join(r, f) for f in fs if f.endswith('.xpps')]
    textures, programs, instances = {}, [], []
    bad = 0
    for i, p in enumerate(sorted(paths)):
        try:
            scan(p, textures, programs, instances)
        except Exception:
            bad += 1
        if (i + 1) % 200 == 0:
            print(f"{i + 1}/{len(paths)}: textures {len(textures)} programs {len(programs)} instances {len(instances)}",
                  flush=True)
    miss = 0
    for inst in instances:
        for t in inst['tex']:
            f = textures.get(int(t[0], 16))
            t.append(f)
            miss += f is None
    json.dump({'textures': len(textures), 'programs': programs, 'instances': instances}, open(sys.argv[1], 'w'))
    print(f"done: {len(paths)} packs ({bad} unreadable), textures {len(textures)}, programs {len(programs)}, "
          f"instances {len(instances)}, instance textures without a known T# {miss}")


main()
