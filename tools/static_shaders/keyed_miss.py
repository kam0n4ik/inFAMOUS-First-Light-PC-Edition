"""Why predicted keys miss real keyed modules: the nearest predicted key of the same program, word diff.
usage: py -I -X utf8 keyed_miss.py <predict.tsv> <material_db.json> <shaders.pack>..."""
import collections, json, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa
RTW = 82  # RuntimeParams words
pred = collections.defaultdict(list)
for line in open(sys.argv[1], encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) != 8 or len(f[6]) % 2:
        continue
    h = f[0]
    ident = (int(h[:16], 16), int(h[16:], 16), int(f[1]), int(f[2]))
    pred[ident].append(tuple(int(x, 16) for x in f[7].split(',') if x))
pset = {(i, w) for i, ws in pred.items() for w in ws}
db = json.load(open(sys.argv[2]))
passfh = {int(q[r], 16) for p in db['programs'] for q in p['passes'] for r in ('vs', 'ps') if q.get(r)}
cat = collections.Counter(); shown = 0
for path in sys.argv[3:]:
    for typ, b in pa.records(path):
        if typ != 'M':
            continue
        r = pa.Reader(b)
        ident = (r.u64(), r.u64(), r.u32(), r.u8()); layout = r.u8(); fh = r.u64()
        r.take(r.u32()); r.take(8); r.take(64); key = tuple(r.words())
        if layout or fh not in passfh or (ident, key) in pset:
            continue
        cands = pred.get(ident, [])
        if not cands:
            cat['no candidate'] += 1
            continue
        best = min(cands, key=lambda w: sum(a != b for a, b in zip(w, key)) + 8 * abs(len(w) - len(key)))
        d = [i for i in range(min(len(best), len(key))) if best[i] != key[i]]
        tag = f'len {len(key)}/{len(best)}' if len(best) != len(key) else ('runtime' if d[0] < RTW else f'tail@{d[0]-RTW}..')
        cat[('vs' if ident[3] == 1 else 'ps', tag if not tag.startswith('tail') else 'tail')] += 1
        if shown < 14:
            shown += 1
            print('stage', ident[3], 'len', len(key), len(best), 'diff', [(i - RTW, hex(key[i]), hex(best[i])) for i in d[:5]])
print(cat)
