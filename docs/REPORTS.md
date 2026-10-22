# Reports — inFAMOUS: Second Son PC Edition

Every report of the project in one list, grouped by area, oldest phase first within a group. Reports in `main` are linked relative to this folder; reports that exist only in a branch link to that branch on GitHub. Most reports are written in Russian, the newer ones partly in English. The summary of the current state is the [README](../README.md).

Dates are the last change of the file (2026).

## 1. Start, audit, migration plan

| Report | Date | What |
|---|---|---|
| [PHASE0.md](PHASE0.md) | 10-08 | Phase 0 result: the game boots through the launcher, first audit runs |
| [AUDIT.md](AUDIT.md) | 10-08 | Stage 1 audit of the project and of shadPS4's role |
| [MIGRATION.md](MIGRATION.md) | 10-08 | Migration log from Phase 1 on |
| [generated/IMPORTS.md](generated/IMPORTS.md) | 10-08 | The eboot's imports (generated) |
| [generated/CALLS.md](generated/CALLS.md) | 10-18 | Which imports the game really calls and who owns them (native 336 / bridge 61 / stub 20) |
| [generated/KERNEL_AUDIT.md](generated/KERNEL_AUDIT.md) | 10-09 | libkernel imports of the game |
| [MANUAL_TEST_PLAN.md](MANUAL_TEST_PLAN.md) | 10-09 | Manual test plan |
| [PATCHES.md](PATCHES.md) | 10-11 | Patches carried on the shadPS4 bridge |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | 10-11 | Known issues and their status |

## 2. Native runtime (Phases 2–3)

| Report | Date | What |
|---|---|---|
| [PHASE2_PLAN.md](PHASE2_PLAN.md) | 10-10 | Phase 2 plan and state: native runtime and playable input |
| [INPUT_MAPPING.md](INPUT_MAPPING.md) | 10-09 | Keyboard, mouse, gamepads (P2.1) |
| [KERNEL_SYNC.md](KERNEL_SYNC.md) | 10-09 | Native kernel synchronization layer (P2.2) |
| [PHASE2_NATIVE_MIGRATION_MATRIX.md](PHASE2_NATIVE_MIGRATION_MATRIX.md) | 10-10 | Bridge functions migration matrix |
| [PHASE2_P23_MEMORY.md](PHASE2_P23_MEMORY.md) | 10-09 | Memory runtime: audit, architecture, results |
| [PHASE2_P23_P24_PLAN.md](PHASE2_P23_P24_PLAN.md) | 10-10 | Memory and thread runtime plan |
| [PHASE2_P232_GPU_MEMORY.md](PHASE2_P232_GPU_MEMORY.md) | 10-09 | Interface between the GPU bridge and the memory manager |
| [PHASE2_P233_MEMORY_DEPENDENCIES.md](PHASE2_P233_MEMORY_DEPENDENCIES.md) | 10-10 | Who else uses the bridge's memory manager |
| [PHASE2_P233_NATIVE_VMM.md](PHASE2_P233_NATIVE_VMM.md) | 10-10 | Own virtual memory manager |
| [PHASE2_P234_NATIVE_MEMORY.md](PHASE2_P234_NATIVE_MEMORY.md) | 10-10 | The game on the own memory manager (`--native memory`) |
| [PHASE2_P24_NATIVE_THREADS.md](PHASE2_P24_NATIVE_THREADS.md) | 10-10 | Own guest thread manager (`--native threads`) |
| [PHASE2_P25_PERFORMANCE.md](PHASE2_P25_PERFORMANCE.md) | 10-10 | Kernel stabilization and performance |
| [PHASE2_TEST_RESULTS.md](PHASE2_TEST_RESULTS.md) | 10-10 | Phase 2 test results |
| [PHASE3_INDEPENDENT_RUNTIME.md](PHASE3_INDEPENDENT_RUNTIME.md) | 10-10 | Phase 3: independent runtime |
| [PHASE3_BRIDGE_DEPENDENCIES.md](PHASE3_BRIDGE_DEPENDENCIES.md) | 10-10 | Bridge dependencies of the guest imports |
| [PHASE3_P34_EXCEPTIONS_CPU.md](PHASE3_P34_EXCEPTIONS_CPU.md) | 10-10 | Native exception and CPU runtime |
| [PHASE3_P35_KERNEL_HLE.md](PHASE3_P35_KERNEL_HLE.md) | 10-10 | Native kernel HLE |
| [PHASE3_P35_REPORT.md](PHASE3_P35_REPORT.md) | 10-10 | P3.5 final report |
| [PHASE3_P36_BRIDGE_ISOLATION.md](PHASE3_P36_BRIDGE_ISOLATION.md) | 10-10 | Bridge isolation and runtime boundaries |
| [GAME_FIXES.md](GAME_FIXES.md) | 10-18 | Races in the game's own code that the PC exposes |

