# First Light sites for the Second Son engine hooks

Second Son eboot `E:\shdps4\CUSA00004\eboot.bin` (SS) mapped to First Light eboot `E:\inFAMOUS-First-Light-PC-Edition\eboot.bin` (FL). Text vaddr 0, so an RVA is a text index.

How the values were found:
- Each FL function was paired with its SS function through a call graph that was itself verified (the same caller or callee in the paired code). The two bodies were then compared side by side, aligned on mnemonics (`research/fl_sbs.py`).
- Globals are the RIP target of the paired FL instruction. Where the code reads `[lea_base + disp]`, the FL global is the FL base plus the FL displacement.
- `research/fl_sites_build.py` recomputes every value from both eboots and asserts the evidence. It writes `research/fl_sites.json`.

**Size and FNV convention.** "FL size" means the function body from its entry up to the end of its last instruction. Trailing `nop` padding is not included. The FNV-1a 64 is taken over `text[rva : rva+size]`.

SS pins are not consistent:
- `0x300b00` is pinned 0x2bb, which ends inside `pop r15`.
- `0x18e210` is pinned 0x2f9, which runs into the next function.
- `0x14d6f0` is pinned 0x593, which includes one nop.

For FL, use the body sizes below. The JSON also lists the padded size and its FNV, in case you prefer to pin up to the next function.

**Prologue rule.** A prologue is the smallest set of whole instructions that is at least 14 bytes. For each site, every instruction in that range was decoded and checked:
- no RIP-relative operand;
- no branch or call;
- the range ends exactly on an instruction boundary;
- no branch inside the function targets the range;
- no rel8 or rel32 branch anywhere in FL text targets the range (`research/fl_branch_into.py`).

The global scan had one hit, at `0x2e7a7a`. It decodes from the middle of an instruction, so it is not a real branch.

## 1. Game menu (`game_menu.cpp`)

| constant | SS value | FL value | evidence | confidence |
|---|---|---|---|---|
| kOpenedSite | {0x27db10, 0xd0, 0x8109e3344cdd9cf3}, 17 | **{0x2e75a0, 0xe2, 0x2a2ccf44d65abfae}, 17** | Same body: test and set the active byte, sound-pause counter inc (SS 0x4879b0 / FL 0x535850), stride imul. Prologue `55 / 4889e5 / 4156 / 53 / 4889fb / 80bb47060000 00` (cmp byte [rbx+0x647],0). | certain |
| kClosedSite | {0x27e210, 0xdf, 0x33ff129a2ada5846}, 16 | **{0x2e7ad0, 0xfa, 0x59fc0e5d56670225}, 17** | Same body (counter dec SS 0x4879d0 / FL 0x535870). **Prologue is 17, not 16**: FL pushes r14 and rbx where SS pushed rbx and rax. | certain |
| kScreenActive (u8) | 0x61b | **0x647** | SS 0x27db1a `cmp byte [rbx+0x61b],0` / FL 0x2e75aa `[rbx+0x647]` | certain |
| kScreenKind (u32) | 0x620 | **0x64c** | SS 0x27db60 `mov esi,[rbx+0x620]` / FL 0x2e75f0 `[rbx+0x64c]`. Reflection table: SS 0xd1b2d0 and FL 0xe10820 both hold `{"SOUND_PAUSE_KIND", 0x620 / 0x64c}` with the same type hash 0x61ee5153abc8ef96 and count 0xa. | certain |
| screen index field | 0x610 | **0x5d4** | SS 0x27db6b / FL 0x2e75fb `movsxd rax,[rbx+…]` | certain |
| element stride | 0x950 | **0x920** | SS 0x27db78 / FL 0x2e7608 `imul rdi,rax,…` | certain |
| element array global | 0xd9de60 | 0xe97058 | RIP target of `add rdi,[rip+…]` (SS 0x27db7f / FL 0x2e760f) | certain |
| kSettingsKind | 7 | **7 (unverified)** | The constructor default is 7 in both builds: SS 0x27d849 `mov dword [rbx+0x620],7`, FL 0x2e7244 `mov dword [rbx+0x64c],7`. Each is the only immediate write of the field in its eboot. Other values come from menu data through the reflected `SOUND_PAUSE_KIND` property (enum 0..9; the consumers clamp to 9 or below in both). | likely |
| DumpScreen window | [0x600..0x640] | [0x5c0..0x680] suggested | The FL constructor writes the screen fields at 0x5c8..0x670 (SS 0x5f8..0x648). | diagnostics |

