"""Per-function facts for the FL counterparts of SS hook functions: body size, fnv1a64, prologue validity."""
import sys, json, difflib; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
ss, fl = load()
PAIRS = [
    # name, ss_rva, ss_size, ss_fnv, ss_prologue, fl_rva
    ('GameMenu.opened', 0x27db10, 0xd0, 0x8109e3344cdd9cf3, 17, 0x2e75a0),
    ('GameMenu.closed', 0x27e210, 0xdf, 0x33ff129a2ada5846, 16, 0x2e7ad0),
    ('FrameBegin', 0x1f0be0, 0x5aa, 0x6273d28a515609b7, 20, 0x237670),
    ('RenderTarget::Configure', 0x300b00, 0x2bb, 0x4489645ef331292a, 0, 0x382420),
    ('ConfigureSceneTargets', 0x18e210, 0x2f9, 0x804f07e3bc802864, 0, 0x1bc1d0),
    ('Camera::SetFov', 0x580580, 0xc1, 0xef658398f5aad2a4, 17, None),
    ('CameraJitter', 0x472810, 0x1ed, 0xaf06825055bbe18d, 17, 0x51c840),
    ('Camera::ResetFrustumWindow', 0x581a10, 0x34, 0x26b44a8bd1f71035, 0, 0x65c210),
    ('Pass32', 0x1929c0, 0x42e, 0xcd353017f8e6ce48, 20, None),
    ('CameraIntegrator', 0x14d6f0, 0x593, 0x724cebb9e1f76f99, 17, 0x171410),
]


def facts(name, sr, ssz, sfnv, sp, fr, quiet=False):
    se, sp_ = func_end(ss, sr)
    out = dict(name=name, ss_rva=hex(sr), ss_size=hex(ssz), ss_fnv=f'{sfnv:016x}', ss_prologue=sp,
               ss_fnv_ok=fnv1a64(ss.text[sr:sr + ssz]) == sfnv, ss_body=hex(se - sr))
    if fr is None:
        return out
    fe, fp = func_end(fl, fr)
    body = fe - fr
    out.update(fl_rva=hex(fr), fl_body=hex(body), fl_padded=hex(fp - fr),
               fl_fnv_body=f'{fnv1a64(fl.text[fr:fr + body]):016x}',
               fl_fnv_padded=f'{fnv1a64(fl.text[fr:fp]):016x}')
    A = [i.mnemonic for i in ss.dis(sr, se - sr)]
    B = [i.mnemonic for i in fl.dis(fr, body)]
    out['mnemonic_ratio'] = round(difflib.SequenceMatcher(None, A, B, autojunk=False).ratio(), 3)
    out['ins_count'] = (len(A), len(B))
    if sp:
        mp = min_prologue(fl, fr)
        ins, probs = prologue_check(fl, fr, mp)
        out['fl_prologue'] = mp
        out['fl_prologue_ins'] = ins
        out['fl_prologue_problems'] = probs
        out['fl_branch_into_prologue'] = internal_branch_into(fl, fr, body, mp)
    return out


if __name__ == '__main__':
    for p in PAIRS:
        r = facts(*p)
        print(json.dumps(r, indent=1))
