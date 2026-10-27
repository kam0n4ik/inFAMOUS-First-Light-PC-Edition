import sys, re; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
from capstone import x86
ss, fl = load()
def scan(b, off, label):
    # all instructions writing/reading [reg+off] dword with imm, via regex on modrm disp32 bytes
    pat = re.compile(re.escape(off.to_bytes(4, 'little')))
    res = []
    for m in pat.finditer(b.text):
        a = m.start()
        for back in range(2, 5):
            st = a - back
            ins = next(md.disasm(b.text[st:st + 15], st), None)
            if ins and ins.address + ins.size > a + 3 and any(o.type == x86.X86_OP_MEM and o.mem.disp == off and o.mem.base != x86.X86_REG_RIP for o in ins.operands):
                res.append(ins)
                break
    print(f'== {label}: {len(res)} uses of +{off:#x}')
    for ins in res:
        if ins.mnemonic in ('mov', 'cmp') and ins.operands[0].type == x86.X86_OP_MEM:
            print(f'  {ins.address:#x}: {ins.mnemonic} {ins.op_str}')
    return res
r1 = scan(ss, 0x620, 'SS kind')
r2 = scan(fl, 0x64c, 'FL kind')
print('--- all FL uses')
for i in r2: print(f'  {i.address:#x}: {i.mnemonic} {i.op_str}')
print('--- SS uses in 0x270000..0x290000 and register writes')
for i in r1:
    if 0x270000 <= i.address < 0x290000 or (i.mnemonic=='mov' and i.operands[0].type==x86.X86_OP_MEM):
        print(f'  {i.address:#x}: {i.mnemonic} {i.op_str}')
print('--- SS 0x4879b0'); show(ss, 0x4879b0, 0x40)
print('--- FL 0x535850'); show(fl, 0x535850, 0x40)
