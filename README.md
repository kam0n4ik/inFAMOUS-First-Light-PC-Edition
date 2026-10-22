# inFAMOUS: Second Son — PC Edition

**A Windows PC port project for inFAMOUS: Second Son.**

An independent engineering project that runs the original PlayStation 4 game on Windows on its own runtime: its own loader, kernel, memory, threading, system services, GPU command processor, Vulkan backend, GCN shader translator and — function by function — native reconstructions of the game engine's own render code.

> **Status: in development, playable.** The game starts, loads saves and is played on the project's own runtime, Vulkan backend and shader translator. Most PlayStation system services are native; what still comes from shadPS4 is listed exactly below. On the development machine the open city runs at ~32 FPS, the other test scenes at 36–55 FPS (table under [Performance](#performance)).

<p align="center">
  <img src="pc-edition-banner.png"
       alt="inFAMOUS: Second Son — PC Edition"
       width="100%">
</p>

<p align="center">
  <a href="#where-the-project-is">Status</a> ·
  <a href="#what-is-new">What is new</a> ·
  <a href="#graphics">Graphics</a> ·
  <a href="#own-shader-translator">Shader translator</a> ·
  <a href="#native-engine-reconstruction">Native engine</a> ·
  <a href="#performance">Performance</a> ·
  <a href="#pc-settings">PC settings</a> ·
  <a href="#input-and-button-prompts">Input</a> ·
  <a href="#work-in-progress-and-experiments">Branches</a> ·
  <a href="#reports">Reports</a> ·
  <a href="#roadmap">Roadmap</a>
</p>

---

## Overview

The project began as a launcher on top of the shadPS4 emulator core and has been replacing it piece by piece. Today the game runs on SecondSonPC's own code for nearly everything that matters at run time; shadPS4 is still linked into the build and provides a shrinking set of functions (below).

It is not a remake and not a source-code reconstruction of the whole game: the original executable and assets are used unchanged. The project replaces the platform underneath them and, since October 2026, also re-implements selected functions of the engine itself, each one checked call by call against the original.

All numbers in this file were measured on the development machine (RTX 2060 6 GB, Ryzen 5 5500, Windows 10, 32 GB RAM) in fixed test scenes — the open city, the village, the beach and the harbour — unless stated otherwise. Every report behind them is listed in [`docs/REPORTS.md`](docs/REPORTS.md).

## Where the project is

### PlayStation functions the game imports

The game imports 417 system functions. Their owner in the current build (snapshot `docs/generated/CALLS.md`, 2026-10-18):

| Owner | Functions | Called in a 2-minute gameplay run |
|---|---:|---:|
| **Native (SecondSonPC)** | **336** | 163 |
| shadPS4 (bridge) | 61 | 8 |
| shadPS4 common stub | 20 | 1 |

The bridge functions the game actually calls: `sceAjmInitialize`, `sceAjmModuleRegister` (audio decoding), `sceCommonDialogInitialize`, `sceNetInit`, `sceNetPoolCreate`, `sceNetInetNtop`, `sceLibcHeapGetTraceInfo`, `fflush`; the common stub: `vprintf`.

### Components

