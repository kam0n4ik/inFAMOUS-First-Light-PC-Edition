"""The prediction's modules against a cache's modules with their keys computed (SSPC_STATIC_KEYS ->
logs\cache_module_keys.tsv): per stage and layout/keyed, how many (program, key) were predicted exactly, and for a
miss of a predicted program which key words differ (CS: the descriptor part).
usage: py -I -X utf8 compare_keys.py <predict.tsv> <cache_module_keys.tsv>"""
import collections, sys
RTW = 82
pred = collections.defaultdict(set)
for line in open(sys.argv[1], encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) != 8 or len(f[6]) % 2:
        continue
    pred[(f[0], f[1], f[2])].add(tuple(int(x, 16) for x in f[7].split(',') if x))
c = collections.Counter(); why = collections.Counter()
for line in open(sys.argv[2], encoding='utf-8'):
    h, n, st, lay, kw = line.rstrip('\n').split('\t')
    key = tuple(int(x, 16) for x in kw.split(',') if x)
    name = {'0': 'ps', '1': 'vs', '6': 'cs'}.get(st, st)
    tag = f"{name} {'layout' if lay == '1' else 'keyed'}"
    ks = pred.get((h, n, st))
    c[tag] += 1
    if not ks:
        c[tag + ' program not predicted'] += 1
        continue
    if key in ks:
        c[tag + ' HIT'] += 1
        continue
    best = min(ks, key=lambda k: sum(a != b for a, b in zip(k, key)) + 8 * abs(len(k) - len(key)))
    d = [i for i in range(min(len(best), len(key))) if best[i] != key[i]]
    why[(name, 'len' if len(best) != len(key) else 'runtime' if d and d[0] < RTW else 'descriptors')] += 1
for k, v in sorted(c.items()):
    print(v, k)
print(dict(why))
