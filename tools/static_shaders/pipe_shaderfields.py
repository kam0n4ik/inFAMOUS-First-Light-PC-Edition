"""Checks the pipeline state's shader-own fields against the PS struct: cb_shader_mask == CB_SHADER_MASK (regs[7]),
targets' export format == the COL_FORMAT nibble, and a target bound only where mrt_mask has its bit."""
import collections, json, struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa
st = json.load(open('out/shader_structs.json'))
c = collections.Counter()
for path in sys.argv[1:]:
    mod = {}
    pipes = []
    for typ, b in pa.records(path):
        r = pa.Reader(b)
        if typ == 'M':
            r.u64(); r.u64(); r.u32(); r.u8(); r.u8(); fh = r.u64(); rt = r.take(r.u32()); r.u32(); r.u32(); r.take(64); r.words(); r.u64()
            mod[(r.u64(), r.u64())] = fh
        elif typ == 'L':
            comp, hp = r.u8(), r.u8(); vs = (r.u64(), r.u64()); ps = (r.u64(), r.u64())
            s = r.take(r.u32()); fs = r.take(r.u32())
            if not comp and hp:
                pipes.append((ps, s, fs))
    for ps, s, fs in pipes:
        fh = mod.get(ps)
        e = st.get('%x' % fh) if fh is not None else None
        if not e or e['role'] != 'ps':
            c['no struct'] += 1; continue
        w = struct.unpack('<55I', s)
        regs = e['regs']
        c['cbmask==regs9 %s' % (w[13] == regs[9])] += 1
        col = struct.unpack_from('<I', fs, 68)[0]
        c['col==regs3 %s' % (col == regs[3])] += 1
        mrt = w[14]
        for t in range(8):
            info, exp = w[15 + t * 5], w[16 + t * 5]
            if info:
                c['bound, mrt bit %d' % ((mrt >> t) & 1)] += 1
                c['export==nibble %s' % (exp == (col >> (4 * t)) & 0xF)] += 1
        c['mrt==bits of col'] += mrt == sum(1 << t for t in range(8) if (col >> 4 * t) & 0xF)
print(dict(sorted(c.items())))
