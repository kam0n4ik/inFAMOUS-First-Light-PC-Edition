"""Rebuilds research/fl_sites.json: every SS constant of the ported hooks with its FL value, recomputed from both
eboots (asserts fail if any evidence no longer holds).  python -I research/fl_sites_build.py"""
import sys, json, re; sys.path.insert(0, r'E:\inFAMOUS-First-Light-PC-Edition\research')
from fl_lib import *
from capstone import x86
ss, fl = load()
OUT = r'E:\inFAMOUS-First-Light-PC-Edition\research\fl_sites.json'


def ins_at(b, a):
    i = next(md.disasm(b.text[a:a + 16], a), None)
    assert i is not None and i.address == a, hex(a)
    return i


def rip_target(b, a):
    i = ins_at(b, a)
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp, f'{a:#x}: {i.mnemonic} {i.op_str}'
    raise AssertionError(f'no RIP operand at {a:#x}')


def disp_of(b, a):
    i = ins_at(b, a)
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base != x86.X86_REG_RIP:
            return op.mem.disp, f'{a:#x}: {i.mnemonic} {i.op_str}'
    for op in i.operands:
        if op.type == x86.X86_OP_IMM:
            return op.imm, f'{a:#x}: {i.mnemonic} {i.op_str}'
    raise AssertionError(hex(a))


rows = []


def row(hook, name, ssv, flv, evidence, conf, note=''):
    rows.append(dict(hook=hook, constant=name, ss=ssv, fl=flv, evidence=evidence, confidence=conf, note=note))


def glob(hook, name, ssv, ss_at, fl_at, add=0, fl_add=None, conf='certain', note=''):
    """SS global = RIP target of ss_at (+add); FL = RIP target of fl_at (+fl_add, default add)."""
    st, si = rip_target(ss, ss_at)
    ft, fi = rip_target(fl, fl_at)
    fa = add if fl_add is None else fl_add
    assert st + add == ssv, (name, hex(st + add), hex(ssv))
    ev = f'SS {si} (+{add:#x}) | FL {fi} (+{fa:#x})' if add or fa else f'SS {si} | FL {fi}'
    row(hook, name, hex(ssv), hex(ft + fa), ev, conf, note)
    return ft + fa


def field(hook, name, ssv, ss_at, fl_at, conf='certain', note=''):
    sd, si = disp_of(ss, ss_at)
    fd, fi = disp_of(fl, fl_at)
    assert sd == ssv, (name, hex(sd), hex(ssv))
    row(hook, name, hex(ssv), hex(fd), f'SS {si} | FL {fi}', conf, note)
    return fd


def func(hook, name, ss_site, fl_rva, prologue_ss=0, conf='certain', note='', evidence=''):
    srva, ssz, sfnv = ss_site
    assert fnv1a64(ss.text[srva:srva + ssz]) == sfnv, name
    e, p = func_end(fl, fl_rva)
    size = e - fl_rva
    d = dict(hook=hook, constant=name, ss=dict(rva=hex(srva), size=hex(ssz), fnv1a64=f'{sfnv:016x}'),
             fl=dict(rva=hex(fl_rva), size=hex(size), fnv1a64=f'{fnv1a64(fl.text[fl_rva:e]):016x}',
                     padded_size=hex(p - fl_rva), fnv1a64_padded=f'{fnv1a64(fl.text[fl_rva:p]):016x}'),
             evidence=evidence, confidence=conf, note=note)
    if prologue_ss:
        mp = min_prologue(fl, fl_rva)
        ins, probs = prologue_check(fl, fl_rva, mp)
        assert not probs, (name, probs)
        assert not internal_branch_into(fl, fl_rva, size, mp), name
        d['ss']['prologue'] = prologue_ss
        d['fl']['prologue'] = mp
        d['fl']['prologue_instructions'] = ins
    rows.append(d)
    return d