## 3. Own GPU backend (Phase 4)

| Report | Date | What |
|---|---|---|
| [PHASE4_GRAPHICS_BACKEND.md](PHASE4_GRAPHICS_BACKEND.md) | 10-10 | Strategy, map and boundaries of the graphics backend |
| [PHASE4_P41_GPU_FOUNDATION.md](PHASE4_P41_GPU_FOUNDATION.md) | 10-10 | GPU research and backend foundation |
| [PHASE4_P42_COMMAND_PROCESSOR.md](PHASE4_P42_COMMAND_PROCESSOR.md) | 10-10 | Own PM4 command processor |
| [PHASE4_P43_SHADER_TRANSLATION.md](PHASE4_P43_SHADER_TRANSLATION.md) | 10-10 | Shader translation path and pipeline cache |
| [PHASE4_P44_NATIVE_VULKAN_RENDERING.md](PHASE4_P44_NATIVE_VULKAN_RENDERING.md) | 10-11 | Own Vulkan backend (`--gpu-backend sspc`) |
| [PHASE4_P45_GRAPHICS_CORRECTNESS.md](PHASE4_P45_GRAPHICS_CORRECTNESS.md) | 10-11 | Graphics correctness, compatibility, stability |
| [PHASE4_P46_GPU_PROFILING.md](PHASE4_P46_GPU_PROFILING.md) | 10-11 | Nsight GPU profiling and frame breakdown |
| [PHASE4_P46_PERFORMANCE.md](PHASE4_P46_PERFORMANCE.md) | 10-11 | Performance and frame pacing of the backend |
| [NIGHT_PERFORMANCE_SPRINT.md](NIGHT_PERFORMANCE_SPRINT.md) | 10-11 | 12-hour performance sprint (beach 20 → 30 FPS) |
| [PHASE4_FOLIAGE_OPTIMIZATION.md](PHASE4_FOLIAGE_OPTIMIZATION.md) | 10-12 | Foliage without quality loss (P4.7) |
| [PHASE4_P49_CPU_GPU_OPTIMIZATION.md](PHASE4_P49_CPU_GPU_OPTIMIZATION.md) | 10-12 | CPU / GPU bottlenecks (P4.9) |
| [PHASE4_P410_PARALLEL_COMMAND_PROCESSING.md](PHASE4_P410_PARALLEL_COMMAND_PROCESSING.md) | 10-12 | Two-thread command processing (P4.10) |
| [PHASE4_P411_PARALLEL_STABILIZATION.md](PHASE4_P411_PARALLEL_STABILIZATION.md) | 10-15 | Stabilizing the parallel mode (P4.11) |
| [PHASE4_P412_FULL_STALL_ANALYSIS.md](PHASE4_P412_FULL_STALL_ANALYSIS.md) | 10-12 | Full stall and critical-path analysis (P4.12) |
| [PHASE4_P413_PLAN.md](PHASE4_P413_PLAN.md) | 10-12 | Optimization plan after P4.12 |
| [PHASE4_P415A_STABILITY.md](PHASE4_P415A_STABILITY.md) | 10-15 | Fixes and stability checks (P4.15A) |
| [NATIVE_CP.md](NATIVE_CP.md) | 10-16 | The native command processor preset |
| [GPU_OPTIMIZATION.md](GPU_OPTIMIZATION.md) | 10-16 | Vulkan / GPU optimization (beach +14.6 %, harbour +17.1 %) |
| [FRAMEBREAK.md](FRAMEBREAK.md) | 10-17 | Frame pacing, 1 % lows, FPS unlock |
| [REA_ENGINE_REVERSE.md](REA_ENGINE_REVERSE.md) | 10-18 | REA + Ghidra: the engine's particles, its GPU memory and the backend |
| [perf_rebuild/phase_b_notes.md](perf_rebuild/phase_b_notes.md) | 10-17 | shadPS4 upstream and BBPort compared |

