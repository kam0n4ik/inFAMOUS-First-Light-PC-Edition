// SecondSonPC — SecondSonPC's command processor in the shadow of the bridge's (Phase 4, P4.2;
// docs/PHASE4_P42_COMMAND_PROCESSOR.md §7). Off unless --gpu-shadow or --gpu-trace; then the bridge's Liverpool
// (patch 0028) reports each submission (the game's thread) and each packet before it executes it (its GPU thread).
//
// The shadow runs SecondSonPC's CommandProcessor in Mode::Shadow on the bridge's GPU thread, packet for packet with
// the bridge: before each packet the bridge takes, the shadow's next packet must be the same one (same guest address,
// or the same dwords for a packet the bridge reassembled at a ring's end); then the shadow processes it. Before every
// draw and dispatch it compares its registers — as the bridge would hold them (cp_validation.h, BridgeMirror) — with
// the bridge's: SH, context and uconfig at every draw, every window every `full_every` checkpoints, the dispatch's
// compute registers, the index buffer size, the draw-indirect base, the target size hints. A difference is reported
// (logs\gpu_shadow.txt) and resynchronised from the bridge so that the next one is a new one.
//
// Nothing of the shadow reaches the game: it writes no guest memory, waits for nothing, signals nothing, registers
// no memory listener and tracks no page; it reads command memory through the GPU memory contract (never faulting)
// and copies each submission when it is made. --gpu-trace additionally records frames for offline replay
// (cp_replay.h; logs\gpu_trace\*.sspctrace).
#pragma once

#include <atomic>
#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::Cp {

/// The bridge's state at a packet, as its command processor's hook sees it (pointers valid during the call).
struct BridgeView {
    const u32* regs = nullptr;      ///< Liverpool's register array, indexed by register (0xD000 dwords)
    const u32* cs = nullptr;        ///< the current queue's compute registers (0x50 dwords)
    u32 index_buffer_size = 0;      ///< INDEX_BUFFER_SIZE as the bridge keeps it
    u64 indirect_args = 0;          ///< SET_BASE's draw-indirect base
    const u32* cb_extent = nullptr; ///< 8 colour target size hints
    u32 db_extent = 0;
};

} // namespace SecondSon::Gpu::Cp

namespace SecondSon::Gpu::CpShadow {

extern std::atomic<bool> g_enabled;

inline bool Enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

struct Options {
    std::filesystem::path dir;  ///< logs: gpu_shadow.txt, gpu_trace\...
    u32 report_seconds = 10;    ///< a SecondSonPC.log line (0: none)
    u32 full_every = 64;        ///< every Nth checkpoint compares every register window
    s32 trace_start_s = -1;     ///< --gpu-trace: start at the first flip after this many seconds (-1: no trace)
    u32 trace_frames = 0;       ///< frames to record
    u32 trace_full_every = 256; ///< a full bridge register snapshot every Nth checkpoint of the trace
    bool exit_step = true;      ///< write the report at the process's exit (Lifecycle)
    bool shaders = false;       ///< --gpu-shaders: SecondSonPC's shader frontend (P4.3) at every draw/dispatch,
                                ///< compared with the pipeline the bridge picks (patch 0031)
    bool shaders_per_bind = false; ///< programs revalidated at every bind (else once a frame)
    bool shaders_translate = true; ///< translations on the frontend's worker (else resources only)
};

/// --gpu-shadow / --gpu-trace: before the GPU command processor takes a packet. Once.
void Enable(const Options& options);

/// A submission (the game's thread, inside the bridge's SubmitGfx/SubmitAsc): copied now. `queue`: 0 graphics,
/// 1..64 compute (Gnm's vqid).
void OnSubmit(u32 queue, const u32* data, size_t dwords);

/// The bridge's GPU thread, before it executes the packet at `header` (`available` dwords readable from it). `buffered`:
/// the bridge reassembled the packet (cut by a compute ring's end) in a buffer of its own.
void OnPacket(u32 queue, const u32* header, size_t available, bool buffered, const Cp::BridgeView& view);

/// Writes logs\gpu_shadow.txt now (also the exit step). Thread-safe.
bool WriteNow();

} // namespace SecondSon::Gpu::CpShadow