# ---------------- 1. game menu
H = 'game_menu'
func(H, 'kOpenedSite (pausing screen opened)', (0x27db10, 0xd0, 0x8109e3344cdd9cf3), 0x2e75a0, 17,
     evidence='same body: test/set active byte, kind -> sound-pause counter inc (SS 0x4879b0 / FL 0x535850), '
              'element stride imul 0x950 -> 0x920; fuzzy ratio 0.909')
func(H, 'kClosedSite (pausing screen closed)', (0x27e210, 0xdf, 0x33ff129a2ada5846), 0x2e7ad0, 16,
     evidence='same body: active byte cleared, kind -> counter dec (SS 0x4879d0 / FL 0x535870)',
     note='FL prologue is 17, not 16 (FL pushes r14+rbx where SS pushed rbx+rax)')
field(H, 'kScreenActive (u8)', 0x61b, 0x27db1a, 0x2e75aa)
field(H, 'kScreenKind (u32, SOUND_PAUSE_KIND)', 0x620, 0x27db60, 0x2e75f0,
      note='reflection table: SS 0xd1b2d0 {"SOUND_PAUSE_KIND", offset 0x620} / FL 0xe10820 {same name, 0x64c}; same '
           'type hash 0x61ee5153abc8ef96 and count 0xa (enum 0..9; the consumer clamps <= 9 in both)')
field(H, 'screen +0x610 (element index)', 0x610, 0x27db6b, 0x2e75fb)
field(H, 'element stride', 0x950, 0x27db78, 0x2e7608)
glob(H, 'element array global (qword)', 0xd9de60, 0x27db7f, 0x2e760f)
row(H, 'kSettingsKind', '7', '7 (unverified)',
    'ctor default: SS 0x27d849 mov dword [rbx+0x620], 7 | FL 0x2e7244 mov dword [rbx+0x64c], 7 (the only immediate '
    'write of the field in either eboot); otherwise set from menu data through the SOUND_PAUSE_KIND property',
    'likely', 'data-driven; confirm with the existing "pausing screen ... opened (kind N)" log line on FL')
row(H, 'DumpScreen tail window', '[0x600..0x640]', '[0x5c0..0x680] suggested',
    'FL ctor writes the screen fields at 0x5c8..0x670 (SS 0x5f8..0x648)', 'certain', 'diagnostics only')

# ---------------- 2. render switch + HUD
H = 'render_resolution (switch/HUD)'
func(H, 'kFrameBeginSite', (0x1f0be0, 0x5aa, 0x6273d28a515609b7), 0x237670, 20,
     evidence='319/319 instructions, mnemonic ratio 1.0; the only operand change is options +0x6f8 -> +0x7a8')
func(H, 'kConfigureTarget (RenderTarget::Configure)', (0x300b00, 0x2bb, 0x4489645ef331292a), 0x382420,
     evidence='FrameBegin SS 0x1f0ecd call 0x300b00 | FL 0x23795d call 0x382420 (3 calls each); exact masked match',
     note='SS pins 0x2bb (ends inside "pop r15"); FL size is the whole body')
func(H, 'kConfigureSceneTargets', (0x18e210, 0x2f9, 0x804f07e3bc802864), 0x1bc1d0,
     evidence='FrameBegin SS 0x1f102b call 0x18e210 | FL 0x237abb call 0x1bc1d0; mnemonic ratio 0.984',
     note='SS pins 0x2f9 (past its own end into the next function); FL size is the body')
glob(H, 'kSceneTargets (object)', 0xe682b0, 0x1f1024, 0x237ab4)
glob(H, 'device global (VideoOut / FrameBegin)', 0xf1bcd0, 0x1f0dc2, 0x237852, note='base of the flag below')
glob(H, 'kQuarterHeightFlag', 0x1d0cbd5, 0x1f0dc2, 0x237852, add=0xdf0f05,
     note='read as cmp byte [rdi+0xdf0f05], rdi = device global (FrameBegin SS 0x1f0df2 / FL 0x237882)')
