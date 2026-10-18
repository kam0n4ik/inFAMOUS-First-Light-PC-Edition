"""Gnmx VS/PS shader structs of every pass element in the KCAP packs (static), keyed by the program's footer hash.
PS struct: dword0 bits 24..31 = input usage slot count, +0x08 SPI_SHADER_PGM_LO (KNLI kind 4) HI RSRC1 RSRC2,
  +0x18 SPI_SHADER_Z_FORMAT, +0x1c SPI_SHADER_COL_FORMAT, +0x20 SPI_PS_INPUT_ENA, +0x24 SPI_PS_INPUT_ADDR,
  +0x28 SPI_PS_IN_CONTROL, +0x2c SPI_BARYC_CNTL, +0x30 DB_SHADER_CONTROL (eboot 0x4468e0 adjusts it at load),
  +0x34 CB_SHADER_MASK, +0x38 input semantic count, +0x3c usage slots (u32 each), then input semantics (u16 each).
VS struct: dword0 bits 24..31 = usage slot count, +0x08 PGM_LO HI RSRC1 RSRC2, +0x18 SPI_VS_OUT_CONFIG,
  +0x1c SPI_SHADER_POS_FORMAT, +0x20 PA_CL_VS_OUT_CNTL, +0x24 u8 input semantic count, u8 export semantic count,
  +0x28 usage slots, then input semantics (u32: semantic, vgpr, size), then export semantics (u16 each).
usage: py -I -X utf8 shader_structs.py <out.json> <game dir>
out: {fh: {"role": "ps"|"vs", "regs": [...], "slots": [...], "in": [...], "exp": [...]}}"""
import json, os, struct, sys
sys.path.insert(0, r"E:\SecondSonPC-rebirth\native\tools\research")
import kcap


def footer(data, hs, code):
    a = hs + code
    if a + 8 > len(data) or struct.unpack_from('<I', data, a)[0] != 0xBEEB03FF:
        return None
    fo = a + (struct.unpack_from('<I', data, a + 4)[0] + 1) * 8
    if fo + 28 > len(data) or data[fo:fo + 7] != b'OrbShdr':
        return None
    return (struct.unpack_from('<I', data, fo + 8)[0] >> 2) & 0xF, struct.unpack_from('<Q', data, fo + 16)[0]


out, conflicts = {}, 0
paths = []
for r, _, fs in os.walk(sys.argv[2]):
    paths += [os.path.join(r, f) for f in fs if f.endswith('.xpps')]
for n, path in enumerate(sorted(paths)):
    data = open(path, 'rb').read()
    try:
        h = kcap.parse(data)
    except Exception:
        continue
    hs = h['header_size']
    dic, links = set(), []
    for s in kcap.segments(data, h):
        if s['pool'] != 2:
            continue
        st = hs + s['offset']
        for tag, p, size in kcap.chunks(data, st, st + s['size']):
            if tag == kcap.TAG_KNLI:
                links += kcap.knli(data, p)
            elif tag == kcap.TAG_DIC:
                dic |= {kcap.u32(data, p + 8 + j * 16) for j in range(kcap.u32(data, p))}
    ptr, k4 = {}, {}
    for k, loc in links:
        if hs + loc + 4 > len(data):
            continue
        v = kcap.u32(data, hs + loc)
        if k == 4:
            k4[loc] = v
        elif k in (0, 3):
            ptr[loc] = v
    seen = set()
    for loc in dic:
        E = loc - 0x20
        for off, role, want in ((0x38, 'vs', 1), (0x40, 'ps', 0)):
            S = ptr.get(E + off)
            if S is None or S in seen or S + 8 not in k4:
                continue
            seen.add(S)
            f = footer(data, hs, k4[S + 8])
            if not f or f[0] != want:
                continue
            u = lambda o: struct.unpack_from('<I', data, hs + S + o)[0]
            nslots = u(0) >> 24
            if role == 'ps':
                regs = [u(o) for o in range(0x10, 0x38, 4)]
                nin = u(0x38)
                so = 0x3c
                ins = list(struct.unpack_from(f'<{nin}H', data, hs + S + so + nslots * 4))
                rec = {'role': 'ps', 'regs': regs, 'slots': [u(so + i * 4) for i in range(nslots)], 'in': ins}
            else:
                regs = [u(o) for o in range(0x10, 0x24, 4)]
                nin, nexp = data[hs + S + 0x24], data[hs + S + 0x25]
                so = 0x28
                io = so + nslots * 4
                ins = [u(io + i * 4) for i in range(nin)]
                exps = list(struct.unpack_from(f'<{nexp}H', data, hs + S + io + nin * 4))
                rec = {'role': 'vs', 'regs': regs, 'slots': [u(so + i * 4) for i in range(nslots)], 'in': ins,
                       'exp': exps, 'w24': u(0x24)}
            key = f"{f[1]:x}"
            if key in out and out[key] != rec:
                conflicts += 1
            out.setdefault(key, rec)
    if (n + 1) % 200 == 0:
        print(n + 1, len(out), conflicts, flush=True)
json.dump(out, open(sys.argv[1], 'w'))
print('done', len(out), 'conflicts', conflicts)
