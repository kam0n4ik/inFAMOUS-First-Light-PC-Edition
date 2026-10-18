"""The compute shaders' descriptors for the static predictor, from shader caches' CS modules (their flattened user
data, cs_flat.py): per slot list (its FNV-1a 64 of the struct's slot words) and start register the most frequent form
(cs_slots.py) with a sharp of that form, and per (usage type, api slot, flags) the same as the fallback. Appends lines
  csig <slot list hash hex> <start register> <dwords hex...>
  cslot <usage type> <api slot> <flags> <dwords hex...>
usage: py -I -X utf8 cs_table.py <out.txt (appended)> <shaders.pack>..."""
import collections, json, sys
sys.path.insert(0, r"E:\research\rea\q")
from cs_flat import cs_modules
idx = json.load(open(r'E:\research\rea\out\pack_shaders.json'))


def form(t, fl, w):
    if t == 1:
        return ('s',) + tuple(w)
    if fl & 1:
        return ('i', (w[1] >> 20) & 0x3F, (w[1] >> 26) & 0xF, w[3] >> 28)
    # every V# bit the key reads (stride, swizzle, word 3's formats, element size, index stride, ADD_TID, MTYPE); a RW
    # buffer's stride comes from the code (gcn_buf.buffer_strides), not from here
    return ('b', 0 if t == 4 else (w[1] >> 16) & 0x3FFF, w[1] >> 31, w[3] & 0x3FFFFFFF)


def sig(slots):
    h = 0xCBF29CE484222325
    for t, api, reg, fl in slots:
        h = ((h ^ (t | api << 8 | reg << 16 | fl << 24)) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


by_sig = collections.defaultdict(collections.Counter)
by_slot = collections.defaultdict(collections.Counter)
sample = {}
for path in sys.argv[2:]:
    for m in cs_modules(path):
        e = idx.get('%x' % m['fh'])
        if not e or m['flat'] is None:
            continue
        sh = sig(e['slots'])
        for t, api, reg, fl in e['slots']:
            if t not in (0, 1, 2, 4):
                continue
            n = 8 if (t in (0, 4) and fl & 1) else 4
            if reg + n > len(m['flat']):
                continue
            w = tuple(m['flat'][reg:reg + n])
            f = form(t, fl, w)
            sample.setdefault(f, w)
            by_sig[(sh, reg)][f] += 1
            by_slot[(t, api, fl)][f] += 1
with open(sys.argv[1], 'a', encoding='utf-8') as out:
    for (sh, reg), c in sorted(by_sig.items()):
        out.write(f"csig {sh:x} {reg} {' '.join('%x' % x for x in sample[c.most_common(1)[0][0]])}\n")
    for (t, api, fl), c in sorted(by_slot.items()):
        out.write(f"cslot {t} {api} {fl} {' '.join('%x' % x for x in sample[c.most_common(1)[0][0]])}\n")
print('csig', len(by_sig), 'cslot', len(by_slot))
