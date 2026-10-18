# inFAMOUS: Second Son — PC Edition

**A Windows PC port project for inFAMOUS: Second Son.**

An independent engineering project focused on running, understanding, and progressively rebuilding the game's runtime for Windows, with a custom Vulkan graphics backend and native system implementations.

> **Status: In development.** The game launches and reaches gameplay. Core runtime components and graphics systems are being progressively replaced with custom implementations. Performance, compatibility, and long-session stability remain under active development.

<p align="center">
  <img src="pc-edition-banner.png"
       alt="inFAMOUS: Second Son — PC Edition"
       width="100%">
</p>

<p align="center">
  <a href="#project-status">Status</a> ·
  <a href="#features">Features</a> ·
  <a href="#architecture">Architecture</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#known-issues">Known issues</a> ·
  <a href="#roadmap">Roadmap</a>
</p>

---

## Overview

*inFAMOUS: Second Son — PC Edition* is an independent project exploring a native Windows implementation of the runtime required to run the original PlayStation 4 game.

The project began with a working compatibility layer and has progressively evolved toward a custom runtime, custom graphics infrastructure, and native replacements for PlayStation system services.

The long-term objective is to reduce and ultimately eliminate mandatory dependencies on shadPS4, while preserving the original game's rendering, gameplay, and content.

This is not a remake, a new game engine, or a reconstruction of the original source code. The project works with the original game's executable and assets while replacing and extending the platform-specific infrastructure around them.

### Project goals

- Run the original game on Windows.
- Build a custom runtime for PlayStation 4 game code.
- Maintain a custom Vulkan graphics backend.
- Replace compatibility-layer system services with native Windows implementations.
- Improve CPU-side command processing and frame-time consistency.
- Preserve original game assets, gameplay logic, and visual quality.
- Investigate the feasibility of binary recompilation for future stages.

## Project status

| Component | Current status |
|---|---|
| Windows executable and custom loader | Implemented |
| Original game code execution | Working |
| Custom graphics backend | Implemented and under active development |
| Vulkan rendering and resource management | Implemented; ongoing compatibility work |
| Native kernel, threading and memory components | Enabled by default; regression testing ongoing |
| Native exception handling and CPU patcher | Implemented; compatibility testing ongoing |
| Offline network and system services | Partially replaced |
| Native save-data implementation | Implemented; compatibility tested on copied saves |
| Native audio output through WASAPI | Implemented; extended stability testing required |
| Video decoding | In development |
| ATRAC9 / Ajm audio decoding | Pending |
| Complete removal of shadPS4 | In progress |
| Custom GCN-to-SPIR-V translator | Future major milestone |
| Binary recompilation into a Windows-native executable | Research phase, not yet implemented |
| Multiplayer co-op | Research idea; no implementation claimed |

The status above describes the state reported during development. Each component may have additional limitations that are not yet documented here.

## Features

### Custom Windows runtime

The project includes a custom executable and runtime infrastructure designed to run the original game on Windows.

Current work includes:

- Custom loading and initialization.
- Native implementations of selected PlayStation kernel interfaces.
- Custom thread and virtual-memory management.
- Native exception handling.
- A CPU patching system.
- Progressive replacement of shadPS4 system services.

The goal is to minimize compatibility overhead and gain direct control over the execution environment.

### Custom Vulkan graphics backend

SecondSonPC has its own graphics backend for translating the game's GPU commands into Vulkan operations.

Ongoing development covers:

- GPU command processing.
- Graphics and compute pipelines.
- Resource bindings and descriptors.
- GPU memory management.
- Synchronization and resource lifetime.
- Shader pipeline caching and compilation.
- CPU-side command preparation.
- Rendering correctness and performance profiling.

The graphics backend is already operational, but the shader translation pipeline still depends on shadPS4 components.

### Lighting and occlusion

A confirmed rendering defect involving vehicle-headlight glow passing through fences and other occluders was traced to incorrect occlusion-query results.

The original game uses visibility queries to determine how much of a light-glow proxy is visible. The previous implementation returned equivalent values for queries with and without depth testing, effectively forcing full visibility.

The fix introduces real Vulkan occlusion queries and makes their results available to the game's original compute shader.

**Implementation:** commit `146a3bd`.

Reported validation results:

