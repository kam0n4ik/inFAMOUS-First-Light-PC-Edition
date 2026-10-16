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

## Tools

- `p53_perf_ab.ps1` configurations: `native_early`, `early_cfirst`, `native_prof` (CPU samples of both GPU threads),
  `native_insp` / `early_insp` (Performance Inspector), `early_trace` (lifecycle trace).
- The deep stall trace's stack sampler (`SSPC_P412_STALL_TRACE=deep`, `StallTrace::SamplerRun`) crashed the process
  once (`opt2_trace`: an access violation in the system's unwinder walking a suspended thread); `sample=0` runs the
  trace without it.