Other screen fields also move by +0x2c (FL 0x2e7130 vs SS ctor): 0x619→0x645, 0x61a→0x646.

## 2. Render switch and HUD (`render_resolution.cpp`)

| constant | SS value | FL value | evidence | confidence |
|---|---|---|---|---|
| kFrameBeginSite | {0x1f0be0, 0x5aa, 0x6273d28a515609b7}, 20 | **{0x237670, 0x5aa, 0x13ecaea8b414b6f9}, 20** | 319/319 instructions, mnemonic ratio 1.0. The only operand change is options +0x6f8 → +0x7a8. Prologue is `push rbp … sub rsp,0x88`; the next instruction is RIP-relative and is not moved. | certain |
| kConfigureTarget | {0x300b00, 0x2bb, 0x4489645ef331292a} | **{0x382420, 0x2c3, 0xcb8210727671d826}** | FrameBegin SS 0x1f0ecd `call 0x300b00` / FL 0x23795d `call 0x382420` (3 calls each); exact masked match | certain |
| kConfigureSceneTargets | {0x18e210, 0x2f9, 0x804f07e3bc802864} | **{0x1bc1d0, 0x2ba, 0x4d6fb6a99a3129fa}** | FrameBegin SS 0x1f102b / FL 0x237abb `call`; ratio 0.984 | certain |
| kSceneTargets | 0xe682b0 | **0xf84740** | `lea rdi,[rip+…]` just before that call (SS 0x1f1024 / FL 0x237ab4) | certain |
| device global (not a hook constant) | 0xf1bcd0 | 0x11362d0 | SS 0x1f0dc2 / FL 0x237852 lea | certain |
| kQuarterHeightFlag | 0x1d0cbd5 | **0x1f271d5** | `cmp byte [rdi+0xdf0f05],0` with rdi = device global (SS 0x1f0df2 / FL 0x237882). 0x11362d0 + 0xdf0f05. | certain |
| kSizeTable and every +0xdf0f14…+0xdf0f7c pair | as in the file | **unchanged** | FrameBegin writes the same displacements (e.g. SS 0x1f0d82 / FL 0x237812 `[r13+0xdf0f14]` … 0x1f0ebf / 0x23794f `+0xdf0f54`) | certain |
| device +0xC/+0x10, +0x8c4, +0x20a*8 (0x1050), +0x2fc*8 (0x17e0) | — | **unchanged** | `lea rdi,[r13+0x8c4]` / `[r13+0x1050]` / `[r13+0x17e0]` in FL FrameBegin. Same mov bytes at 0x2369d7. | certain |
| kScreenWidth | 0x1d0ccc0 | **0x1f272c0** | VideoOut `lea r12,[rip+…]` (SS 0x1f03ea / FL 0x236e7a); the screen site stores `[r12]` = w | certain |
| kScreenHeight | 0x1d0ccc4 | **0x1f272c4** | `lea r15,[rip+…]` (SS 0x1f03e3 / FL 0x236e73); `[r15]` = h | certain |
| kMarginX | 0x1d0ccc8 | **0x1f272c8** | SS 0x1f0480 / FL 0x236f10 lea | certain |
| kMarginY | 0x1d0cccc | **0x1f272cc** | SS 0x1f04ab / FL 0x236f3b lea | certain |
| resolution sites | 0x1eff47 / 0x1eff55 / 0x1f03f8 | 0x2369d7 / 0x2369e5 / 0x236e88 | Each byte pattern is unique in FL (already in the file). | certain |
| comments: VideoOut init, margin recompute | 0x1eff00, 0x1f2a60, 0x1f2b60 | 0x236990, 0x239500, 0x239600 | Function starts holding the kMarginX lea | certain |

FL screen globals = device global + 0xdf0ff0, the same relation as in SS.

## 3. FOV (`render_resolution.cpp`)