- Headlight glow behind a fence was absent in 8 out of 8 tested frames after the fix.
- Directly visible streetlights and selected environmental lights remained visible.
- Vulkan Validation and Synchronization Validation reported no messages during the documented test.

The tested scene did not include a vehicle with headlights directly visible to the camera, so further regression testing is required.

### Native audio output

The project now includes a Windows WASAPI audio-output implementation, allowing the game to output audio without the bridge's SDL audio implementation.

**Implementation:** commit `245d86d`.

A compatibility fallback remains available through:

`play_rea.cmd --bridge audio`

Extended stability testing is still required following a previous crash associated with near-exhaustion of system commit memory. The cause has not been conclusively linked to audio.

### Save-data compatibility

The custom save-data implementation has loaded existing saves and completed automatic save operations using copied user save data.

The `param.sfo` output was reported to match the bridge's output byte for byte in the tested scenario.

The original user saves were not modified during those tests.

This is an important compatibility milestone, but it does not yet establish correctness for every save-data operation or every possible recovery scenario.

### Input and controller support

SecondSonPC includes custom keyboard and mouse input handling together with PC-oriented control settings.

The goal is to provide a natural PC gameplay experience while preserving the original game's visual identity and compatibility with controller-based play.

Current work in this area includes:

- Custom keyboard and mouse input handling.
- PC-specific control settings and key bindings.
- Adaptation of contextual gameplay prompts for keyboard and mouse.
- Preservation of the original PlayStation prompt artwork and overall visual style.
- Development of controller-specific prompt variants for PlayStation and Xbox layouts.
- Input-device-aware prompt switching based on the currently active control method.

The intended behavior is to display the correct contextual prompts for the active input device. When the player switches between keyboard/mouse and a gamepad, the game should update the visible prompts accordingly without permanently replacing the original assets.

### Input and controller support

SecondSonPC includes custom keyboard and mouse input handling and PC-oriented control settings, bringing the original PlayStation 4 game closer to a native PC experience.

The goal is to provide intuitive PC controls while preserving the original game's visual identity and compatibility with supported game controllers.

Current implementation and development include:

- Custom keyboard and mouse input handling.
- PC-specific control settings and key bindings.
- Adaptation of contextual gameplay prompts for keyboard and mouse.
- Keyboard key representations integrated into the original prompt design.
- Preservation of original PlayStation button icons for players using PlayStation controllers.
- Support for different controller prompt layouts without permanently replacing the original game assets.
- Development of automatic prompt selection based on the active input device.

The intended behavior is to display the appropriate button prompts for the player's current input method. Keyboard and mouse prompts should be displayed during keyboard/mouse gameplay, while the original controller prompts remain available when using a supported gamepad.

#### Keyboard and mouse prompt adaptation

The original game features distinctive, textured button prompts that are integrated into its visual design. For the PC version, these prompts are being adapted to use keyboard keys while retaining the original artwork's colors, textures, and overall presentation.

The examples below demonstrate the visual adaptation of gameplay prompts, including keyboard mappings such as `F`, `V`, `G`, `X`, and `TAB`.

<p align="center">
  <img src="prw1.png" alt="Keyboard and mouse prompt adaptations for inFAMOUS: Second Son PC Edition" width="900">
</p>

#### Controller and alternate prompt variants

Controller support remains an important part of the PC version. Original PlayStation button icons are not permanently replaced, allowing the game to retain its original presentation when played with a PlayStation controller.

Alternative prompt designs are also being explored for other supported input methods, including Xbox-style controller layouts and keyboard/mouse controls.

<p align="center">
  <img src="prw2.png" alt="Original-style controller prompts and alternative button layouts" width="900">
</p>

**Development status:** Custom keyboard and mouse input handling and PC control settings are implemented. Prompt adaptation and input-device-aware switching are being developed and require further in-game verification before they can be considered complete.
## Performance and profiling

Performance work focuses on identifying real bottlenecks rather than applying visual shortcuts.

The project uses controlled experiments, instrumentation, and A/B comparisons to investigate:

- CPU command-processing overhead.
- Graphics and compute pipeline costs.
- Shadow rendering.
- GPU memory transfers.
- Synchronization stalls.
- Pipeline creation and caching.
- Resource binding and command preparation.
- Frame-time consistency.

### Shadow rendering investigation

