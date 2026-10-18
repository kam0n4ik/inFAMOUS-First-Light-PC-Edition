"""Does a pass's class determine its stage's RuntimeParams? Uses the shader caches of several scenes
(shaders.pack, read like E:\SecondSonPC\tools\p414\pack_analyze.py) and the static material database.
For every module whose footer hash is a pass VS/PS: key (pass class, role). For each scene X against the rest:
the new layout modules of X (program unseen elsewhere) - is their RuntimeParams (without RSRC1/2, per program) the
one the rest most often has for that (class, role)? An upper bound of statically predicting a new program's
translation inputs from its pass class.
usage: py -I -X utf8 predict_runtime.py <material_db.json> name=<shaders.pack> ...
"""
import collections
import json
import os
import sys
STATE_ONLY = os.environ.get('STATE_ONLY') == '1'

sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa  # noqa: E402

db = json.load(open(sys.argv[1]))
role_of = collections.defaultdict(set)
for p in db['programs']:
    for ps in p['passes']:
        for role in ('vs', 'ps'):
            if ps.get(role):
                role_of[int(ps[role], 16)].add((ps['cls'], ps['type'], role))


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
        w = bytearray(pa.runtime_wo_rsrc(rt))
        if STATE_ONLY:
            # runtime_wo_rsrc drops 8 bytes: RuntimeParams offset o -> o - 8. Shader-own (static) fields zeroed:
            # ps_input_ena..col_format (44..72), ps_input_cntl (92..220), vs_out_cntl (36..40)
            for a, b2 in ((44, 72), (92, 220), (36, 40)):
                w[a - 8:b2 - 8] = bytes(b2 - a)
        out.append((ident, layout, fh, bytes(w)))
    return out


scenes = {}
for a in sys.argv[2:]:
    n, p = a.split('=', 1)
    scenes[n] = modules(p)
for x, mods in scenes.items():
    rest = [m for n, ms in scenes.items() if n != x for m in ms]
    rest_ids = {m[0] for m in rest}
    by_cls = collections.defaultdict(collections.Counter)
    for ident, layout, fh, rt in rest:
        for k in role_of.get(fh, ()):
            by_cls[k][rt] += 1
    c = collections.Counter()
    for ident, layout, fh, rt in mods:
        if ident in rest_ids or not layout:
            continue
        c['new layout modules'] += 1
        ks = role_of.get(fh)
        if not ks:
            c['not a pass shader'] += 1
            continue
        c['pass shader'] += 1
        cand = collections.Counter()
        for k in ks:
            cand.update(by_cls.get(k, {}))
        if not cand:
            c['  class unseen in rest'] += 1
        elif cand.most_common(1)[0][0] == rt:
            c['  top runtime of class == actual'] += 1
        elif rt in cand:
            c['  actual among class runtimes (%d alts)' % min(len(cand), 9)] += 1
        else:
            c['  actual not seen for class'] += 1
    print(x, dict(sorted(c.items())))

# which RuntimeParams fields differ between the actual and the class's top runtime (misses only)
import struct  # noqa: E402
F = (['hw_sw_pad', 'step_rate0', 'step_rate1', 'rect_list', 'quad_list', 'vertex_sgpr_offset', 'instance_sgpr_offset',
      'vs_out_cntl', 'clip_cntl', 'ps_input_ena', 'ps_input_addr', 'num_interp', 'front_face_all_bits',
      'db_shader_control', 'z_format', 'col_format', 'aa_samples_log2', 'dual_source', 'stages_en', 'ps_vs_out_cntl',
      'ps_ucp_enable'] + [f'ps_input_cntl{i}' for i in range(32)] +
     [f't{i}.{f}' for i in range(8) for f in ('bound', 'info', 'minmax')] + ['thr0', 'thr1', 'thr2'])
diff = collections.Counter()
for x, mods in scenes.items():
    rest = [m for n, ms in scenes.items() if n != x for m in ms]
    rest_ids = {m[0] for m in rest}
    by_cls = collections.defaultdict(collections.Counter)
    for ident, layout, fh, rt in rest:
        for k in role_of.get(fh, ()):
            by_cls[k][rt] += 1
    for ident, layout, fh, rt in mods:
        if ident in rest_ids or not layout or not role_of.get(fh):
            continue
        cand = collections.Counter()
        for k in role_of[fh]:
            cand.update(by_cls.get(k, {}))
        if not cand or cand.most_common(1)[0][0] == rt:
            continue
        top = cand.most_common(1)[0][0]
        a, b = struct.unpack(f'<{len(rt) // 4}I', rt), struct.unpack(f'<{len(top) // 4}I', top)
        names = [F[i] if i < len(F) else str(i) for i in range(len(a)) if a[i] != b[i]]
        role = 'ps' if any(k[2] == 'ps' for k in role_of[fh]) else 'vs'
        diff[(role, ' '.join(n for n in names if not n.startswith('ps_input_cntl')) +
              (' ps_input_cntl*' if any(n.startswith('ps_input_cntl') for n in names) else ''))] += 1
for k, v in diff.most_common(25):
    print(v, k)