for off, ssa, fla in ((0xdf0f14, 0x1f0d82, 0x237812), (0xdf0f1c, 0x1f0d98, 0x237828), (0xdf0f24, 0x1f0dd4, 0x237864),
                      (0xdf0f2c, 0x1f0e00, 0x237890), (0xdf0f34, 0x1f0e5b, 0x2378eb), (0xdf0f3c, 0x1f0e78, 0x237908),
                      (0xdf0f44, 0x1f0e94, 0x237924), (0xdf0f4c, 0x1f0eaa, 0x23793a), (0xdf0f54, 0x1f0ebf, 0x23794f),
                      (0xdf0f5c, 0x1f0e47, 0x2378d7), (0xdf0f64, 0x1f0da6, 0x237836), (0xdf0f6c, 0x1f0db4, 0x237844),
                      (0xdf0f74, 0x1f0de4, 0x237874)):
    field(H, f'device size table +{off:#x}', off, ssa, fla)
field(H, 'device +0x8c4 (display target 1)', 0x8c4, 0x1f0eb8, 0x237948)
field(H, 'device +0x1050 (= 0x20a*8, display target 2)', 0x1050, 0x1f0ee0, 0x237970)
field(H, 'device +0x17e0 (= 0x2fc*8, display target 3)', 0x17e0, 0x1f0c46, 0x2376d6)
row(H, 'device +0xC / +0x10 (render w/h)', '0xc/0x10', '0xc/0x10',
    'FL 0x2369d7 holds the same mov [rbx+0xC] / [rbx+0x10] bytes as SS 0x1eff47', 'certain')
glob(H, 'kScreenWidth', 0x1d0ccc0, 0x1f03ea, 0x236e7a)
glob(H, 'kScreenHeight', 0x1d0ccc4, 0x1f03e3, 0x236e73)
glob(H, 'kMarginX', 0x1d0ccc8, 0x1f0480, 0x236f10)
glob(H, 'kMarginY', 0x1d0cccc, 0x1f04ab, 0x236f3b)
row(H, 'kRenderSizeSite / kDisplaySite / kScreenSite', '0x1eff47/0x1eff55/0x1f03f8', '0x2369d7/0x2369e5/0x236e88',
    'each byte pattern unique in FL text; screen site stores [r15]=0x1f272c4 (h), [r12]=0x1f272c0 (w)', 'certain',
    'already in render_resolution.cpp')
row(H, 'VideoOut init / margin recompute (comments)', '0x1eff00 / 0x1f2a60 / 0x1f2b60',
    '0x236990 / 0x239500 / 0x239600', 'function starts holding the kMarginX lea (FL 0x2395aa / 0x2396aa)', 'certain')
glob(H, 'options global (FrameBegin)', 0xefacf8, 0x1f104e, 0x237ade, note='options +0x6f8 -> +0x7a8 there')

# ---------------- 3. FOV
H = 'render_resolution (FOV)'
func(H, 'kSetFovSite (Camera::SetFov)', (0x580580, 0xc1, 0xef658398f5aad2a4), 0x65af10, 17,
     evidence='same 15 call sites in the same order (14 call + 1 jmp; SS 0x14f512 <-> FL 0x172fa9); FL body = SS '
              'body from 0x58059b on',
     note='FL has no "vmovss [rbx+0x5d4],xmm0; mov dword [rbx+0x5dc],0" head (why the fuzzy match looked off); the '
          'probe (scale xmm0) works the same')
field(H, 'camera +0x590 (fov)', 0x590, 0x5805a9, 0x65af27)
field(H, 'camera +0x5a4 (tan(fov/2))', 0x5a4, 0x5805be, 0x65af3c)
field(H, 'camera +0x5a0 (aspect*tan)', 0x5a0, 0x5805d1, 0x65af4f)
field(H, 'camera +0x598', 0x598, 0x580624, 0x65afa2)
row(H, 'camera +0x5d4 / +0x5dc (SS only)', '0x5d4/0x5dc', 'not written by FL SetFov', 'FL 0x65af10 listing', 'certain')
row(H, 'kPlayerSetFovReturn', '0x14f517', '0x172fae',
    'SS 0x14f512 call 0x580580 | FL 0x172fa9 call 0x65af10, both after vmovss xmm0,[c+0x468]; vaddss xmm0,[c+0x358]',
    'certain', 'FL caller = camera update 0x171c00 (SS 0x14ddd0)')
