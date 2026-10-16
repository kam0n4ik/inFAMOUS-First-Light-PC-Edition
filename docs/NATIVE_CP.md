# SecondSon Native CP

Stage 3 of the PC Edition (2026-10-16). The command processor of the native backend (`--gpu-backend sspc`) is
developed further, not written again: the existing Parallel CP (P4.10-P4.12) is already SecondSonPC's own code from
PM4 to `vkQueueSubmit2`, and it is the base. Only measured figures are written here.

## CP-0: what exists

Path: game threads (`sceGnmSubmit*`, `gnm_driver.cpp`) -> the backend's submission queue -> thread
`SecondSonPC:GpuNative` (`Backend::Loop`: rounds of at most 4096 packets per queue, `CommandProcessor::Step` ->
`Pm4::Decode` -> handlers; the shader frontend prepares each draw/dispatch: program lookup, SRT walk, descriptors, key,
pipeline id) -> an SPSC ring of 1024 records + the register journal -> thread `SecondSonPC:GpuRecord` (`Vk::Executor`:
buffers, images, descriptors, barriers, `vkCmd*`, `vkQueueSubmit2` every 500 draws or at a fence / flip / sync).

| Part | Own code | Notes |
|---|---|---|
| PM4 decoding, packets, registers | yes (`pm4.h`, `command_processor.*`, `RegisterFile`) | one decoder |
| Draw / dispatch, pipeline state | yes (`command_processor.cpp`, `PipelineStateCache`) | |
| Resource binding, SRT, PrepareDraw / PrepareStage | yes (`shader_frontend.*`, SRT interpreter) | stage memo (P4.7), lookup memo (P4.9) |
| CopyStage (frontend result into the record) | yes (`parallel_cp.cpp`) | |
| GPU synchronization, labels, fences, WAIT_REG_MEM | yes | held fences, timeline semaphore |
| Vulkan recording, barriers, submission, readback | yes (`vk_executor.*`, caches, `vk_presenter.cpp`) | |
| Shader translation (GCN -> SPIR-V) | **no**: shadPS4's recompiler behind `ShaderTranslator` | not on the per-frame path (disk cache, precompiler) |
| Detiling kernels | shadPS4's SPIR-V (`host_shaders`) | |

Modes that really exist: **Legacy** = one thread (`SSPC_P410_PARALLEL_CP=0`), **Parallel** = two threads (the
default since P5.3D). Duplicated mechanisms found: three register file copies kept by one journal (processor, recording
thread, optional frontend thread), three memo layers, three guest-read run caches, two frame-row logs named
`gpu_frames.csv`.

## CP-0: baseline (2026-10-16, HEAD 50a242e)

Conditions: the existing route (`tools/tests/p46_runs.ps1`: Continue, three camera turns at 56-60 s), window
62-88 s, 1296×759 window, mailbox present, **no upscaler, no frame generation** (scripted runs use SecondSonPC.toml
alone), the same exe copy for the series, configurations interleaved. Scenes: `beach` (beach, heavy foliage after the
turn = the forest/foliage case), `early` (harbour), `user1022` (forest village with fires and smoke = the village and
the particles case).

Legacy vs Parallel (`logs/p53/cp0_base`, n = 2 each; [min..max]):

| Scene | Mode | Avg FPS | 1% low | P50 ms | P95 ms | P99 ms | CP thread ms | GPU busy ms | process CPU, % of a core |
|---|---|---|---|---|---|---|---|---|---|
| beach | Legacy | 34.3 [34.2..34.5] | 25.1 [22.7..27.6] | 33.0 | 34.0 | 34.2-34.3 | 27.0 | 13.7 | 240 |
| beach | Parallel | 44.6 [43.5..45.8] | 28.7 [28.0..29.4] | 17.0-17.1 | 33.9 | 34.0-34.1 | 19.8 | 13.8 | 331 |
| early | Legacy | 31.0 [30.7..31.3] | 29.2 | 33.1 | 34.0 | 34.2 | 29.6 | 15.6 | 235 |
| early | Parallel | 39.9 [39.9..40.0] | 29.3 | 17.6-32.5 | 34.0 | 34.1 | 21.3 | 15.7 | 324 |
| user1022 | Legacy | 29.1 [28.2..29.9] | 19.8 | 33.2-33.4 | 34.2-50.1 | 50.1-50.5 | 29.7 | 20.7 | 217 |
| user1022 | Parallel | 35.6 [34.6..36.6] | 26.5 [23.7..29.2] | 33.0 | 34.0 | 34.1-34.3 | 22.5 | 21.5 | 289 |

