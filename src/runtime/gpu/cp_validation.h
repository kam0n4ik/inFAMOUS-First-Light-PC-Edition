// SecondSonPC — validation of SecondSonPC's command processor against the bridge's (Phase 4, P4.2;
// docs/PHASE4_P42_COMMAND_PROCESSOR.md §7): what both share for a comparison, live (cp_shadow.h) and offline
// (cp_replay.h).
//
// BridgeMirror keeps the registers as the bridge's Liverpool holds them. SecondSonPC's register file follows the
// hardware; the bridge differs on purpose or by its design in a few known places, which the mirror reproduces so
// that everything else compares exactly:
//   * CLEAR_STATE: the bridge clears every register (config, SH, uconfig too); the hardware only the context;
//   * INDEX_BUFFER_SIZE: the bridge keeps it beside its registers, VGT_DMA_SIZE stays (compared on its own);
//   * DRAW_INDIRECT / DRAW_INDEX_INDIRECT: the bridge does not write VGT_DRAW_INITIATOR;
//   * the compute registers: the bridge keeps them per queue (as SecondSonPC does) and never in its register array.
// The trace format (TraceWriter/TraceReader) records what a replay needs: the bridge's state at the start, every
// submission the shadow took, the command memory it read, and the bridge's state at every draw and dispatch.
#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/cp_shadow.h" // BridgeView

namespace SecondSon::Gpu::Cp {

inline constexpr u32 kBridgeRegs = 0xD000;

/// The register ranges compared at every draw (SH, context, the first 1024 uconfig registers) and at full checks
/// (every window).
struct RegRange {
    u32 first; ///< register index
    u32 count;
};
inline constexpr RegRange kDrawRanges[] = {{0x2C00, 0x400}, {0xA000, 0x400}, {0xC000, 0x400}};
inline constexpr RegRange kAllRanges[] = {{0x2000, 0xC00}, {0x2C00, 0x400}, {0xA000, 0x400}, {0xC000, 0x1000}};

class BridgeMirror final : public PacketObserver {
public:
    /// Installs itself as `cp`'s observer and its register file's mirror (from the register file's current state).
    explicit BridgeMirror(CommandProcessor& cp);
    ~BridgeMirror() override;
    BridgeMirror(const BridgeMirror&) = delete;
    BridgeMirror& operator=(const BridgeMirror&) = delete;

    /// The register as the bridge would hold it.
    u32 Get(u32 reg) const;
    const u32* Flat() const {
        return mirror_.data();
    }
    /// The registers (ours and the mirror) set to `reg`'s bridge value: a resynchronisation after a reported difference.
    void Resync(u32 reg, u32 value);
    /// Both from a snapshot of the bridge (flat order).
    void Load(const u32* flat);

    void BeforePacket(u32 queue, u64 addr, std::span<const u32> packet) override;
    void AfterPacket(u32 queue, u64 addr, std::span<const u32> packet) override;

private:
    CommandProcessor& cp_;
    std::array<u32, kNumRegs> mirror_{};
    u32 saved_dma_size_ = 0;
    u32 saved_initiator_ = 0;
};

/// Hash of the registers of `ranges`: of the bridge's array (indexed by register) or of a flat array.
u64 HashBridgeRegs(const u32* regs, std::span<const RegRange> ranges);
u64 HashFlatRegs(const u32* flat, std::span<const RegRange> ranges);
/// Hash of compute registers [4, kComputeCount): the dispatch's own initiator and group counts left out.
u64 HashCompute(const u32* cs);
/// The bridge's registers in flat order (the windows).
void BridgeToFlat(const u32* regs, u32* flat);

/// One difference found by a comparison.
struct RegDiff {
    u32 reg = 0;
    u32 ours = 0;
    u32 bridge = 0;
};
/// The registers of `ranges` where the mirror and the bridge (indexed by register) differ; at most `max` listed in
/// `out`, the count returned.
u32 DiffRegs(const BridgeMirror& mirror, const u32* bridge_regs, std::span<const RegRange> ranges,
             std::vector<RegDiff>& out, u32 max);
/// The same against a flat snapshot of the bridge.
u32 DiffFlat(const BridgeMirror& mirror, const u32* bridge_flat, std::span<const RegRange> ranges,
             std::vector<RegDiff>& out, u32 max);

// --- trace files ---

inline constexpr char kTraceMagic[8] = {'S', 'S', 'P', 'C', 'T', 'R', 'C', '1'};
inline constexpr u32 kTraceVersion = 1;

enum class TraceRecord : u8 {
    Info = 'H',       ///< text
    State = 'R',      ///< the bridge's state at the capture's start
    Compute = 'Q',    ///< a queue's compute registers when the trace first sees the queue
    Submission = 'S', ///< a submission as the shadow took it
    Memory = 'M',     ///< command memory the shadow read (indirect buffers)
    Checkpoint = 'C', ///< the bridge's state before a draw or dispatch
    Full = 'F',       ///< every bridge register at a checkpoint
    Flip = 'X',       ///< a flip marker processed
    End = 'E',        ///< text
};

struct TraceState {
    u32 index_buffer_size = 0;
    u64 indirect_args = 0;
    std::array<u32, 8> cb_extent{};
    u32 db_extent = 0;
    std::array<u32, kNumRegs> flat{};
};

struct TraceCheckpoint {
    u32 seq = 0;
    u32 queue = 0;
    u32 kind = 0; ///< 0 draw, 1 dispatch
    u64 addr = 0;
    u64 regs_hash = 0; ///< HashBridgeRegs(kDrawRanges)
    u64 cs_hash = 0;   ///< HashCompute of the queue (dispatch)
    u32 index_buffer_size = 0;
    u32 reg_dma_size = 0; ///< the bridge's VGT_DMA_SIZE register
    u64 indirect_args = 0;
    std::array<u32, 8> cb_extent{};
    u32 db_extent = 0;
};

class TraceWriter {
public:
    bool Open(const std::filesystem::path& path, const std::string& info);
    bool IsOpen() const {
        return out_.is_open();
    }
    void State(const TraceState& s);
    void Compute(u32 queue, const u32* cs);
    void Submission(u32 queue, bool ring, u64 addr, std::span<const u32> data);
    void Memory(u64 addr, std::span<const u8> data);
    void Checkpoint(const TraceCheckpoint& c);
    void Full(u32 seq, const u32* flat);
    void Flip(u32 frame);
    void Close(const std::string& summary);
    u64 Bytes() const {
        return bytes_;
    }

private:
    void Record(TraceRecord type, const void* a, size_t na, const void* b = nullptr, size_t nb = 0);
    std::ofstream out_;
    u64 bytes_ = 0;
};

/// A trace loaded whole; records in file order.
struct TraceItem {
    TraceRecord type;
    std::span<const u8> payload;
};
class TraceReader {
public:
    bool Load(const std::filesystem::path& path, std::string& error);
    const std::vector<TraceItem>& Items() const {
        return items_;
    }

private:
    std::vector<u8> data_;
    std::vector<TraceItem> items_;
};

} // namespace SecondSon::Gpu::Cp