## 4. Rendering fixes

| Report | Date | What |
|---|---|---|
| [PHASE4_SMOKE_PIPELINE.md](PHASE4_SMOKE_PIPELINE.md) | 10-11 | Smoke pipeline and the flicker fix (SSPC-020) |
| [SMOKE_SHADER_MAP.md](SMOKE_SHADER_MAP.md) | 10-11 | Map of the smoke shaders |
| [SMOKE_QUALITY.md](SMOKE_QUALITY.md) | 10-15 | Smoke quality, particle resolution, async compute |
| [LIGHT_GLOW_OCCLUSION.md](LIGHT_GLOW_OCCLUSION.md) | 10-18 | Light glow through walls: the guest's occlusion queries |
| [LIGHTING_PROFILE.md](LIGHTING_PROFILE.md) | 10-18 | Lighting cost in the city at dusk |

## 5. Shaders

| Report | Date | What |
|---|---|---|
| [SHADER_CACHE.md](SHADER_CACHE.md) | 10-10 | Shader and pipeline cache on the bridge |
| [PHASE4_SHADER_PRECOMPILER.md](PHASE4_SHADER_PRECOMPILER.md) | 10-15 | Persistent shader cache and the start-up precompiler |
| [OFFLINE_SHADER_PRECOMPILATION.md](OFFLINE_SHADER_PRECOMPILATION.md) | 10-15 | Shaders found offline in the game's files, predictive precompilation |
| [../tools/static_shaders/README.md](../tools/static_shaders/README.md) | 10-18 | Static shader / pipeline prediction tools |
| [OWN_SHADER_TRANSLATOR.md](OWN_SHADER_TRANSLATOR.md) | 10-19 | Own GCN → SPIR-V translator, now the default |

## 6. Upscaling and frame generation

| Report | Date | What |
|---|---|---|
| [UPSCALING_FRAME_GENERATION.md](UPSCALING_FRAME_GENERATION.md) | 10-15 | DLSS / FSR 3.1 upscaling, FSR 3.1 frame generation |

## 7. City performance (CITY-OPT)

| Report | Date | What |
|---|---|---|
| [CITY_PERFORMANCE.md](CITY_PERFORMANCE.md) | 10-21 | FPS in the city on the own backend, all phases |
| [CITY_ENGINE_REVERSE.md](CITY_ENGINE_REVERSE.md) | 10-20 | Phase 3: where the engine's 2 700 draws of pass p462 come from |
| [CITY_DRAW_PRECOMPUTE.md](CITY_DRAW_PRECOMPUTE.md) | 10-19 | Phase 4: draws prepared once |
| [CITY_ENGINE_OPTIMIZATION.md](CITY_ENGINE_OPTIMIZATION.md) | 10-20 | Phase 5: engine draw runs (city 25.3 → 29.4 FPS) |
| [CITY_ENGINE_RESOURCES.md](CITY_ENGINE_RESOURCES.md) | 10-20 | Phase 6: G-buffer runs (city 27.9 → 31.6 FPS) |

## 8. Engine reverse engineering (P5)

