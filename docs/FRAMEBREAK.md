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
