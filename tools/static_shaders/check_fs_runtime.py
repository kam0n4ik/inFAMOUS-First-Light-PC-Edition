"""Checks the fragment stage's RuntimeParams of every cached pipeline (shaders.pack 'L' records) against what the
static Gnmx structs (shader_structs.json) give: the PS's own registers, and SPI_PS_INPUT_CNTL computed from the
pair's VS export semantics and PS input semantics. Prints match rates per field and samples of mismatches.
usage: py -I -X utf8 check_fs_runtime.py <shader_structs.json> <shaders.pack>..."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa  # noqa: E402

ss = json.load(open(sys.argv[1]))
OFF = {'ps_input_ena': 44, 'ps_input_addr': 48, 'num_interp': 52, 'front_face_all_bits': 56,
       'db_shader_control': 60, 'z_format': 64, 'col_format': 68}
PSREG = {'z_format': 2, 'col_format': 3, 'ps_input_ena': 4, 'ps_input_addr': 5, 'in_control': 6, 'baryc': 7, 'db': 8}


def cntl_predict(vs, ps):
    exp = {e & 0xff: (e >> 8) & 0x1f for e in vs['exp']}
    out = []
    for s in ps['in']:
        sem = s & 0xff
        if sem in exp:
            v = exp[sem]
        else:
            v = 0x20 | (((s >> 8) & 3) << 8)
        if (s >> 10) & 1:
            v |= 1 << 10
        if (s >> 12) & 1:
            v |= 1 << 5
        out.append(v)
    return out


res = collections.Counter()
samples = collections.defaultdict(list)
for path in sys.argv[2:]:
    spv2fh = {}
    pipes = []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            r.u64(); r.u64(); r.u32(); r.u8(); r.u8()
            fh = r.u64()
            r.take(r.u32()); r.take(8); r.take(64); r.words(); r.u64()
            spv2fh.setdefault((r.u64(), r.u64()), set()).add(fh)
        elif typ == 'L':
            compute, has_ps = r.u8(), r.u8()
            vs = (r.u64(), r.u64()); ps = (r.u64(), r.u64())
            r.take(r.u32())
            fs = r.take(r.u32())
            if not compute and has_ps:
                pipes.append((vs, ps, fs))
    for vs, ps, fs in pipes:
        vfs, pfs = spv2fh.get(vs, set()), spv2fh.get(ps, set())
        Ps = [ss[f"{x:x}"] for x in pfs if f"{x:x}" in ss]
        Vs = [ss[f"{x:x}"] for x in vfs if f"{x:x}" in ss]
        res[f'vs candidates {len(Vs)}'] += 1
        P = Ps[0] if Ps else None
        V = Vs[0] if Vs else None
        if not P:
            res['ps struct unknown'] += 1
            continue
        res['pipelines'] += 1
        g = lambda o: struct.unpack_from('<I', fs, o)[0]
        R = P['regs']
        pred = {'ps_input_ena': R[4], 'ps_input_addr': R[5], 'num_interp': R[6] & 0x3f,
                'front_face_all_bits': (R[7] >> 24) & 1, 'z_format': R[2], 'col_format': R[3],
                'db_shader_control': R[8] & (0x7 | 1 << 7 | 1 << 8 | 1 << 12)}
        for k, o in OFF.items():
            ok = g(o) == pred[k]
            res[f'{k} {"ok" if ok else "BAD"}'] += 1
            if not ok and len(samples[k]) < 4:
                samples[k].append((hex(g(o)), hex(pred[k])))
        actual = [g(92 + 4 * i) for i in range(32)]
        if V:
            pc = cntl_predict(V, P) + [0] * (32 - len(P['in']))
            ok = any((cntl_predict(v, p) + [0] * 32)[:32] == actual for v in Vs for p in Ps)
            res[f'ps_input_cntl {"ok" if ok else "BAD"}'] += 1
            if not ok:
                cat = ('missing-sem' if any((x & 0xff) not in {e & 0xff for e in V['exp']} for x in P['in']) else 'all-found')
                res['cntl BAD ' + cat] += 1
            if not ok and len(samples['cntl' + cat]) < 5:
                samples['cntl' + cat].append(([hex(x) for x in actual[:len(P['in'])]], [hex(x) for x in pc[:len(P['in'])]],
                                        [hex(x) for x in P['in']], [hex(x) for x in V['exp']]))
        else:
            res['vs struct unknown'] += 1
for k, v in sorted(res.items()):
    print(f'{k}: {v}')
for k, v in samples.items():
    print(k)
    for s in v:
        print('   ', s)

# mismatches: which VS struct (any) reproduces the actual table, and is it paired with this PS in a pass?
db = json.load(open(r'E:\research\rea\out\material_db.json'))
pair = collections.defaultdict(set)
for p in db['programs']:
    for q in p['passes']:
        if q['ps'] and q['vs']:
            pair[q['ps']].add(q['vs'])
allvs = {k: v for k, v in ss.items() if v['role'] == 'vs'}
why = collections.Counter()
for path in sys.argv[2:]:
    spv2fh, pipes = {}, []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            r.u64(); r.u64(); r.u32(); r.u8(); r.u8()
            fh = r.u64()
            r.take(r.u32()); r.take(8); r.take(64); r.words(); r.u64()
            spv2fh[(r.u64(), r.u64())] = fh
        elif typ == 'L':
            compute, has_ps = r.u8(), r.u8()
            vs = (r.u64(), r.u64()); ps = (r.u64(), r.u64())
            r.take(r.u32()); fs = r.take(r.u32())
            if not compute and has_ps:
                pipes.append((vs, ps, fs))
    for vs, ps, fs in pipes:
        vf, pf = spv2fh.get(vs), spv2fh.get(ps)
        P, V = ss.get(f"{pf:x}" if pf else ''), ss.get(f"{vf:x}" if vf else '')
        if not P or not V:
            continue
        actual = [struct.unpack_from('<I', fs, 92 + 4 * i)[0] for i in range(32)]
        if (cntl_predict(V, P) + [0] * 32)[:32] == actual:
            continue
        hits = [k for k, v in allvs.items() if (cntl_predict(v, P) + [0] * 32)[:32] == actual]
        paired = pair.get(f"{pf:x}", set())
        why['miss'] += 1
        why['some VS reproduces' if hits else 'no VS reproduces'] += 1
        if set(hits) & paired:
            why['  a VS paired with this PS in a pass reproduces'] += 1
        why[f'  draw VS paired with PS in a pass: {f"{vf:x}" in paired}'] += 1
print(dict(why))