| Report | Date | What |
|---|---|---|
| [native-reconstruction/BINARY_ANALYSIS.md](native-reconstruction/BINARY_ANALYSIS.md) | 10-14 | Binary analysis |
| [native-reconstruction/ENGINE_ARCHITECTURE.md](native-reconstruction/ENGINE_ARCHITECTURE.md) | 10-14 | Game → engine → PS4 dependencies → compatibility layer → Windows |
| [native-reconstruction/PS4_DEPENDENCIES.md](native-reconstruction/PS4_DEPENDENCIES.md) | 10-14 | Map of PS4 dependencies |
| [native-reconstruction/JOB_SYSTEM_RECONSTRUCTION.md](native-reconstruction/JOB_SYSTEM_RECONSTRUCTION.md) | 10-14 | The engine's job system reconstructed |
| [native-reconstruction/GNMX_RECONSTRUCTION.md](native-reconstruction/GNMX_RECONSTRUCTION.md) | 10-14 | Gnm / Gnmx inside the eboot |
| [native-reconstruction/GFX_CONTEXT_LAYOUT.md](native-reconstruction/GFX_CONTEXT_LAYOUT.md) | 10-14 | The engine's render context and frame submission |
| [native-reconstruction/RENDER_ENGINE_ARCHITECTURE.md](native-reconstruction/RENDER_ENGINE_ARCHITECTURE.md) | 10-14 | Sucker Punch render engine: architecture map |
| [native-reconstruction/RENDER_ENGINE_PASSES.md](native-reconstruction/RENDER_ENGINE_PASSES.md) | 10-14 | Contexts between frames, 41 passes, dependencies |
| [native-reconstruction/PERFORMANCE_BASELINE.md](native-reconstruction/PERFORMANCE_BASELINE.md) | 10-14 | Baseline measurements |
| [native-reconstruction/P52_PERFORMANCE.md](native-reconstruction/P52_PERFORMANCE.md) | 10-14 | Profiling the graphics path, the first fix |
| [native-reconstruction/NATIVE_RENDER_INTERFACE.md](native-reconstruction/NATIVE_RENDER_INTERFACE.md) | 10-15 | Native render interface (P5.3B–D) |
| [native-reconstruction/P54_FRAME_LIFECYCLE.md](native-reconstruction/P54_FRAME_LIFECYCLE.md) | 10-15 | Frame lifecycle and early frame submission |
| [native-reconstruction/NATIVE_MIGRATION_ROADMAP.md](native-reconstruction/NATIVE_MIGRATION_ROADMAP.md) | 10-14 | Step-by-step migration plan |
| [native-reconstruction/RECONSTRUCTION_PROGRESS.md](native-reconstruction/RECONSTRUCTION_PROGRESS.md) | 10-14 | Reconstruction progress |
| [native-reconstruction/OPEN_QUESTIONS.md](native-reconstruction/OPEN_QUESTIONS.md) | 10-14 | Open questions |
| [../tools/native-reconstruction/ghidra/queries/README.md](../tools/native-reconstruction/ghidra/queries/README.md) | 10-20 | Ghidra query snippets |

## 9. Native engine R5–R7 (in `main`)

