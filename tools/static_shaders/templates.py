"""Descriptor templates for the static predictor: what the engine binds for a program's NAMED (global) resources,
from the binding log of earlier runs (the engine's render targets, constant buffers - the same for every draw that
names them). Material textures are not here: they come from the packs (material_db.py).
  tex <name> <8 hex dwords> <count> <distinct formats>     global texture by name (most frequent T#)
  rbuf <name> <4 hex dwords> <count> <distinct key forms>  buffer bound as a resource (usage type 0/4, 4 dwords),
                                                           named by the program's texture records (most frequent V#)
  cb <4 hex dwords> <count> <distinct key forms>           constant buffer (usage type 2; never named by a pass)
  smp <4 hex dwords> <count>                               samplers, most frequent first (key: dword0 bits)
usage: py -I -X utf8 templates.py <material_db.json> <pack_shaders.json> <bindings.jsonl> <out.txt>"""
import collections, json, sys

db = json.load(open(sys.argv[1]))
idx = json.load(open(sys.argv[2]))
names = collections.defaultdict(lambda: collections.defaultdict(set))  # fh -> (kind, api) -> names
for p in db['programs']:
    for q in p['passes']:
        for role, stage in (('vs', 0), ('ps', 3)):
            h = q.get(role)
            if not h:
                continue
            for st, api, rid, nm, mi in q['tex']:
                if st == stage and mi < 0:
                    names[h][('t', api)].add(nm)
            for st, api, rid, nm, mi in q['buf']:
                if st == stage and mi < 0:
                    names[h][('b', api)].add(nm)
tex = collections.defaultdict(collections.Counter)
buf = collections.defaultdict(collections.Counter)
smp = collections.Counter()
cb = collections.Counter()
for line in open(sys.argv[3], encoding='utf-8'):
    try:
        m = json.loads(line)
    except json.JSONDecodeError:
        continue
    sh = idx.get(m['fh'])
    for s in m.get('s', []):
        if s[0] == 0:
            smp[tuple(s[1])] += 1
    if not sh or m['fh'] not in names:
        continue
    r2a = {}
    for ut, api, reg, fl in sh['slots']:
        r2a.setdefault((ut, reg), api)
    nm = names[m['fh']]
    for st, summ, mask, offs, raw in m['i']:
        if st == 0 and offs:
            api = r2a.get((0, offs[0]))
            for n in nm.get(('t', api), ()):
                tex[n][tuple(raw)] += 1
    for b in m['b']:
        st, kind, offs, raw = b[0], b[1], b[4], b[5]
        if st != 0 or kind != 0 or not offs:
            continue
        if (2, offs[0]) in r2a:
            cb[tuple(raw)] += 1
        for ut in (0, 4):
            api = r2a.get((ut, offs[0]))
            for n in nm.get(('t', api), ()) if api is not None else ():
                buf[n][tuple(raw)] += 1


def tfmt(r):
    return ((r[1] >> 20) & 0x3f, (r[1] >> 26) & 0xf, r[3] >> 28)


def bform(r):
    return (r[1] >> 16) & 0x3fff, r[3] & 0x7ffff


with open(sys.argv[4], 'w', encoding='utf-8') as f:
    for n, c in sorted(tex.items()):
        raw, k = c.most_common(1)[0]
        f.write(f"tex {n} {' '.join(f'{x:08x}' for x in raw)} {sum(c.values())} {len({tfmt(r) for r in c})}\n")
    for n, c in sorted(buf.items()):
        raw, k = c.most_common(1)[0]
        f.write(f"rbuf {n} {' '.join(f'{x:08x}' for x in raw)} {sum(c.values())} {len({bform(r) for r in c})}\n")
    for raw, k in cb.most_common(1):
        f.write(f"cb {' '.join(f'{x:08x}' for x in raw)} {k} {len({bform(r) for r in cb})}\n")
    for raw, k in smp.most_common(16):
        f.write(f"smp {' '.join(f'{x:08x}' for x in raw)} {k}\n")
print('global textures', len(tex), 'one format', sum(1 for c in tex.values() if len({tfmt(r) for r in c}) == 1))
print('constant buffer forms', len({bform(r) for r in cb}), 'resource buffers', len(buf), 'one key form', sum(1 for c in buf.values() if len({bform(r) for r in c}) == 1))
print('sampler raws', len(smp), 'force_unnorm/degamma bits set:',
      sum(k for r, k in smp.items() if (r[0] >> 15) & 1), sum(k for r, k in smp.items() if (r[0] >> 20) & 1),
      'of', sum(smp.values()))
