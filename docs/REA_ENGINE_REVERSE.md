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