| Report | Date | What |
|---|---|---|
| [NATIVE_ENGINE_RECONSTRUCTION.md](NATIVE_ENGINE_RECONSTRUCTION.md) | 10-20 | First native replacements of engine functions, static recompilation |
| [NATIVE_ENGINE_FUNCTIONS.md](NATIVE_ENGINE_FUNCTIONS.md) | 10-20 | Function registry: every function and its status |
| [NATIVE_RECOMP_COMPILER.md](NATIVE_RECOMP_COMPILER.md) | 10-20 | The static recompiler (R5) |
| [NATIVE_RECOMP_CUE.md](NATIVE_RECOMP_CUE.md) | 10-20 | R5-C: CUE buffer setters, commit analysis |
| [NATIVE_RECOMP_R5_D.md](NATIVE_RECOMP_R5_D.md) | 10-20 | R5-D: material setup, CUE setters, CUE commit |
| [NATIVE_RECOMP_R5_E_GBUFFER.md](NATIVE_RECOMP_R5_E_GBUFFER.md) | 10-20 | R5-E: G-buffer / geometry pass |
| [NATIVE_RECOMP_R5_F_SHADOW.md](NATIVE_RECOMP_R5_F_SHADOW.md) | 10-20 | R5-F: shadow subsystem |
| [NATIVE_RENDER_R6_CORE.md](NATIVE_RENDER_R6_CORE.md) | 10-21 | R6-Core: native render bridge |
| [NATIVE_RENDER_R6_GBUFFER.md](NATIVE_RENDER_R6_GBUFFER.md) | 10-21 | R6-G: G-buffer draws verified in game |
| [NATIVE_RENDER_R6_SHADOWS.md](NATIVE_RENDER_R6_SHADOWS.md) | 10-21 | R6-S: shadow loop and draw verified |
| [NATIVE_RENDER_R6_CP_OPTIMIZATION.md](NATIVE_RENDER_R6_CP_OPTIMIZATION.md) | 10-21 | R6-P: command processor (city +3.97 FPS) |
| [NATIVE_RENDER_R6_RESULTS.md](NATIVE_RENDER_R6_RESULTS.md) | 10-21 | **R6 results** |
| [NATIVE_ENGINE_R7_CORE.md](NATIVE_ENGINE_R7_CORE.md) | 10-21 | R7-Core: native render layer |
| [NATIVE_ENGINE_R7_GBUFFER.md](NATIVE_ENGINE_R7_GBUFFER.md) | 10-21 | R7-G: native G-buffer pass |
| [NATIVE_ENGINE_R7_SHADOWS.md](NATIVE_ENGINE_R7_SHADOWS.md) | 10-21 | R7-S: hand-written shadow engine |
| [NATIVE_ENGINE_R7_RESOURCES.md](NATIVE_ENGINE_R7_RESOURCES.md) | 10-21 | R7-R: native resource / material / CUE state |
| [NATIVE_ENGINE_R7_RESULTS.md](NATIVE_ENGINE_R7_RESULTS.md) | 10-21 | **R7 results**: 13 functions at once, 0 mismatches, FPS neutral |

## 10. PC input, interface and settings

| Report | Date | What |
|---|---|---|
| [PHASE4_PC_INPUT_UI_RESEARCH.md](PHASE4_PC_INPUT_UI_RESEARCH.md) | 10-10 | Keyboard, mouse, touchpad, prompts, menus: research |
| [PHASE4_PC_UI_FOUNDATION.md](PHASE4_PC_UI_FOUNDATION.md) | 10-11 | PC interface foundation |
| [PC_INPUT_PROMPTS_PLAN.md](PC_INPUT_PROMPTS_PLAN.md) | 10-09 | PC button prompts: research and plan |
| [PC_INPUT_PROMPTS.md](PC_INPUT_PROMPTS.md) | 10-18 | Button prompts by input device |
| [PC_CONTROLS_SETTINGS_INTEGRATION.md](PC_CONTROLS_SETTINGS_INTEGRATION.md) | 10-12 | PC controls and settings merged into the project |
| [PC_SETTINGS_IMPLEMENTATION.md](PC_SETTINGS_IMPLEMENTATION.md) | 10-20 | PC settings and the F1 menu (history) |
| [PC_EDITION_UI.md](PC_EDITION_UI.md) | 10-20 | Settings in the game's own menu, the mouse in its menus |
| [PC_SETTINGS_UI.md](PC_SETTINGS_UI.md) | 10-20 | **The redesigned settings window** |

## 11. Branches: work in progress and experiments

Branch reports, linked on GitHub (they are not in `main`).

**R8 — native CP optimization** (in progress):

