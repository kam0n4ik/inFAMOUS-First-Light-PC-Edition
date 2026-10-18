"""Scores the static prediction's compute modules (logs/static_predict_modules.tsv) against scenes' caches: CS layout
modules by (program, RuntimeParams), keyed ones by (program, key); 'new': programs absent from the other scenes' caches.
(A layout record keeps no key: its exact descriptors are scored by cs_holdout.py on the flattened user data.)
usage: py -I -X utf8 compare_cs.py <predict.tsv> name=<shaders.pack>..."""
import collections, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa
lay, key, ids = set(), set(), set()
for line in open(sys.argv[1], encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) != 8 or len(f[6]) % 2 or f[2] != '6':
        continue
    i = (int(f[0][:16], 16), int(f[0][16:], 16), int(f[1]))
    ids.add(i); lay.add((i, bytes.fromhex(f[6]))); key.add((i, tuple(int(x, 16) for x in f[7].split(',') if x)))
print('predicted CS programs', len(ids), 'modules', len(key))
S = {}
for a in sys.argv[2:]:
    n, p = a.split('=', 1)
    out = []
    for typ, b in pa.records(p):
        if typ != 'M': continue
        r = pa.Reader(b); i = (r.u64(), r.u64(), r.u32()); st = r.u8(); l = r.u8(); r.u64()
        rt = bytes(r.take(r.u32())); r.take(8); r.take(64); k = tuple(r.words())
        if st == 6: out.append((i, l, rt, k))
    S[n] = out
for x, ms in S.items():
    others = {m[0] for n, o in S.items() if n != x for m in o}
    c = collections.Counter()
    for i, l, rt, k in ms:
        for scope in ('all', 'new') if i not in others else ('all',):
            kind = 'layout' if l else 'keyed'
            c[f'{scope} {kind}'] += 1
            c[f'{scope} {kind} program predicted'] += i in ids
            c[f'{scope} {kind} hit'] += ((i, rt) in lay) if l else ((i, k) in key)
    print(x, ', '.join(f'{k} {v}' for k, v in sorted(c.items())))
