"""Scores the predicted pipelines (logs/static_predict_pipelines.tsv, static_predictor.cpp PredictPipelines) against
real runs' caches. A cached graphics pipeline of a pass PS is matched by its stages' (program identity, specialization
key) - found through its own cache's module records by SPIR-V, so another cache generation compares too - and its
GraphicsPipelineState bytes:
  exact:  both stages and the state predicted together -> created before the game needs it
  stages: both stages predicted together (another state)
  new:    the same for the pipelines absent from the other scenes' caches (a first visit)
usage: py -I -X utf8 compare_pipes.py <pipelines.tsv> <material_db.json> name=<shaders.pack>..."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa


def ident(h, n):
    return (int(h[:16], 16), int(h[16:], 16), int(n))


def words(s):
    return tuple(int(x, 16) for x in s.split(',') if x)


exact, stages = set(), set()
for line in open(sys.argv[1], encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) != 7:
        continue
    vk, pk = words(f[2]), words(f[5])
    st = bytes.fromhex(f[6])
    # a stage as a cache names it: keyed (identity, key) or a layout (identity, runtime = the key's first 82 words)
    for v in (('K', ident(f[0], f[1]), vk), ('L', ident(f[0], f[1]), vk[:82])):
        for p in (('K', ident(f[3], f[4]), pk), ('L', ident(f[3], f[4]), pk[:82])):
            stages.add((v, p))
            exact.add((v, p, st))
db = json.load(open(sys.argv[2]))
passfh = {int(q['ps'], 16) for pr in db['programs'] for q in pr['passes'] if q.get('ps')}
print(f'predicted: {len(exact) // 4} pipelines, {len(stages) // 4} stage pairs')


def pipes(path):
    mods = collections.defaultdict(set)
    fh_of = {}
    raw = []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            i = (r.u64(), r.u64(), r.u32()); r.u8(); r.u8()
            fh = r.u64(); rt = r.take(r.u32()); r.take(8); r.take(64)
            key = tuple(r.words()); r.u64()
            sp = (r.u64(), r.u64())
            mods[sp].add(('K', i, key) if key else ('L', i, struct.unpack('<82I', rt)))
            fh_of[sp] = fh
        elif typ == 'L':
            comp, hp = r.u8(), r.u8(); vs = (r.u64(), r.u64()); ps = (r.u64(), r.u64())
            st = r.take(r.u32())
            if not comp and hp:
                raw.append((vs, ps, bytes(st)))
    out = []
    for vs, ps, st in raw:
        if fh_of.get(ps) not in passfh:
            continue
        out.append(({(v, p) for v in mods[vs] for p in mods[ps]}, st))
    return out


scenes = {}
for a in sys.argv[3:]:
    n, p = a.split('=', 1)
    scenes[n] = pipes(p)
for x, pl in scenes.items():
    others = {(pair, st) for n, o in scenes.items() if n != x for pairs, st in o for pair in pairs}
    c = collections.Counter()
    for pairs, st in pl:
        new = not any((pair, st) in others for pair in pairs)
        for scope in ('all', 'new') if new else ('all',):
            c[scope] += 1
            if any((v, p, st) in exact for v, p in pairs):
                c[scope + ' exact'] += 1
            elif any(pair in stages for pair in pairs):
                c[scope + ' stages only'] += 1
    print(x, ', '.join(f'{k} {v}' for k, v in sorted(c.items())))
