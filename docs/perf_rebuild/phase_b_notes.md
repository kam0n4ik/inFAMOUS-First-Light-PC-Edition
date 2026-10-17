# Phase B notes (2026-10-17): shadPS4 upstream and BBPort

Source review only (GitHub pages/raw files); no game run on another system - a direct A/B is not possible (shadPS4 and
BBPort run other games; the bridge copy in third_party/shadPS4 is an older fork of shadPS4's renderer).

- **shadPS4 upstream (main)**: one Vulkan queue (graphics family, index 0, also present); no async compute. Compute
  rings (1 gfx + 56 ACE) are coroutines resumed round-robin by one thread; everything recorded into one command
  buffer. GPU-written memory is **not** read back by default (`readbacks_mode` Disabled; Relaxed/Precise read back on a
  page-protection fault into host-cached staging, window up to 512 KiB). Push descriptors, dynamic rendering, sync2,
  EDS3; no descriptor buffers, no multithreaded recording. Recent perf PRs: #5047/#5100 (sparse arena buffer cache,
  batched uploads), #5112/#5219 (renderer CPU optimizations), #5277 (lock-free region manager) - no frame numbers given.
- **BBPort** (github.com/deadinside28/bloodborne_pc, GPL-2.0, 2026-10): native x86-64 eboot, own HLE, vendored shadPS4
  video_core. No async compute. docs/parallel_gpu.md: a two-stage pipeline (stage A PM4 decode + register deltas into a
  16 MiB ring, stage B owns caches/descriptors/recording) +18-19 %; speculative "DrawPrep" workers verified word by word
  +11.5 %; render-state memo +9 %, texture-set memo +5.6 %, 32 MiB constant ring +11 %, 64 KiB write-fault unprotect
  window +22 %, LTO+PGO +4 % (their numbers, their game).

| Subsystem | SecondSonPC | shadPS4 upstream / BBPort | Transferable idea |
|---|---|---|---|
| CP thread | PM4 + per-draw preparation on thread 1 (~1.7-1.8 us each) | BBPort: decode only on stage A, preparation on stage B | SecondSonPC already splits CP / recording; the preparation could move (P5.3 frontend thread measured no gain - recording thread and GPU limit) |
| Draw preparation memo | stage memo (misses: dispatch inputs differ 91 % of the time, CC-1) | render-state / texture-set memo | the memo exists; misses are inherent here |
| Recording | one thread, push descriptors, EDS3, dynamic rendering | same in upstream | nothing new |
| Async compute | one queue (AQ below) | none in either | own implementation (FrameBreak AQ) |
| GPU write-back | eager at batch end, 2.2-2.5 ms/frame (village) | upstream: none by default; lazy on fault | SSPC_FB_LAZY_WB already does the lazy model; correctness requires the readers SecondSonPC has |
