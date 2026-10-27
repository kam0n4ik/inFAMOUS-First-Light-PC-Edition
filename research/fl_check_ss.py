import sys; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
ss, fl = load()
SITES = {
 'menu_open': (0x27db10, 0xd0, 0x8109e3344cdd9cf3, 17),
 'menu_close': (0x27e210, 0xdf, 0x33ff129a2ada5846, 16),
 'jitter': (0x472810, 0x1ed, 0xaf06825055bbe18d, 17),
 'resetwin': (0x581a10, 0x34, 0x26b44a8bd1f71035, 0),
 'pass32': (0x1929c0, 0x42e, 0xcd353017f8e6ce48, 20),
 'framebegin': (0x1f0be0, 0x5aa, 0x6273d28a515609b7, 20),
 'cfgtarget': (0x300b00, 0x2bb, 0x4489645ef331292a, 0),
 'cfgscene': (0x18e210, 0x2f9, 0x804f07e3bc802864, 0),
 'setfov': (0x580580, 0xc1, 0xef658398f5aad2a4, 17),
 'integrate': (0x14d6f0, 0x593, 0x724cebb9e1f76f99, 17),
}
for k, (r, s, h, p) in SITES.items():
    e, pe = ss.func_extent(r)
    ok = fnv1a64(ss.text[r:r+s]) == h
    nxt = [t for t in ss.call_targets() if t > r][0]
    print(f'{k:12s} {r:#x} size {s:#x} fnv_ok {ok} code_end {e - r if e else None:#x} padded {pe - r if pe else 0:#x} next_ct {nxt - r:#x}')