| Component | State |
|---|---|
| Executable, loader (SELF/ELF, relocations, TLS, NID table) | Own |
| Kernel: threads, synchronization, time, event queues, virtual / direct memory | Own, on by default |
| Exception handling, CPU patcher (TCB / FS patches) | Own, on by default |
| File system, system / user services, PlayGo, app content | Own |
| Save data (`param.sfo` byte-identical to the bridge's in tests) | Own |
| Audio output (WASAPI) | Own |
| Video decoding (`sceVideodec*`, cutscenes) | Own |
| Offline network, NP, trophies, HTTP | Own (offline behaviour) |
| Keyboard / mouse input, pad API, button prompts | Own |
| PC settings window inside the game's own menu | Own |
| GPU: PM4 command processor | Own |
| GPU: Vulkan backend (default `--gpu-backend sspc`) | Own |
| Shader translation GCN → SPIR-V | Own, by default for compute, vertex and pixel programs; shadPS4's recompiler is kept only as an A/B reference (`SSPC_OWN_SHADERS=bridge`) |
| Engine render functions (G-buffer and shadow passes, materials, constant update engine) | Own reconstructions of 18 engine functions, verified in game; off by default (`SSPC_NATIVE_ENGINE`) |
| Ajm / ATRAC9 audio decoding | shadPS4 |
| Common dialogs, a few network / libc functions | shadPS4 |
| Build without `third_party/shadPS4` | Not yet |

## What is new

Newest first. Everything below is in `main` unless marked as a branch.

**October 2026, second half**

- **Native engine, R5–R7.** 18 functions of the engine's render path — the G-buffer and shadow passes, their draws, material setup, the constant update engine (CUE) setters and its commit, object constants, the frame allocator — now have SecondSonPC implementations, hand-written or produced by the project's own static recompiler. Each runs in the real game in `verify` mode (both paths, compared), `guarded` mode (native with sampled checks and automatic fallback) or `native` mode. All 13 functions of R7 at once: **0 mismatches, 0 fallbacks** (for example 47.4 M native CUE commits with 185 k compared). FPS is unchanged — measured, explained [below](#native-engine-reconstruction).
- **Static recompiler v2.** Engine functions are translated from Ghidra P-code to C++ and built into a DLL: AVX, floating point, wide values and calls to other engine functions are supported; a recompiled function is checked against the original instructions offline before it may be installed.
- **Faster command processor in the city (R6-P).** Memory waits whose condition holds for any value no longer read guest memory: **city 28.8 → 32.8 FPS (+3.97), 1 % low +4.9, command processing −4.2 ms**; part of the city preset since `6167315`, neutral elsewhere.
- **CITY-OPT Phase 6.** The G-buffer's instanced draws join the engine draw runs: **city 27.9 → 31.6 FPS**, 1 % low 24.2 → 26.6.
- **CITY-OPT Phases 3–5.** The engine's own draw functions tag their draws; runs of draws that differ only in geometry are prepared and recorded as one unit: **city 25.3 → 29.4 FPS**.
- **PC settings window, redesigned.** One compact window with seven pages, opened from the game's own *Settings* screen; live changes, restart badges, gamepad / mouse / keyboard navigation, the FPS counter on the native backend ([PC settings](#pc-settings)).
- **One launcher with every measured improvement** — `play_unlimited.cmd` (the city preset plus the frame-rate limit from the settings). Before it carried the full preset the city ran at 24.8 FPS with DLSS; with it **31.9 FPS**, 1 % low 19.6 → 26.9.
- **Own shader translator by default** for compute, vertex and pixel programs; 0 programs fall back to the bridge in the test scenes.
- **Shader cache and preparation window.** Translated shaders and Vulkan pipelines are stored on disk and predicted in advance from the game's own data files; a repeat start takes ~2 s.
- **Branches in progress:** R8 — taking work off the command processor thread with the native engine's knowledge; the *fork port* — the frontend on its own thread, an opt-in potato mode for weak PCs and a second Vulkan recording thread; *Native Rebirth* — a separate native program that plays the prologue's first mission from the original data. See [Work in progress and experiments](#work-in-progress-and-experiments).

**October 2026, first half**

- **Own Vulkan backend is the default.** The game's GPU command buffers are processed by SecondSonPC's PM4 command processor and recorded into Vulkan by its own executor; the bridge renderer is only a fallback (`--gpu-backend bridge`).
- **Upscaling and frame generation.** DLSS Super Resolution, AMD FSR 3.1 Super Resolution and FSR 3.1 Frame Generation inside the game's frame.
- **Frame pacing.** The emulated 59.94 Hz vblank was the cap of the 1 % lows; the presenter now paces freely with a frame-rate limit (default 60, up to unlimited). The game's clock was measured to be independent of FPS.
- **Rendering fixes:** light glow through walls (real occlusion queries), flickering smoke (particle sort race), lighting colour channels.
- **Button prompts follow the input device** — keyboard/mouse, Xbox or PlayStation.
- **Native audio output (WASAPI), video decoding and save data.**

## Graphics

### Own command processor and Vulkan backend

- PM4 packets of the game's command buffers are decoded and executed by SecondSonPC (graphics and async compute queues).
- Resources, descriptors, GPU memory import, synchronization, barriers by ranges, write-back of GPU results the game reads.
- Two-thread mode: command processing and Vulkan recording on separate threads.
- Early frame submission: the frame is handed to the command processor as soon as the game has finished it, not one frame later (`SSPC_P54_EARLY_SUBMIT`, in the native preset; village +17.6 % FPS when introduced).
- Vulkan Validation and Synchronization Validation were clean in the documented test scenes.

### Rendering fixes

| Problem | Cause | Fix |
|---|---|---|
| Vehicle-light glow visible through fences and walls | visibility queries always reported "fully visible" | real Vulkan occlusion queries, results given to the game's compute shader (`146a3bd`) |
| Smoke flickering | the particle sort merged all widths in one dispatch; on PS4 that is one 64-lane wave, on PC two 32-lane subgroups raced | one dispatch per merge level with a barrier (`01a3edd`) |
| Wrong lighting colours | colour target component swap was masked out | COMP_SWAP honoured |
| Races in the game's own code that the PC's timing exposes | — | targeted fixes, `docs/GAME_FIXES.md` |

### Shader cache, pipeline prediction

- Disk cache of translated SPIR-V with reflection, plus `VkPipelineCache` (`vulkan.bin`) with device/driver checks.
- Static prediction: shader programs, descriptor layouts and pipeline states are derived from the game's pack files and engine structures. Hold-out accuracy for whole pipelines of render passes: 87–90 %; compute ~87 % of keys.
- A preparation window builds them before the game starts (a repeat start takes ~2 s).

### Upscaling and frame generation

Native backend only. The upscaler replaces the game's own (PS4 Pro-style) scaling pass; the HUD is drawn afterwards at full resolution and never goes through the upscaler. Motion vectors, reversed depth and jitter are taken from the game's frame.

| Mode (RTX 2060) | Village FPS / 1 % low | Beach FPS / 1 % low |
|---|---|---|
| Native | 35.4 / 25.8 | 41.6 / 28.0 |
| DLSS Quality | 36.3 / 27.8 | 38.8 / 26.8 |
| FSR Quality | 38.7 / 29.2 | 44.7 / 29.3 |
| FSR Performance | 40.7 / 29.2 | 45.3 / 29.3 |

(Measured on 2026-10-15, before the city work; the frame is CPU-bound, so upscaling helps little.) FSR 3.1 Frame Generation: beach 43–53 game frames/s → 86–105 shown; the HUD is kept out of generated frames. DLSS Frame Generation is not implemented (needs RTX 40/50). Generated frames still need visual review.

## Own shader translator

SecondSonPC's GCN → SPIR-V translator (`src/runtime/gpu/gcn/`): decoder, control-flow graph, structurization, resource analysis with its own SRT walk, and a SPIR-V emitter. It follows the bridge's binding and interface conventions, so its modules can be mixed with the bridge's in one pipeline, and decides per program: an instruction or shape it does not handle sends that program to the bridge's recompiler with the reason logged.

**It is the default** for compute, vertex and pixel programs. `SSPC_OWN_SHADERS` selects a subset (`cs`, `vs`, `ps` in any combination); `SSPC_OWN_SHADERS=bridge` (`play_rea_bridge.cmd`) uses the bridge's recompiler for everything — kept as the A/B reference.

| Stage | Own / sent to the bridge (2- and 6-minute gameplay runs) | `spirv-val` |
|---|---:|---|
| Compute | 95 / 0, 103 / 0 | all valid |
| Vertex (incl. rect / quad lists) | 133 / 0, 149 / 0 | all valid |
| Pixel | 369 / 0, 516 / 0 | all valid |
| Auxiliary tessellation stages for rect / quad lists | 10 quad modules in game | all valid |

Offline, over all 10 301 GCN programs in the game's files: 7 934 modules emitted in the offline test form, all valid (the rest need the game's runtime state, which only the game supplies). Vulkan Validation with Synchronization Validation: 0 messages with the own stages. The picture was checked in game against the bridge's translation. Details: `docs/OWN_SHADER_TRANSLATOR.md`.

## Native engine reconstruction

The game's executable is x86-64 code that runs directly on the PC's CPU. The native engine work replaces individual **engine functions** with SecondSonPC's own implementations, in memory, after loading — the files on disk are never changed.

How a function is replaced:

- A registry entry names the function by its eboot offset and pins it by a hash of its original body; on a different executable nothing is installed.
- The entry is redirected to SecondSonPC's implementation; a relocated clone of the original stays callable.
- Modes per function: `verify` (both run, every write and every call to other engine code is compared), `guarded` (native, with sampled comparisons; a mismatch switches the function back to the original on the fly), `native`.
- Implementations are hand-written C++ or produced by the static recompiler (Ghidra P-code → C++ → DLL) and verified offline against the original instructions first (tens to hundreds of thousands of generated cases per function).
- Everything is off by default: `SSPC_NATIVE_ENGINE=verify|guarded|native` with `SSPC_NATIVE_ENGINE_FUNCS=<names>`.

Verified in the game (city, CITY-OPT preset):

| Area | Functions (eboot offsets) | Implementation | In-game check |
|---|---|---|---|
| Object constants, frame allocator | `0x11c890`, `0x1f0870` | hand-written and recompiled | 19.0 M / 21.2 M calls in `verify`, 0 mismatches |
| Constant update engine: setters | `0x16aa0`, `0x16b70`, `0x169d0`, `0x16d10`, `0x16eb0`, `0x16c40`, `0x16de0` | hand-written and recompiled | e.g. 219 M calls of the first two in `verify`, 0 mismatches |
| Constant update engine: commit | `0x14ad0` | recompiled | 47.4 M native, 185 k compared, 0 mismatches |
| Material setup, draw constants | `0x448640`, `0x11e5d0` | hand-written and recompiled | 15.4 M / 9.0 M calls in `verify`, 0 mismatches |
| G-buffer pass and its draws | `0x1a2030`, `0x1a1770`, `0x1a1960` | hand-written | draws: 6.3 M / 21.3 M in `verify`, 0 mismatches; the pass loop on every branch the city reaches (rarer branches go to the original) |
| Shadow pass, instanced shadow draw, shadow UVs | `0x43d590`, `0x43d480`, `0x451700` | hand-written and recompiled | 17.5 M native shadow draws, 4.4 M UV calls, 0 mismatches |

**Results.** All 13 functions of R7 together, final A/B against the preset in four scenes (3 interleaved rounds each): city +0.21, village −0.82, beach +0.22, harbour +1.39 FPS — within the run-to-run spread, i.e. **neither faster nor slower**. The frame is limited by the command processor thread (~30 ms of a ~31 ms city frame), while these engine functions run on the game's own job threads. What R5–R7 deliver is correctness infrastructure and an engine whose render path increasingly runs on code the project owns; turning that knowledge into less command-processor work is the goal of R8 (in progress, below). Details: `docs/NATIVE_ENGINE_RECONSTRUCTION.md`, `docs/NATIVE_ENGINE_FUNCTIONS.md`, `docs/NATIVE_RENDER_R6_RESULTS.md`, `docs/NATIVE_ENGINE_R7_RESULTS.md`.

## Performance

The frame is limited by **CPU-side command processing and Vulkan recording**, not by the GPU: the open city issues ~12 000 draws per frame, the GPU needs ~18 ms of it. Cutscenes with ~1 200 draws run at 60–90 FPS.

Current state (measurement stand with the switches of `play_unlimited.cmd`; base side of the R7 final A/B, 2026-10-21, 3 runs per scene):

| Scene | FPS | 1 % low | Command processor thread | Draws / frame |
|---|---:|---:|---:|---:|
| City | 32.1 | 27.9 | 30.6 ms | 11 975 |
| Village | 35.6 | 26.4 | 20.9 ms | 4 284 |
| Beach | 55.3 | 39.1 | 15.3 ms | 5 580 |
| Harbour | 43.6 | 25.0 | 18.5 ms | 5 264 |

Measured improvements (interleaved A/B, n ≥ 3, before → after):

| Change | City | Beach | Harbour | Village |
|---|---|---|---|---|
| GPU optimizations (`3da31cf`) | — | 48.1 → 55.1 FPS | 42.6 → 49.9 FPS | 37.3 → 38.6 FPS |
| Free frame pacing, 1 % low (`2f984a9`) | — | 29.4 → 48.8 | 29.4 → 46.8 | 29.3 → 35.3 |
| CITY-OPT Phase 5: engine draw runs | 25.3 → 29.4 FPS | — | — | — |
| CITY-OPT Phase 6: G-buffer runs | 27.9 → 31.6 FPS | not slower | — | unchanged |
| R6-P: trivial memory waits skip the read | 28.8 → 32.8 FPS | neutral | neutral | neutral |

**City (CITY-OPT, `SSPC_CITY_PERFORMANCE=1`).** The city's ~12 000 draws per frame went through command processing and Vulkan recording one by one. The engine's two main draw functions (meshes and shadow casters, ~70 % of the draws) are now tagged by small hash-pinned probes in the driver NOP that follows every draw — no extra command dword, the game's code unchanged — and a run of tagged draws that differ only in geometry and instance constants is one record: the first draw is prepared and recorded in full, the others only bind their own vertices, indices and constants. Phase 6 brings the G-buffer's instanced draws (`0x1a1770`) into runs too, re-walking a member's shader resources when the constant update engine moved its table, and a probe on the material setup (`0x448640`) turns a material change into an immediate refusal. Checked: every joined draw also prepared in full and compared (23.1 M members, 0 different), the command processor's registers against the recording thread's copy, Vulkan Validation + Sync (0 messages), frame captures (depth and shadow maps bit-exact). Details: `docs/CITY_PERFORMANCE.md`, `docs/CITY_ENGINE_OPTIMIZATION.md`, `docs/CITY_ENGINE_RESOURCES.md`.

**Rejected by measurement** (documented so they are not repeated): the P5.3 hybrid native render bridge in the city (−4.4 FPS), CUE snapshot reuse (+0.00 FPS on top of R6-P), the R7 native G-buffer transport (−1.6 FPS), compute queue priority (village 1 % low 29 → 20), BBPort's GPU library as the backend (25 vs 40 FPS in the village, missing smoke and fire), the uncharted_pc / bbport optimization ports (threaded recording −14…−19 % recording time but no proven FPS; draw-preparation workers and LTO −11…−12 % on the beach; PGO neutral).

Performance work is done by controlled A/B runs; rejected experiments are documented (`docs/GPU_OPTIMIZATION.md`, `docs/FRAMEBREAK.md`, `docs/LIGHTING_PROFILE.md`, the R6/R7 result reports). No quality setting, light or shadow is reduced to gain FPS; the potato mode on the fork-port branch is a separate, opt-in user choice.

## PC settings

The PC options live inside the game's own menu: **Esc (Options) → Settings → "Additional settings"** (the line that used to read "HDR Rendering — Unavailable"; the game's files are not changed — the string is replaced as it is read). The window is drawn over the game in the game's own style:

- seven pages — **Graphics, Display, Controls, Keys, Audio, Game, Advanced**;
- toggles, sliders, drop-down lists, two key cells per action; keyboard, mouse and gamepad navigation, prompts for the last device used;
- changes apply immediately where the subsystem allows it (upscaler and quality, sharpness, frame generation, Reflex, smoke quality, particle resolution, frame-rate limit, mouse and motion settings, volume); window mode, size and monitor changes ask for confirmation for 15 s; options that need a restart show a badge and the value in effect now;
- unavailable options say why (another backend or hardware needed);
- the window scales with the game window and DPI (layout checked from 800 × 600 to 4K and 21:9);
- defaults in `SecondSonPC.toml`, the user's changes in `%LOCALAPPDATA%\SecondSonPC\settings.toml` (keys from other versions are kept).

Details: `docs/PC_SETTINGS_UI.md`.

## Native audio, video and saves

- **Audio output** through WASAPI (`245d86d`); `--bridge audio` switches back to the bridge's output.
- **Video decoding** (`sceVideodec*`) is native; cutscenes play.
- **Save data**: existing saves load and autosaves are written; tests always run on copies of the user's saves.

## Input and button prompts

Keyboard and mouse are handled natively, with PC key bindings. The game draws every button prompt from one icon font; SecondSonPC redraws the physical-button cells for the active input device, so **all** prompts of the game — static and composite, in every language — switch between keyboard/mouse, Xbox and PlayStation without editing strings or replacing the original files on disk.

<p align="center">
  <img src="prw1.png" alt="Keyboard and mouse prompt adaptations for inFAMOUS: Second Son PC Edition" width="900">
</p>

<p align="center">
  <img src="prw2.png" alt="Original-style controller prompts and alternative button layouts" width="900">
</p>

The original PlayStation icons stay when a PlayStation controller is used.

## Work in progress and experiments

Work that is not in `main` yet lives in branches of this repository:

| Branch | What | State |
|---|---|---|
| [`feature/native-engine-r8`](../../tree/feature/native-engine-r8) (+ `feature/r8-cp-profile`, `r8-native-cp`, `r8-render-layer`, `r8-recomp`, `r8-verify`) | **R8: native CP optimization.** A profiler of the command processor thread (`SSPC_R8_CP_PROFILE`), a channel that publishes the engine's CUE snapshots to the command processor (18.4 k per frame, found for 76 % of full stage preparations), a deterministic verification mode (fixed 1/30 s step, frame comparison), one native entry for the CUE commit, a CUE audit | in progress; first finding: reusing pixel-shader preparations by the engine's state is worth only ~0.75 ms per frame without touching the shader frontend — the design for the larger cut is under review |
| [`feature/fork-port-prep-workers`](../../tree/feature/fork-port-prep-workers) | **Fork port.** Ideas from the axylpup shadPS4 fork for Second Son: the shader frontend on its own thread now keeps the whole city preset (runs, GEOFAST, async acquire), occlusion queries and flips queued, a second Vulkan recording thread (`SSPC_VK_EMIT_THREAD`). **Potato mode for weak PCs** — `[Graphics] potatoMode`, live in the settings window: level 1 drops the spot-light shadow atlas (headlights, street lamps), 2 also the sun's far shadow cascade, 3 also small objects' shadows, 4 also small instanced G-buffer objects (HUD and map kept) | 14 minutes of play without a crash in front mode; the thread's remaining cost is waiting (memory waits, GDS), not preparation; the emit thread is not measured yet |
| [`feature/native-missions`](../../tree/feature/native-missions) (on top of [`feature/native-rebirth`](../../tree/feature/native-rebirth)) | **Native Rebirth.** A separate native program (`native/`) that does not run the game's executable or shadPS4: it reads the game's packages itself (all 1 621 parse), renders the world with its own Vulkan renderer, plays Delsin's original animation clips (the `'70AE'` format decoded, 2 034 clips), walks on the original Havok collision, loads the original mission data (65 mission packages, 59 graffiti packages) and runs the prologue's stencil-art graffiti mission end to end | research vertical slice, not the game: no game lighting, no original animation graph, parts of the cannery approximated; 510–730 FPS in its scenes, Vulkan Validation 0, ASan clean |
| [`experiment/bbport-gpu`](../../tree/experiment/bbport-gpu) | BBPort's GPU library (Bloodborne PC fork) as the game's GPU backend | rejected: 25 vs 40 FPS in the village, smoke and fire missing, VRAM at its budget |
| [`feature/uncharted-bbport-optimizations`](../../tree/feature/uncharted-bbport-optimizations) | uncharted_pc / bbport techniques ported: threaded Vulkan recording, draw-preparation workers, LTO, PGO | measured; nothing taken into `main` (verdicts above) |

## Architecture

```text
Original PS4 game (eboot.bin + modules, unchanged on disk)
        |
        v
SecondSonPC loader and runtime
        +-- kernel: threads, sync, time, event queues
        +-- virtual / direct memory, exception handling, CPU patcher
        +-- file system, system / user services, PlayGo
        +-- save data, WASAPI audio, video decoding
        +-- offline network / NP / trophies
        +-- PC settings window in the game's menu, input, button prompts
        +-- [shadPS4 bridge: Ajm, common dialogs, a few net / libc functions]
        |
        +-- native engine: hash-pinned replacements of engine functions
        |     (hand-written / recompiled; verify, guarded, native; off by default)
        |
        v
SecondSonPC GPU
        +-- PM4 command processor (graphics + async compute)
        +-- shader frontend: SRT walk, resource plans, specialization keys
        +-- GCN -> SPIR-V: own translator (default; bridge recompiler as A/B reference)
        +-- engine draw runs: hash-pinned probes tag the engine's draws (CITY-OPT)
        +-- shader + pipeline disk cache, static pipeline prediction
        +-- Vulkan executor: descriptors, memory import, barriers, write-back
        +-- presenter: frame pacing, DLSS / FSR upscaling, FSR frame generation
        |
        v
Vulkan driver
```

## Known issues

- FPS is bound by CPU command processing in dense scenes (foliage, city streets): ~32 FPS in the open city on the development machine, far from 60.
- The native engine functions are off by default and give no FPS on their own yet.
- Ajm / ATRAC9, common dialogs and a few network / libc functions still come from shadPS4; the build still needs `third_party/shadPS4`.
- On a machine without a page file, total committed memory can reach the Windows limit in long sessions (32 GB RAM); fixed causes are documented in `docs/KNOWN_ISSUES.md`, long-session soak testing continues.
- Frame generation has not been reviewed frame by frame; input latency with it is not measured.
- No full-campaign compatibility guarantee.

## Roadmap

### Runtime and services
- [x] Own loader, kernel, threads, memory, exceptions, CPU patcher
- [x] File system, system services, save data, offline network / trophies
- [x] WASAPI audio output, video decoding
- [ ] Ajm / ATRAC9 decoding
- [ ] Common dialogs, remaining network and libc functions

### Graphics
- [x] Own PM4 command processor and Vulkan backend (default)
- [x] Occlusion queries, smoke and lighting fixes
- [x] Shader / pipeline disk cache, static pipeline prediction, preparation window
- [x] DLSS / FSR 3.1 upscaling, FSR 3.1 frame generation
- [x] Free frame pacing
- [x] City: draw runs from the engine's draw functions (CITY-OPT, phases 1–6)
- [x] Trivial memory waits without guest reads (R6-P, in the city preset)
- [ ] Less command-processor work for the remaining draws (R8 — the current FPS limit)
- [ ] Potato mode for weak PCs in `main` (branch `feature/fork-port-prep-workers`)

### Own shader translator
- [x] Compute programs
- [x] Vertex programs
- [x] Pixel programs: interpolation, colour / depth exports, implicit-LOD sampling
- [x] Rect / quad lists and auxiliary stages (quad lists seen in game; rect lists only validated offline)
- [x] Own translator by default
- [ ] Other districts, fights and story scenes run through the own translator

### Native engine
- [x] Function registry, hash-pinned install, verify / guarded / native modes, fallback
- [x] Static recompiler v2 (AVX, floating point, calls)
- [x] G-buffer and shadow passes, materials, CUE setters and commit — verified in game
- [ ] The native render layer submits its draws without the game's command buffer
- [ ] The engine's state used by the command processor to skip work (R8)

### PC experience
- [x] PC settings window in the game's menu
- [x] Button prompts for keyboard/mouse, Xbox, PlayStation
- [ ] A tested public build with an installer

### Independence
- [ ] Remove the bridge (`src/bridge/shadps4`)
- [ ] Build without `third_party/shadPS4`
- [ ] Validate gameplay, saves, audio, video and graphics against a controlled baseline

## Installation

**This project does not distribute the original game, its executable, assets, firmware or any copyrighted content.** A legally obtained copy of the game is required; you prepare your own game files.

Development builds are started with the scripts in the repository root. `play_unlimited.cmd` starts the game with every measured improvement (the native command processor preset, the city preset, the frame-rate limit from the settings); `play.cmd`, `play_native.cmd`, `play_rea.cmd`, `play_rea_own.cmd` and `play_rea_bridge.cmd` are narrower variants used for A/B. Their names and arguments change during development. Requirements: 64-bit Windows, a Vulkan 1.3 GPU and driver, the user's own game files. A tested public build and its setup procedure are not published yet.

## Reports

Every phase of the project ends with a report: what was measured, what was kept, what was rejected and why. The full list — about 120 reports in `main` and in the branches above, grouped by area — is in **[`docs/REPORTS.md`](docs/REPORTS.md)**. Most reports are written in Russian, the newer ones partly in English.

## Contributing

Reproducible bug reports, traces and documentation fixes are welcome. For performance changes: record a baseline, change one thing at a time, check rendering and Vulkan validation, report average FPS and 1 % lows, and document failed experiments. Do not submit game dumps or any proprietary game content.

## Legal notice

This is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment or Sucker Punch Productions.

inFAMOUS and inFAMOUS: Second Son are trademarks and/or copyrighted works of their respective owners.

No original game content is distributed by this repository. Users must supply their own legally obtained game files.

Third-party components, including the parts of shadPS4 still used during the migration, remain under their own licences.

## Acknowledgements

- The shadPS4 project, whose emulator core the port started from.
- The axylpup shadPS4 fork for Second Son, whose split of command processing inspired the fork-port branch.
- The BBPort (Bloodborne PC) and uncharted_pc projects, whose techniques were measured here.
- The authors and maintainers of the open-source tools and Vulkan libraries used here, Ghidra among them.
- Researchers documenting PlayStation 4 hardware, GCN and executable formats.

## Disclaimer

Experimental software: features may be incomplete and regressions may occur. Back up your save data before testing a development build.
