"""Hold-out: the descriptors of new CS programs predicted from the other scenes' dispatches, slot by slot (the CS
struct's input usage slots in pack_shaders.json; a slot's sharp = flatbuf[start register]):
  sig:  a program with the same slot list (type, api, register, flags) - its sharp at that register
  slot: else the most frequent at (type, api, flags)
Compares the formats the specialization key uses (buffers: stride, word 3 format bits; images: data/num format, type;
samplers: all)."""
import collections, json, sys
sys.path.insert(0, r"E:\research\rea\q")
from cs_flat import cs_modules
idx = json.load(open(r'E:\research\rea\out\pack_shaders.json'))
def form(t, flags, w):
    if t == 1:
        return ('s',) + tuple(w[:4])
    if t in (0, 4) and flags & 1:
        return ('i', (w[1] >> 20) & 0x3F, (w[1] >> 26) & 0xF, w[3] >> 28)
    if t in (0, 2, 4):
        return ('b', (w[1] >> 16) & 0x3FFF, w[1] >> 31, w[3] & 0x3FFFFFFF)
    return None
def slots(fh):
    e = idx.get('%x' % fh)
    return tuple(tuple(s) for s in e['slots']) if e else None
def descs(m):
    sl = slots(m['fh'])
    if sl is None or m['flat'] is None:
        return None, None
    d = {}
    for t, api, reg, fl in sl:
        n = 8 if (t in (0, 4) and fl & 1) else 4
        if t in (0, 1, 2, 4) and reg + n <= len(m['flat']):
            d[(t, api, reg, fl)] = form(t, fl, m['flat'][reg:reg + n])
    return sl, d
base = r'E:/SecondSonPC/logs/p414/m1/cache/%s/CUSA00004/native/d6b7e3be2b07131a/shaders.pack'
S = {s: cs_modules(base % s) for s in ('early', 'beach', 'user1022')}
for x, ms in S.items():
    rest = [m for n, o in S.items() if n != x for m in o]
    ids = {m['id'] for m in rest}
    by_sig = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    by_slot = collections.defaultdict(collections.Counter)
    for m in rest:
        sl, d = descs(m)
        if d is None: continue
        for s, f in d.items():
            by_sig[sl][s][f] += 1
            by_slot[(s[0], s[1], s[3])][f] += 1
    c = collections.Counter()
    for m in ms:
        if m['id'] in ids or not m['layout']:
            continue
        sl, d = descs(m)
        if d is None:
            c['no struct'] += 1; continue
        c['new programs'] += 1
        c['same signature known'] += sl in by_sig
        ok = True
        for s, f in d.items():
            cand = by_sig[sl].get(s) if sl in by_sig else None
            if not cand:
                cand = by_slot.get((s[0], s[1], s[3]))
            hit = bool(cand) and cand.most_common(1)[0][0] == f
            c['slots'] += 1; c['slot hit'] += hit
            ok &= hit
        c['all slots hit'] += ok
    print(x, dict(c))
