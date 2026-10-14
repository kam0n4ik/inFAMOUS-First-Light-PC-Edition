# args: depth addr... ; caller tree
dep=int(args[0])
def walk(f, d, seen):
    for c in sorted(f.getCallingFunctions(None), key=lambda x: x.getEntryPoint().getOffset()):
        k=c.getEntryPoint().getOffset()
        print("  "*d + "<- %s 0x%x size=%d" % (c.getName(), k, c.getBody().getNumAddresses()))
        if d < dep and k not in seen:
            seen.add(k); walk(c, d+1, seen)
for s in args[1:]:
    f=F(int(s,16)); print("== %s 0x%x" % (f.getName(), f.getEntryPoint().getOffset())); walk(f,1,set())