Frame times are vblank-quantized (16.7 / 33.4 ms): the average FPS follows the share of one-vblank frames. Device
lost 0, `[Error]` 0 in all 12 runs. The machine drifts between series by several percent (the same Parallel
configuration gave 44.6 and 48.4 FPS on the beach in two series an hour apart): only interleaved runs of one series
are compared.

### Where the processor's thread goes (Parallel, sampled every 1 ms with stacks, `logs/cp/cp0_prof`)

| ms per frame | beach | early | user1022 |
|---|---|---|---|
| thread 1 (GpuNative) busy | 19.7 | 21.9 | 22.6 |
| shader frontend (PrepareDraw / PrepareStage: program lookup, SRT walk, descriptor decode, key, pipeline id) | 6.8 (30 %) | 8.6 (34 %) | 6.7 (24 %) |
| PM4 read / decode, registers, journal | 5.7 (25 %) | 7.1 (28 %) | 7.0 (25 %) |
| waiting for the recording thread (drains, WaitHead) | 2.6 (12 %) | 2.2 (9 %) | 3.2 (12 %) |
| CopyStage | 2.1 (9 %) | 2.2 (9 %) | 2.2 (8 %) |
| thread 2 (GpuRecord) idle, waiting for records | 6.5 | 8.8 | 9.7 |
| GPU busy | 13.8 | 15.7 | 21.3 |

The historical figures hold on the current code: PrepareDraw/PrepareStage 24-34 % (was 34-39 %), WaitHead 9-12 %
(was ~12 %), CopyStage 8-9 % (was 9.4 %). The beach and the harbour are limited by thread 1; the village is mixed
(thread 1 also idles ~7 ms per frame waiting for the GPU and for labels: `SSPC_P412_STALL_TRACE=light`).

Thread 1's waits by reason (new counters, `logs/cp/cp0_drains`, per frame): **round end 1.26 ms (beach) / 2.05 ms
(village)**, met memory waits 0.69 / 0.97, timeline writes and GDS < 0.1. Batch submissions by cause (beach / village,
whole run): full 18731 / 14224, held fence 22707 / 24427, processor idle 391 / 381, flip 4506 / 3302.

### Existing options measured again (`logs/p53/cp0_opts`, n = 2, interleaved)

| Option | beach FPS | user1022 FPS | Decision |
|---|---|---|---|
| Parallel (default) | 48.4 | 37.3 | base |
| frontend on its own thread (`SSPC_P53_FRONTEND_THREAD=1`) | 45.6 | 36.7 | stays off |
| met memory waits without a drain (`SSPC_P411_ASYNC_ACQUIRE=1`) | 46.1 | 37.6 | stays off (no gain) |

## CP-0: conclusions, plan

1. The critical path is the processor's thread; the recording thread idles 6.5-9.7 ms per frame. Work removed from
   thread 1 turns into frames directly; work moved to thread 2 is free up to that idle time.
2. The largest wait is the drain at the end of each round (CP-5 first).
3. Then the per-draw preparation (CP-2: the stage memo is taken for only ~25 % of stages, ~88 000 SRT reads per
   frame) and CopyStage.
4. PM4 decoding itself (`Pm4::Decode` 2 %) is not worth a rewrite; register writes + journal (~6 %) may be.

## CP-5: the drains (measured, not taken)