Profiling of a city scene identified approximately 1,100 small shadow draw calls per frame associated with vehicle headlights and spotlights.

The reported CPU command-processing cost was approximately 2.4 ms per frame for this work.

An experiment that attempted to reuse shader preparation was reverted because the relevant draw calls required different object-specific data and did not benefit from the proposed cache.

The findings are documented in `docs/LIGHTING_PROFILE.md`.

The current approach is to preserve shadow quality and investigate architectural optimizations only when a measurable and correct improvement is plausible.

### Performance reporting

Performance figures should always identify the tested scene, build, graphics settings, measurement interval, and whether the system was under other CPU/GPU load.

Historical measurements have been affected by concurrent builds and background workloads. They should not be treated as representative benchmarks for the current build without a controlled retest.

No universal FPS target or hardware recommendation is claimed at this stage.

## Architecture

The project is being developed in several layers.

```text
Original PS4 game
        |
        v
Custom loader and runtime
        |
        +-- Native kernel interfaces
        +-- Threading and virtual memory
        +-- Exception handling and CPU patching
        +-- System service compatibility
        +-- Save-data handling
        +-- Audio and video subsystems
        |
        v
Custom graphics command processor
        |
        +-- GPU command translation
        +-- Resource and descriptor management
        +-- Pipeline caching
        +-- Vulkan synchronization
        |
        v
Vulkan graphics backend
        |
        v
Windows GPU driver
```

This is a conceptual overview, not a complete call graph. Some subsystems still use shadPS4 implementations, and the remaining dependencies are being audited.

### Native system implementations

The following components have been replaced or substantially implemented outside the bridge, according to current development reports:

- Kernel, threading, memory management, exception handling, and CPU patching.
- Selected offline networking and security-related services.
- HTTP offline functions.
- Trophy service compatibility.
- Kernel event queues.
- Save-data handling.
- WASAPI audio output.

The precise set of functions and their completeness should be tracked in the technical documentation and verified against the current build.

### Remaining compatibility dependencies

The current work includes replacing:

- Video decoding interfaces (`sceVideodec*`).
- Ajm / ATRAC9 decoding.
- Remaining network and socket functions.
- Save-data and system-message dialogs.
- Small libc compatibility functions such as `fflush` and `vprintf`.

After the system-service work, a major remaining task is the GCN-to-SPIR-V shader translator. The long-term objective is to remove mandatory shadPS4 dependencies from the build and runtime, not merely rename or wrap them.

## Development history

The project has progressed through several major stages.

### Stage 1 — Initial Windows execution

- Established a custom executable and launch workflow.
- Reached playable game scenes.
- Built the foundation for PC-specific controls and settings.
- Began profiling CPU and GPU bottlenecks.

### Stage 2 — Custom graphics infrastructure

- Developed GPU command-processing components.
- Integrated a Vulkan rendering path.
- Added pipeline caching and shader-related instrumentation.
- Investigated graphics correctness and GPU synchronization.

### Stage 3 — Performance and rendering research

- Profiled CPU command processing and GPU workloads.
- Investigated shadow rendering and lighting costs.
- Implemented real Vulkan occlusion queries for light-glow visibility.
- Added controlled A/B experiments and documented unsuccessful optimization attempts.

### Stage 4 — Native runtime replacement

- Enabled custom kernel, threading, memory, exception, and CPU-patching components by default.
- Replaced selected offline system services.
- Implemented event-queue handling.
- Added native save-data functionality.
- Implemented WASAPI audio output.

### Stage 5 — Current development

- Replace remaining video, audio-decoding, networking, and dialog dependencies.
- Validate long-session stability and memory behavior.
- Finish controller-prompt improvements.
- Continue reducing shadPS4 dependencies.
- Investigate the feasibility of a custom shader translator and binary recompilation.

## Installation

**This project does not distribute the original game, game executable, game assets, console firmware, or copyrighted game content.**

A legally obtained copy of the original game is required. Users are responsible for obtaining and preparing their own game files.

The exact required game version, directory layout, and setup procedure must be documented against a tested public build before this section is considered complete.

### Requirements

- A supported 64-bit Windows installation.
- A compatible graphics card and Vulkan driver.
- The user's own supported game files.
- Any runtime dependencies specified by the corresponding release.

**Tested hardware and software configurations:** to be documented after reproducible compatibility testing.

