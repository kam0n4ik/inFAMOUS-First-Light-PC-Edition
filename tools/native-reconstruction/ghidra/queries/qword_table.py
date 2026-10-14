# args: addr count stride_qwords ; print qword table entries with function names
import struct
from jpype import JArray, JByte
a=int(args[0],16); n=int(args[1]); s=int(args[2])
jb=JArray(JByte)(n*s*8); mem.getBytes(A(a), jb); b=bytes(x&0xff for x in jb)
for i in range(n):
    row=struct.unpack_from("<%dQ"%s, b, i*s*8)
    def nm(v):
        f=fm.getFunctionAt(A(v)) if 0<v<0x2000000 else None
        return "%x%s"%(v, "("+f.getName()+")" if f else "")
    print(i, " ".join(nm(v) for v in row))
for x in args[3:]:
    ad=int(x,16); ins=listing.getInstructionAt(A(ad)); k=0
    print("-- disasm", x)
    while ins and k<30:
        print(" ", ins.getAddress(), ins); ins=ins.getNext(); k+=1