field(H, 'controller +0x468 (base fov)', 0x468, 0x14f4fd, 0x172f94)
field(H, 'controller +0x358 (fov offset)', 0x358, 0x14f509, 0x172fa0)

# ---------------- 4. TAA
H = 'render_resolution (anti-aliasing)'
func(H, 'kCameraJitterSite', (0x472810, 0x1ed, 0xaf06825055bbe18d), 0x51c840, 17,
     evidence='only caller SS 0x1901a4 / FL 0x1be204 (frame fn SS 0x1901a0 / FL 0x1be200); ratio 0.96',
     note='SS pins 0x1ed (to the stack-check call); FL body 0x205')
func(H, 'kResetWindow (Camera::ResetFrustumWindow)', (0x581a10, 0x34, 0x26b44a8bd1f71035), 0x65c210,
     evidence='identical instructions; window constant SS 0xae45a0 / FL 0xbd3050 = (-1,-1,1,1)')
func(H, 'kPass32Site', (0x1929c0, 0x42e, 0xcd353017f8e6ce48), 0x1c0b40, 20,
     evidence='reads the no-AA byte: SS 0x192c59 or al,[r14+0x1eb8d] | FL 0x1c0e26 or al,[r14+0x1f31d] (the only '
              'other readers are pass 33 SS 0x1958b5/0x195936, FL 0x1c3bc5/0x1c3c46); same frame/epilogue',
     note='fuzzy candidate 0x1c1a20 was wrong; FL adds code (sub rsp 0x818 -> 0x828)')
glob(H, 'kTemporalAaFlag', 0xda07f9, 0x472862, 0x51c892)
glob(H, 'kReevaluate', 0x7e00964, 0x47283a, 0x51c86a)
assert disp_of(ss, 0x4728fd)[0] == 0x1e5 and disp_of(fl, 0x51c934)[0] == 0x225
glob(H, 'kDisableSmaa', 0xe87c85, 0x4728f6, 0x51c92d, add=0x1e5, fl_add=0x225,
     note='SS mov al,[rax+0x1e5] | FL mov al,[rax+0x225] (0x4728fd / 0x51c934)')
assert disp_of(ss, 0x1901d0)[0] == 0x1eb8d and disp_of(fl, 0x1be230)[0] == 0x1f31d
glob(H, 'kNoAa', 0xe86e3d, 0x1901b7, 0x1be217, add=0x1eb8d, fl_add=0x1f31d,
     note='writer SS 0x1901d0 mov [rdx+0x1eb8d],cl | FL 0x1be230 mov [rdx+0x1f31d],cl; rdx = scene-targets object')
assert disp_of(ss, 0x472910)[0] == 0x1c0d8 and disp_of(fl, 0x51c947)[0] == 0x1c338
glob(H, 'kMainCamera', 0x7f2d2b8, 0x472909, 0x51c940, add=0x1c0d8, fl_add=0x1c338,
     note='SS mov rdi,[rax+0x1c0d8] | FL mov rdi,[rax+0x1c338] (also frame fn SS 0x190283 / FL 0x1be2e3)')
field(H, 'kCameraWindow camera +0x5ac', 0x5ac, 0x581a21, 0x65c221)
func(H, 'Camera::BuildProjection (upscaler)', (0x581c90, 0x156, fnv1a64(ss.text[0x581c90:0x581c90 + 0x156])), 0x65c470,
     evidence='unique exact masked match of the whole SS body', note='not hooked; vk_executor_upscale.cpp reads its fields')
