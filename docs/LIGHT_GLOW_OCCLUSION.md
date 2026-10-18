# Light glows through walls: the guest's occlusion queries (2026-10-18)

Symptom (user, city, rain, daytime): the glows of car headlights behind a fence/wall shone through it - small
blue-white spots over the fence (photo 2026-10-18 14:12). Not surface lighting, not shadows: the glow sprites.

## Cause (Ghidra + code, confirmed by A/B in the game)

The game fades each light's glow by an occlusion test of its own (eboot, App 01.07):

| Address | What |
|---|---|
| `0x194060` | per light with a glow (list at `+0x88`): two queries, index `2k` and `2k+1`, through `0x1ee0c0` |
| `0x1ee0c0` / `0x1ee320` | a proxy at the light (`0x1ee320`: a cone from the spot angle, `tanf(angle/2)`), drawn by `0x448ce0` between a query's begin and end; depth control `GEQUAL`, no depth write; query `2k` with the depth test, `2k+1` without |
| `0x6277a0` (Gnm) | begin: DMA clear of the 0x100-byte result slot, `PIXEL_PIPE_STAT_CONTROL` + `PIXEL_PIPE_STAT_DUMP` at the slot; end: the same 8 bytes further (DB records `{begin u64, end u64}`, 8 or 16 DBs, stride 16) |
| `0x195250` -> `0x1ee6a0` -> `0x1ed990` | a compute shader (1x1x1) reads the 2n result slots (stride 0x100) and writes n floats - the visible part = depth-tested count / untested count; `0x1a82f0` draws the glows with it |

The results never reach the CPU: GPU -> GPU. Our command processor did not count pixels; it wrote ever-growing
equal counts to both queries ("every query reads as visible", `command_processor.cpp`), so the ratio was always 1 and
every glow was drawn at full strength wherever it stood. The PS4 counts the proxy's pixels, so the original hides them.

## Fix

- `GpuWork::ZpassOnTimeline` (CP, Isolated mode): an occlusion count event goes to the executor instead of the
  processor's own write (the old write stays the fallback; `SSPC_OCCLUSION=off` restores it for A/B).
- `Vk::Executor::ZpassOnTimeline`: begin event - the DB records written on the timeline as `{0, 0}` with the valid bit
  (bit 63); each draw until the end event is wrapped in a host `VK_QUERY_TYPE_OCCLUSION` query (precise -
  `occlusionQueryPrecise`, one slot per draw, at most one per DB record); end event - `vkCmdCopyQueryPoolResults`
  (32-bit, `WAIT`) of each draw's count into the low dword of a DB record's end count, the slots reset. The game's
  shader sums the DBs as on the hardware. Barriers / write tracking as `CopyOnTimeline`.
- Parallel CP: drained once per event (before: up to 8 drained timeline writes per event).

## Checks (city save of the user, a copy; 85 s each, same input)

- A/B frames (`SSPC_DIAG_CAPTURE_FRAMES=1200:8:150`), region of the photo: old path - headlight glows behind the
  fence in 3 of 8 frames and one behind a tree; new path - none in 8 of 8. Glows in plain view (street lamps, shop
  sign lights, bollards) unchanged.
- `--gpu-native-validate` (validation + synchronization validation), 110 s: 0 messages, `[Error]` 0.
- Cost, one run each: 34.3 vs 35.0 FPS (run spread larger), GPU busy 17.9 vs 18.0 ms; the processor's GPU wait
  2.5 vs 1.1 ms a frame (the copies wait for their queries) - batching the copies is a candidate optimization.

Not checked by a run: a car whose headlights face the camera in plain view (the scene had none) - the same ratio
path as the street lamps, which stay.
