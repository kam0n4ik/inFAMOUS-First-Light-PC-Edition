import struct
from jpype import JArray, JByte
a=int(args[0],16); n=int(args[1]); jb=JArray(JByte)(n*4); mem.getBytes(A(a), jb)
print(hex(a), struct.unpack("<%dI"%n, bytes(x&0xff for x in jb)))