### Launching the game

The current development workflow uses the project's launch scripts, including `play.cmd`.

Launch script names and required arguments may change during development. Follow the instructions included with the specific build rather than assuming every development script is suitable for public distribution.

## Known issues

The project is under active development. Known or unresolved areas include:

- Performance varies significantly by scene and CPU-side workload.
- Long-session stability and system-memory usage require additional testing.
- The WASAPI implementation needs further validation after a memory-exhaustion crash.
- Video decoding and ATRAC9 support are still being developed.
- Some system services remain dependent on shadPS4.
- The shader translation pipeline is not yet fully independent.
- Controller prompts and other UI improvements require final integration and regression testing.
- Lighting fixes have been validated in specific scenes but require broader testing.
- No complete campaign compatibility guarantee is claimed.

## Roadmap

### Phase 1 — Runtime and system services

- [x] Custom kernel, threading, memory, exception handling, and CPU patching enabled by default.
- [x] Offline HTTP implementation.
- [x] Selected offline network and trophy services.
- [x] Kernel event queues.
- [x] Save-data implementation and compatibility checks.
- [x] Native WASAPI audio output.
- [ ] Complete stability and memory regression testing.
- [ ] Implement remaining networking and libc functions.
- [ ] Complete system-message and save-data dialog support.

### Phase 2 — Audio and video

- [x] Native audio-output foundation.
- [ ] Implement and validate `sceVideodec*` using an independently linked FFmpeg dependency.
- [ ] Implement Ajm / ATRAC9 decoding.
- [ ] Validate cutscene playback and audio/video synchronization.
- [ ] Test repeated playback and resource cleanup.

### Phase 3 — Graphics and performance

- [x] Custom Vulkan graphics backend.
- [x] Real Vulkan occlusion queries for light-glow visibility.
- [x] Lighting and shadow performance profiling.
- [ ] Reduce unnecessary CPU-side draw preparation.
- [ ] Improve frame-time consistency.
- [ ] Complete GPU synchronization and resource-lifetime regression tests.
- [ ] Replace remaining shader translation dependencies.

### Phase 4 — Complete independence

- [ ] Audit all runtime and build dependencies.
- [ ] Replace remaining mandatory shadPS4 components.
- [ ] Build and launch without the bridge.
- [ ] Validate gameplay, saves, audio, video, and graphics.
- [ ] Verify performance against a controlled baseline.

### Phase 5 — Binary recompilation research

After the runtime becomes independent, investigate whether parts of the original x86-64 game code can be translated into a Windows-compatible executable.

Initial research will focus on individual functions and modules, correctness validation, ABI compatibility, and measurable performance.

Full-game binary recompilation is a research objective, not a completed feature.

### Future ideas

- Expanded controller support and contextual prompts.
- Further native Windows integration.
- Research into cooperative multiplayer feasibility.

These ideas are exploratory and are not currently promised features.

## Contributing

Technical contributions, reproducible bug reports, performance traces, and documentation improvements are welcome, subject to the project's contribution policy.

Before submitting a performance optimization:

1. Record a reproducible baseline.
2. Change one variable at a time.
3. Validate rendering correctness.
4. Run the relevant Vulkan validation checks.
5. Report average FPS, 1% lows, and frame-time percentiles when available.
6. Document failed experiments to prevent repeated work.

Do not submit game dumps, copyrighted game assets, or other proprietary game content.

## Legal notice

This is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment or Sucker Punch Productions.

inFAMOUS and inFAMOUS: Second Son are trademarks and/or copyrighted works of their respective owners.

No original game content is intentionally distributed by this repository. Users must supply their own legally obtained game files.

The licensing terms of third-party components remain applicable to those components. Consult the relevant license files for details.

## Acknowledgements

- The authors and maintainers of the open-source tools used in the project.
- The maintainers of Vulkan-related libraries and development tooling.
- Researchers documenting PlayStation 4 hardware, graphics behavior, and executable formats.
- Contributors who help reproduce bugs, analyze traces, and validate fixes.

## Disclaimer

This project is experimental software. Features may be incomplete, compatibility may vary, and regressions may occur. Back up save data before testing any development build.

**The goal is a technically independent Windows port that preserves the original game while replacing the platform-specific infrastructure required to run it.**
