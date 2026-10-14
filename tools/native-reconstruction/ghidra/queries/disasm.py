# args: func ; print full disassembly
f=F(int(args[0],16)); it=listing.getInstructions(f.getBody(), True)
for ins in it: print(ins.getAddress(), ins)
