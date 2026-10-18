"""Checks the static material database (material_db.py) against the binding log of earlier runs (a hold-out: the log is
only read to score, never to fill the prediction).

For every logged pixel/vertex shader module whose footer hash is a pass shader of some program object:
  image at a direct input usage slot (offsets[0] == startRegister, usage type 0) -> api slot
  api slot -> program texture entry (stage, api) -> material texture i  -> formats of instance textures i (static)
                                                 -> global (name hash)  -> one format per name? (consistency)
usage: py -I -X utf8 validate_material.py <material_db.json> <pack_shaders.json> <bindings.jsonl>
"""
import collections
import json
import sys

db = json.load(open(sys.argv[1]))
idx = json.load(open(sys.argv[2]))


def fi(raw):
    return ((raw[1] >> 20) & 0x3F, (raw[1] >> 26) & 0xF, raw[3] >> 28)


# per program: material texture i -> formats over its instances
inst_fmts = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
cat_of = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
for inst in db['instances']:
    key = (inst['pack'], inst['x'])
    for i, (tid, cat, f) in enumerate(inst['tex']):
        inst_fmts[key][i][tuple(f[:3]) if f else None] += 1
        cat_of[key][i][cat] += 1

# shader hash -> list of (program key, pass tex entries)
by_shader = collections.defaultdict(list)
stage_hits = collections.Counter()
for p in db['programs']:
    key = (p['pack'], p['x'])
    for ps in p['passes']:
        for role in ('vs', 'ps'):
            h = ps[role]
            if h:
                by_shader[h].append((key, role, ps['tex'], ps['buf']))
print(f"programs {len(db['programs'])}, instances {len(db['instances'])}, shaders referenced {len(by_shader)}")

# which pass stage code is which hardware stage: api slots of texture entries vs the shader's usage-0 api slots
for h, uses in by_shader.items():
    sh = idx.get(h)
    if not sh:
        continue
    apis = {s[1] for s in sh['slots'] if s[0] == 0}
    for key, role, tex, buf in uses:
        for st, api, rid, nm, mi in tex:
            stage_hits[(role, st, api in apis)] += 1
print('stage code vs shader usage-0 api slots (role, stage, api present):', dict(sorted(stage_hits.items())))

res = collections.Counter()
glob = collections.defaultdict(collections.Counter)
mods = 0
for line in open(sys.argv[3], encoding='utf-8'):
    try:
        m = json.loads(line)
    except json.JSONDecodeError:
        continue
    uses = by_shader.get(m['fh'])
    sh = idx.get(m['fh'])
    if not uses or not sh:
        res['module not a material pass shader'] += 1
        continue
    mods += 1
    reg2api = {}
    for ut, api, reg, fl in sh['slots']:
        if ut == 0:
            reg2api.setdefault(reg, api)
    for st, summ, mask, offs, raw in m['i']:
        if st != 0:
            continue
        api = reg2api.get(offs[0]) if offs else None
        if api is None:
            res['image: not a direct slot'] += 1
            continue
        f = fi(raw)
        mat_sets, names = [], set()
        for key, role, tex, buf in uses:
            for s, a, rid, nm, mi in tex:
                if a != api:
                    continue
                if mi >= 0:
                    mat_sets.append(inst_fmts[key][mi])
                else:
                    names.add(nm)
        if mat_sets:
            allf = collections.Counter()
            for c in mat_sets:
                allf.update(c)
            if f in allf:
                res['material image: format among instances'] += 1
                if len([k for k in allf if k]) == 1:
                    res['material image: single format -> exact'] += 1
            elif not allf:
                res['material image: program without instances'] += 1
            else:
                res['material image: format NOT among instances'] += 1
        elif names:
            res['global image'] += 1
            for nm in names:
                glob[nm][f] += 1
        else:
            res['image api slot not in any pass'] += 1
print('modules matched', mods)
for k, v in sorted(res.items()):
    print(f"  {k}: {v}")
cons = sum(1 for c in glob.values() if len(c) == 1)
print(f"global texture names seen {len(glob)}, with one format {cons}")
for nm, c in sorted(glob.items(), key=lambda kv: -sum(kv[1].values()))[:25]:
    print('   ', nm, dict(c))
