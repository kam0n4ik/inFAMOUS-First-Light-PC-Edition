# SecondSonPC: Vulkan and GPU performance optimization

The stage after the Native CP (`0743ef0`, docs/NATIVE_CP.md). Only measured figures are written here; every A/B is
interleaved (`tools/tests/p53_perf_ab.ps1`), one exe copy per series, window 62-88 s, 1296×759, mailbox, no upscaler,
no frame generation. RTX 2060 6 GB, Ryzen 5 5500, 32 GB.

Frame times are quantized by the 60 Hz vblank: a frame takes 16.7 or 33.4 ms (rarely 50). The average FPS follows the
share of one-vblank frames; the 1 % low stays at ~29.3 FPS (two vblanks) until fewer than 1 % of frames miss the
16.7 ms - the 1 % low only moves when a scene is close to 60 FPS or when frames take three vblanks.

## GPU-1: the frame handed to the processor at sceGnmSubmitDone (taken into Native)

P5.4's experiment (`SSPC_P54_EARLY_SUBMIT=1`, docs/native-reconstruction/P54_FRAME_LIFECYCLE.md): the engine's
SubmitFrame has its command buffer list ready at its step 4 (`sceGnmSubmitDone`) and then waits for the previous
frame's end of pipe before step 6 (`sceGnmSubmitAndFlipCommandBuffers`). The frame is handed to the command processor
at step 4; step 6 returns that result. The processor works on frame k while the GPU finishes k-1. Measured before
only on the old Parallel base; never together with Native.

A/B Native vs Native + early (`logs/p53/opt1_early`, n = 3, interleaved):

| Scene | Native FPS | + early FPS | 1 % low | frames of 1 vblank | p95 ms |
|---|---|---|---|---|---|
| beach | 45.87 [42.38..48.20] | **50.74 [48.85..54.01]** (+10.6 %) | 28.88 -> 29.34 | 69 % -> 82 % | 33.81 -> 33.52 |
| early (harbour) | 41.65 [39.24..43.07] | **49.65 [49.20..49.93]** (+19.2 %) | 28.86 -> 29.33 | 56 % -> 79 % | 33.95 -> 33.73 |
| user1022 (village) | 37.38 [37.03..37.74] | 37.61 [37.52..37.74] (within the spread) | 29.18 -> 29.25 | 40 % -> 41 % | 34.03 -> 34.02 |

The ranges of the beach and the harbour do not overlap. The processor's and the GPU's work per frame did not change
(cp 19.2 / 19.3 ms, GPU 13.8 / 14.0 ms on the beach): the gain is the processor no longer idle in the GPU's tail.
The early mode was active in all 9 runs, `[Error]` 0, device lost 0.

Checks: Vulkan Validation + Sync with Native + early, beach and village (`logs/opt/opt_early_validate`): **0
messages**. With the upscalers (beach, validation on): FSR 3.1 Quality - the same 41 messages as before inside AMD's
FSR shader; DLSS Quality + FSR frame generation and FSR Native AA + frame generation - 241 / 191 messages, **the same
241 without the early mode** (`up_dlss_fg_noearly`): not from this change (see GPU-2).

Taken into the Native preset (`cp_mode.cpp`): `SSPC_P54_EARLY_SUBMIT=1`; `SSPC_P54_EARLY_SUBMIT=0` given explicitly
turns it off. Its own guards stay (SubmitFrame hash, exact return address, the frame object, 1..512 buffers): any
check that fails runs the ordinary path.

### Why the village did not gain: the async compute wait

Lifecycle trace, Native + early (`logs/p412/opt2_trace2`, `tools/p54/p54_lifecycle.py`, 436 frames): what held the
next frame - the game waiting for its async compute label (0x1695e0) 62 %, the processor still on frame k 38 %. The
game waits twice a frame (19.1 ms in all):

