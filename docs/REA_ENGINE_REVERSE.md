# SecondSonPC: REA + Ghidra - the engine's particles, its GPU memory and our native backend

Started 2026-10-17 on `feature/pc-edition` (base `ec0ab8d`). Goal: use the original game's code to find what our Native
CP / Vulkan backend does that the PS4 never did, and remove it without changing the picture or the game's logic.

## 1. Tools

| Tool | Version / place | Result |
|---|---|---|
| REA (`rea-agents`, morluto/rea) | 6.5.0, `npx -y rea-agents@6.5.0 setup --client claude_code` (npm cache `E:\tools\npm-cache`) | MCP `rea` registered in `~/.claude.json` (backup `.rea.backup`), skill in `~/.claude/skills/reverse-engineer-anything`; `claude mcp list`: connected; `rea doctor --client claude_code --provider ghidra`: healthy |
| Ghidra | 12.1.4, `E:\tools\ghidra` (the only version REA accepts) | REA's Windows provider (experimental P0) |
| JDK | Temurin 21.0.10 | accepted by REA and Ghidra |
| Node / npm | 24.15.0 / 11.12.1 | supported by REA (24.11+) |
| pyghidra | `E:\tools\ghidra-mcp\.venv`, scripts `tools/native-reconstruction/ghidra/run.py` | the analysis of the game's code |

**What REA does with the game.** Checked by running it, not by reading its documentation:

- `rea analyze E:\tools\7zip\7z.exe --provider ghidra` (a PE): a full evidence bundle in 75 s (`E:\research\rea\analyze_7z_pe.json`). REA works.
- `rea inspect-artifact E:\shdps4\CUSA00004\eboot.bin`: `unsupported_target / file_format_rejected`. The eboot is a PS4 SELF (magic `4F 15 3D 1D`), not a PE.
- `rea inspect-binary-layout eboot.bin`: `capability_unavailable` ("the pwntools artifact profile supports Linux x64 only"). A SELF is not a plain ELF either.
- `rea analyze E:\tools\ghidra-projects\scripts\SecondSon.gpr`: `file_format_rejected` (a Ghidra project is not an input).

So REA on Windows cannot open the PS4 binary. The game's code was analysed directly in the existing Ghidra project
(`E:\tools\ghidra-projects\SecondSon`, program `/eboot`, image base 0, loaded earlier through a PS4 loader; blocks
`text 0..0xcc55b7`, `data 0xda0000..`), with pyghidra queries (`E:\research\rea\q\*.py`, run through `run.py`; while
pyghidra-mcp holds the project, `SSPC_GHIDRA_PROJECT_DIR=E:\tools\ghidra-projects\scripts` - its copy). No virtual machine
or second OS was set up: a Linux REA host would not read a SELF either.

## 2. The particles' frame (Ghidra, confirmed by disassembly and cross-references)

Main loop `0x29c8d0`, in order (only the particle parts):

