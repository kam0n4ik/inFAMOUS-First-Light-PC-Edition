# SecondSonPC: Project FrameBreak - frame pacing, 1 % low, FPS unlock

Measured figures only. RTX 2060 6 GB, Ryzen 5 5500, 32 GB, display 1920×1080 **165 Hz**; runs 1296×759 window,
MAILBOX, no upscaler, no frame generation, `--cp-mode native`, window 62-88 s (`tools/tests/p53_perf_ab.ps1`).

## FB-1: what limited the frame rate

| Limiter | Where | Effect |
|---|---|---|
| **Emulated vblank** | `native_videoout.cpp` `PresentThread`: a flip taken only at a 59.94 Hz tick (every flip_rate + 1-th) | the cap of 60 and the 16.7 / 33.4 / 50 ms steps: a frame that misses a tick waits for the next one |
| The game's flip rate | `sceVideoOutSetFlipRate` (eboot `0x1f2eb0`, its Options - Frame Rate lock): 0 = 60 Hz, 1 = 30 Hz | honoured in every pacing |
| The game's double buffering | `sceGnmInsertWaitFlipDone` (`0x628640`): the next frame waits for the buffer shown before | at most one frame ahead of the display: no queue builds up |
| SubmitFrame's EOP wait | engine `0x1f1e30` (P5.4) | one frame in flight on the GPU |
| Present mode | the native presenter always took MAILBOX | fixed: `[Display] presentMode` honoured |
| `0x1f0be0` `usleep` loop | waits for pending flips only while re-registering the display buffers (HDR / size change) | not a limiter |
| `0x3052b0` streaming budget | spins up to 10 % (90 % while loading) of 1/30 s per frame on its queue | a time budget per frame, not a cap |

The surface offers FIFO, FIFO_RELAXED, MAILBOX, IMMEDIATE; 2..8 images.

**Why the 1 % low sat at ~29.3 FPS:** the benchmark's 1 % low is 1000 / mean of the worst 1 % *present intervals*. With
the vblank pacing a frame computed in 18 ms was shown 33.4 ms after the previous one; once more than 1 % of frames
missed 16.7 ms the 1 % low was 1000 / 34 ms whatever the frames cost. It measured the vblank, not the renderer.

## FB-2: present pacing without the vblank (`4237c99`)

`VideoOut::Pacing`: `console` (as before) or free - a flip as soon as its GPU work completes (`Sink::WaitReady` on the
timeline), with an optional cap (waitable timer + 200 µs spin) and the game's flip rate honoured; the 59.94 Hz vblank
count still advances. `[Display] frameRateLimit` = `console | 30 | 60 | 90 | 120 | 144 | 165 | 240 | unlimited`
(settings menu, live); `SSPC_FB_FRAME_LIMIT`, `SSPC_FB_SPIN_US` for test runs.

`tools/frame_stats.py` now reports three streams - `present_` (display intervals), `engine_` (the game's flip
requests), `render_` (the processor's flip markers) - each with both 1 % low definitions (`low1_fps` = worst 1 % mean,
`low1_p99_fps` = 1000 / p99), 0.1 % low, p99.9 and the shares of frames under 8.33 / 11.11 / 16.67 ms and over
25 / 33.33 / 50 ms. The older keys (`avg_fps`, `low1_fps`, `ft_p95_ms`, ...) are unchanged.

A/B `logs/p53/fb2_ab` (n = 3 per mode, interleaved; medians [min..max]):

| Scene | mode | avg FPS | 1 % low | p99 ms | CP / GPU ms |
|---|---|---|---|---|---|
| beach | console | 53.4 [50.6..54.8] | 29.4 [29.3..29.4] | 34.0 | 18.2 / 13.3 |
| beach | unlimited | 56.4 [50.5..56.6] | **48.8** [28.9..51.4] | **19.6** [19.1..33.1] | 17.7 / 13.4 |
| harbour | console | 51.4 [50.8..51.4] | 29.4 | 34.0 | 18.8 / 15.3 |
| harbour | unlimited | 51.9 [51.3..52.2] | **46.8** [45.2..47.1] | **21.1** | 18.9 / 15.3 |
| village | console | 39.0 [38.4..39.0] | 29.3 | 34.1 | 20.5 / 20.7 |
| village | unlimited | 40.0 [35.8..40.0] | **35.3** [28.9..35.4] | **27.4** | 20.3 / 20.7 |

