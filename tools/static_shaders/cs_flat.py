"""The CS modules of shader caches with their flattened user data (the descriptors a dispatch gave), by footer hash.
A module record ends with ... reflection, flatbuf (Words), fetch code (Words), vertex V# count + V#s, two u64 times; a
CS has no fetch code and no V#s, and its flatbuf starts with its 16 user data registers (in the record's head too):
found from the end."""
import struct, sys
sys.path.insert(0, r"E:\SecondSonPC\tools\p414")
import pack_analyze as pa


def cs_modules(path):
    out = []
    for typ, b in pa.records(path):
        if typ != 'M':
            continue
        r = pa.Reader(b)
        ident = (r.u64(), r.u64(), r.u32()); st = r.u8(); lay = r.u8(); fh = r.u64()
        if st != 6:
            continue
        rt = r.take(r.u32()); start = (r.u32(), r.u32()); ud = r.take(64); key = tuple(r.words())
        end = len(b) - 16
        nvs, nfetch = struct.unpack_from('<I', b, end - 4)[0], struct.unpack_from('<I', b, end - 8)[0]
        if nvs or nfetch:
            continue
        fend = end - 8
        flat = None
        for n in range(16, 4096):
            p = fend - 4 * n - 4
            if p < r.p:
                break
            if struct.unpack_from('<I', b, p)[0] == n and b[p + 4:p + 68] == ud:
                flat = struct.unpack_from(f'<{n}I', b, p + 4)
                break
        out.append({'id': ident, 'layout': lay, 'fh': fh, 'rt': bytes(rt), 'start': start, 'key': key, 'flat': flat})
    return out


if __name__ == '__main__':
    import collections
    c = collections.Counter()
    for m in cs_modules(sys.argv[1]):
        c['flat found' if m['flat'] else 'flat missing'] += 1
    print(c)
