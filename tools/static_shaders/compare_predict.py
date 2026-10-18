"""Scores the static prediction (logs/static_predict_modules.tsv, static_predictor.cpp) against real runs' caches.
For each scene cache: its modules of pass shaders (and of all programs) -
  layout: (identity, RuntimeParams bytes) predicted -> the draw needs no first translation
  keyed:  (identity, key words) predicted -> the draw's specialization is ready
  new:    the same, counting only modules absent from the other scenes' caches (a first visit)
usage: py -I -X utf8 compare_predict.py <predict.tsv> <material_db.json> name=<shaders.pack>..."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa

pred_layout, pred_key, pred_ids = set(), set(), set()
for line in open(sys.argv[1], encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) != 8 or len(f[6]) % 2:
        continue  # (a line cut by a crashed run)
    h = f[0]
    ident = (int(h[:16], 16), int(h[16:], 16), int(f[1]), int(f[2]))
    rt = bytes.fromhex(f[6])
    words = tuple(int(x, 16) for x in f[7].split(',') if x)
    pred_ids.add(ident)
    pred_layout.add((ident, rt))
    pred_key.add((ident, words))
db = json.load(open(sys.argv[2]))
passfh = set()
for p in db['programs']:
    for q in p['passes']:
        for r in ('vs', 'ps'):
            if q.get(r):
                passfh.add(int(q[r], 16))
print(f'predicted: {len(pred_ids)} programs, {len(pred_layout)} (program, runtime), {len(pred_key)} keys')


def modules(path):
    out = []
    for typ, b in pa.records(path):
        if typ != 'M':
            continue
        r = pa.Reader(b)
        ident = (r.u64(), r.u64(), r.u32(), r.u8())
        layout = r.u8()
        fh = r.u64()
        rt = r.take(r.u32())
        r.take(8); r.take(64)
        key = tuple(r.words())
        out.append((ident, layout, fh, bytes(rt), key))
    return out


scenes = {}
for a in sys.argv[3:]:
    n, p = a.split('=', 1)
    scenes[n] = modules(p)
for x, mods in scenes.items():
    others = {m[0] for n, ms in scenes.items() if n != x for m in ms}
    c = collections.Counter()
    for ident, layout, fh, rt, key in mods:
        if fh not in passfh:
            continue
        for scope in ('all', 'new') if ident not in others else ('all',):
            kind = 'layout' if layout else 'keyed'
            c[(scope, kind, 'total')] += 1
            c[(scope, kind, 'program predicted')] += ident in pred_ids
            if layout:
                c[(scope, kind, 'HIT (program, runtime)')] += (ident, rt) in pred_layout
            else:
                c[(scope, kind, 'HIT key')] += (ident, key) in pred_key
                c[(scope, kind, '(program, runtime) predicted')] += (ident, rt) in pred_layout
    print(x)
    for k in sorted(c):
        print('   ', ' / '.join(k), c[k])