field(H, 'camera +0x584 (near)', 0x584, 0x581cac, 0x65c48c)
field(H, 'camera +0x588 (far)', 0x588, 0x581cb4, 0x65c494)
field(H, 'camera +0x5a4 (tan(fov/2), BuildProjection)', 0x5a4, 0x581cc4, 0x65c4a4)
row(H, 'Camera::SetFrustumWindow (comment)', '0x581a50', '0x65c250', 'jitter SS 0x4729db call 0x581a50 | FL 0x51ca12 call 0x65c250', 'certain')
row(H, 'frame fn / jitter caller (comment, "0x1901a0")', '0x1901a0', '0x1be200', 'identical 0x4c-byte body', 'certain')
row(H, 'GPU timer (comment)', '0x196ef0', '0x1c5e80', 'jitter SS 0x47285d / FL 0x51c88d call', 'certain')

# ---------------- 5. mouse camera
H = 'mouse_camera'
func(H, 'kIntegrateSite (camera integrator)', (0x14d6f0, 0x593, 0x724cebb9e1f76f99), 0x171410, 17,
     evidence='only caller FL 0x1720c7 (camera update 0x171c00); same argument registers (rdi cam, rsi params, rdx '
              'params2, rcx, r8/r9 out, xmm0 yaw, xmm1 pitch); ratio 0.921',
     note='SS pins 0x593 (one padding nop); prologue includes "and rsp,-32" as in SS')
field(H, 'kCameraAngles camera +0x330', 0x330, 0x14da47, 0x1717e7)
assert disp_of(ss, 0x14e5ce)[0] == 0x7c0 and disp_of(fl, 0x1738f8)[0] == 0x7d0
row(H, 'kCameraLocks camera +0x7c0', '0x7c0', '0x7d0',
    'SS 0x14e5ce mov eax,[r13+0x7c0]; test al,1 / al,2 after the pad +0x70 call | FL 0x1738f8 mov eax,[r12+0x7d0]; '
    'same tests after the pad +0x70 call (FL moved the stick read into helper 0x1738a0, rdi = the same camera)',
    'certain')
glob(H, 'kOptions', 0xefacf8, 0x14e5f3, 0x173916)
assert disp_of(fl, 0x17391d)[0] == 0x785 and disp_of(fl, 0x17393d)[0] == 0x786
row(H, 'options +0x6d5 / +0x6d6 (camera x/y inversion)', '0x6d5/0x6d6', '0x785/0x786',
    'SS 0x14e5fa cmp byte [rax+0x6d5] / 0x14e626 [rax+0x6d6] | FL 0x17391d [rax+0x785] / 0x17393d [rax+0x786]; '
    'pad getter SS 0x24cba3 movzx word [rax+0x6d5] | FL 0x2a91c3 [rax+0x785]', 'certain')
row(H, 'options second inversion pair (getter B)', '0x6d7/0x6d8', '0x787/0x788',
    'SS 0x24cc33 [rax+0x6d7] | FL 0x2a9253 [rax+0x787]', 'certain')
assert disp_of(fl, 0x16ffec)[0] == 0x1c0 and disp_of(fl, 0x16ffce)[0] == 0x1d0 and disp_of(fl, 0x16ffde)[0] == 0x220
row(H, 'kPitchLimits in params (arg 2)', 'params+0x1c0', 'params+0x1c0',
    '0x14c270/0x16ff90: SS 0x14c2cc [rsi+0x1c0] | FL 0x16ffec [rsi+0x1c0]', 'certain')
row(H, 'kPitchLimits in params2 (arg 3)', 'params2+0x1c0', 'params2+0x1d0',
    'SS 0x14c2ae [r13(=rdx)+0x1c0] | FL 0x16ffce [r13+0x1d0]; stick fn SS 0x14c135 [params2+0x1c0] | FL 0x16fe5b '
    '[params2+0x1d0]', 'certain',
    'params and params2 no longer share a layout in FL: p1 = params+0x1c0, p2 = params2+0x1d0')
row(H, 'kPitchLimitsAuto params2+0x210', 'params2+0x210', 'params2+0x220',
    'SS 0x14c2be [r13+0x210] | FL 0x16ffde [r13+0x220]', 'certain')
row(H, 'stick / automatic camera helpers (comments)', '0x14a810 / 0x14c270', '0x16e470 / 0x16ff90',
    'integrator SS 0x14d99d / 0x14d9c2 | FL 0x17173d / 0x171762', 'certain')
