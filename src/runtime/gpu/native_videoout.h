// SecondSonPC — SecondSonPC's own libSceVideoOut (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §F), bound
// only with --gpu-backend sspc, together with SecondSonPC's GnmDriver (one owner of Resource::Presentation and
// Resource::CommandStream; backend.h).
//
// The port, its display buffers and labels, the flip queue, the flip/vblank status and events (Kernel::EventQueues),
// and the present thread's vblank (60 Hz, the game's flip rate). A GPU flip (SubmitAndFlip) is requested by the
// driver (SubmitEopFlip) and becomes a request when the command processor reaches the frame's flip marker, carrying the
// GPU timeline value the frame's work completes at; the present thread shows it at the first vblank after that work
// completed — never earlier — through the backend's sink (its swapchain), then releases the previous buffer's label
// (what the game's WAIT_REG_MEM after sceGnmInsertWaitFlipDone waits for). Ported from shadPS4's videoout
// (GPL-2.0-or-later); no presenter of the bridge exists in this mode.
#pragma once

#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Gpu::VideoOut {

inline constexpr u32 kMaxBuffers = 16;

/// A display buffer as registered (sceVideoOutRegisterBuffers).
struct BufferInfo {
    u64 address = 0;
    u32 pixel_format = 0; ///< the VideoOut pixel format (0x80000000 A8R8G8B8 sRGB, ...)
    u32 tiling_mode = 0;  ///< 0 tiled, 1 linear
    u32 width = 0, height = 0, pitch = 0;
};

struct FlipRequest {
    s32 index = -1;       ///< -1: a blank frame
    s64 flip_arg = 0;
    bool eop = false;     ///< from a GPU flip marker
    u64 gpu_value = 0;    ///< the backend's timeline value the frame's work completes at (0: none to wait for)
    u64 flip_id = 0;      ///< 1, 2, ... (logs, captures)
    s32 frame = -1;       ///< the presenter's frame the buffer was copied into (-1: none: shown blank)
    BufferInfo buffer;
};

/// What shows a frame: the backend's presenter.
class Sink {
public:
    virtual ~Sink() = default;
    /// Has the GPU work up to `gpu_value` completed? (the present thread polls it at each vblank)
    virtual bool Ready(u64 gpu_value) = 0;
    /// Waits up to `timeout_ns` for it (the free pacing: a frame shown as soon as its work completed). true: done.
    virtual bool WaitReady(u64 gpu_value, u64 /*timeout_ns*/) { return Ready(gpu_value); }
    /// Must flips stay at the vblank now whatever the pacing? (FSR frame generation: its present blocks inside the
    /// queue's lock when real frames come faster than it shows them - a deadlock with the GPU thread's submissions)
    virtual bool NeedsVblank() { return false; }
    /// Shows `request` (its buffer, or blank). Called on the present thread.
    virtual void Present(const FlipRequest& request) = 0;
};

/// How flips reach the display (FrameBreak FB-2, docs/FRAMEBREAK.md). The game's clock is the TSC (eboot 0x12a160:
/// the frame's delta = TSC ticks / sceKernelGetTscFrequency, at most 1/30 s), not the vblank count, so its world runs
/// at the same speed whatever the flip cadence.
struct Pacing {
    /// true ("console"): a flip only at an emulated 59.94 Hz vblank (every flip_rate + 1-th), as the PS4's output: a
    /// frame that misses one waits for the next (16.7 / 33.4 / 50 ms). false: a flip as soon as its GPU work completed.
    bool vblank = true;
    u32 cap_fps = 0; ///< free pacing: at most this many flips a second (0: none); the game's own flip rate still holds
    u32 spin_us = 0; ///< free pacing with a cap: the last microseconds before a flip are spun, not slept (precision)
};
/// Any thread, while the game runs too (the settings menu).
void SetPacing(const Pacing& pacing);
Pacing GetPacing();
/// "console" | "unlimited" | "30" ... "360" -> the pacing; false: not one of them.
bool ParsePacing(const std::string& text, Pacing& out);

/// Starts the port's present thread with `sink` (the backend's); Stop joins it. Idempotent.
void Start(Sink* sink);
void Stop();
bool Started();

/// GnmDriver: a GPU flip of `index` requested by SubmitAndFlip (it happens at the frame's flip marker). 0 or a
/// VideoOut error.
s32 SubmitEopFlip(s32 handle, u32 index, u32 flip_mode, s64 flip_arg);
/// The buffer the next GPU flip shows (the oldest requested one); false: none requested.
bool NextFlipBuffer(BufferInfo& out, s32& index);
/// The command processor reached a flip marker; the frame's work (and the copy of its buffer into the presenter's
/// `frame`) completes at `gpu_value`. false: no EOP flip was requested for it (counted, logged once).
bool OnFlipMarker(u64 gpu_value, s32 frame);
/// `address` is a registered display buffer (Upscaling: the draws that write the displayed frame).
bool IsDisplayBuffer(u64 address);
/// Flip id the next OnFlipMarker gives (captures name it).
u64 NextFlipId();
/// GPU flips the game asked for so far (SubmitEopFlip): the flip id the last one gets (NVIDIA Reflex's frame id).
u64 RequestedFlips();
/// GnmDriver: the guest address of the port's labels (16 × u64).
bool BufferLabelAddress(s32 handle, u64& address);

/// The labels are SecondSonPC's host memory (not guest memory, and above the 40-bit GPU space): the command
/// processor's memory serves reads and writes of them here. true: [addr, addr + bytes) is inside the labels.
bool LabelRead(u64 addr, void* dst, u64 bytes);
bool LabelWrite(u64 addr, const void* src, u64 bytes);
/// [addr, addr + bytes) is inside the labels (no GPU work reads them; P4.6).
bool IsLabel(u64 addr, u64 bytes);
/// Called (from the present thread) after a label changed: the backend wakes its waiting queues.
void SetLabelCallback(void (*fn)(void* ctx), void* ctx);

struct Stats {
    u64 flips_requested = 0, flips_presented = 0, blank = 0, vblanks = 0, markers_without_flip = 0;
    u64 waited_vblanks = 0; ///< vblanks a request waited for its GPU work
    u64 free_flips = 0;     ///< flips shown by the free pacing (not at a vblank)
    u64 capped_waits = 0;   ///< ... of them held back by the cap or the game's flip rate
};
Stats GetStats();

std::span<const Runtime::Export> Exports();

} // namespace SecondSon::Gpu::VideoOut