| constant | SS value | FL value | evidence | confidence |
|---|---|---|---|---|
| kSetFovSite | {0x580580, 0xc1, 0xef658398f5aad2a4}, 17 | **{0x65af10, 0xaf, 0x30d2052b9c584467}, 17** | The same 15 call sites in the same order (14 `call` + 1 `jmp`; SS 0x14f512 ↔ FL 0x172fa9). The FL body equals the SS body from 0x58059b on. Prologue `55 / 4889e5 / 53 / 50 / 4889fb / c5f82e8390050000` (vucomiss [rbx+0x590]). | certain |
| writes +0x590 / +0x5a4 / +0x5a0 / +0x598 | as SS | **unchanged** | SS 0x5805a9/0x5805be/0x5805d1/0x580624 ↔ FL 0x65af27/0x65af3c/0x65af4f/0x65afa2 | certain |
| kPlayerSetFovReturn | 0x14f517 | **0x172fae** | SS `vmovss xmm0,[r13+0x468]; vaddss xmm0,[r13+0x358]; call 0x580580` ↔ FL 0x172f94…0x172fa9 (same offsets, r15), `call 0x65af10` | certain |

The fuzzy match "looked off" because FL SetFov has no `vmovss [rbx+0x5d4],xmm0; mov dword [rbx+0x5dc],0` head. FL never writes those two fields. The probe only rescales xmm0 for one return address, so it is unaffected.

## 4. Anti-aliasing / TAA (`render_resolution.cpp`, `vk_executor_upscale.cpp`)

| constant | SS value | FL value | evidence | confidence |
|---|---|---|---|---|
| kCameraJitterSite | {0x472810, 0x1ed, 0xaf06825055bbe18d}, 17 | **{0x51c840, 0x205, 0x1403a59b70323dd4}, 17** | Only caller SS 0x1901a4 / FL 0x1be204. Ratio 0.96. Prologue `push rbp … sub rsp,0x18`; the next instruction is RIP-relative and is not moved. | certain |
| kResetWindow | {0x581a10, 0x34, 0x26b44a8bd1f71035} | **{0x65c210, 0x34, 0xba919ade99796653}** | Identical instructions. Window constant SS 0xae45a0 / FL 0xbd3050 = (-1,-1,1,1). | certain |
| kPass32Site | {0x1929c0, 0x42e, 0xcd353017f8e6ce48}, 20 | **{0x1c0b40, 0x54c, 0xbec00e949ba22471}, 20** | It reads the no-AA byte (SS 0x192c59 `or al,[r14+0x1eb8d]` / FL 0x1c0e26 `[r14+0x1f31d]`); the other readers are pass 33 only. Same frame and epilogue (`sub rsp,0x818` → `0x828`). Prologue `push rbp … sub rsp,0x828`. The fuzzy candidate 0x1c1a20 was **wrong**. | certain |
| pass 32 writer ("0x1901a0") | 0x1901a0 | **0x1be200** | Identical 0x4c-byte body; it calls the jitter, then writes the no-AA byte | certain |
| kTemporalAaFlag | 0xda07f9 | **0xe9c829** | Jitter SS 0x472862 / FL 0x51c892 `lea r15,[rip+…]` | certain |
| kReevaluate | 0x7e00964 | **0x806be14** | Jitter SS 0x47283a / FL 0x51c86a `mov eax,[rip+…]` | certain |
| kDisableSmaa | 0xe87c85 | **0xfae035** | SS `lea rax,0xe87aa0; mov al,[rax+0x1e5]` / FL `lea rax,0xfade10; mov al,[rax+0x225]` (0x51c92d/0x51c934). **The displacement changed.** | certain |
| kNoAa | 0xe86e3d | **0xfa3a5d** | Writer SS 0x1901d0 `mov [rdx+0x1eb8d],cl`, rdx = 0xe682b0 / FL 0x1be230 `[rdx+0x1f31d]`, rdx = 0xf84740 | certain |
| kMainCamera | 0x7f2d2b8 | **0x81a41f8** | Jitter SS `lea rax,0x7f111e0; mov rdi,[rax+0x1c0d8]` / FL `lea rax,0x8187ec0; mov rdi,[rax+0x1c338]` (0x51c940/0x51c947). Also frame fn 0x1be2e3. | certain |
| kCameraWindow +0x5ac | 0x5ac | **0x5ac** | ResetFrustumWindow `vmovups [rbx+0x5ac]` in both | certain |
| camera +0x584 / +0x588 / +0x5a4 (upscaler) | as SS | **unchanged** | Camera::BuildProjection SS 0x581c90 ↔ FL 0x65c470 (unique exact masked match of the whole 0x156 body) | certain |
| comments: SetFrustumWindow, GPU timer | 0x581a50, 0x196ef0 | 0x65c250, 0x1c5e80 | Calls from the jitter | certain |

