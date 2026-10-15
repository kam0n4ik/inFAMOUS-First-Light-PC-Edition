// SecondSonPC — NVIDIA Reflex low latency (VK_NV_low_latency2; docs/UPSCALING_FRAME_GENERATION.md). No Vulkan types
// here: the game's Gnm driver marks its frames, the executor and the presenter theirs.
//
// A frame's id is its flip id (VideoOut: 1, 2, ... in the order the game asked for the flips) - the presentID of its
// present, so the driver ties the markers, the GPU work and the present of one frame together. While Reflex is on, the
// game's thread sleeps after it handed a frame on (sceGnmSubmitAndFlipCommandBuffers) until the driver says the next
// one should start: the CPU no longer runs frames ahead of the GPU, and the input the game samples then is shown
// sooner. The driver's timings of each frame give the latency measured (logged every 5 s, on and off alike).
#pragma once

#include "base/abi.h"

namespace SecondSon::Gpu::Reflex {

/// The device has VK_NV_low_latency2 (+ VK_KHR_present_id) enabled: the menu's row can be used.
bool Available();

/// The game's thread, after its frame `flip_id` was handed on: simulation end / render submit start of that frame,
/// then (Reflex on) the sleep, then simulation start / input sample of the next one.
void GameFrameSubmitted(u64 flip_id);
/// The executor, when it finished recording frame `flip_id` (its flip marker).
void RenderSubmitEnd(u64 flip_id);

} // namespace SecondSon::Gpu::Reflex
