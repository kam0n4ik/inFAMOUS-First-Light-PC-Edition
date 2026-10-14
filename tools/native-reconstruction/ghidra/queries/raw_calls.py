import struct
from jpype import JArray, JByte
tg=[int(a,16) for a in args]
for blk in mem.getBlocks():
    if not blk.isInitialized() or not blk.isExecute(): continue
    base=blk.getStart().getOffset(); size=int(blk.getSize()); jb=JArray(JByte)(size); blk.getBytes(blk.getStart(), jb)
    d=bytes(x&0xff for x in jb)
    for i in range(size-5):
        if d[i] in (0xE8,0xE9):
            t=base+i+5+struct.unpack_from("<i",d,i+1)[0]
            if t in tg:
                f=fm.getFunctionContaining(A(base+i))
                print(hex(t),"<-",hex(base+i),"E8" if d[i]==0xE8 else "E9", f.getName() if f else "NOFUNC", "insn" if listing.getInstructionAt(A(base+i)) else "not-disassembled")
