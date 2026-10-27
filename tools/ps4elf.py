import struct, sys, json, re, hashlib

B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def dec_id(s):
    v = 0
    for c in s:
        v = v * 64 + B64.index(c)
    return v


class Image:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        self.raw = d
        self.is_self = struct.unpack_from('<I', d, 0)[0] == 0x1D3D154F
        self.segs = []
        off = 0
        if self.is_self:
            cnt = struct.unpack_from('<H', d, 0x18)[0]
            for i in range(cnt):
                flags, foff, fsz, msz = struct.unpack_from('<QQQQ', d, 0x20 + i * 32)
                self.segs.append((flags, foff, fsz, msz))
            off = 0x20 + cnt * 32
        self.elf_off = off
        assert d[off:off + 4] == b'\x7fELF', 'no ELF'
        (self.e_type, self.e_machine, _, self.entry, self.phoff, _, _, _, self.phentsize,
         self.phnum) = struct.unpack_from('<HHIQQQIHHH', d, off + 16)
        self.phdrs = []
        for i in range(self.phnum):
            p = struct.unpack_from('<IIQQQQQQ', d, off + self.phoff + i * 56)
            self.phdrs.append(dict(type=p[0], flags=p[1], offset=p[2], vaddr=p[3], paddr=p[4], filesz=p[5],
                                   memsz=p[6], align=p[7]))

    def read(self, foff, size):
        if not self.is_self:
            return self.raw[foff:foff + size]
        for flags, soff, fsz, msz in self.segs:
            if not (flags & 0x800):
                continue
            idx = (flags >> 20) & 0xFFF
            if idx >= len(self.phdrs):
                continue
            ph = self.phdrs[idx]
            if ph['offset'] <= foff < ph['offset'] + ph['filesz']:
                s = soff + (foff - ph['offset'])
                return self.raw[s:s + size]
        return None

    def seg(self, ph):
        return self.read(ph['offset'], ph['filesz'])

    def analyze(self):
        out = dict(path=self.path, is_self=self.is_self, entry=hex(self.entry), e_type=hex(self.e_type),
                   segments=[])
        dyn = dyndata = None
        for ph in self.phdrs:
            out['segments'].append({k: (hex(v) if isinstance(v, int) else v) for k, v in ph.items()})
            if ph['type'] == 2:
                dyn = self.seg(ph)
            elif ph['type'] == 0x61000000:
                dyndata = self.seg(ph)
            elif ph['type'] == 0x61000001:
                pp = self.seg(ph)
                if pp:
                    size, magic, cnt, sdk = struct.unpack_from('<QIIQ', pp, 0)
                    out['procparam'] = dict(size=size, magic=hex(magic), sdk=hex(sdk))
        for ph in self.phdrs:
            if ph['type'] == 1 and ph['flags'] & 1:
                code = self.seg(ph) or b''
                out['text_sha1'] = hashlib.sha1(code).hexdigest()
                out['text_size'] = len(code)
                break
        tags = {}
        for i in range(0, len(dyn), 16):
            t, v = struct.unpack_from('<qQ', dyn, i)
            if t == 0:
                break
            tags.setdefault(t & 0xffffffffffffffff, []).append(v)
        strtab_off = tags[0x61000035][0]
        symtab_off = tags[0x61000039][0]
        symtab_sz = tags[0x6100003f][0]

        def s(o):
            o += strtab_off
            return dyndata[o:dyndata.index(b'\0', o)].decode()

        mods = {}
        for v in tags.get(0x6100000f, []):
            mods[v >> 48] = s(v & 0xffffffff)
        libs = {}
        for v in tags.get(0x61000015, []):
            libs[v >> 48] = s(v & 0xffffffff)
        out['needed_modules'] = sorted(set(mods.values()))
        out['import_libraries'] = sorted(set(libs.values()))
        out['export_libraries'] = [s(v & 0xffffffff) for v in tags.get(0x61000013, [])]
        if 0x6100000d in tags:
            out['module_name'] = s(tags[0x6100000d][0] & 0xffffffff)
        imports = set()
        exports = 0
        for i in range(0, symtab_sz, 24):
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', dyndata, symtab_off + i)
            if name_off == 0:
                continue
            parts = s(name_off).split('#')
            if len(parts) != 3:
                continue
            nid, l, m = parts
            if shndx == 0:
                imports.add((nid, libs.get(dec_id(l), '?'), mods.get(dec_id(m), '?'), info & 0xf))
            else:
                exports += 1
        out['imports'] = sorted(imports)
        out['exports_count'] = exports
        return out


if __name__ == '__main__':
    res = Image(sys.argv[1]).analyze()
    json.dump(res, open(sys.argv[2], 'w'), indent=1)
    print(sys.argv[1], 'self' if res['is_self'] else 'elf', 'entry', res['entry'], 'imports', len(res['imports']),
          'exports', res['exports_count'], res.get('procparam'), 'text', res.get('text_size'))
