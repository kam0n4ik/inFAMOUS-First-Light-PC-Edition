"""Does a pass's class (and its PS's COL_FORMAT) determine the pipeline's fixed-function state? Reads shader caches'
pipeline records ('L') and modules ('M'): the PS of a pipeline by its SPIR-V -> footer hash -> pass class (material db).
The state without the shader's own fields (cb_shader_mask, mrt_mask, the targets' export format) is compared, per
scene X against the rest, with the rest's most frequent state of that (class, COL_FORMAT).
usage: py -I -X utf8 pipe_state.py <material_db.json> name=<shaders.pack> ...
"""
import collections
import json
import struct
import sys

sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa  # noqa: E402

FIELDS = ['vs_module', 'ps_module', 'prim_type', 'poly_mode', 'provoking_last', 'clip_space', 'depth_clip',
          'depth_clamp', 'z_format', 'stencil_format', 'depth_samples_log2', 'logic_op', 'color_mode', 'cb_shader_mask',
          'mrt_mask'] + [f't{i}.{f}' for i in range(8) for f in ('info', 'export', 'blend', 'mask', 'samples')]
SHADER_OWN = {'vs_module', 'ps_module', 'cb_shader_mask', 'mrt_mask'} | {f't{i}.export' for i in range(8)}

db = json.load(open(sys.argv[1]))
cls_of = collections.defaultdict(set)
for p in db['programs']:
    for q in p['passes']:
        for role in ('vs', 'ps'):
            if q.get(role):
                cls_of[(role, int(q[role], 16))].add(q['cls'])


def load(path):
    spirv_fh = {}
    pipes = []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            r.u64(); r.u64(); r.u32(); r.u8(); r.u8()
            fh = r.u64()
            rt = r.take(r.u32())
            r.u32(); r.u32(); r.take(64)
            r.words()
            r.u64()
            sp = (r.u64(), r.u64())
            role = 'ps' if rt[0] == 0 else 'vs' if rt[0] == 1 else 'cs'
            spirv_fh[sp] = (role, fh)
        elif typ == 'L':
            compute, has_ps = r.u8(), r.u8()
            vs = (r.u64(), r.u64())
            ps = (r.u64(), r.u64())
            st = r.take(r.u32())
            fs = r.take(r.u32())
            n = r.u32()
            binds = r.take(8 * n)
            if not compute:
                pipes.append((vs, ps if has_ps else None, st, fs, binds))
    out = []
    for vs, ps, st, fs, binds in pipes:
        w = list(struct.unpack(f'<{len(st) // 4}I', st))
        col = struct.unpack_from('<I', fs, 68)[0] if ps else 0
        norm = tuple(0 if FIELDS[i] in SHADER_OWN else w[i] for i in range(len(w)))
        pfh = spirv_fh.get(ps) if ps else None
        vfh = spirv_fh.get(vs)
        cls = set()
        if pfh:
            cls = cls_of.get(('ps', pfh[1]), set())
        elif vfh:
            cls = cls_of.get(('vs', vfh[1]), set())
        out.append({'cls': cls, 'col': col, 'norm': norm, 'raw': tuple(w), 'ps': ps is not None})
    return out


scenes = {}
for a in sys.argv[2:]:
    n, p = a.split('=', 1)
    scenes[n] = load(p)
    s = scenes[n]
    print(n, 'graphics pipelines', len(s), 'with a pass class', sum(1 for x in s if x['cls']))

diff = collections.Counter()
for x, pl in scenes.items():
    table = collections.defaultdict(collections.Counter)
    for n, ps in scenes.items():
        if n == x:
            continue
        for p in ps:
            for cl in p['cls']:
                table[(cl, p['col'])][p['norm']] += 1
    c = collections.Counter()
    for p in pl:
        if not p['cls']:
            c['no class'] += 1
            continue
        cand = collections.Counter()
        for cl in p['cls']:
            cand.update(table.get((cl, p['col']), {}))
        if not cand:
            c['class/col unseen'] += 1
            continue
        ranked = [k for k, _ in cand.most_common()]
        if ranked[0] == p['norm']:
            c['top1'] += 1
        elif p['norm'] in ranked[:3]:
            c['top3'] += 1
        elif p['norm'] in ranked:
            c['among (%d)' % min(len(ranked), 20)] += 1
        else:
            c['unseen'] += 1
            top = ranked[0]
            diff[' '.join(FIELDS[i] for i in range(len(top)) if top[i] != p['norm'][i])] += 1
    print(x, dict(sorted(c.items())))
for k, v in diff.most_common(20):
    print(v, k)