row(H, 'camera update (comment)', '0x14ddd0', '0x171c00', 'holds the SetFov call and the only integrator call',
    'certain', 'FL split the SS update into helpers (stick read 0x1738a0)')
assert fl.u64(0xdd2478) == 0x2a91a0 and fl.u64(0xdd2480) == 0x2a9230 and fl.u64(0xdd2358 + 0x70) == 0x2a9bb0
row(H, 'pad vtable (comment)', '0xcddf70', '0xdd2358',
    'ctor lea SS 0x24ea46 -> 0xcddf60 (+0x10) | FL 0x2ab00a -> 0xdd2348 (+0x10); slots 0..26 equal bodies',
    'certain', 'FL inserts 2 virtual slots after +0xd0')
row(H, 'kVtableSlotA / kOriginalA', '0xcde080 / 0x24cb80', '0xdd2478 / 0x2a91a0',
    'slot +0x110 -> +0x120; getter body equal except options +0x785', 'certain', 'SSPC_MOUSE_PROBE diagnostics')
row(H, 'kVtableSlotB / kOriginalB', '0xcde088 / 0x24cc10', '0xdd2480 / 0x2a9230',
    'slot +0x118 -> +0x128; getter body equal except options +0x787', 'certain', 'SSPC_MOUSE_PROBE diagnostics')
row(H, 'pad vtable +0x70 stick read (comment)', '0x24d740', '0x2a9bb0',
    'slot 14 in both, bodies equal; called at FL 0x1738f5', 'certain')
row(H, 'watch offsets pad +0x14/+0x18/+0x1c/+0xd4', '0x14/0x18/0x1c/0xd4', 'same (stick state +0x14 checked)',
    '+0x70 reader uses [state+rcx*4+0x14] identically; +0xd4 not checked', 'likely', 'SSPC_MOUSE_WATCH diagnostics')

# ---------------- 6. game fixes
H = 'game_fixes'
S = bytes([0xF0, 0xFF, 0x4F, 0x10, 0x83, 0x7F, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7F, 0x14, 0x00, 0x74, 0x01, 0xC3, 0x48,
           0x8B, 0x07, 0xFF, 0x60, 0x10])
W = bytes([0xF0, 0x41, 0xFF, 0x4E, 0x14, 0x41, 0x83, 0x7E, 0x14, 0x00, 0x75, 0x1F, 0x41, 0x83, 0x7E, 0x10, 0x00, 0x74,
           0x0F])
assert [m.start() for m in re.finditer(re.escape(S), fl.text)] == [0x1fe540]
assert [m.start() for m in re.finditer(re.escape(W), fl.text)] == [0x1fe4e2]
row(H, 'kStrongRva', '0x1be900', '0x1fe540', '23-byte pattern verbatim, unique in FL text', 'certain')
row(H, 'kWeakRva', '0x1be8a2', '0x1fe4e2', '19-byte pattern verbatim, unique (weak release SS 0x1be820 / FL 0x1fe460)',
    'certain')
for n, s, f in (('kWeakDone', 0x1be8cd, 0x1fe50d), ('kWeakDestroy', 0x1be8c4, 0x1fe504),
                ('kWeakNotify', 0x1be8b5, 0x1fe4f5)):
    assert f - 0x1fe4e2 == s - 0x1be8a2
    row(H, n, hex(s), hex(f), 'same layout (+0x3fc40): jne -> done 0x1fe50d, je -> destroy 0x1fe504, fallthrough -> '
        'notify 0x1fe4f5 (decoded)', 'certain')
row(H, 'rsp alignment at the weak site', '16-aligned', '16-aligned',
    'FL 0x1fe460: push rbp + 5 pushes + sub rsp,0x28 (identical to SS) -> rsp % 16 == 0 at 0x1fe4e2', 'certain')

json.dump(rows, open(OUT, 'w'), indent=1)
print(len(rows), 'rows ->', OUT)
