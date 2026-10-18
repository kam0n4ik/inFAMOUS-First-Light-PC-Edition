"""Hold-out of the CS descriptor prediction: buffer strides from the code (gcn_buf.buffer_strides), the rest by the
same slot list / the slot in the other scenes' dispatches (cs_slots.py). A new program counts when every slot's form is
right."""
import collections, sys
sys.path.insert(0, r"E:\research\rea\q")
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa
from gcn_buf import buffer_strides
sys.argv = sys.argv[:1]
exec(open(r'E:\research\rea\q\cs_slots.py', encoding='utf-8').read().split("for x, ms in S.items():")[0])


def program_code(path):
    out = {}
    for typ, b in pa.records(path):
        if typ == 'P':
            r = pa.Reader(b)
            ident = (r.u64(), r.u64(), r.u32()); r.u8(); r.u64()
            out[ident] = bytes(r.take(4 * r.u32()))
    return out


codes = {s: program_code(base % s) for s in S}
for x, ms in S.items():
    rest = [m for n, o in S.items() if n != x for m in o]; ids = {m['id'] for m in rest}
    by_sig = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    by_slot = collections.defaultdict(collections.Counter)
    for m in rest:
        sl, d = descs(m)
        if d is None: continue
        for s, f in d.items():
            by_sig[sl][s][f] += 1; by_slot[(s[0], s[1], s[3])][f] += 1
    c = collections.Counter(); why = collections.Counter()
    for m in ms:
        if m['id'] in ids or not m['layout']: continue
        sl, d = descs(m)
        if d is None: continue
        st = buffer_strides(codes[x][m['id']], sl) if m['id'] in codes[x] else {}
        c['new programs'] += 1; ok = True
        for s, f in d.items():
            cand = by_sig[sl].get(s) if sl in by_sig else None
            if not cand: cand = by_slot.get((s[0], s[1], s[3]))
            top = cand.most_common(1)[0][0] if cand else None
            src = 'tmpl'
            if f[0] == 'b' and s[0] == 4 and s[2] in st:  # (read-only buffers: the template's - static_predictor.cpp)
                top = ('b', st[s[2]]) + (tuple(top[2:]) if top else (0, 160260)); src = 'code'
            hit = top == f
            c['slots'] += 1; c['slot hit'] += hit; ok &= hit
            if not hit: why[(s[0], f[0], src, 'stride' if top and top[:2] != f[:2] else 'fmt')] += 1
        c['all slots hit'] += ok
    print(x, dict(c), dict(why))