| Address | What it does |
|---|---|
| `0x3ee7d0` (`DAT_07d3eb10`) | the frame's particle update: waits for its jobs, a **30 MiB arena per frame, two of them** (`param[5]` flips `1 - param[5]` each frame, arena = `base + param[5] * 0x1e00000`), the 12 MiB pool `0x20000 x 0x60` (`DAT_07d3eb40`) taken from it, emitter updates (`sspc_particles_update`), emitter dispatches (`0x3f09a0`, `0x3f1470`, `0x3f1a60`), in-queue barriers (`0x1694e0`: EOS -> WAIT_REG_MEM on itself), then `0x1691e0` |
| `0x1691e0` | the compute submission: `++q[+0x1b88]`, RELEASE_MEM writes that value into the label `labels[q[+0x1b8c]]` (`DAT_00e17e80`), `sceGnmDingDong` |
| `0x3edef0` | **wait A**: `0x1695e0` (spin until the label reaches `q[+0x1b88]`), then for every emitter of two lists `0x3f3ed0`, then jobs `0x3ee190` |
| `0x3f3ed0` | per emitter, **the CPU reads GPU-written emitter state** `*(e+0xf0)`: `+4` the particle count (copied to `*(e+0xe8)+8`, read by its draws), `+0x28` the count of **particle lights**: slots in the frame's light list (`0x16dd50` on `DAT_00e17f90`: records 0xb0 + 0x50 bytes) filled by a DMA from the emitter's GPU buffers (`0x11fc0` -> `0x63c2c0`), then `0x1691e0` again; emitters with `cfg+300 != -1` queue a job |
| `0x3ee190` (job) | reads `state+8` = {offset, count} and passes the CPU `DAT_07d3eb40 + offset * 0x60` - the pool records of that emitter (the ~36 KiB of 12 MiB the CPU reads, FB-7) |
| `0x3ee470` | waits for those jobs, then **wait B**: a tail call (`JMP 0x1695e0` at `0x3ee4e7`; return address in the main loop `0x29cd5b`) |
| `0x196c30` -> `0x1f1e30` | SubmitFrame; its first steps close the frame's light list (`0x16df30`: terminator record, count) |

**Wait B orders GPU work, not CPU work**: after it the CPU reads nothing the DMA wrote; the frame's graphics (tiled
lighting) reads the light list the DMA filled. It is a GPU -> GPU dependency (compute DMA before graphics) expressed as a
CPU block. **Wait A is a real CPU dependency** (the counts and light counts the CPU uses at once).

## 3. The GPU memory the engine uses, against our backend

New diagnostic `SSPC_DIAG_LARGE_BINDS=1` (off): the bindings `BufferCache::Prepare` leaves on the host import, by range and
reason (`gpu_native.txt`), raw V# of bindings over 256 MiB and of `SSPC_DIAG_WATCH_BIND=<addr>:<size>` (log), write-back
and kept-back blocks per MiB of address, the physical layout of the heaps (REA-4).

Village, 90 s (`logs/rea/rea1_diag*`):

