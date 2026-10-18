# inFAMOUS: Second Son — PC Edition

**A Windows PC port project for inFAMOUS: Second Son.**

An independent engineering project that runs the original PlayStation 4 game on Windows on its own runtime: its own loader, kernel, memory, threading, system services, GPU command processor, Vulkan backend and — step by step — its own GCN shader translator.

> **Status: in development, playable.** The game starts, loads saves and is played on the project's own runtime and Vulkan backend. Most PlayStation system services are native. What still comes from shadPS4 is listed exactly below; removing it is the current work.

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
  <a href="#performance">Performance</a> ·
  <a href="#input-and-button-prompts">Input</a> ·
  <a href="#architecture">Architecture</a> ·
  <a href="#known-issues">Known issues</a> ·
  <a href="#roadmap">Roadmap</a>
</p>

---

## Overview

The project began as a launcher on top of the shadPS4 emulator core and has been replacing it piece by piece. Today the game runs on SecondSonPC's own code for nearly everything that matters at run time; shadPS4 is still linked into the build and provides a shrinking set of functions (below).

It is not a remake and not a source-code reconstruction: the original executable and assets are used unchanged. The project replaces the platform underneath them.

All numbers in this file were measured on the development machine (RTX 2060, Windows 10, 32 GB RAM) in fixed test scenes, unless stated otherwise.

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
| Keyboard / mouse input, pad API | Own |
| GPU: PM4 command processor | Own |
| GPU: Vulkan backend (default `--gpu-backend sspc`) | Own |
| Shader translation GCN → SPIR-V | Own for compute and vertex programs (opt-in), shadPS4's recompiler for the rest by default |
| Ajm / ATRAC9 audio decoding | shadPS4 |
| Common dialogs, a few network / libc functions | shadPS4 |
| Build without `third_party/shadPS4` | Not yet |

## What is new

The main changes since the first public description:

- **Own Vulkan backend is the default.** The game's GPU command buffers are processed by SecondSonPC's PM4 command processor and recorded into Vulkan by its own executor; the bridge renderer is only a fallback (`--gpu-backend bridge`).
- **Own shader translator.** Every compute program met in the test scenes and all vertex programs except rect/quad lists are translated by SecondSonPC's GCN → SPIR-V translator in the opt-in mode (details below).
- **Shader cache and preparation window.** Translated shaders and Vulkan pipelines are stored on disk; before the game starts, a preparation window predicts and builds the pipelines the game will need, from the game's own data files (no save scanning).
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
- Vulkan Validation and Synchronization Validation were clean in the documented test scenes.

### Rendering fixes

| Problem | Cause | Fix |
|---|---|---|
| Vehicle-light glow visible through fences and walls | visibility queries always reported "fully visible" | real Vulkan occlusion queries, results given to the game's compute shader (`146a3bd`) |
| Smoke flickering | the particle sort merged all widths in one dispatch; on PS4 that is one 64-lane wave, on PC two 32-lane subgroups raced | one dispatch per merge level with a barrier (`01a3edd`) |
| Wrong lighting colours | colour target component swap was masked out | COMP_SWAP honoured |

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

FSR 3.1 Frame Generation: beach 43–53 game frames/s → 86–105 shown; the HUD is kept out of generated frames. DLSS Frame Generation is not implemented (needs RTX 40/50). Generated frames still need visual review.

## Own shader translator

SecondSonPC's GCN → SPIR-V translator (`src/runtime/gpu/gcn/`): decoder, control-flow graph, structurization, resource analysis with its own SRT walk, and a SPIR-V emitter. It follows the bridge's binding and interface conventions, so its modules can be mixed with the bridge's in one pipeline, and decides per program: an instruction or shape it does not handle sends that program to the bridge's recompiler with the reason logged.

Enable it with the environment variable `SSPC_OWN_SHADERS` (or `play_rea_own.cmd` for compute):

| Value | Own translator for |
|---|---|
| `cs` | compute programs |
| `cs,vs` | compute and vertex programs |

Results (2-minute gameplay run, no cache, synchronous pipeline builds):

| Stage | Own | Sent to the bridge | `spirv-val` |
|---|---:|---:|---|
| Compute | 132 translations | 0 | all valid |
| Vertex | 284 translations | 6 (rect / quad lists) | all valid |
| Pixel | — | all (in progress) | — |

Offline, over all 10 301 GCN programs in the game's files: 7 820 SPIR-V modules emitted, 7 820 valid.