`vk_executor_upscale.cpp` keeps the camera read behind `#if !defined(SSPC_PRODUCT_FIRST_LIGHT)`. For FL it would read `base + 0x81a41f8`, and the offsets stay +0x584 / +0x588 / +0x5a4.

## 5. Mouse camera (`mouse_camera.cpp`)

| constant | SS value | FL value | evidence | confidence |
|---|---|---|---|---|
| kIntegrateSite | {0x14d6f0, 0x593, 0x724cebb9e1f76f99}, 17 | **{0x171410, 0x612, 0x40e7fea5cb4aa405}, 17** | Only caller FL 0x1720c7 (camera update 0x171c00). Same argument registers (rdi camera, rsi params, rdx params2, rcx, r8/r9 outputs, xmm0 yaw, xmm1 pitch). Ratio 0.921. Prologue is the 7 pushes plus `and rsp,-32`, as in SS. | certain |
| kCameraAngles | camera +0x330 | **+0x330** | `vmovups xmm0,[rbx+0x330]` SS 0x14da47 / FL 0x1717e7 | certain |
| kCameraLocks | camera +0x7c0 | **+0x7d0** | SS 0x14e5ce `mov eax,[r13+0x7c0]; test al,1 / al,2` after the pad +0x70 call. FL has the same pattern at 0x1738f8 `[r12+0x7d0]`, in the new helper 0x1738a0 that the update calls with the same camera. | certain |
| kOptions | 0xefacf8 | **0x1112cf0** | SS 0x14e5f3 / FL 0x173916 lea (also the FrameBegin and pad getters) | certain |
| options +0x6d5 / +0x6d6 | 0x6d5 / 0x6d6 | **0x785 / 0x786** | SS 0x14e5fa / 0x14e626 ↔ FL 0x17391d / 0x17393d `cmp byte [rax+…],0`; getter SS 0x24cba3 ↔ FL 0x2a91c3 | certain |
| options +0x6d7 / +0x6d8 (comment) | 0x6d7 / 0x6d8 | 0x787 / 0x788 | Getter SS 0x24cc33 ↔ FL 0x2a9253 | certain |
| kPitchLimits, params (arg 2) | params +0x1c0 | **params +0x1c0** | SS 0x14c2cc ↔ FL 0x16ffec `vmovups xmm0,[rsi+0x1c0]` | certain |
| kPitchLimits, params2 (arg 3) | params2 +0x1c0 | **params2 +0x1d0** | SS 0x14c2ae `[r13+0x1c0]` ↔ FL 0x16ffce `[r13+0x1d0]`, with r13 = rdx = params2. The stick fn has the same change (SS 0x14c135 ↔ FL 0x16fe5b). | certain |
| kPitchLimitsAuto | params2 +0x210 | **params2 +0x220** | SS 0x14c2be ↔ FL 0x16ffde | certain |
| comments: 0x14a810 / 0x14c270 / 0x14ddd0 / 0x24d740 | — | 0x16e470 / 0x16ff90 / 0x171c00 / 0x2a9bb0 | Call pairs in the integrator / update | certain |
| pad vtable (comment) | 0xcddf70 | 0xdd2358 | Ctor lea SS 0x24ea46 → 0xcddf60 (+0x10) / FL 0x2ab00a → 0xdd2348 (+0x10). Slots 0..26 have equal bodies. **FL inserts 2 virtual slots after +0xd0.** | certain |
| kVtableSlotA / kOriginalA | 0xcde080 / 0x24cb80 | **0xdd2478 / 0x2a91a0** | Slot +0x110 → +0x120; the FL file holds 0x2a91a0 there (unrelocated, as in SS) | certain |
| kVtableSlotB / kOriginalB | 0xcde088 / 0x24cc10 | **0xdd2480 / 0x2a9230** | Slot +0x118 → +0x128 | certain |
| watch offsets +0x14/+0x18/+0x1c/+0xd4 | — | same (likely) | The +0x70 reader uses `[state+rcx*4+0x14]` identically; +0xd4 not checked | likely |