| | short wait | long wait |
|---|---|---|
| DingDong -> the processor holds the RELEASE_MEM, ms | 1.5 | 12.1 |
| of it: before the first compute slice | 1.4 | 1.4 |
| compute slices (the compute queue's own packets) | 0.05 ms, 175 packets | 7.0 ms, ~20 800 packets in ~6 slices |
| graphics slices between them | 1.2 | 4.1 |
| the work on the GPU (batch start -> end) | | 1.3 (all waits) |

**Compute priority (measured, rejected).** `SSPC_CP_COMPUTE_PRIORITY=1` (compute queues first in every round and
without the 4096-packet limit; `logs/p53/opt3_cprio`, n = 3): village 37.84 -> **36.07 FPS, 1 % low 29.26 -> 19.83,
p99 34.1 -> 50.2 ms** (the GPU tail after the processor's flip 6.3 -> 8.7 ms: frame k's graphics end later); beach
and harbour within the spread. The code was removed. The compute wait is the processor's own work on ~20 800 packets
(7 ms): what would shorten it is less processor time per compute packet, not another order.

## GPU-2: frame generation's backbuffer copy (fixed)

`vk_presenter.cpp`: the frame without the HUD at the backbuffer's size (`fg_hudless_bb`), handed to AMD's frame
generation, was created without `TRANSFER_SRC`; AMD's library copies out of it (VUID-vkCmdCopyImage-aspect-06662,
VkImageMemoryBarrier-oldLayout-01212 - 80 of the 241 messages). After: DLSS + FSR FG 241 -> 159 messages, `[Error]`
43 -> 23 (`logs/opt/up_dlss_fg_fix`). Remaining, not fixed here: inside AMD's library (`AMD FSR Replacement
BackBuffer` layout at a draw - VUID-vkCmdDraw-None-09600 20; `AMD FSR PresentQueue` signalling a swapchain semaphore -
00067 4) and two seen once at one submission (a clear after a transition - SYNC-HAZARD-WRITE-AFTER-WRITE; an image
view of a transfer-only image - 04441).

## Where the time goes after GPU-1 (Native + early, 2026-10-16)

GPU time per effect category, Performance Inspector (`SSPC_INSPECTOR=light`: timestamps at category changes, window
62-88 s; `logs/performance_inspector/2026-10-16_09-21-29`, `_09-25-51`, `_09-30-13`), ms per frame; "rec" = the
recording thread's CPU time of those commands:

| Category | beach GPU (rec) | harbour GPU (rec) | village GPU (rec) | draws / dispatches (beach / village) |
|---|---|---|---|---|
| all | 13.89 | 15.90 | 21.64 | |
| compute, other | 1.14 (0.28) | 1.73 (0.67) | **4.07** (4.56) | 46 / 1110 dispatches |
| geometry / G-buffer | 2.70 (3.56) | 3.52 (5.09) | 3.28 (3.56) | 854 / 855 draws |
| post-processing | 2.85 (0.45) | 2.94 (0.44) | 3.12 (0.44) | 63 / 67 |
| write-backs and copies (GPU -> host) | 1.44 | 1.43 | **3.45** | |
| smoke | 0.26 (0.03) | 0.56 | **2.03** (0.03) | 2 / 2 draws |
| lighting | 1.17 | 1.15 | 1.43 | 1 dispatch |
| shadows | 1.35 (**4.73**) | 1.33 (4.01) | 0.84 (3.43) | **3487** / 2271 draws |
| particle simulation | 1.19 (2.37) | 1.06 | 0.96 | 518 / 413 dispatches |
| foliage (classified) | 0.06 (1.69) | 0.02 | 0.02 | 836 / 219 draws |

Foliage costs the GPU almost nothing; its cost is per draw on the CPU (the shadow cascades draw the foliage again:
3487 shadow draws a frame on the beach, 4.7 ms of recording). The village's GPU is compute (1110 small dispatches),
the write-backs and one smoke draw (2 ms: 5-6 thousand particle quads blended into a 1920×1080 RGBA16F target).

The processor's thread (CPU samples, `logs/p53/opt4_prof`): the frontend's PrepareStage 27-32 %, CopyStage 8-11 %,
**ShaderFrontend::GpuWrote 5.2 % (beach) / 8.4 % (village)**, RegisterFile::Write 4-6 %, waits 10-15 %.

## GPU-3: tracked shader code blocks by a bitmap (taken)

`ShaderFrontend::GpuWrote` (P5.2 code trust) runs for every written binding of every draw and dispatch and looked up
an `unordered_set` once per 64 KiB block of the binding (a 16 MiB pool = 256 lookups; MSVC's `std::hash<u64>` is
FNV-1a). Now a bitmap of the blocks below 1 TiB, checked a 64-bit word at a time, and the blocks' bounds; the set
stays for anything above. The same answer by construction (a randomized check of the word masks against the set:
200 000 ranges, 0 different); self-tests (shader) 126 / 0.

A/B (`logs/p53/opt5_codebits`, n = 3, two exes interleaved: `p53_perf_ab.ps1 -ExeB`):

| Scene | before | after | processor thread ms |
|---|---|---|---|
| beach | 53.97 [53.94..54.01] | **55.19 [55.13..55.24]** (+2.3 %) | 18.05 -> 17.63 |
| early | 50.07 [50.00..50.20] | **50.68 [50.59..50.82]** (+1.2 %) | 19.30 -> 18.94 |
| user1022 | 37.36 [36.67..37.79] | 37.24 [35.50..38.22] | 21.55 -> 21.58 |

The village's low run (35.50, 1 % low 20.1) is the new exe's first run: its shaders were translated again while it
ran (829 translations, 1.2 s of pipeline creation in the measured window); its other two runs 38.22 / 38.00.

## GPU-4: unchanged draw stages handed on as one shared copy (measured, rejected)

Thread 1 copies each draw's two prepared stages into the record (CopyStage). A stage the frontend's memo kept
unchanged was handed on as one shared immutable copy instead (a generation counter per stage). Verify mode
(`logs/opt/share_verify`): 9.57 M stages shared on the beach, 7.90 M in the village, **0 different**; register
replica 35.9 M draws/dispatches, 0 different. But A/B (`logs/p53/opt6_share`, n = 3): beach 54.55 -> 53.93 FPS,
processor +0.18 ms; harbour and village within the spread - one allocation per 5 shares and the shared pointers'
atomic counts between the two threads cost what the copies saved. Removed.

## GPU-5: a two-multiplication hash for the per-draw maps keyed by u64 (taken)

MSVC's `std::hash<u64>` is FNV-1a over 8 bytes (`std::_Fnv1a_append_value` 1.5-2.4 % of the processor's thread).
`runtime/gpu/u64_hash.h` (MurmurHash3's finalizer) for the maps looked up per draw / per binding: programs by address
and fetch shaders (`shader_cache.h`), pipelines and layouts (`vk_pipelines.h`), the texture memo and the pending fast
clears (`vk_executor.h`), image views and samplers (`vk_image_cache.h`). No code depends on these maps' order
(lookups, or walks that mark every match).

A/B (`logs/p53/opt7_hash`, n = 3, the new build's shader cache warmed first: `logs/opt/warm_opt6`): beach 54.58
[54.40..54.78] -> **55.15 [54.86..55.40]** (+1.0 %, processor 17.83 -> 17.66 ms); harbour 50.53 -> 50.60 and village
38.12 -> 38.05, within the spread.

Note for every A/B of a build: the persistent shader cache's generation is a hash of `src/runtime/gpu/shader/*`
(CMakeLists.txt): a change there makes the new build translate its shaders again in its first run (GPU-3's village
run). Warm the new build's cache before a series.

## GPU-6: written bindings in video memory are not shadowed (taken)

`Executor::PrepareShadows` copies a dispatch's written bindings (<= 256 KiB, >= 8 workgroups) into dense shadows and
back with two global barriers, because a shader's stores into the host memory mirror are ~150x slower (P4.4). A
binding the buffer cache placed in video memory is written as fast as a shadow: copying it in and out was work for
nothing. Such bindings are now left in place (`SSPC_GPU_SHADOW_VRAM=keep`: as before). Counted: in the village 12 872
of 56 699 shadow candidates were in video memory (`logs/opt/shvram_validate`); Vulkan Validation + Sync, village and
beach: **0 messages**.

A/B (`logs/p53/opt8_shvram`, n = 3, interleaved):

| Scene | GPU busy ms | FPS |
|---|---|---|
| beach | 13.81 [13.78..13.83] -> **13.44 [13.41..13.48]** | 55.28 -> 55.24 (processor-bound) |
| early | 15.74 [15.73..15.76] -> **15.39 [15.37..15.44]** | 50.78 -> 50.80 (processor-bound) |
| user1022 | 21.26 [21.20..21.36] -> **20.93 [20.81..21.02]** | 38.09 [37.66..38.34] -> **38.66 [38.31..38.89]** (+1.5 %) |

What stays: most written compute bindings (the 12 MiB particle pool, the sort lists) live in the host mirror, not
in video memory: the buffer cache takes only write-tracked ranges, and the windows imported straight from the guest's
memory (no separate backing) are never armed (`GuestBuffers`, `PageTracker::NeverArm`: protecting imported memory
lost the device on NVIDIA, P4.4). The village's ~1100 small dispatches store into host memory over PCIe - the largest
GPU item left there (see "Next").

## Tools

- `p53_perf_ab.ps1` configurations: `native_early`, `early_cfirst`, `native_prof` (CPU samples of both GPU threads),
  `native_insp` / `early_insp` (Performance Inspector), `early_trace` (lifecycle trace).
- The deep stall trace's stack sampler (`SSPC_P412_STALL_TRACE=deep`, `StallTrace::SamplerRun`) crashed the process
  once (`opt2_trace`: an access violation in the system's unwinder walking a suspended thread); `sample=0` runs the
  trace without it.