Covered so far: scalar / vector ALU including 64-bit and double precision, wave64 semantics on 32-lane subgroups, buffer / typed buffer / formatted loads and stores, LDS and GDS, images (load, store, sampling with explicit LOD and texel offsets, cube and 1D views), the fetch shader and vertex attribute formats, position / parameter / clip-distance exports. The picture was checked in game against the bridge's translation.

## Performance

The frame is currently limited by **CPU-side command processing and Vulkan recording** (~20–25 ms in heavy scenes, 6–10 thousand draws per frame), not by the GPU. Cutscenes with ~1 200 draws run at 60–90 FPS.

Measured improvements (A/B, n = 3, before → after):

| Change | Beach | Harbour | Village |
|---|---|---|---|
| GPU optimizations (`3da31cf`) | 48.1 → 55.1 FPS | 42.6 → 49.9 FPS | 37.3 → 38.6 FPS |
| Free frame pacing, 1 % low (`2f984a9`) | 29.4 → 48.8 | 29.4 → 46.8 | 29.3 → 35.3 |

Performance work is done by controlled A/B runs; rejected experiments are documented (`docs/GPU_OPTIMIZATION.md`, `docs/FRAMEBREAK.md`, `docs/LIGHTING_PROFILE.md`). No quality setting, light or shadow is reduced to gain FPS.

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

## Architecture

```text
Original PS4 game (eboot.bin + modules, unchanged)
        |
        v
SecondSonPC loader and runtime
        +-- kernel: threads, sync, time, event queues
        +-- virtual / direct memory, exception handling, CPU patcher
        +-- file system, system / user services, PlayGo
        +-- save data, WASAPI audio, video decoding
        +-- offline network / NP / trophies
        +-- [shadPS4 bridge: Ajm, common dialogs, a few net / libc functions]
        |
        v
SecondSonPC GPU
        +-- PM4 command processor (graphics + async compute)
        +-- shader frontend: SRT walk, resource plans, specialization keys
        +-- GCN -> SPIR-V: own translator (compute, vertex) / bridge recompiler (rest)
        +-- shader + pipeline disk cache, static pipeline prediction
        +-- Vulkan executor: descriptors, memory import, barriers, write-back
        +-- presenter: frame pacing, DLSS / FSR upscaling, FSR frame generation
        |
        v
Vulkan driver
```

## Known issues

- FPS is bound by CPU command processing in dense scenes (foliage, city streets).
- Pixel shaders, rect/quad-list vertex shaders and auxiliary tessellation stages still use the bridge's recompiler.
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
- [ ] Faster command recording (the current FPS limit)

### Own shader translator
- [x] Compute programs
- [x] Vertex programs (except rect / quad lists)
- [ ] Pixel programs: interpolation, colour / depth exports, implicit-LOD sampling
- [ ] Rect / quad lists and auxiliary stages
- [ ] Own translator by default

### Independence
- [ ] Remove the bridge (`src/bridge/shadps4`)
- [ ] Build without `third_party/shadPS4`
- [ ] Validate gameplay, saves, audio, video and graphics against a controlled baseline

## Installation

**This project does not distribute the original game, its executable, assets, firmware or any copyrighted content.** A legally obtained copy of the game is required; you prepare your own game files.

Development builds are started with the scripts in the repository root (`play.cmd`, `play_native.cmd`, `play_rea.cmd`, `play_rea_own.cmd`). Their names and arguments change during development. Requirements: 64-bit Windows, a Vulkan 1.3 GPU and driver, the user's own game files. A tested public build and its setup procedure are not published yet.

## Contributing

Reproducible bug reports, traces and documentation fixes are welcome. For performance changes: record a baseline, change one thing at a time, check rendering and Vulkan validation, report average FPS and 1 % lows, and document failed experiments. Do not submit game dumps or any proprietary game content.

## Legal notice

This is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment or Sucker Punch Productions.

inFAMOUS and inFAMOUS: Second Son are trademarks and/or copyrighted works of their respective owners.

No original game content is distributed by this repository. Users must supply their own legally obtained game files.

Third-party components, including the parts of shadPS4 still used during the migration, remain under their own licences.

## Acknowledgements

- The shadPS4 project, whose emulator core the port started from.
- The authors and maintainers of the open-source tools and Vulkan libraries used here.
- Researchers documenting PlayStation 4 hardware, GCN and executable formats.

## Disclaimer

Experimental software: features may be incomplete and regressions may occur. Back up your save data before testing a development build.