The frame's own cost is unchanged (CP and GPU per frame the same): the gain is the vblank wait removed, not a faster
renderer. 18 runs: `[Error]` 0, device lost 0. The low ends of the ranges are runs whose whole window ran 1.8-2.2 ms
slower on the CP (village unlimited r3: on the GPU too) - the machine's known slow mode (P4.11/P4.12), seen in both
modes (beach console 50.6 FPS), not the pacing.

## FB-3: the game's time at different frame rates

**Code:** the game's clock `0x12a160` (main loop `0x29c8d0`, object `eboot+0xe16290`): `dt = (rdtsc - last) /
sceKernelGetTscFrequency`, clamped to 1/30 s (1/15 with a flag), negative -> 0; four clocks accumulate `dt * scale` in
ticks of 1/1440 s. The world's time is real time, not a vblank or frame count.

**Measured** (`SSPC_FB_CLOCK_PROBE=1`: `src/engine/game_clock_probe.*` reads the clock every 100 ms, read-only;
`tools/framebreak/clock_rate.py`; `logs/p53/fb3_clock2`, beach, game seconds per wall second, window 62-88 s):

| Pacing | mean frame (dt) | game s / wall s (all four clocks) |
|---|---|---|
| cap 30 | 33.7 ms (65 % of frames at the 1/30 clamp) | 0.99994 |
| console | 17.9 ms | 0.99998 |
| unlimited | 19.8 ms | 0.99999 |

The game's time does not depend on the pacing. Above 60 FPS it is not measured: no gameplay scene here reaches it
(the CP takes ~18 ms a frame). Not checked either: per-system timings (physics substeps, animation, particles, scripted
events, cutscenes, mouse sensitivity) beyond the global clock.

The screenshot comparison (camera after a scripted stick turn, `logs/p53/fb3_time2`, `fb3_turn`) was inconclusive: by
the 90 s capture the game's own camera recentring decides the view (a 2 s turn ends where three 0.25 s turns do); at
30 FPS the view stays ~40-75 px apart from ~55 FPS in both series - cause not found.

**Default:** `frameRateLimit = "60"` - the free pacing capped at 60: no vblank wait, the measured rates, no frame rate
above the one the game was made for (menus and loading screens ran at ~200 FPS unlimited: ~18 000 flips a run against
~5 000). `console` and `unlimited` stay in the menu.

## Tools

- `p53_perf_ab.ps1` configs `fb_console`, `fb_unl`, `fb_30` ... `fb_144`, `fbc_*` (with the clock probe); `-Press`
  for a scripted route of one's own (p46_runs.ps1 `-Press`).
- `SSPC_SELFTEST_ONLY=settings`: the PC settings' self-test groups alone (the full self-test needs more commit than
  this PC has without a page file - it crashed the same way on the pre-FrameBreak exe).

## FB-6: the command processor (in progress)

Profile without the vblank (`logs/p53/fb6_prof`, beach, `native_prof`): thread 1 (CP + shader frontend) 18.2 ms a
frame, of it 3.3 ms waiting for thread 2 (drains at round ends ~1.6 ms, met memory waits ~0.8 ms); thread 2
(recording) busy 15.8, idle 2.4. Thread 1: PrepareDraw 42 % incl. (PrepareStage 29 %), CopyStage 11 %, SetRegs /
RegisterFile::Write ~7 %, memmove 8.6 % excl.; thread 2: the driver's vkCmd* ~18 % (shown as vkGetInstanceProcAddr),
RecordDraw 13 %, BindStage 7 %, GuestRangeImpl 9 %.

**CP-5 "both" again without the vblank (rejected again).** A round of 65536 packets + met memory waits without a drain
(`SSPC_CP_ROUND_PACKETS=65536;SSPC_P411_ASYNC_ACQUIRE=1`; `logs/p53/fb6_both`, n = 3, unlimited pacing): thread 1
-0.7..-0.9 ms, but the frames come later to the GPU - beach 52.8 [49.5..55.4] -> 48.2 [46.7..49.4] FPS, harbour 51.4
[47.9..51.8] -> 44.3 [43.7..44.8], village 36.9 -> 36.5 / r3 35.5 -> 32.4 (1 % low 26.3 -> 19.7). Several base runs
fell into the machine's slow mode (49.5 / 47.9 / 34.3).

**Stage reuse (rejected, off).** `SSPC_FB_STAGE_REUSE=1` (`verify` compares every reused stage byte for byte): a draw
stage the frontend did not prepare again (a memo hit) is not copied into the record again - thread 2 takes the last
copy (`parallel_cp.cpp` ReuseView). Correct: 9.2 M (beach) and 8.2 M (village) reused stages verified, 0 mismatches.
A/B `logs/p53/fb6_reuse_ab2` (n = 3, interleaved, unlimited pacing; avg FPS per run):

| Scene | base | reuse |
|---|---|---|
| beach | 48.1 / 53.2 / 53.8 | 49.5 / 49.1 / 54.3 |
| harbour | 48.7 / 49.3 / 50.7 | 49.2 / 45.8 / 47.7 |
| village | 36.5 / 34.4 / 39.6 | 39.4 / 39.2 / 39.3 |

No gain beyond the run-to-run spread (the machine's slow mode again in several runs: CP 20-24 ms against ~18.5);
harbour lower in two of three. The copy it saves (CopyStage, ~11 % of thread 1) is not what limits the frame. Kept
off.

## FB-7: frame generation paced by its own presents (taken)

`cfc43d5` kept flips at the emulated 59.94 Hz vblank while FSR frame generation is on (its present blocked inside the
queue's lock - a deadlock). Measured: the game's frames then stop at 59.9/s and every frame that misses a vblank
waits for the next - the 1 % low back at ~29 FPS, p99 ~34 ms, as before FB-2. Now the presenter waits, before the
present and outside the queue's lock, until at most one game frame's presents are still pending in FFX (its present
count, `FsrFrameGen::WaitForPresents`; 100 ms at most, then logged); the free pacing and the frame rate limit apply
with frame generation too. FFX's own `WAIT_FOR_PRESENTS` dispatch lost the device here (it waits on the game's queue,
which other threads submit to); waiting until all presents were shown left a gap (beach 53 game frames/s).
`SSPC_FB_FG_VBLANK=1` brings the vblank back.

A/B (`logs/fb7/fb7_fgc_*`, FSR 3.1 Quality + FSR frame generation, unlimited, 2 rounds, old = the exe of `3a6727c`):

| Scene | game frames/s old -> new | 1 % low old -> new | p99 ms old -> new |
|---|---|---|---|
| beach | 52.4 / 52.0 -> 52.9 / 53.6 | 29.5 / 29.4 -> **45.3 / 47.9** | 33.8 -> 20.3-20.8 |
| harbour | 48.9 / 49.1 -> 49.4 / 48.2 | 29.4 / 29.4 -> **44.7 / 43.5** | 33.9 -> 21.9-22.6 |
| village | 40.2 / 40.3 -> 41.7 / 41.3 | 29.2 / 29.2 -> **35.6 / 35.7** | 34.2 -> 27.5-27.6 |

Displayed = 2x the game's frames. `[Error]` 0, no wait timed out. The mission a10 save (the earlier deadlock) 150 s:
no hang, game frames up to 104/s (displayed 209/s), avg 53 FPS.

## FB-7: the machine's "slow mode" (not reproduced)

FB-6's series had runs with the processor 2-4 ms slower (CP 20-24 ms against ~18.5). Each A/B run now also samples, once
a second, the CPU's performance (% Processor Performance, the clock as % of base), system context switches and each
core's load (`perf.csv`, WMI; `tools/framebreak/slow_mode.py` lines them up with FPS and CP time). Two series of the
current build, 21 runs in a row (`logs/p53/fb7_slowmode`, `fb7_slowmode2`, unlimited pacing): **none slow** - beach
54.2-55.9 FPS (CP 17.85-18.24 ms), harbour 49.0-51.8 (19.06-20.34), village 39.6-40.3 (20.27-20.98); the CPU at
114-115 % of its base clock in every run (~4.1 GHz boost, no drop), 22-26 thousand context switches a second, the game
at 2.9-3.7 cores, no core saturated. The slow runs of FB-6 (night of 2026-10-17, 00:20-00:58) fell into a time when
other work ran on this machine; with nothing else running the effect did not appear. They stay in FB-6's statistics
as measured; the sampler now records the clock and load of any future slow run.

## FB-7: the village's GPU - the write-backs (measured)

GPU time per command (`SSPC_GPU_PROFILE=1`, `logs/fb7/fb7_prof*`, village, unlimited pacing; timestamps after every
command serialize the GPU: 35 ms a frame instead of ~20, but intervals between batch-end marks are not inflated):

| | ms / frame |
|---|---|
| batch end (16 a frame): barrier + write-back copies (key 6, new mark) | 0.31 |
| batch end: the changed-words shader `wb_diff.comp` (key 5) | **2.70** |
| the same shader, its host stores sent to the shadow instead (`SSPC_DIAG_MEASURE=wb_nohost`, measurement only) | 0.55 |
| the same shader writing whole 32-word subgroup lines where any word changed (measurement build, removed) | 3.02 |
| smoke draw (pipeline 544, 1920x1080) | 1.85 |
| tiled lighting dispatch (120x68 groups) | 1.32 |

The written ranges are the particle pools (2 x 12 MiB at 0x26d558000 / 0x26f358000, written by most particle
dispatches), their 1 MiB lists, and ~1.1-1.6 MiB ranges at 0x2712xxxxx - compared in full at each batch end
(~51 MiB a frame). **~2.2-2.5 ms a frame are the shader's stores into host memory over PCIe**; writing whole lines
costs the same as single words: the GPU changes most of the lines it touches - the volume, not the store size.
Upper bound without them (`logs/p53/fb7_nohost_ab`, n = 3, interleaved, measurement only - the host's copy is wrong):
village **38.6 [35.3..40.6] -> 42.8 [42.7..43.0] FPS**, GPU busy 20.3 -> 18.4 ms.

The guest maps all of it CPU read/write + GPU read/write (`0x33`, one area 0x268000000..0x31fff0000): nothing says the
CPU never reads it. Writing whole lines where the host is known to equal the shadow would have been safe (write-
protected pages, a CPU write waiting for the write-back), but saves nothing (above). What would save the 2.2-2.5 ms is
not writing back what no one reads: GPU-written ranges kept in video memory until a reader needs them - the
executor's own readers (host-import bindings, timeline copies, frontend reads) can be ordered, but a guest CPU read
can only be seen through read protection of the guest's view (not the import) with a wait for the write-back in the
fault handler. Not done: a change of the memory model that needs its own stage and decision.

**Who reads the pool (`SSPC_DIAG_READ_WATCH=<hex address>:<hex size>`, diagnostic).** Armed (write-protected) pages
in the window get no access on the guest's view (never the Vulkan import); a CPU read faults, is counted and gets the
page back read-only. Village, 90 s (`logs/fb7/fb7_watch*`): the pool 0x26d558000 + its list (to 0x26e258000) - **1385
reads, all in 0x26d76da0c..0x26d776e8c** (~36 KiB of 12 MiB); a window up to 0x270158000 also took the small
per-emitter state buffers after the lists (0x270059f04..0x270157104): 757 557 reads. So the CPU reads a small part of a
pool (some emitter's particles) and the per-emitter states constantly; a write-back kept back until a reader needs it
would have to turn a block back to the eager write-back at its first read.

## FB-7: lazy write-back (`SSPC_FB_LAZY_WB=1`, off by default)

At a batch end the 64 KiB blocks wholly inside a large GPU-written range (all pages valid, every page armed) are not
written back: the block is **hidden** (`PageTracker::Hide`) - its armed pages get no access on the guest's view (never
the Vulkan import, so not the protection that lost the device before) and the buffer cache keeps it in a set of
kept-back blocks. Every reader writes them back first (`BufferCache::FlushDeferred`, the changed-words shader against
the block's shadow, adjacent blocks one dispatch; without a valid shadow a plain copy into the import and the shadows):

- a GPU reader of the host import (`FlushWritten`) - the block stays hidden and is kept back again at the next batch
  end (no protection change);
- the executor's CPU-side reads (`TimelineReader::Read`, `SyncForFrontendRead`, `Synchronize`) and the processor's
  (`Memory::Read` / `WriteGuest`, after a drain);
- a guest CPU access: the fault (`PageTracker::OnFault`) posts a request (`Executor::RequestKeptBack`), served by
  the recording thread after its record or by the processor's thread once the queue is drained (it is woken), waits
  for the batch, then shows the block (armed pages read-only again); such a block is never kept back again (`eager`);
- the game's own writes reported by the frontend (`OnGuestWrite`): written back before the invalidation.

The parallel processor is required (without it the mode stays off). Checks: `verify` counts what would be kept back
without doing it; the report counts CPU faults on hidden blocks, invalidations of a still hidden block (an unsynced
writer: 0 in every run) and the readers by kind. Vulkan validation with synchronization validation, village and beach
(`logs/fb7/fb7_lazy_val`): 0 messages. The picture matches the eager write-back (screenshots, village).

Village, 90 s: ~100 blocks a frame kept back, written back by a GPU reader of the host import about once a frame
(~3400 rounds of 90 s; 6-7 thousand runs of adjacent blocks), 6-19 CPU faults a run. Interleaved A/B, same exe
(`logs/p53/fb7_lazy_ab3`, n = 4; beach from `fb7_lazy_ab`, n = 3, before the runs were merged):

| | eager (`fb_unl`) | lazy (`fb7_lazy`) |
|---|---|---|
| village avg FPS | 39.51 [38.99..40.04] | **41.73 [41.51..42.09]** |
| village 1% low | 33.35 [27.75..35.45] | 33.76 [33.37..34.23] |
| village frame p95 ms | 27.66 [26.94..29.39] | 28.41 [28.19..28.75] |
| village GPU busy ms | 20.83 | **18.33** |
| harbour (`early`) avg FPS | 50.76 [49.76..51.38] | 50.68 [50.40..50.90] |
| harbour GPU busy ms | 15.28 | 14.35 |
| beach avg FPS / 1% low | 55.20 / 48.63 | 55.22 / 49.36 |

Three village series agree: average FPS +5 % (41.5-41.7 against 39.3-39.7) and GPU busy -2.4 ms, but **1% low does
not move and the frame p95 is ~0.8 ms higher** - more frames reach the next vblank interval, the slow frames (the
command processor's, ~23 ms of CP) stay as slow. The first version cost the harbour 0.5 ms of CP (a dispatch per
kept-back block); merging adjacent blocks removed it. Left off by default: the goal is the 1% low, which the GPU no
longer limits in the village - the CP does.

## CP-8: the village's critical path and the processor's waits on it

**What holds the village's frame** (lifecycle trace `logs/cp8/cp8_trace2`, Native + early, unlimited pacing, 470
frames; `tools/p54/p54_lifecycle.py`): in 98.5 % of frames the game itself is late with the next frame - it waits
twice a frame for its async compute label (0x1695e0), 19.0 ms of a 25.4 ms period; its own CPU work is ~4 ms. One
wait (9.5 ms on average): the processor reaching the compute queue's RELEASE_MEM 6.3 ms, the batch submitted 0.25,
waiting on the GPU behind graphics batches 1.5, executing 1.4, the label 0.2. Frames with the compute waits out of
the way would be limited by the GPU (20.9 ms busy) and the recording thread (18.8 ms).

Thread 1 per frame (the trace's queue slices): graphics 12.6 ms (~71 500 packets, 177 ns each), compute 5.0 ms (~20 600
packets, 244 ns each), waiting for the recording thread 4.0 ms (46 drains), idle while the GPU works 4.2 ms. Worst 1 %
of frames vs the median (`gpu_frames.csv`, 3 base runs): the same structure - the frame is the processor's span plus
the GPU's tail after the flip (~6 ms), the processor 27 vs 20.6 ms (recording 20.9 vs 16.4), idle waiting for the game
~5.3 ms in both.

The processor's time by opcode (`SSPC_DIAG_PACKET_TIME=1`, new, diagnostic; whole 90-s run, ms): draws (DRAW_INDEX_2)
1.8 us each, dispatches 1.7 us each (the frontend's preparation), SET_SH_REG ~35 ns, WAIT_REG_MEM 31-37 us (drains);
**the compute ring's read pointer, written to guest memory after every packet, 5 352 ms - a third of the compute
queue's 10 807 ms**: 58 million writes of the same value (the ring's position does not move while the queue executes
an indirect buffer; the ring is 8 192 dwords, nearly all compute packets are in indirect buffers).

From a game submission to its slice on thread 1 (`logs/cp8/cp8_trace2`): a compute doorbell (53 a frame) waits
1.26 ms on average - mostly thread 1 in a graphics slice (1.32 ms) or a drain; the early frame handoff 0.51 ms, 0.69
ms when it fell into thread 1's idle wait for the GPU: that wait (`vkWaitSemaphores` on the oldest batch, up to 1 ms)
did not see the game's submissions.

**Taken (default; old path by a variable):**
- the read pointer is written only when its value changes (`CommandProcessor::UpdateReadPointer`; the memory holds the
  same value as before at every point; `SSPC_CP_RPTR_EVERY_PACKET=1`: after every packet);
- the idle wait for the GPU ends at the oldest batch's completion **or** a game submission: a second timeline
  semaphore, both waited on with `VK_SEMAPHORE_WAIT_ANY_BIT`, signalled from `Push` while thread 1 waits
  (`Timeline::WaitOrWake` / `Wake`; `SSPC_CP_GPU_WAIT_WAKE=0`: the old wait).

A/B (`logs/p53/cp8_screen2` village, `logs/p53/cp8_ab_be` beach and harbour; n = 3, interleaved, one exe):

| Scene | Metric | old | new |
|---|---|---|---|
| village | avg FPS | 39.80 [39.60..40.01] | **40.46 [40.32..40.69]** |
| | 1 % low | 35.06 | 35.82 |
| | p95 / p99 ms | 26.98 / 27.94 | 26.43 / 27.39 |
| beach | avg FPS | 55.66 | 55.75 |
| | 1 % low | 46.54 | 49.50 |
| | p95 / p99 ms | 18.80 / 19.88 | 18.74 / 19.62 |
| harbour | avg FPS | 51.38 [50.85..51.90] | 51.02 [50.85..51.28] |
| | 1 % low | 44.73 | 44.84 |
| | p95 / p99 ms | 20.63 / 21.32 | 20.82 / 21.58 (within the spread) |

Vulkan Validation + Sync, village and beach (`logs/cp8/cp8_val`): 0 messages, device lost 0, `[Error]` 0.

**Measured, not taken (options, off):**

| Village, n = 2-3 | avg FPS | 1 % low | p95 ms | GPU tail ms |
|---|---|---|---|---|
| compute slices of 16 384 packets (`SSPC_CP_COMPUTE_ROUND_PACKETS`) | 36.87 (base 39.28) | 33.74 | 29.0 | 8.05 |
| compute slices unlimited | 35.21 | 31.57 | 30.5 | 8.87 |
| compute first in a round (`SSPC_DIAG_QUEUE_ORDER=compute-first`) | 41.14 (new 40.46) | 33.55 | 28.08 | 6.17 |
| + a compute doorbell ends the graphics slice (`SSPC_CP_COMPUTE_PREEMPT=1`) | 41.41 | 35.17 | 27.11 | 6.32 |

Serving the game's compute earlier raises the average but delays the graphics frame on the processor and on the GPU
(one Vulkan queue): the tails do not improve. Not taken - the goal is the 1 % low.

**After CP-8 (`logs/p53/cp8_screen3`, village, n = 3):** the frontend on its own thread (`SSPC_P53_FRONTEND_THREAD=1`)
40.54 FPS / 1 % low 36.28 / p95 26.11 against 40.46 / 36.35 / 26.10 - no change (thread 1 -0.75 ms, the recording thread
and the GPU limit). Lazy write-back again (`SSPC_FB_LAZY_WB=1`): **42.83 FPS (+5.9 %), GPU 18.40 ms (-2.4), but 1 %
low 34.35 and p95/p99 27.84 / 28.61 ms (+1.7 / +1.6)** - as before CP-8. Its worst 1 % of frames: the game hands the
frame on 2.2 ms later, the processor 23.7 ms (base 22.0), the recording thread +1.1 ms - the readers' write-backs land
in the heaviest frames and on the game's compute waits. Stays off.

## P5.x Native Rebirth: what it could remove from the village's frame

Measured split of the village's frame (CP-8) and who owns each part:

| Part (ms per frame) | Size | Already own code | What a native path could remove |
|---|---|---|---|
| PM4 decode, registers, journal, NOPs (thread 1) | ~4 | yes (Native CP) | only by not building PM4 at all - the game's renderer would have to be replaced |
| per-draw / per-dispatch preparation: program lookup, SRT walk, descriptors, key, CopyStage (thread 1) | ~10 (1.7-1.8 us each, ~4 400 draws + ~1 500 dispatches) | yes (SecondSonPC frontend; P5.3 native cache for 2 passes) | the most: a renderer that knows Second Son's resource tables binds without interpreting them; partly reachable now by the engine-table cache (P5.3) for more passes and for dispatches |
| Vulkan recording (thread 2) | ~18 | yes | little: ~5 900 draws/dispatches must be recorded by any renderer; fewer only by merging draws (engine knowledge) |
| GPU work | 18-21 | translated game shaders | write-backs to host memory 2.2-2.5 (the guest's CPU-visible memory model); smoke 2 ms and the rest is the game's own work |
| the game's async compute waits | 19 of 25.4 (latency) | the game's design | a native port could restructure; inside the emulated frame only by serving the compute queue faster (CP-8) |

Shader translation (shadPS4's recompiler) is not on the per-frame path (disk cache). The parts worth a native
rewrite are the frontend's preparation (~10 ms of thread 1, of it ~2.5 ms the compute dispatches on the game's wait)
and the write-back model; the recording thread and the GPU stay whatever the front end. A gain of 10 % or more is not
guaranteed by any single part: thread 1, the recording thread and the GPU are each 70-85 % busy in a 25 ms frame.

## CC: the compute critical path (village)

**The game's blocked compute request, link by link** (lifecycle trace `logs/cc/cc_trace_base`, 470 frames, 944 waits;
`tools/p54/cc_chain.py`, ms): doorbell -> the game starts waiting 0.23; **the processor reaches the label's RELEASE_MEM
6.00 (p90 11.6)**; batch submitted 0.11; waiting on the GPU behind graphics 1.59 (p90 4.3); GPU execution of that batch
1.68; label written 0.18; game wakes 0.03; whole wait 9.59 (p90 15.3). The game rings the doorbell and blocks 0.2 ms
later - the request is on the critical path from its first packet.

What thread 1 does in those 6.0 ms (`tools/p54/cc_window.py`): graphics slices 2.34 (11 400 packets), compute slices
2.49 (9 000 packets), round-end drains 0.89 (2.8), drains at met WAIT_REG_MEM 0.38 (9.4), idle ~0. The slowest 10 % of
waits (12.2 ms) are the same mix 2.3x larger - more compute work in that request (5.4 ms), not another cause.

**One dispatch's preparation** (`SSPC_GPU_TIMINGS=sample:8`, new per-stage split for compute, ns): program lookup 296
(p95 1 920: the per-frame validation of a program), SRT walk + descriptors 793, specialization key 153, the rest of the
stage (memo, module search) ~310, runtime/pipeline ~200, the record's copy of the stage view 269 - ~2.0 us, x ~1 500 a
frame. Repeated work: every dispatch rebuilt its runtime parameters with a new version, so the stage memo never held a
dispatch; now the version moves only when they change (`SSPC_CC_CS_RUNTIME=0`: the old way). It still does not hit:
consecutive dispatches run different programs (memo misses on the program 3.24 M, on user data 0.43 M). A cache of
preparations by inputs would not either (`SSPC_DIAG_CS_REPEAT=1`): 91 % of dispatches come with inputs not seen in the
last 30 frames (their user data point into ring allocations), 3.8 % repeat with the same bytes read (21.5 reads, 86
bytes, 1.6 lines a dispatch). Not built.

**Dependency-aware priority (CC-2, `SSPC_CC_CRITICAL=1`, off).** The engine's wait 0x1695e0 (the lifecycle trace's
replacement loop, installed by this switch alone too) counts the game threads blocked in it
(`runtime/gpu/critical_compute.h`); while one is, the processor steps the compute queues first, the graphics slice ends
early when compute work is ready (every 64 packets), and is cut to `SSPC_CC_CRITICAL_GFX` (512) packets only while a
compute queue stopped at its limit with work left - a compute queue empty or blocked (on graphics, possibly) gives
graphics its whole slice. Without a waiting game the round is unchanged. Vulkan Validation + Sync with it and met
waits without a drain, village and beach (`logs/cc/cc_val`): 0 messages, device lost 0.

It does shorten the request: the processor reaches the RELEASE_MEM in 3.65 ms (6.00), the whole wait 7.38 (9.59), p90
11.0 (15.3). But then 43 % of frames are held by the processor still on the previous frame (step 5 of SubmitFrame, 5.1
ms) - the game alternates between the two limits and its own frames become 17 / 31 ms:

| Village, n = 3 (`logs/p53/cc_v4`, `cc_soft`) | avg FPS | 1 % low | p95 / p99 ms | engine p95 ms | submit -> present ms | step mismatch ms |
|---|---|---|---|---|---|---|
| base (two undisturbed runs; cc_soft n = 3) | 40.07 / 40.27 | 35.68 / 34.79 | 26.9 / 27.6 | 28.1 | 27.2 / 26.9 | 1.1-1.2 |
| met waits without a drain (`SSPC_P411_ASYNC_ACQUIRE=1`) | 40.19 | 35.02 | 26.76 / 27.79 | 28.1 | 26.8 | 1.15 |
| CC-2 | 41.70 | 35.00 | 27.30 / 28.07 | 33.4 | 29.1 | 4.8 |
| CC-2 + met waits without a drain | 41.73 | 35.60 | 27.00 / 27.71 | 33.0 | 28.9 | 5.1 |
| CC-2, graphics share 2048 | 41.63 | 35.06 | 27.14 / 27.93 | 33.3 | 29.1 | 5.1 |

(step mismatch: |the game's step between its flips - the display's step between presents| per shown frame, mean;
`tools/framebreak/cc_pacing.py`.) **+3.5-4 % average FPS, the 1 % low and p95/p99 unchanged, but 2 ms more latency
from the game's submission to the present and a 4x larger mismatch between the game's time steps and the display's**
- not taken: the task allows no worse input latency or pacing. What would keep the gain: pacing the game's own loop
(its submissions) at the rate the processor and the GPU sustain, so the time the compute wait no longer takes does not
turn into alternating frames - a next step, not part of the processor.

Met waits without a drain (P4.11) remain neutral in the village (+0.3 %): thread 1's drains are not what limits it -
the recording thread (17.6 ms) and the GPU (20.8 ms busy, ~4 ms idle a frame waiting for the game's chain) are.

Beach and harbour (`logs/p53/cc_be`, n = 2): CC-2 beach 55.43 -> 56.16 FPS, 1 % low 49.84 -> 50.23, submit -> present
26.2 -> 26.6 ms; harbour 51.49 -> 51.63, 44.00 -> 44.68, **27.5 -> 29.5 ms**. Met waits without a drain: harbour 51.78 /
1 % low 46.53, beach one of two runs in the machine's slow mode (48.8). Both stay off.
