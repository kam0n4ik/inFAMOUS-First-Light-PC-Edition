"""Render state of a pass class, for the static predictor: the RuntimeParams of cached modules of pass shaders with
the fields the shader's own Gnmx struct gives zeroed (RSRC1/2, vs_out_cntl, PS inputs/exports, ps_input_cntl,
ps_vs_out_cntl and ps_ucp_enable which follow the paired VS). One line per (role, class, PS COL_FORMAT) variant:
  state <vs|ps> <class hex> <col_format hex> <count> <328-byte RuntimeParams hex>
usage: py -I -X utf8 state_table.py <material_db.json> <out.txt (appended)> <shaders.pack>..."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa

db = json.load(open(sys.argv[1]))
cls_of = collections.defaultdict(set)
for p in db['programs']:
    for q in p['passes']:
        for role in ('vs', 'ps'):
            if q.get(role):
                cls_of[(role, int(q[role], 16))].add(q['cls'])
ZERO = [(4, 12), (36, 40), (44, 72), (84, 92), (92, 220)]
c = collections.Counter()
for path in sys.argv[3:]:
    for typ, b in pa.records(path):
        if typ != 'M':
            continue
        r = pa.Reader(b)
        r.u64(); r.u64(); r.u32(); r.u8(); r.u8()
        fh = r.u64()
        rt = bytearray(r.take(r.u32()))
        if len(rt) != 328:
            raise SystemExit(f'RuntimeParams is {len(rt)} bytes, expected 328')
        role = 'ps' if rt[0] == 0 else 'vs' if rt[0] == 1 else None
        if role is None:
            continue
        col = struct.unpack_from('<I', rt, 68)[0] if role == 'ps' else 0
        for a, e in ZERO:
            rt[a:e] = bytes(e - a)
        for cl in cls_of.get((role, fh), ()):
            c[(role, cl, col, bytes(rt))] += 1
with open(sys.argv[2], 'a', encoding='utf-8') as f:
    for (role, cl, col, rt), n in sorted(c.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2], -kv[1])):
        f.write(f"state {role} {cl} {col:x} {n} {rt.hex()}\n")
print('state variants', len(c), 'classes', len({(k[0], k[1]) for k in c}))
