"""Joint render state of a draw: (PS runtime state, VS runtime state, pipeline state) per (pass class, COL_FORMAT),
normalized as state_table.py / pipe_state.py do. Hold-out coverage of the top-K joint triples, and the number of
distinct pipelines per (VS program, PS program).
usage: py -I -X utf8 pipe_joint.py <material_db.json> name=<shaders.pack> ..."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
sys.path.insert(0, r"E:\research\rea\q")
import pack_analyze as pa
ZERO = [(4, 12), (36, 40), (44, 72), (84, 92), (92, 220)]
FIELDS = ['vs_module', 'ps_module', 'prim_type', 'poly_mode', 'provoking_last', 'clip_space', 'depth_clip',
          'depth_clamp', 'z_format', 'stencil_format', 'depth_samples_log2', 'logic_op', 'color_mode', 'cb_shader_mask',
          'mrt_mask'] + [f't{i}.{f}' for i in range(8) for f in ('info', 'export', 'blend', 'mask', 'samples')]
OWN = {'vs_module', 'ps_module', 'cb_shader_mask', 'mrt_mask'} | {f't{i}.export' for i in range(8)}
db = json.load(open(sys.argv[1]))
cls_of = collections.defaultdict(set)
for p in db['programs']:
    for q in p['passes']:
        for role in ('vs', 'ps'):
            if q.get(role):
                cls_of[(role, int(q[role], 16))].add(q['cls'])
def norm_rt(rt):
    rt = bytearray(rt)
    for a, e in ZERO:
        rt[a:e] = bytes(e - a)
    return bytes(rt)
def load(path):
    mod = {}
    pipes = []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            ident = (r.u64(), r.u64(), r.u32(), r.u8()); r.u8()
            fh = r.u64(); rt = r.take(r.u32()); r.u32(); r.u32(); r.take(64); key = r.words(); r.u64()
            sp = (r.u64(), r.u64())
            mod[sp] = (fh, rt, ident, key)
        elif typ == 'L':
            compute, has_ps = r.u8(), r.u8()
            vs = (r.u64(), r.u64()); ps = (r.u64(), r.u64())
            st = r.take(r.u32()); fs = r.take(r.u32()); n = r.u32(); r.take(8 * n)
            if not compute and has_ps:
                pipes.append((vs, ps, st, fs))
    out = []
    for vs, ps, st, fs in pipes:
        if vs not in mod or ps not in mod:
            continue
        vfh, vrt, vid, vkey = mod[vs]; pfh, prt, pid, pkey = mod[ps]
        w = struct.unpack(f'<{len(st) // 4}I', st)
        pn = tuple(0 if FIELDS[i] in OWN else w[i] for i in range(len(w)))
        col = struct.unpack_from('<I', fs, 68)[0]
        for cl in cls_of.get(('ps', pfh), ()):
            out.append({'k': (cl, col), 'j': (norm_rt(prt), norm_rt(vrt), pn), 'pp': (vid, pid)})
            break
    return out
scenes = {}
for a in sys.argv[2:]:
    n, p = a.split('=', 1); scenes[n] = load(p)
for K in (1, 2, 3, 5, 8):
    tot = collections.Counter()
    for x, pl in scenes.items():
        t = collections.defaultdict(collections.Counter)
        for n, ps in scenes.items():
            if n != x:
                for p in ps:
                    t[p['k']][p['j']] += 1
        for p in pl:
            top = [j for j, _ in t[p['k']].most_common(K)]
            tot['all'] += 1
            tot['hit'] += p['j'] in top
            tot['unseen key'] += not t[p['k']]
    print(f'top{K} joint: {tot["hit"]}/{tot["all"]} (class/col unseen {tot["unseen key"]})')
per = collections.Counter()
for pl in scenes.values():
    c = collections.Counter(p['pp'] for p in pl)
    per.update(c.values())
print('pipelines per (VS program, PS program):', sorted(per.items()))
# variants per key across all
allk = collections.defaultdict(collections.Counter)
for pl in scenes.values():
    for p in pl:
        allk[p['k']][p['j']] += 1
print('keys', len(allk), 'joint variants hist', sorted(collections.Counter(len(v) for v in allk.values()).items()))
