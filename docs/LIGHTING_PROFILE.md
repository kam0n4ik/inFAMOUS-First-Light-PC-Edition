# Lighting cost in the city at dusk (2026-10-18, phase C of the icons/lighting/headlights task)

Scene: the user's save (a copy), city street in rain with parked and passing cars, street lamps; 90 s runs, the same
input; native backend, parallel CP, RTX 2060. The frame is held by the command processor (CP 25-27 ms vs GPU busy
17-18 ms, ~34 FPS), so GPU savings alone do not raise the frame rate here.

## GPU (SSPC_GPU_PROFILE=1, 300 frames, 22.2 ms of GPU commands a frame)

| What | ms / frame | Note |
|---|---|---|
| tiled lighting CS `0x2edafa400` (120x68 groups) | 2.69 | 1.2-1.4 ms in the daytime village/beach: more lights here |
| batch-end write-backs of GPU writes to the host | 2.72 + 2.30 | REA §3: ~76 % is the per-tile light index list this CS writes (`0x2edc..`), inside the 2.5 GiB heap's range; the REA switches that drop it (`SSPC_FB_LAZY_WB` + `SSPC_REA_HEAP_READS`) cut GPU 15-22 % but add ~2 ms CP - not repeated |
| spot shadow atlas 1024x1024 (`0x304610000`, pass 28) | 0.16 | ~1 100-1 200 draws a frame, 91 % one pipeline (VS `0x2dfd6af00`, empty PS) |
| occlusion queries of the light glows | - | counted since `146a3bd`; drains of the parallel CP for them 0.07 ms / frame |

## Command processor - what the headlights cost

The spot shadow atlas is where lights (headlights, lamps) cost the system: ~1 100 simple draws a frame (one 0x48-byte
constant buffer, one 16-bit position stream, 18-288 vertices each). Measured with a diagnostic that drops exactly those
draws before any preparation (`SSPC_DIAG_SKIP_DEPTH_SIZE=1024`, **changes the picture, measurement only**):

| | FPS (5 windows of 10 s) | CP ms / frame | GPU busy ms |
|---|---|---|---|
| normal | 34.2 33.7 32.7 34.4 35.5 (34.1) | 26.1 | 17.8 |
| atlas draws dropped | 38.3 39.3 37.1 39.2 36.0 (38.0) | 23.7 | 17.8 |

So the spot shadows cost ~2.4 ms of CP and ~4 FPS here - per draw, not per pixel (their GPU time is 0.16 ms). The
CP's time on them is the generic per-draw preparation (CPU samples of the CP thread: ShaderFrontend::PrepareStage
40 %, FrontReader::Read 14 %, the SRT walk 16 %): the stage memo (P4.7) misses every one of them, as each draw's user
data carries its own constant buffer pointer. Making such draws cheaper is the "binding by the engine's table layout"
stage (REA §7 item 2, P5.3 native frontend extended to pass 28) - not started here.

## Diagnostics added (off by default)

- `SSPC_DIAG_SKIP_DEPTH_SIZE=<width>`: depth-only draws into a depth target of that width dropped before preparation.
- `SSPC_DIAG_DRAW_HISTO_P` also takes a VS address (decimal) - pipeline numbers move between runs.

## Per-draw cost of the atlas draws, and a relaxed stage memo (measured, rejected)

Thread 1 per atlas draw (TSC ticks, temporary counters, ~1.4 M draws): ParallelCp::PrepareDraw 6 770 (~1.9 us) =
the frontend's PrepareDraw 4 400 (VS stage 2 850 - a memo miss every time, PS stage 1 070, pipeline ~500) + the
stage copies and write notes ~2 370.

Tried: the stage memo (P4.7) taken also when the user data differs only in registers the preparation did not depend
on (`UserDataUsed`), the executor writing every draw's own registers into the flattened user data. Correct
(`SSPC_DIAG_STAGE_MEMO=verify`: 5.69 M hits prepared in full and compared, 0 different) but **0 relaxed hits** in
the city: the registers the atlas draws differ in are the ones their SRT walk reads (the per-object table pointer).
Removed. Cheaper atlas draws need their preparation without the per-draw SRT walk (binding by the engine's table
layout) or without the per-record stage copy - both larger than this stage; GPU-4 (shared stage copies) and the
frontend thread (NATIVE_CP.md) measured no gain before.
