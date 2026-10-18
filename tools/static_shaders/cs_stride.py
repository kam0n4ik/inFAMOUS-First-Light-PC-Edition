"""Does a CS buffer slot's stride follow from the code? For each cached CS module, each buffer slot (V#): the actual
stride (its flatbuf sharp) against the largest offset + access size of the indexed accesses through that V# (directly
in the user data SGPRs, or loaded from the extended user data by an s_load)."""
import collections, json, sys
sys.path.insert(0, r"E:\research\rea\q")
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa
from cs_flat import cs_modules
from gcn_buf import walk, access_bytes
idx = json.load(open(r'E:\research\rea\out\pack_shaders.json'))


def program_code(path):
    out = {}
    for typ, b in pa.records(path):
        if typ == 'P':
            r = pa.Reader(b)
            ident = (r.u64(), r.u64(), r.u32()); r.u8(); r.u64()
            out[ident] = bytes(r.take(4 * r.u32()))
    return out


def infer(code, slots):
    """slot register -> (max end of indexed accesses, any raw (offen-only) access)"""
    eud = [reg for t, api, reg, fl in slots if t == 27]
    reg_of = {}  # SGPR holding a V# -> the slot register it came from
    for t, api, reg, fl in slots:
        if t in (0, 2, 4) and not fl & 1 and reg < 16:
            reg_of[reg] = reg
    acc = collections.defaultdict(lambda: [0, False, 0])
    for kind, f in walk(code):
        if kind == 'smrd' and eud and f['sbase'] == eud[0] and f['imm'] and f['op'] in (1, 2, 3):
            reg_of[f['sdst']] = 16 + f['off']
        elif kind == 'buf':
            src = reg_of.get(f['srsrc'])
            if src is None:
                continue
            a = acc[src]
            if f['idxen']:
                a[0] = max(a[0], f['off'] + access_bytes(f))
                a[2] += 1
            else:
                a[1] = True
    return acc


c = collections.Counter(); ex = []
for path in sys.argv[1:]:
    code = program_code(path)
    for m in cs_modules(path):
        e = idx.get('%x' % m['fh'])
        if not e or m['flat'] is None or m['id'] not in code:
            continue
        acc = infer(code[m['id']], e['slots'])
        for t, api, reg, fl in e['slots']:
            if t not in (0, 4) or fl & 1 or reg + 4 > len(m['flat']):
                continue
            stride = (m['flat'][reg + 1] >> 16) & 0x3FFF
            a = acc.get(reg)
            if a is None or a[2] == 0:
                c['no indexed access (stride %s)' % ('0' if stride == 0 else '>0')] += 1
                continue
            mx = a[0]
            if mx == stride: c['exact'] += 1
            elif (mx + 15) // 16 * 16 == stride: c['round16'] += 1
            elif mx < stride: c['below'] += 1; ex.append((stride, mx))
            else: c['above'] += 1; ex.append((stride, mx))
print(c)
print(collections.Counter(ex).most_common(20))