The round limit and the met-memory-wait drain, alone and together (`logs/p53/cp5_round`, `logs/p53/cp5_combo`, n = 2
each, interleaved; thread 1's drain wait in seconds per 90-s run):

| Configuration | beach FPS | user1022 FPS | thread 1 ms | drain wait beach / village, s | where the wait went |
|---|---|---|---|---|---|
| Parallel (4096 packets per round) | 44.1 [42.6..45.7] / 43.3 [41.8..44.7] | 36.7 / 36.8 | 20.1-20.7 / 21.8-21.9 | 9.1-9.9 / 10.4-10.6 | round end 5.7-6.3, memory wait 3.1-3.3 |
| 16384 per round | 46.0 | **32.7** | 19.2 / 21.4 | 7.9 / 8.9 | memory wait 4.9 |
| 65536 per round | 45.8 | 38.2 (other draw count: 4010 vs 4296 per frame - not counted) | 18.9 / 20.0 | 7.2 / 9.0 | memory wait 6.6-8.3 |
| met memory waits without a drain | 46.1 | 37.1 | 19.1 / 21.6 | 8.3 / 10.6 | round end, GDS |
| both | 45.9 | 37.0 | 18.8 / 20.9 | 6.5 / 6.1 | timeline write + GDS 3.2-4.9 |

Removing one kind of drain moves the same wait to the next point where thread 1 needs the executor: the recording
thread's lag behind the processor is the cost, not a particular drain. Thread 1 gains at most ~1.9 ms, the FPS stays
within the base's spread, the village does not improve. **Not taken**: the limit stays 4096, async acquire stays off;
`SSPC_CP_ROUND_PACKETS` remains for experiments. Device lost 0, `[Error]` 0 in all 24 runs. What would remove the
waits for real is less work per record on either thread.

## CP-2: shader code trusted by write tracking, read again once in 8 frames (taken into Native)

The frontend read and compared every program's code once per frame (~1 000 reads, ~1.6 MB per frame on the beach).
P5.2's code trust (`SSPC_P52_CODE_TRUST=1`: trusted while its write-tracked pages stay unwritten) measured CP -1.4 ms /
+4-5 % FPS but stayed off: a shader's store of unknown extent (BDA, storage images) into shader code would not be seen.
New mode **`sampled`** (`shader_cache.h`, `CodeTrust::Sampled`): trusted as before, yet every program and fetch shader
is still read and compared once in 8 frames (spread by its address): such a write is found within 8 frames instead of
the next one, at 1/8 of the reads. Self-test: `Shader(validate)` checks it (an unseen rewrite found within 8 frames,
no read before its frame).

A/B (`logs/p53/cp2_trust`, n = 2, interleaved; FG off):

| Scene | Parallel FPS | + trust sampled FPS | CP thread ms | 1% low | missed writes |
|---|---|---|---|---|---|
| beach | 45.8 [45.5..46.1] | **47.1 [47.0..47.2]** (+2.8 %) | 19.3 -> 18.6 | 29.3 / 29.3 | 0 |
| early | 40.6 [40.6..40.6] | **41.7 [41.6..41.7]** (+2.6 %) | 20.9 -> 20.0 | 29.3 / 29.3 | 0 |
| user1022 | 36.6 [36.5..36.8] | 36.5 [36.5..36.5] (0) | 21.9 -> 21.9 | 29.3 / 29.2 | 0 |

The village is limited by the GPU as much as by the processor: no change there. Device lost 0, `[Error]` 0.

## CP-2: register writes that change nothing are not journaled (taken into Native)

The recording thread keeps a replica of the register file from a journal of every register write. 66-68 % of the
game's register writes repeat the value already there (new counter: `register writes ..., of them changing a value
... (32-34 %)`); a packet that changes no register now writes nothing to the journal
(`RegisterFile::SetSinkChangesOnly`, `SSPC_CP_JOURNAL_CHANGES=1`): the replica already holds those values.

Check: `SSPC_P410_VERIFY=1` (the replica's registers compared with the processor's at every draw and dispatch):
33.3 million compared, **0 different** (beach and village, `logs/cp/journal_verify`).

A/B Native vs Native + this (`logs/p53/cp2_journal`, n = 2, interleaved):

| Scene | Native FPS | + journal changes only | CP thread ms | recording thread ms |
|---|---|---|---|---|
| beach | 44.3 [42.4..46.1] | 47.6 [47.5..47.7] | 19.9 -> 18.2 | 16.6 -> 15.5 |
| early | 40.9 [40.8..41.1] | **42.3 [42.2..42.4]** (+3.4 %) | 20.5 -> 19.7 | 16.8 -> 16.3 |
| user1022 | 36.7 [36.4..37.1] | **37.3 [37.2..37.4]** (+1.6 %) | 21.9 -> 21.3 | 18.1 -> 17.9 |

1% low unchanged (29.2-29.3). Device lost 0, `[Error]` 0.

## CP-2: SRT walks read guest memory by 64-byte lines (taken into Native)

An SRT walk (the game's resource table walker, interpreted) loads 4-8 bytes per guest memory read: ~88 000 reads
per frame, each through the frontend's reader with its check for a pending GPU write. Now a walk reads aligned 64-byte
lines, each once per walk (a line never crosses a page; one that cannot be read whole falls back to the exact load;
`SrtProgram::SetLineCache`, `SSPC_CP_SRT_LINES=1`; `verify`: every load from a line also read exactly and compared).
Memory reads per run 238 M loads -> 19 M lines (12.4×). Check (`logs/cp/srt_verify`, beach + village): 518 million
loads verified, **0 differed**. Self-test `Shader(srt)`: the same walks give the same flattened data with fewer reads.

A/B Native (with the journal change) vs + lines (`logs/p53/cp2_srt`, n = 2, interleaved):

| Scene | Native FPS | + SRT lines | CP thread ms |
|---|---|---|---|
| beach | 47.2 [47.0..47.3] | **48.5 [48.4..48.5]** (+2.7 %) | 18.5 -> 17.9 |
| early | 41.6 [41.5..41.8] | **42.8 [42.5..43.0]** (+2.7 %) | 20.0 -> 19.4 |
| user1022 | 37.4 [36.7..38.2] | 37.9 [37.4..38.5] (within the spread) | 21.0 -> 20.5 |

## Modes (CP-3.14)

`--cp-mode legacy|parallel|native` (or `SSPC_CP_MODE`; `runtime/gpu/cp_mode.*`), chosen once before the backend
starts (a switch while playing would mix the processors' states: a restart changes it). A variable set explicitly
keeps its value; the log says what the mode set (`command processor mode: native ... set: ...`).

| Mode | What runs | Default |
|---|---|---|
| legacy | one thread (`SSPC_P410_PARALLEL_CP=0`) | |
| parallel | the processor + frontend, and the Vulkan recording thread (P4.10) | **yes** (until Native passes long play) |
| native | parallel + code trust `sampled` + journal of changes only + SRT lines | |

### Checks of Native (2026-10-16)

| Check | Result |
|---|---|
| Vulkan Validation + Sync, beach and village (`logs/cp/native_validate2`) | **0 messages**, `[Error]` 0, device lost 0 |
| DLSS Quality + FSR frame generation + Reflex, beach | DLSS 1280×720 -> 1920×1080; real 41.4 FPS, displayed 83-100/s (the statistics count real frames only) |
| FSR 3.1 Quality + Reflex, beach, validation | works; 41 warnings, all inside AMD FSR 3.1's own shader (`rw_luma_history`: Rgba8 operand on an RGBA16F view) - not SecondSonPC's code |
| FSR Native AA + FSR frame generation, beach | real 39.9 FPS, displayed 79.7/s |
| The picture, Parallel vs Native, beach (`logs/cp/img_native1`, `tools/tests/cp_image_ab.ps1`) | by eye the same (geometry, textures, light, smoke, HUD); the numeric comparison is weak here: the game's pause did not freeze the scene, two Parallel runs differ by MAE 8.1/255, Native by 9.3 |

Found on the way and fixed (not the processor): **a race on the swapchain with Reflex** (commit 4d04823):
`vkSetLatencyMarkerNV` / `vkLatencySleepNV` on the game's thread used the swapchain while the present thread ran
`vkAcquireNextImageKHR` / `vkQueuePresentKHR` (validation: 6 `UNASSIGNED-Threading-MultipleThreads` per two scenes).
The presenter now acquires and presents under Reflex's lock (`Reflex::LockSwapchain`, after the queue's mutex as the
executor's submissions): 0 messages after.

Not passing on this machine, not from this work: the bridge's `GuestMemory(api)` self-test group crashes at the
system commit limit (32 652 / 32 655 MiB, no page file) on HEAD 50a242e as well; the shader groups
(`SSPC_SELFTEST_ONLY=shader`) pass: 125 checks, 0 failures.

## Instrumentation added

- `gpu_native.txt`: drains that waited, by reason, with their time; batch submissions by cause (the fence / idle
  counters were declared and never counted before).
- `SSPC_CP_ROUND_PACKETS=<n>`: a queue's packets per round (default 4096) - the CP-5 experiment.
