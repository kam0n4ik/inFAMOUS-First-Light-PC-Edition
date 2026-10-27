"""Masked search of an SS byte range (rel branch/RIP disp wildcarded) in FL text."""
import sys, re; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\tools')
from sigmap import mask_of


def masked(ss, fl, rva, n, limit=8):
    body = ss.text[rva:rva + n]
    keep, dec = mask_of(body, rva, md)
    pat = b''.join(re.escape(bytes([b])) if k else b'.' for b, k in zip(body, keep))
    return [m.start() for m in re.finditer(pat, fl.text, re.DOTALL)][:limit]


if __name__ == '__main__':
    ss, fl = load()
    rva, n = int(sys.argv[1], 16), int(sys.argv[2], 16)
    print([hex(h) for h in masked(ss, fl, rva, n)])