| Finding | Evidence |
|---|---|
| **The engine's vertex-data heaps**: one V# `71258500 00020002 4e9b6c56 20014004` (base `0x271258500`, stride 2, 0x4e9b6c56 records = **2.5 GiB**, formatted, read-only) and one over `0x248002000` (542 MiB), from user data 36..39 of nearly every program | 862 056 + 238 088 bindings / 90 s on the host import (larger than the 16 MiB cache limit) |
| The 542 MiB heap crosses two physical backing runs (`0x248000000 + 512 MiB`, `0x268000000 + 2.9 GiB`): imported through the guest's own pages, its whole window was never tracked again (`NeverArm`) | ~1.5 M small constant-buffer bindings (`0x2481xxxxx`, `0x2488xxxxx`, 20-30 KiB) a run read over PCIe |
| The two runs are **host-contiguous** (`host 0x10048010000 + 512 MiB = 0x10068010000`) | REA-4 log |
| **76 % of the changed-words write-back** is not the particle pools but `0x2edc12000`, 6.25 MiB, written every frame by the **tiled lighting CS `0x2edafa400`** (V# stride 2, 0x320000 records) and read by ~28 pixel shaders: a per-tile light index list, GPU-only | 18.9 of 24.8 GB compared / 90 s; kept-back blocks 0x2edc..0x2ee2 |
| It lies inside the 2.5 GiB heap's address range: every heap reading (host import) wrote it back first | lazy WB: 302 300 of 302 711 kept-back blocks written back by "a GPU reader of the host import" |

## 4. Changes (all behind switches, off by default)

| Switch | What | Correctness checks |
|---|---|---|
| `SSPC_REA_HEAP_READS=1` (with `SSPC_FB_LAZY_WB=1`) | host-import readings by a heap (read-only, >= 256 MiB) do not write back GPU-written data in their range | `SSPC_REA_POISON=1`: the host copy of every kept-back block filled with 0xFFFFFFFF until a reader writes it back - village and beach pictures intact, `[Error]` 0; kept-back blocks ~900 a run (was ~300 000 written back again) |
| `SSPC_REA_MERGE_RUNS=1` | `GuestBuffers::Get` joins host-contiguous backing runs: the 542 MiB heap imported through the backing view, its window trackable - its constants cached in video memory | windows through the guest's pages 0 (was 1); bindings left on the import: the two heaps only; village GPU 18.7 ms in a single run |
| `SSPC_REA_DEFER_WAIT_B=1` | wait B does not block the game: `Gnm::SetGraphicsGate` puts WAIT_REG_MEM(label >= target) in front of the next graphics submission (the processor's cross-queue wait, as the game's own waits) | village: >1000 gates a run, `[Error]` 0, no device loss; beach: wait B already met (no gate) |

## 5. Results

A/B `logs/p53/rea4_ab1` (one exe, interleaved, 2 rounds, Native, unlimited pacing, 90 s; `rea_merge` =
`SSPC_REA_MERGE_RUNS=1`, `rea_all` = + `SSPC_FB_LAZY_WB=1` + `SSPC_REA_HEAP_READS=1`). `[Error]` 0, no device loss:

| | base | rea_merge | rea_all |
|---|---|---|---|
| village avg FPS | 38.0 / 39.0 | 40.6 / 38.9 | 37.2 / 39.4 |
| village GPU busy ms | 21.0 / 20.8 | 18.8 / 19.2 | **16.4 / 16.3** |
| village CP ms | 22.1 / 21.2 | 20.6 / 21.8 | 24.5 / 23.1 |
| beach avg FPS | 46.4 / 53.1 | 49.5 / 50.5 | 45.3 / 53.0 |
| beach GPU busy ms | 13.6 / 13.8 | 12.9 / 12.7 | **11.7 / 11.6** |
| harbour avg FPS | 38.8 / 48.4 | 40.9 / 47.2 | 47.1 / 47.3 |
| harbour GPU busy ms | 15.7 / 15.7 | 14.5 / 14.5 | **13.2 / 13.4** |

**The GPU's work falls 15-22 % in every scene (village -4.5 ms); the frame rate does not move**: every scene is held by
the command processor (19-25 ms this evening - slower than the 18 ms of FB-7's series, cause not found: no other process
took CPU, sampled), and the run-to-run spread (up to 8 FPS) is larger than any effect. In the village `rea_all` adds ~2
ms of CP (the lazy write-back's bookkeeping, as in FB-7). All three stay off by default; the GPU headroom is real and
waits for the command processor.

`SSPC_REA_DEFER_WAIT_B=1`, single village run: 39.2 FPS (base ~39.5-40.3), CP 23 ms, GPU 21 ms - the game no longer
blocks in wait B, but the frame is then held by the command processor and the GPU (as CP-8 predicted: ~48 FPS at most
with the compute waits gone). Kept off; useful once those limits fall.

## 6. Rejected / not built

- REA on the PS4 SELF (format not admitted on Windows; a Linux host would not read a SELF either).
- Moving wait A: the CPU uses its data at once; a one-frame-late read would desynchronise the draws' counts from the
  pools (the arenas flip every frame).
- Extending the P5.3 native frontend to more passes (survey of `docs/native-reconstruction/*`): no measured gain anywhere,
  ~3500 of ~4700 stages a frame read unique per-object bytes; the way on is binding by the engine's table layout without
  hashing content - its own stage.
- Multi-threaded Vulkan recording (design survey): only the command emission (~25-35 % of thread 2) can run in parallel;
  REC (`feature/uncharted-bbport-optimizations`) already measured -14..-19 % thread 2 time without proven FPS - only
  worth it once thread 1 is shorter.

## 7. Next

1. The 2.5 GiB heap in video memory (all of it GPU-mapped, one backing run): vertex data read from VRAM instead of
   PCIe, and the light list's write-back gone with it; ~3 GiB of a 6 GiB card (1.6 GiB used now).
2. Thread 1: binding by the engine's table layout (pass 1, 12, 14) instead of the generic SRT walk.
3. Then multi-threaded recording.

## 8. Materials and shaders from the game files (static, no run)

Goal: know every draw's shader inputs before the game starts, so the precompiler is not limited to pipelines a
previous run created (`docs/OFFLINE_SHADER_PRECOMPILATION.md` §6 item 3). Research tools in `E:\research\rea\q\`
(`material_db.py`, `shader_structs.py`, `validate_material.py`, `predict_runtime.py`, `check_fs_runtime.py`); the
game files are only read.

**Engine (Ghidra).** `0x4468e0(X)` creates, for every pass of a program object X, the runtime material through the
class table `DAT_00ce56b8[pass+0x20]` and calls `0x447190`, which builds the pass's texture/buffer API-slot tables;
`0x448640` binds them per draw (descriptor = mesh instance +0x28 + i*8 dwords + 8); `0x445be0` binds the engine's
global textures. `0x4468e0` also rewrites the PS struct's `DB_SHADER_CONTROL` (+0x30): bit 0x40 -> 0x60, 0x900 -> 0x920.

**Package layout (KCAP).**
- Program object X (found as the owner of a pass array, 22 518 in the game; the u64 at +0x18 is a per-program hash,
  not a class): +0x2c ntex, +0x30 nbuf, +0x80 passes (stride 0xb8), +0x88 npass, +0xc0 buffer records (0x20: u64 name,
  +8 API slot), +0xd0 texture records (0x10: u64 name, +0xb API slot), +0xe0 material texture ids, +0xf0 buffer ids.
- Pass element E: DIC fixup at E+0x20 (the runtime pass class index; the class hash says which render pass), +0x30
  fetch shader, +0x38 VS struct, +0x40 PS struct, +0x88/+0x90 buffer pairs {u8 stage, u8 id}, +0x98/+0xa0 texture
  pairs. Stage 0 = VS, 3 = PS (512 377 of 512 382 entries agree with the shaders' own usage slots).
- Material instance D (33 110): +0x20 -> X, +0x30 texture bindings (0x30 each, u64 texture id), +0x38 count = X's
  ntex; binding i is the program's material texture i. Texture object: +0x28 T#, +0x48 u64 id (9 180 textures, all but
  2 instance textures resolve).
- Gnmx shader structs (2 325 VS/PS, one struct per footer hash, 0 conflicts): dword0 bits 24-31 = input usage slot
  count; +0x08 PGM_LO (KNLI kind 4), HI, RSRC1, RSRC2; PS: +0x18 Z_FORMAT, COL_FORMAT, PS_INPUT_ENA, PS_INPUT_ADDR,
  PS_IN_CONTROL, BARYC_CNTL, DB_SHADER_CONTROL, CB_SHADER_MASK, +0x38 input semantic count, +0x3c usage slots, then
  u16 input semantics; VS: +0x18 VS_OUT_CONFIG, POS_FORMAT, PA_CL_VS_OUT_CNTL, +0x24 u8 input / u8 export semantic
  counts, +0x28 usage slots, u32 input semantics, u16 export semantics (semantic, out index << 8); CS: +0x10 RSRC1,
  RSRC2, +0x18 NUM_THREAD_X/Y/Z, then usage slots.
- Usage slots {u8 type, u8 API slot, u8 start register, u8 flags}: start < 16 = user data register; type 27 at a
  register = the extended user data pointer, a resource with start R >= 16 sits at EUD[R - 16].

**Checked against the runs we already had (hold-out, never used to build the prediction).**
- Material textures (binding log, 1 382 logged pass shaders): the logged format is among the instances' formats in
  3 364 of 3 436 images (98 %), exactly the only one in 2 716; global textures: 64 of 74 names have one format.
- Fragment runtime parameters of all 1 149 cached graphics pipelines (three scenes): PS_INPUT_ENA/ADDR, num_interp,
  front_face_all_bits, Z/COL format, DB_SHADER_CONTROL (with `kDbShaderMask`) from the PS struct: 1 149 of 1 149.
  SPI_PS_INPUT_CNTL from the pass's VS export / PS input semantics (out index; flat bit 10 -> bit 10; semantic bit 12
  -> bit 5; no export -> 0x20 | default << 8): 1 091 of 1 149, and all 58 others are draws whose VS is not the PS's
  pass partner (effects), i.e. every material pass pair is exact.
- The rest of the runtime parameters (render targets, instancing, clip state) by pass class: for the new programs of a
  scene (absent from the two other scenes' caches) the class's most frequent state is the actual one in 90 of 101,
  31 of 31 and 46 of 49 new pass shaders, the remaining ones among 2-3 alternatives; misses are instancing step rates
  (5) and render targets (5).
- The new modules that are not pass shaders are all compute (26 / 7 / 105 layout modules): CS runtime parameters are
  in its struct (RSRC1/2, threads); their resources are bound by engine code (effects, particles) - next.

**So:** for material draws, a translation request (code, user data, runtime parameters, flattened user data with the
predicted T# formats) can be built from the files alone; what still needs work is the vertex side (fetch shader at
pass +0x30, V# formats of the meshes), constant buffer/sampler templates, and compute.

### 8.1 The static predictor (`src\runtime\gpu\static\`, `--predict-shaders`)

- `StaticDraws` (static_draws.cpp) reads every pack (21.3 GiB, 80 s, one thread): 22 518 programs, 138 900 passes,
  33 110 instances, 32 866 geometries (distinct stream formats), 9 180 textures, 2 325 VS/PS, 109 fetch shaders.
- `PredictModules` (static_predictor.cpp) builds, per pass, the request a draw would give the frontend: the program
  (read with the frontend's own `ReadProgram`, so its identity is the runtime's), RuntimeParams (shader fields from
  the structs, render state from the template's pass class), user data and a synthetic guest memory behind them
  (extended user data, fetch shader, vertex buffer table = the geometry's V# array). Material T# from the instances,
  named textures/buffers, the constant buffer and samplers from the template, a skinned variant (stream 0 as
  32_32_32 float, stride 12: what skinned draws read). PS first; the VS starts where the PS's bindings end. The key is
  made as `ShaderFrontend::Prepare` makes a layout's; the same key from the SRT walk over the synthetic memory is
  checked (30 of ~128 000 differ). Modules go to the disk cache through `AddModule`, the first of a (program, runtime)
  as the layout - a game start restores them like any other record; a miss costs only its record.
- Off by default. `SecondSonPC.exe --gpu-backend sspc --predict-shaders tools\static_shaders\static_templates.txt`
  (no game, before the guest address space: a device without presentation and the translator only) writes into
  `[ShaderCache] dir\<game>\native\<generation>`; `SSPC_STATIC_SHADERS=<templates>` does it at a game start instead.
  22 programs make the bridge's recompiler assert (`resource_discover_pass.cpp:257`, never met in a run): the process
  ends; `tools\static_shaders\predict_loop.ps1` puts the unfinished ones on `logs\static_predict_skip.txt` and resumes.
- One run: ~128 000 modules translated in 75 s (15 threads), 0 failures, 111 MiB of cache.

Measured (hold-out: the template's render state built from the two other scenes' caches; logs\p414\m1\cache):

| scene | layout modules of pass shaders: (program, RuntimeParams) predicted | keyed modules: exact key predicted |
|---|---|---|
| village (early) | 527 / 546 (96.5 %) | 164 / 218 (75 %); 169 with 64 variants and the skinned one |
| beach | 398 / 412 (96.6 %) | 105 / 129 (81 %) (16 variants, before the skinned one) |
| harbour (user1022) | 437 / 452 (96.7 %) | 112 / 148 (76 %) |

(before: a module's exact key seen in other scenes ≤ 5 % - docs\OFFLINE_SHADER_PRECOMPILATION.md §3). The keyed
misses: one image word (view type 2D / 2D array, or another format: instances beyond the 16 variants, named
textures with more than one format) and skinned/instanced vertex formats. The named textures' and buffers' templates
come from the binding log of earlier runs, not held out.

### 8.2 Pipelines of the predicted draws

What a draw's pipeline is beyond its two modules is its GraphicsPipelineState (targets, blending, depth, topology).
Measured on the pass pipelines of three scenes' caches (`tools\static_shaders\pipe_state.py`, `pipe_joint.py`,
`pipe_shaderfields.py`):

- The state without its shader-own fields is a function of (pass class, the PS's COL_FORMAT) for most draws: the
  other scenes' most frequent state is the scene's in 88 %, one of the three most frequent in 97 %. Taken jointly
  with the PS's and the VS's render state (one draw: PS RuntimeParams, VS RuntimeParams, pipeline state) the three
  most frequent joint states of a key cover 94 %, five 96.5 % (65-71 keys, 145-162 joint states).
- The shader-own fields follow the PS struct exactly (1 149 / 1 149 pipelines): CB_SHADER_MASK = the struct's,
  the fragment exports = the non-zero nibbles of its COL_FORMAT (= its translation's `mrt_mask`), a bound target's
  export format = its COL_FORMAT nibble.

`PredictPipelines` (static_predictor.cpp, after the module phase): per pass, per joint state of its (class,
COL_FORMAT) (template lines `pipe`, `pipe_table.py`; at most 3), per (material, vertex) format pair its program's
geometry actually has (the geometry's own instance - at most 16, the skinned form too), the PS and the VS such a draw
binds (the VS starting where that PS's bindings end; translated unless done) and the state with the shader-own fields
filled. `--predict-shaders` describes each as the executor does (`PipelineFactory::DescribeGraphics`: SPIR-V, state,
fragment runtime, bindings) and creates each distinct one once through the driver's pipeline cache, destroying it
again: `vulkan.bin` then holds what the driver compiled, and the game's creation of that pipeline is a cache hit.
`SSPC_STATIC_PIPELINES=0`: modules only; `SSPC_STATIC_MANIFEST=1`: the descriptions also go into the manifest (the
start-up precompiler then builds them at every start - off: they are created on demand, from the cache).

One run (hold-out templates): ~490 000 draws tried, 216 000-256 000 (stages, state) combinations, **5 500 distinct
pipelines** (most variants differ only in descriptor formats the SPIR-V does not depend on); ~290 000 modules in
155 s; the pipelines built in 19 s cold (37 ms each, 11 threads; 4 s with the driver's own shader cache warm),
created again from `vulkan.bin` in **0.24-0.29 ms** each (1 024 / 1 024 cache hits). Cache: 146 MiB modules, 50 MiB
`vulkan.bin`.

Measured (hold-out: the joint states built from the two other scenes' caches; `compare_pipes.py`, stages matched by
(program, key) - layout records by their RuntimeParams - so another cache generation compares):

| scene | pass pipelines: stages + state predicted | state differs | not first visited elsewhere: exact |
|---|---|---|---|
| village (early) | 410 / 470 (87.2 %) | 14 | 129 / 155 (83 %) |
| beach | 292 / 326 (89.6 %) | 9 | 62 / 66 |
| harbour (user1022) | 309 / 353 (87.5 %) | 7 | 52 / 61 |

The misses are mostly a stage: a keyed PS whose material texture format was not among the variants (as in §8.1),
then the joint state beyond the three most frequent.

### 8.3 Compute shaders

The packs hold 8 049 compute shaders as Gnmx CsShader structs (a kind-4 link to a program of footer type 4: RSRC1/2 at
+0x10, COMPUTE_NUM_THREAD_X/Y/Z at +0x18, input usage slots at +0x28). Their RuntimeParams are exactly what
`BuildComputeRuntime` makes of a dispatch's registers (233 / 233 cached modules). Their descriptors are bound by the
engine's code, so:

- The descriptors of a slot come from earlier dispatches (`csig`/`cslot` template lines, `tools\static_shaders\
  cs_table.py`, from the caches' flattened user data - `cs_flat.py`): by the struct's whole slot list and start
  register, else by (usage type, api slot, flags); the template sharp carries every V# bit the key reads (stride,
  swizzle, formats, element size, index stride, ADD_TID, MTYPE - an arbitrary sharp of a coarser form lost MTYPE:
  113 -> 288 below).
- A RW buffer's stride (the particle structs: 48-176 bytes, different in nearly every new program) follows from the
  code: the largest offset + size of the MUBUF/MTBUF accesses with IDXEN through that V# (in the user data SGPRs or
  loaded from the extended user data), rounded up to 16 when an access moves 16 bytes or more or an offset is not
  16-aligned (`BufferStrides`, `gcn_buf.py`; 1 350 of 1 582 slots of three scenes' dispatches). Read-only buffers keep
  the template's (the engine gives many of them stride 0 although the code indexes them).

Hold-out on the descriptors (new CS programs of a scene, templates from the other two; `cs_holdout.py`): every slot
right for 18 / 26 (village), 6 / 7 (beach), 102 / 105 (harbour) - from 7, 4, 23 by the templates alone. In the
translation: the first module of all 8 049 programs, (program, RuntimeParams) of every CS layout module of the three
scenes (26 / 26, 7 / 7, 105 / 105 new ones). The key a layout gets in the game (the live cache of this generation,
its keys computed by `SSPC_STATIC_KEYS=<dir>` -> `compare_keys.py`; not a hold-out): CS 288 / 353 (82 %), PS 370 / 466
(79 %), VS 163 / 214 (76 %). Not predicted: the second and later key of a program (121 keyed CS modules there).

With compute: ~300 000 modules in 3 min, 13 550 pipelines (8 049 compute) compiled into `vulkan.bin` (181 MiB) in
36-120 s, 0.2-0.3 ms each from it afterwards. `SSPC_STATIC_COMPUTE=0`: graphics only.

### 8.4 A game run with the predicted cache (2026-10-18, hand play, 11.7 min)

`--predict-shaders` into the live cache (297 636 modules, 13 555 pipelines into `vulkan.bin`, 6 min), then
`play_unlimited.cmd`. At the start: the cache read in 234 ms (470 MiB), `vulkan.bin` 186 MiB loaded; the offline index:
the cache had 10 213 of the game's 10 301 programs (99.1 %). During the run (the records the game appended):

- translated anew: 1 023 modules - 8 first modules of a program (5 PS, 3 VS), the rest a program's second or later
  key (678 CS, 258 PS, 79 VS); 2 programs new to the cache;
- pipelines created on demand: 2 452 (the start-up manifest had 992); byte for byte one of the prediction's
  descriptions (`SSPC_STATIC_SCORE_LAST=<n>` on a copy of the cache): graphics 531 / 700 (76 %), compute 1 063 /
  1 752 (61 %) - those were created from `vulkan.bin` instead of compiled.

The run ended in the game's movie decoder thread (an execute of 0x0 while the powers menu opened and closed its
`powerk_*.bsf` movies at each key press), with the system's commit at its limit at some moment (peak 32 654 of
32 655 MiB, no page file); no shader work on that thread. The predicted cache adds its `vulkan.bin` to the driver's
memory.

Not yet: more than one key per CS program (the largest remainder); the material textures' streaming placeholders (a 1x1 1D T# of the
texture's format while it streams in: 158 of 682 logged material modules had one).
