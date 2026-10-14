# run.py snippet: eboot-relative addresses (hex, one per line) -> address<TAB>function entry<TAB>function name
# usage: run.py resolve_addrs.py <addrs.txt> <out.tsv>
with open(args[0]) as fi, open(args[1], "w", encoding="utf-8") as fo:
    for line in fi:
        a = int(line, 16)
        f = fm.getFunctionContaining(A(a))
        fo.write(f"{a:x}\t{f.getEntryPoint().getOffset():x}\t{f.getName()}\n" if f else f"{a:x}\t\t?\n")
