// SecondSonPC — the PM4 command stream as SecondSonPC itself reads it (Phase 4, P4.1.2;
// docs/PHASE4_P41_GPU_FOUNDATION.md §3). Decoding and classification only: nothing here executes a packet. The
// opcode values and field positions are the Liverpool (GCN 1.1) command processor's, the hardware's facts; the
// statistics (pm4_stats.h) use them now, SecondSonPC's own command processor (P4.2) later.
#pragma once

#include <span>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu::Pm4 {

/// Which stream a packet came from: the graphics ring's draw engine (dcb), its constant engine (ccb), a compute
/// queue (asc).
enum class Engine : u8 { Draw, Constant, Compute, Count };

enum class Category : u8 {
    Nop,       ///< NOP (padding, the driver's markers, the flip marker)
    Register,  ///< SET_*_REG, LOAD_*_REG, register read-modify-write
    DrawState, ///< index type/base/size, instance count, SET_BASE
    Draw,      ///< DRAW_*
    Dispatch,  ///< DISPATCH_*
    Sync,      ///< events, end-of-pipe/-shader writes, waits, semaphores, cache actions
    Memory,    ///< DMA_DATA, WRITE_DATA, COPY_DATA, CP_DMA, atomics
    Indirect,  ///< INDIRECT_BUFFER(_CONST), conditional indirect buffer
    Constant,  ///< constant engine RAM and counters
    Control,   ///< CONTEXT_CONTROL, CLEAR_STATE, REWIND, conditional execution, predication
    Query,     ///< occlusion queries, LOD statistics, streamout updates
    Unknown,   ///< an opcode SecondSonPC has no name for
    Count,
};

/// Type-3 opcodes (IT_*).
namespace Op {
inline constexpr u8 Nop = 0x10;
inline constexpr u8 SetBase = 0x11;
inline constexpr u8 ClearState = 0x12;
inline constexpr u8 IndexBufferSize = 0x13;
inline constexpr u8 DispatchDirect = 0x15;
inline constexpr u8 DispatchIndirect = 0x16;
inline constexpr u8 AtomicGds = 0x1D;
inline constexpr u8 Atomic = 0x1E;
inline constexpr u8 OcclusionQuery = 0x1F;
inline constexpr u8 SetPredication = 0x20;
inline constexpr u8 RegRmw = 0x21;
inline constexpr u8 CondExec = 0x22;
inline constexpr u8 PredExec = 0x23;
inline constexpr u8 DrawIndirect = 0x24;
inline constexpr u8 DrawIndexIndirect = 0x25;
inline constexpr u8 IndexBase = 0x26;
inline constexpr u8 DrawIndex2 = 0x27;
inline constexpr u8 ContextControl = 0x28;
inline constexpr u8 IndexType = 0x2A;
inline constexpr u8 DrawIndirectMulti = 0x2C;
inline constexpr u8 DrawIndexAuto = 0x2D;
inline constexpr u8 NumInstances = 0x2F;
inline constexpr u8 DrawIndexMultiAuto = 0x30;
inline constexpr u8 IndirectBufferConst = 0x33;
inline constexpr u8 StrmoutBufferUpdate = 0x34;
inline constexpr u8 DrawIndexOffset2 = 0x35;
inline constexpr u8 WriteData = 0x37;
inline constexpr u8 DrawIndexIndirectMulti = 0x38;
inline constexpr u8 MemSemaphore = 0x39;
inline constexpr u8 WaitRegMem = 0x3C;
inline constexpr u8 IndirectBuffer = 0x3F;
inline constexpr u8 CopyData = 0x40;
inline constexpr u8 CpDma = 0x41;
inline constexpr u8 PfpSyncMe = 0x42;
inline constexpr u8 SurfaceSync = 0x43;
inline constexpr u8 CondWrite = 0x45;
inline constexpr u8 EventWrite = 0x46;
inline constexpr u8 EventWriteEop = 0x47;
inline constexpr u8 EventWriteEos = 0x48;
inline constexpr u8 ReleaseMem = 0x49;
inline constexpr u8 PreambleCntl = 0x4A;
inline constexpr u8 DmaData = 0x50;
inline constexpr u8 ContextRegRmw = 0x51;
inline constexpr u8 AcquireMem = 0x58;
inline constexpr u8 Rewind = 0x59;
inline constexpr u8 LoadShReg = 0x5F;
inline constexpr u8 LoadConfigReg = 0x60;
inline constexpr u8 LoadContextReg = 0x61;
inline constexpr u8 SetConfigReg = 0x68;
inline constexpr u8 SetContextReg = 0x69;
inline constexpr u8 SetContextRegIndirect = 0x73;
inline constexpr u8 SetShReg = 0x76;
inline constexpr u8 SetShRegOffset = 0x77;
inline constexpr u8 SetQueueReg = 0x78;
inline constexpr u8 SetUconfigReg = 0x79;
inline constexpr u8 LoadConstRam = 0x80;
inline constexpr u8 WriteConstRam = 0x81;
inline constexpr u8 DumpConstRam = 0x83;
inline constexpr u8 IncrementCeCounter = 0x84;
inline constexpr u8 IncrementDeCounter = 0x85;
inline constexpr u8 WaitOnCeCounter = 0x86;
inline constexpr u8 WaitOnDeCounterDiff = 0x88;
inline constexpr u8 GetLodStats = 0x8E;
inline constexpr u8 DrawIndexIndirectCountMulti = 0x9D;
} // namespace Op

