"""Shared helpers for the First Light site mapping (research only)."""
import os, struct, sys, bisect
import capstone
from capstone import x86
ROOT = r'E:\inFAMOUS-First-Light-PC-Edition'
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from ps4elf import Image  # noqa
from sigmap import fnv1a64  # noqa

SS_PATH = r'E:\shdps4\CUSA00004\eboot.bin'
FL_PATH = os.path.join(ROOT, 'eboot.bin')

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


class Bin:
    def __init__(self, path):
        self.img = Image(path)
        self.loads = []
        for ph in self.img.phdrs:
            if ph['type'] in (1, 0x61000010) or ph['type'] == 0x6fffff01:
                pass
            if ph['type'] == 1 or ph['type'] == 0x61000010:
                data = self.img.seg(ph) or b''
                self.loads.append((ph['vaddr'], ph['memsz'], data, ph))
        self.text = self.loads[0][2]
        self._ct = None

    def read(self, va, n):
        for v, m, d, ph in self.loads:
            if v <= va < v + m:
                o = va - v
                b = d[o:o + n]
                return b + b'\0' * (n - len(b))
        return None

    def u64(self, va):
        return struct.unpack('<Q', self.read(va, 8))[0]

    def call_targets(self):
        if self._ct is None:
            t = set()
            text = self.text
            i = text.find(b'\xe8')
            while i != -1 and i + 5 <= len(text):
                tgt = i + 5 + struct.unpack_from('<i', text, i + 1)[0]
                if 0 <= tgt < len(text) and tgt % 16 == 0:
                    t.add(tgt)
                i = text.find(b'\xe8', i + 1)
            self._ct = sorted(t)
        return self._ct

    def dis(self, at, n):
        return list(md.disasm(self.text[at:at + n], at))

    def func_extent(self, start, maxlen=0x4000):
        """Walk instructions linearly from start; track furthest branch target inside; end = after a terminal
        instruction (ret/jmp/ud2) at or beyond every internal target. Returns (code_end, padded_end)."""
        far = start
        end = None
        for ins in md.disasm(self.text[start:start + maxlen], start):
            if (ins.group(capstone.CS_GRP_JUMP)) and ins.operands and ins.operands[0].type == x86.X86_OP_IMM:
                tgt = ins.operands[0].imm
                if start <= tgt < start + maxlen:
                    far = max(far, tgt)
            nxt = ins.address + ins.size
            term = ins.mnemonic in ('ret', 'ud2', 'int3', 'hlt') or (ins.mnemonic == 'jmp')
            if term and nxt > far:
                end = nxt
                break
        if end is None:
            return None, None
        p = end
        t = self.text
        while p % 16 and t[p] in (0xcc, 0x90, 0x00):
            p += 1
        # multi-byte nops
        while p % 16:
            ins = next(md.disasm(t[p:p + 16], p), None)
            if ins is None or not ins.mnemonic.startswith('nop'):
                break
            p += ins.size
        return end, p

    def rip_targets(self, at, n):
        out = []
        for ins in self.dis(at, n):
            for op in ins.operands:
                if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                    out.append((ins.address, f'{ins.mnemonic} {ins.op_str}', ins.address + ins.size + op.mem.disp))
            if ins.mnemonic == 'lea' and False:
                pass
        return out


def prologue_check(b, rva, length):
    """Instructions covering `length` bytes: must end exactly at length, no RIP-relative, no rel branch."""
    probs = []
    off = 0
    ins_list = []
    for ins in md.disasm(b.text[rva:rva + 64], rva):
        if off >= length:
            break
        ins_list.append(f'{ins.address:#x}: {ins.bytes.hex()} {ins.mnemonic} {ins.op_str}')
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                probs.append(f'RIP-relative at {ins.address:#x}')
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_RET):
            probs.append(f'branch at {ins.address:#x}')
        off += ins.size
    if off != length:
        probs.append(f'instruction boundary at {off}, not {length}')
    if length < 14:
        probs.append('shorter than 14')
    return ins_list, probs


def min_prologue(b, rva):
    off = 0
    for ins in md.disasm(b.text[rva:rva + 64], rva):
        off += ins.size
        if off >= 14:
            return off


def internal_branch_into(b, start, size, upto):
    """Branches inside [start, start+size) that target (start, start+upto) — would land inside moved prologue."""
    hits = []
    for ins in b.dis(start, size):
        if ins.group(capstone.CS_GRP_JUMP) and ins.operands and ins.operands[0].type == x86.X86_OP_IMM:
            t = ins.operands[0].imm
            if start < t < start + upto:
                hits.append(hex(ins.address))
    return hits


_ss = _fl = None


def load():
    global _ss, _fl
    if _ss is None:
        _ss, _fl = Bin(SS_PATH), Bin(FL_PATH)
    return _ss, _fl


def show(b, at, n, mark=None):
    for ins in b.dis(at, n):
        extra = ''
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                extra = f'   ; -> {ins.address + ins.size + op.mem.disp:#x}'
        print(f'{ins.address:#9x}: {ins.bytes.hex():24s} {ins.mnemonic} {ins.op_str}{extra}')


STACK_CHK = {}


def linear(b, start, n):
    return list(md.disasm(b.text[start:start + n], start))


def func_end(b, start, maxlen=0x8000):
    """End of function body (exclusive, no padding) by linear decode: the first terminal instruction (ret, jmp,
    call to a noreturn - detected as followed by padding) that is followed by nop/int3 padding to a 16-aligned
    address holding a function start (call target or push rbp; mov rbp,rsp), and no branch from before it targets
    beyond it."""
    cts = set(b.call_targets())
    far = start
    t = b.text
    for ins in md.disasm(t[start:start + maxlen], start):
        if ins.group(capstone.CS_GRP_JUMP) and ins.operands and ins.operands[0].type == x86.X86_OP_IMM:
            tgt = ins.operands[0].imm
            if start <= tgt < start + maxlen:
                far = max(far, tgt)
        nxt = ins.address + ins.size
        if ins.mnemonic in ('ret', 'jmp', 'call', 'ud2', 'int3') and nxt > far:
            p = nxt
            while p % 16 and t[p] in (0x90, 0xcc):
                p += 1
            if p % 16 == 0 and (p in cts or t[p:p + 4] == b'\x55\x48\x89\xe5'):
                return nxt, p
    return None, None
