"""Joint render state of the material draws, for the static predictor's pipelines: per (pass class, the PS's
COL_FORMAT) the (PS runtime state, VS runtime state, pipeline state) of the cached pipelines, normalized - the
RuntimeParams with the shader's own fields zeroed as state_table.py does, the GraphicsPipelineState without the module
ids, CB_SHADER_MASK, the fragment exports and the targets' export formats (the predictor fills them from the PS struct
and its translation; checked exact on every pass pipeline of three scenes: pipe_shaderfields.py). Appends lines
  pipe <class hex> <col_format hex> <count> <PS RuntimeParams hex> <VS RuntimeParams hex> <GraphicsPipelineState hex>
usage: py -I -X utf8 pipe_table.py <material_db.json> <out.txt (appended)> <shaders.pack>..."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa
ZERO = [(4, 12), (36, 40), (44, 72), (84, 92), (92, 220)]
OWN = [0, 1, 13, 14] + [16 + 5 * t for t in range(8)]  # vs/ps module, cb_shader_mask, mrt_mask, targets' export
db = json.load(open(sys.argv[1]))
cls_of = collections.defaultdict(set)
for p in db['programs']:
    for q in p['passes']:
        if q.get('ps'):
            cls_of[int(q['ps'], 16)].add(q['cls'])
def norm_rt(rt):
    rt = bytearray(rt)
    for a, e in ZERO:
        rt[a:e] = bytes(e - a)
    return bytes(rt)
c = collections.Counter()
for path in sys.argv[3:]:
    mod, pipes = {}, []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            r.u64(); r.u64(); r.u32(); r.u8(); r.u8()
            fh = r.u64(); rt = r.take(r.u32()); r.u32(); r.u32(); r.take(64); r.words(); r.u64()
            mod[(r.u64(), r.u64())] = (fh, rt)
        elif typ == 'L':
            comp, hp = r.u8(), r.u8(); vs = (r.u64(), r.u64()); ps = (r.u64(), r.u64())
            st = r.take(r.u32()); fs = r.take(r.u32())
            if not comp and hp:
                pipes.append((vs, ps, st, fs))
    for vs, ps, st, fs in pipes:
        if vs not in mod or ps not in mod:
            continue
        pfh, prt = mod[ps]
        _, vrt = mod[vs]
        if len(st) != 220 or len(prt) != 328 or len(vrt) != 328:
            raise SystemExit('unexpected record sizes')
        w = list(struct.unpack('<55I', st))
        for i in OWN:
            w[i] = 0
        col = struct.unpack_from('<I', fs, 68)[0]
        j = (norm_rt(prt), norm_rt(vrt), struct.pack('<55I', *w))
        for cl in cls_of.get(pfh, ()):
            c[(cl, col, j)] += 1
with open(sys.argv[2], 'a', encoding='utf-8') as f:
    for (cl, col, j), n in sorted(c.items(), key=lambda kv: (kv[0][0], kv[0][1], -kv[1])):
        f.write(f"pipe {cl} {col:x} {n} {j[0].hex()} {j[1].hex()} {j[2].hex()}\n")
print('pipe variants', len(c), 'keys', len({(k[0], k[1]) for k in c}))