/// One packet's header, decoded. `dwords`: the whole packet, header included.
struct Packet {
    u8 type = 0;            ///< 0, 2 or 3 (1 is reserved and refused)
    u8 opcode = 0;          ///< type 3
    bool predicate = false; ///< type 3: a predicated packet
    bool compute = false;   ///< type 3: shader type compute
    u32 dwords = 0;
    u32 base = 0;           ///< type 0: the first register written
};

enum class DecodeError : u8 { None, Empty, Truncated, ReservedType };

/// The packet at the start of `stream`. false with `error` when the stream is empty, the packet is longer than
/// what is left, or its type is the reserved 1. Reads only the header dword.
bool Decode(std::span<const u32> stream, Packet& out, DecodeError& error);

/// The opcode's name ("" for one SecondSonPC does not know) and category.
std::string_view OpcodeName(u8 opcode);
Category Classify(u8 opcode);
std::string_view CategoryName(Category category);
std::string_view EngineName(Engine engine);

/// The register spaces of SET_*_REG: the register is the space's base + the packet's 16-bit offset (body dword 0).
enum class RegSpace : u8 { Config, Sh, Context, Uconfig, Count };
inline constexpr u32 kRegBase[] = {0x2000, 0x2C00, 0xA000, 0xC000};
inline constexpr u32 kRegSpaceSize = 0x400; ///< registers per space the statistics keep apart (the rest: "beyond")

/// The space a SET_*_REG opcode writes; false for other opcodes.
bool SetRegSpace(u8 opcode, RegSpace& out);
std::string_view RegSpaceName(RegSpace space);

/// The driver's markers in a NOP's first payload dword (the values the PS4 driver writes; the bridge's list).
enum class NopMarker : u8 {
    Empty, ///< no payload: padding
    DebugMarkerPush,
    DebugMarkerPop,
    DebugSetMarker,
    SetVsharpInUdata,
    SetTsharpInUdata,
    SetSsharpInUdata,
    DebugColorMarkerPush,
    PatchedFlip, ///< the flip marker sceGnmSubmitAndFlip* leaves behind: one per presented frame
    PrepareFlip,
    PrepareFlipLabel,
    PrepareFlipInterrupt,
    PrepareFlipInterruptLabel,
    OneDword, ///< one unknown payload dword: the size hints the driver puts after a colour/depth target setup
    Other,    ///< a longer unknown payload
    Count,
};

/// `packet`: a NOP, header included (its declared length; nothing beyond is read).
NopMarker ClassifyNop(std::span<const u32> packet);
std::string_view NopMarkerName(NopMarker marker);

/// Fields of the packets the statistics break down (body = the dwords after the header).
inline u32 EventType(u32 event_cntl) {
    return event_cntl & 0x3F; ///< VGT_EVENT_INITIATOR.EVENT_TYPE (EVENT_WRITE*, RELEASE_MEM)
}
inline u32 EopIntSel(u32 data_cntl) {
    return (data_cntl >> 24) & 0x3; ///< EVENT_WRITE_EOP dword 2
}
inline u32 EopDataSel(u32 data_cntl) {
    return (data_cntl >> 29) & 0x7;
}
inline u32 DmaSrcSel(u32 dw) {
    return (dw >> 29) & 0x3; ///< DMA_DATA dword 0: 0 memory, 1 GDS, 2 data, 3 memory through L2
}
inline u32 DmaDstSel(u32 dw) {
    return (dw >> 20) & 0x3; ///< 0 memory, 1 GDS, 3 memory through L2
}
inline u32 DmaBytes(u32 command) {
    return command & 0x1FFFFF; ///< DMA_DATA dword 5
}
inline u32 IndirectSize(u32 dw2) {
    return dw2 & 0xFFFFF; ///< INDIRECT_BUFFER dword 2: size in dwords
}
inline u32 WaitFunction(u32 dw) {
    return dw & 0x7; ///< WAIT_REG_MEM dword 0
}
inline bool WaitOnMemory(u32 dw) {
    return (dw >> 4) & 1;
}

} // namespace SecondSon::Gpu::Pm4
