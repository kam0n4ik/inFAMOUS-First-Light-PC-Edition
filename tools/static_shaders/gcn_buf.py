"""A minimal GCN (Sea Islands) instruction walker: instruction lengths, MUBUF/MTBUF accesses (op, offset, idxen, offen,
srsrc), SMRD loads (sdst, sbase, offset) - enough to tie a buffer access to the user data slot its V# came from."""
import struct

MUBUF_BYTES = {0: 4, 1: 8, 2: 12, 3: 16, 4: 4, 5: 8, 6: 12, 7: 16, 8: 1, 9: 1, 10: 2, 11: 2, 12: 4, 13: 8, 14: 16,
               15: 12, 24: 1, 26: 2, 28: 4, 29: 8, 30: 16, 31: 12}


def walk(code):
    """code: bytes from the first instruction. Yields (kind, fields) until s_endpgm."""
    n = len(code) // 4
    w = struct.unpack(f'<{n}I', code[:n * 4])
    i = 0
    while i < n:
        d = w[i]
        top = d >> 26
        if d >> 31 == 0:  # VOP2 / VOP1 / VOPC
            src0 = d & 0x1FF
            op = (d >> 25) & 0x3F
            lit = src0 == 255 or ((d >> 25) < 0x3E and op in (32, 33))
            i += 2 if lit else 1
            continue
        if d >> 30 == 2:  # scalar ALU
            if d >> 23 == 0x17F:  # SOPP
                if (d >> 16) & 0x7F == 1:
                    return
                i += 1
                continue
            if d >> 23 in (0x17D, 0x17E):  # SOP1 / SOPC
                lit = (d & 0xFF) == 255 or (d >> 23 == 0x17E and ((d >> 8) & 0xFF) == 255)
                i += 2 if lit else 1
                continue
            if d >> 28 == 0xB:  # SOPK
                i += 2 if ((d >> 23) & 0x1F) == 21 else 1
                continue
            lit = (d & 0xFF) == 255 or ((d >> 8) & 0xFF) == 255  # SOP2
            i += 2 if lit else 1
            continue
        if d >> 27 == 0x18:  # SMRD
            imm, off = (d >> 8) & 1, d & 0xFF
            yield 'smrd', {'op': (d >> 22) & 0x1F, 'sdst': (d >> 15) & 0x7F, 'sbase': ((d >> 9) & 0x3F) * 2,
                           'imm': imm, 'off': off}
            i += 2 if (not imm and off == 255) else 1
            continue
        if top == 0x32:  # VINTRP
            i += 1
            continue
        if i + 1 >= n:
            return
        e = w[i + 1]
        if top == 0x38:  # MUBUF
            yield 'buf', {'op': (d >> 18) & 0x7F, 'off': d & 0xFFF, 'offen': (d >> 12) & 1, 'idxen': (d >> 13) & 1,
                          'srsrc': ((e >> 16) & 0x1F) * 4, 'tbuf': 0}
        elif top == 0x3A:  # MTBUF
            yield 'buf', {'op': (d >> 16) & 7, 'off': d & 0xFFF, 'offen': (d >> 12) & 1, 'idxen': (d >> 13) & 1,
                          'srsrc': ((e >> 16) & 0x1F) * 4, 'tbuf': 1, 'nfmt': (d >> 23) & 7, 'dfmt': (d >> 19) & 0xF}
        i += 2


def access_bytes(f):
    if f['tbuf']:
        return 4 * ((f['op'] & 3) + 1)
    return MUBUF_BYTES.get(f['op'], 8 if f['op'] >= 80 else 4)


def buffer_strides(code, slots):
    """slot register -> the stride its indexed accesses imply (cs_stride.py: the largest offset + size, rounded up to
    16 when an access is 16 bytes or more or an offset is not 16-aligned); slots without an indexed access absent."""
    eud = [reg for t, api, reg, fl in slots if t == 27]
    reg_of = {}
    for t, api, reg, fl in slots:
        if t in (0, 2, 4) and not fl & 1 and reg < 16:
            reg_of[reg] = reg
    acc = {}
    for kind, f in walk(code):
        if kind == 'smrd' and eud and f['sbase'] == eud[0] and f['imm'] and f['op'] in (1, 2, 3):
            reg_of[f['sdst']] = 16 + f['off']
        elif kind == 'buf' and f['idxen']:
            s = reg_of.get(f['srsrc'])
            if s is None:
                continue
            b = access_bytes(f)
            a = acc.setdefault(s, [0, 0, False])
            a[0] = max(a[0], f['off'] + b)
            a[1] = max(a[1], b)
            a[2] |= f['off'] % 16 != 0
    out = {}
    for s, (mx, big, unaligned) in acc.items():
        out[s] = (mx + 15) // 16 * 16 if (big >= 16 or unaligned) else mx
    return out