In FL, params and params2 no longer share a layout:
- The integrator's params-only reads moved by −0x20: `[r14+rax*4+0x408]` became `+0x3e8`.
- The params2 reads moved by +0x10.

So in IntegrateProbe use `p1 = params+0x1c0`, `p2 = params2+0x1d0`, `p3 = params2+0x220`.

## 6. Game fixes (`game_fixes.cpp`)

| constant | SS value | FL value | evidence | confidence |
|---|---|---|---|---|
| kStrongRva (23-byte pattern) | 0x1be900 | **0x1fe540** | Verbatim, unique in FL text | certain |
| kWeakRva (19-byte pattern) | 0x1be8a2 | **0x1fe4e2** | Verbatim, unique, inside the weak release FL 0x1fe460 (SS 0x1be820). The whole function aligns 1:1. | certain |
| kWeakDone | 0x1be8cd | **0x1fe50d** | `jne` target, decoded | certain |
| kWeakDestroy | 0x1be8c4 | **0x1fe504** | `je` target (`mov rax,[r14]; mov rdi,r14; call [rax+0x10]`) | certain |
| kWeakNotify | 0x1be8b5 | **0x1fe4f5** | Fallthrough (`mov rax,[r14]; lea rsi,[rbp-0x40]; …; call [rax+0x18]`) | certain |
| rsp alignment at the weak site | 16-aligned | **16-aligned** | FL 0x1fe460: `push rbp` + 5 pushes + `sub rsp,0x28`, identical to SS, so rsp % 16 == 0 at 0x1fe4e2 | certain |

All branches into either patched range are internal ones that the patch overwrites. The weak continuation uses only r14 and the rbp frame, both preserved across the host call.

## Open questions / risks

1. **kSettingsKind.** The value is data-driven (`SOUND_PAUSE_KIND`), so it cannot be settled statically. 7 is the constructor default in both builds and was the SS settings screen's value. To confirm on FL, run once with the FL probes and read the existing log line `game menu: pausing screen … opened (kind N)` while opening Options from the pause menu during play. Setting `SSPC_DIAG_GAME_MENU=1` and moving the dump window to `[0x5c0..0x680]` also shows the whole screen-field block.
2. **Close probe prologue.** It must be 17 on FL. Reusing SS's 16 would cut `cmp byte [rbx+0x647],0` in half.
3. **Displacement changes.** Three globals now need FL-specific displacements: kDisableSmaa (`+0x225`), kNoAa (`+0x1f31d`) and kMainCamera (`+0x1c338`). The FL addresses above already include them.
4. **Mouse-camera layout changes.** The locks field moved to +0x7d0, params2's limits to +0x1d0 / +0x220, and the inversion bytes to +0x785 / +0x786. If the SS constants were kept, the lock bits, pitch clamps and inversion would read wrong data. Nothing would crash, but behaviour would be silently wrong.
5. **FL SetFov.** It no longer stores +0x5d4 / +0x5dc. Nothing in the hook relies on them.
6. **Pin sizes.** The FL hashes in this report are over the code body (no padding). If a port reuses the SS habit of odd pin lengths, recompute the FNV over that exact length; the JSON has the padded variant too.
7. **Prologues.** No probe prologue contains a RIP-relative operand or a relative branch. FrameBegin, CameraJitter and Pass32 each have a RIP-relative `mov` immediately after the prologue, so the lengths must be exactly 20 / 17 / 20; they are.

Scripts: `research/fl_lib.py` (loader, function end, prologue checks), `fl_sbs.py` (side by side), `fl_xref.py`, `fl_callers.py`, `fl_disp.py`, `fl_masked.py`, `fl_branch_into.py`, `fl_kind.py`, `fl_funcs.py`, `fl_sites_build.py` (rebuilds the JSON with asserts). Saved listings: `sbs_videoout.txt`, `sbs_integrator.txt`, `sbs_autocam.txt`, `sbs_stickvel.txt`, `sbs_camupdate.txt`, `sbs_pass32.txt`.
