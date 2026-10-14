# args: func addrs ; find vtables containing them and try RTTI names
import struct
from jpype import JArray, JByte
blobs=[]
for blk in mem.getBlocks():
    if not blk.isInitialized(): continue
    size=int(blk.getSize()); jb=JArray(JByte)(size); blk.getBytes(blk.getStart(), jb)
    blobs.append((blk.getStart().getOffset(), bytes(x&0xff for x in jb)))
def rd(a):
    for base,d in blobs:
        if base<=a<base+len(d)-8: return struct.unpack_from("<Q",d,a-base)[0]
def cstr(a):
    for base,d in blobs:
        if base<=a<base+len(d):
            e=d.find(b"\0",a-base); return d[a-base:e].decode("latin1")
cnt=0
for base,d in blobs:
    i=d.find(b"_ZTS")
    while i>=0 and cnt<5: print("typeinfo name sample:", d[i:i+60].split(b"\0")[0]); cnt+=1; i=d.find(b"_ZTS",i+1)
for s in args:
    t=int(s,16); pat=struct.pack("<Q",t)
    for base,d in blobs:
        i=d.find(pat)
        while i>=0:
            a=base+i
            # walk back to vtable start: previous qword pair (offset_to_top==0, typeinfo ptr)
            k=a; slot=0
            while k>base+16:
                v=rd(k-8)
                f=fm.getFunctionAt(A(v)) if v and v<0x2000000 else None
                if f is None: break
                k-=8; slot+=1
            ti=rd(k-8); nm=None
            if ti:
                np=rd(ti+8)
                if np: nm=cstr(np)
            print(hex(t),"in table at",hex(a),"slot",slot,"(off 0x%x)"%(slot*8),"vtbl",hex(k),"typeinfo",hex(ti or 0),"name",nm)
            i=d.find(pat,i+1)