| Report | Branch | What |
|---|---|---|
| [NATIVE_ENGINE_R8_CORE.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-engine-r8/docs/NATIVE_ENGINE_R8_CORE.md) | `feature/native-engine-r8` | Goals, ownership, interfaces, baseline series of R8 |
| [NATIVE_ENGINE_R8_CP_PROFILE.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-engine-r8/docs/NATIVE_ENGINE_R8_CP_PROFILE.md) | `feature/native-engine-r8`, `feature/r8-cp-profile` | Profile of the command processor thread |
| [NATIVE_ENGINE_R8_CP_OPTIMIZATION.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/r8-native-cp/docs/NATIVE_ENGINE_R8_CP_OPTIMIZATION.md) | `feature/r8-native-cp` | The native state channel to the command processor, PS reuse design |
| [NATIVE_ENGINE_R8_RENDER_LAYER.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/r8-render-layer/docs/NATIVE_ENGINE_R8_RENDER_LAYER.md) | `feature/r8-render-layer` | Native render layer: one entry for the CUE commit, mesh / shadow draws |
| [NATIVE_ENGINE_R8_RECOMPILER.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/r8-recomp/docs/NATIVE_ENGINE_R8_RECOMPILER.md) | `feature/r8-recomp` | CUE audit: the commit's Gnm writers, other user-data writers |

**Fork port** — [`feature/fork-port-prep-workers`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/tree/feature/fork-port-prep-workers): the frontend thread with the city preset, queued occlusion queries and flips, the Vulkan emit thread, potato mode for weak PCs. No separate report yet; the commit messages describe each step.

**Native Rebirth** — a separate native program on the game's data ([`feature/native-rebirth`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/tree/feature/native-rebirth), continued in [`feature/native-missions`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/tree/feature/native-missions)):

| Report | What |
|---|---|
| [native/ARCHITECTURE.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/ARCHITECTURE.md) | Architecture of the native program |
| [native/N0_DEPENDENCY_AUDIT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/N0_DEPENDENCY_AUDIT.md) | Dependency audit: no eboot, no shadPS4 |
| [native/DEPENDENCIES.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/DEPENDENCIES.md) | Dependencies |
| [native/RESOURCE_FORMATS.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/RESOURCE_FORMATS.md) | KCAP packages, textures, skeletons, animation clips, Havok |
| [native/REVERSE_ENGINEERING.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/REVERSE_ENGINEERING.md) | Engine functions behind the formats |
| [native/N11_FIRST_PLAYABLE_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/N11_FIRST_PLAYABLE_REPORT.md) | N11: first playable — original animations, collision, several world chunks |
| [native/missions/MISSION_FORMAT_RESEARCH.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/missions/MISSION_FORMAT_RESEARCH.md) | Mission data format |
| [native/missions/GRAFFITI_RESEARCH.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/missions/GRAFFITI_RESEARCH.md) | Graffiti mini-game research |
| [native/missions/N12_CAMPAIGN_RECONSTRUCTION.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/missions/N12_CAMPAIGN_RECONSTRUCTION.md) | N12: the prologue's mission reconstructed |
| [native/missions/N12_TEST_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/missions/N12_TEST_REPORT.md) | N12 tests, runs, performance, screenshots |
| [native/KNOWN_LIMITATIONS.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/KNOWN_LIMITATIONS.md) | What is approximated or missing |
| [native/IMPLEMENTATION_LOG.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/IMPLEMENTATION_LOG.md) | Implementation log |
| [native/SESSION_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/SESSION_REPORT.md) | Session report |
| [native/ROADMAP.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/native-missions/docs/native/ROADMAP.md) | Roadmap of the native program |

**Measured and rejected:**

| Report | Branch | What |
|---|---|---|
| [BBPORT_GPU_EXPERIMENT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/experiment/bbport-gpu/docs/BBPORT_GPU_EXPERIMENT.md) | `experiment/bbport-gpu` | BBPort's GPU library as the backend: 25 vs 40 FPS, effects missing |
| [PORT_OPTIMIZATION_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/feature/uncharted-bbport-optimizations/docs/PORT_OPTIMIZATION_REPORT.md) | `feature/uncharted-bbport-optimizations` | uncharted_pc / bbport techniques: what was ported, A/B of 7 configurations, verdicts |
