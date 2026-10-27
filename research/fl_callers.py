import sys, struct; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *


def callers(b, target):
    t = b.text
    out = []
    i = t.find(b'\xe8')
    while i != -1 and i + 5 <= len(t):
        if i + 5 + struct.unpack_from('<i', t, i + 1)[0] == target:
            out.append(('call', i, i + 5))
        i = t.find(b'\xe8', i + 1)
    i = t.find(b'\xe9')
    while i != -1 and i + 5 <= len(t):
        if i + 5 + struct.unpack_from('<i', t, i + 1)[0] == target:
            out.append(('jmp', i, i + 5))
        i = t.find(b'\xe9', i + 1)
    return out


if __name__ == '__main__':
    ss, fl = load()
    b = ss if sys.argv[1] == 'ss' else fl
    for a in sys.argv[2:]:
        a = int(a, 16)
        print(f'== {a:#x}')
        for k, at, ret in callers(b, a):
            print(f'  {k} at {at:#x} ret {ret:#x}')
