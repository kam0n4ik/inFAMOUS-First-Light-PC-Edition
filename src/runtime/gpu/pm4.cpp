// SecondSonPC — PM4 packet decoding and classification (P4.1.2). See pm4.h.
#include "runtime/gpu/pm4.h"

#include <array>

namespace SecondSon::Gpu::Pm4 {

namespace {

struct OpcodeInfo {
    std::string_view name;
    Category category = Category::Unknown;
};

constexpr std::array<OpcodeInfo, 256> MakeOpcodeTable() {
    std::array<OpcodeInfo, 256> t{};
    const auto set = [&t](u8 op, std::string_view name, Category c) { t[op] = OpcodeInfo{name, c}; };
    set(Op::Nop, "NOP", Category::Nop);
    set(Op::SetBase, "SET_BASE", Category::DrawState);
    set(Op::ClearState, "CLEAR_STATE", Category::Control);
    set(Op::IndexBufferSize, "INDEX_BUFFER_SIZE", Category::DrawState);
    set(Op::DispatchDirect, "DISPATCH_DIRECT", Category::Dispatch);
    set(Op::DispatchIndirect, "DISPATCH_INDIRECT", Category::Dispatch);
    set(Op::AtomicGds, "ATOMIC_GDS", Category::Memory);
    set(Op::Atomic, "ATOMIC_MEM", Category::Memory);
    set(Op::OcclusionQuery, "OCCLUSION_QUERY", Category::Query);
    set(Op::SetPredication, "SET_PREDICATION", Category::Control);
    set(Op::RegRmw, "REG_RMW", Category::Register);
    set(Op::CondExec, "COND_EXEC", Category::Control);
    set(Op::PredExec, "PRED_EXEC", Category::Control);
    set(Op::DrawIndirect, "DRAW_INDIRECT", Category::Draw);
    set(Op::DrawIndexIndirect, "DRAW_INDEX_INDIRECT", Category::Draw);
    set(Op::IndexBase, "INDEX_BASE", Category::DrawState);
    set(Op::DrawIndex2, "DRAW_INDEX_2", Category::Draw);
    set(Op::ContextControl, "CONTEXT_CONTROL", Category::Control);
    set(Op::IndexType, "INDEX_TYPE", Category::DrawState);
    set(Op::DrawIndirectMulti, "DRAW_INDIRECT_MULTI", Category::Draw);
    set(Op::DrawIndexAuto, "DRAW_INDEX_AUTO", Category::Draw);
    set(Op::NumInstances, "NUM_INSTANCES", Category::DrawState);
    set(Op::DrawIndexMultiAuto, "DRAW_INDEX_MULTI_AUTO", Category::Draw);
    set(Op::IndirectBufferConst, "INDIRECT_BUFFER_CONST", Category::Indirect);
    set(Op::StrmoutBufferUpdate, "STRMOUT_BUFFER_UPDATE", Category::Query);
    set(Op::DrawIndexOffset2, "DRAW_INDEX_OFFSET_2", Category::Draw);
    set(Op::WriteData, "WRITE_DATA", Category::Memory);
    set(Op::DrawIndexIndirectMulti, "DRAW_INDEX_INDIRECT_MULTI", Category::Draw);
    set(Op::MemSemaphore, "MEM_SEMAPHORE", Category::Sync);
    set(Op::WaitRegMem, "WAIT_REG_MEM", Category::Sync);
    set(Op::IndirectBuffer, "INDIRECT_BUFFER", Category::Indirect);
    set(Op::CopyData, "COPY_DATA", Category::Memory);
    set(Op::CpDma, "CP_DMA", Category::Memory);
    set(Op::PfpSyncMe, "PFP_SYNC_ME", Category::Sync);
    set(Op::SurfaceSync, "SURFACE_SYNC", Category::Sync);
    set(Op::CondWrite, "COND_WRITE", Category::Memory);
    set(Op::EventWrite, "EVENT_WRITE", Category::Sync);
    set(Op::EventWriteEop, "EVENT_WRITE_EOP", Category::Sync);
    set(Op::EventWriteEos, "EVENT_WRITE_EOS", Category::Sync);
    set(Op::ReleaseMem, "RELEASE_MEM", Category::Sync);
    set(Op::PreambleCntl, "PREAMBLE_CNTL", Category::Control);
    set(Op::DmaData, "DMA_DATA", Category::Memory);
    set(Op::ContextRegRmw, "CONTEXT_REG_RMW", Category::Register);
    set(Op::AcquireMem, "ACQUIRE_MEM", Category::Sync);
    set(Op::Rewind, "REWIND", Category::Control);
    set(Op::LoadShReg, "LOAD_SH_REG", Category::Register);
    set(Op::LoadConfigReg, "LOAD_CONFIG_REG", Category::Register);
    set(Op::LoadContextReg, "LOAD_CONTEXT_REG", Category::Register);
    set(Op::SetConfigReg, "SET_CONFIG_REG", Category::Register);
    set(Op::SetContextReg, "SET_CONTEXT_REG", Category::Register);
    set(Op::SetContextRegIndirect, "SET_CONTEXT_REG_INDIRECT", Category::Register);
    set(Op::SetShReg, "SET_SH_REG", Category::Register);
    set(Op::SetShRegOffset, "SET_SH_REG_OFFSET", Category::Register);
    set(Op::SetQueueReg, "SET_QUEUE_REG", Category::Register);
    set(Op::SetUconfigReg, "SET_UCONFIG_REG", Category::Register);
    set(Op::LoadConstRam, "LOAD_CONST_RAM", Category::Constant);
    set(Op::WriteConstRam, "WRITE_CONST_RAM", Category::Constant);
    set(Op::DumpConstRam, "DUMP_CONST_RAM", Category::Constant);
    set(Op::IncrementCeCounter, "INCREMENT_CE_COUNTER", Category::Constant);
    set(Op::IncrementDeCounter, "INCREMENT_DE_COUNTER", Category::Constant);
    set(Op::WaitOnCeCounter, "WAIT_ON_CE_COUNTER", Category::Constant);
    set(Op::WaitOnDeCounterDiff, "WAIT_ON_DE_COUNTER_DIFF", Category::Constant);
    set(Op::GetLodStats, "GET_LOD_STATS", Category::Query);
    set(Op::DrawIndexIndirectCountMulti, "DRAW_INDEX_INDIRECT_COUNT_MULTI", Category::Draw);
    return t;
}

constexpr auto kOpcodes = MakeOpcodeTable();

struct MarkerValue {
    u32 value;
    NopMarker marker;
};

constexpr MarkerValue kMarkers[] = {
    {0x68750001u, NopMarker::DebugMarkerPush},
    {0x68750002u, NopMarker::DebugMarkerPop},
    {0x68750003u, NopMarker::DebugSetMarker},
    {0x68750004u, NopMarker::SetVsharpInUdata},
    {0x68750005u, NopMarker::SetTsharpInUdata},
    {0x68750006u, NopMarker::SetSsharpInUdata},
    {0x6875000Eu, NopMarker::DebugColorMarkerPush},
    {0x68750776u, NopMarker::PatchedFlip},
    {0x68750777u, NopMarker::PrepareFlip},
    {0x68750778u, NopMarker::PrepareFlipLabel},
    {0x68750780u, NopMarker::PrepareFlipInterrupt},
    {0x68750781u, NopMarker::PrepareFlipInterruptLabel},
};

} // namespace

bool Decode(std::span<const u32> stream, Packet& out, DecodeError& error) {
    out = Packet{};
    if (stream.empty()) {
        error = DecodeError::Empty;
        return false;
    }
    const u32 h = stream[0];
    out.type = static_cast<u8>(h >> 30);
    switch (out.type) {
    case 0:
        out.base = h & 0xFFFF;
        out.dwords = ((h >> 16) & 0x3FFF) + 2; // header + count + 1 register values
        break;
    case 2:
        out.dwords = 1; // padding
        break;
    case 3:
        out.predicate = (h & 1) != 0;
        out.compute = ((h >> 1) & 1) != 0;
        out.opcode = static_cast<u8>(h >> 8);
        out.dwords = ((h >> 16) & 0x3FFF) + 2;
        break;
    default:
        error = DecodeError::ReservedType;
        return false;
    }
    if (out.dwords > stream.size()) {
        error = DecodeError::Truncated;
        return false;
    }
    error = DecodeError::None;
    return true;
}

std::string_view OpcodeName(u8 opcode) {
    return kOpcodes[opcode].name;
}

Category Classify(u8 opcode) {
    return kOpcodes[opcode].category;
}

std::string_view CategoryName(Category category) {
    static constexpr std::string_view kNames[] = {"nop",      "register", "draw-state", "draw",
                                                  "dispatch", "sync",     "memory",     "indirect",
                                                  "constant", "control",  "query",      "unknown"};
    static_assert(std::size(kNames) == static_cast<size_t>(Category::Count));
    return category < Category::Count ? kNames[static_cast<size_t>(category)] : "?";
}

std::string_view EngineName(Engine engine) {
    static constexpr std::string_view kNames[] = {"dcb", "ccb", "asc"};
    static_assert(std::size(kNames) == static_cast<size_t>(Engine::Count));
    return engine < Engine::Count ? kNames[static_cast<size_t>(engine)] : "?";
}

bool SetRegSpace(u8 opcode, RegSpace& out) {
    switch (opcode) {
    case Op::SetConfigReg:
        out = RegSpace::Config;
        return true;
    case Op::SetShReg:
        out = RegSpace::Sh;
        return true;
    case Op::SetContextReg:
        out = RegSpace::Context;
        return true;
    case Op::SetUconfigReg:
        out = RegSpace::Uconfig;
        return true;
    default:
        return false;
    }
}

std::string_view RegSpaceName(RegSpace space) {
    static constexpr std::string_view kNames[] = {"config", "sh", "context", "uconfig"};
    static_assert(std::size(kNames) == static_cast<size_t>(RegSpace::Count));
    return space < RegSpace::Count ? kNames[static_cast<size_t>(space)] : "?";
}

NopMarker ClassifyNop(std::span<const u32> packet) {
    if (packet.size() < 2) {
        return NopMarker::Empty;
    }
    for (const auto& m : kMarkers) {
        if (packet[1] == m.value) {
            return m.marker;
        }
    }
    return packet.size() == 2 ? NopMarker::OneDword : NopMarker::Other;
}

std::string_view NopMarkerName(NopMarker marker) {
    static constexpr std::string_view kNames[] = {"empty",
                                                  "debug-marker-push",
                                                  "debug-marker-pop",
                                                  "debug-set-marker",
                                                  "set-vsharp-in-udata",
                                                  "set-tsharp-in-udata",
                                                  "set-ssharp-in-udata",
                                                  "debug-color-marker-push",
                                                  "patched-flip",
                                                  "prepare-flip",
                                                  "prepare-flip-label",
                                                  "prepare-flip-interrupt",
                                                  "prepare-flip-interrupt-label",
                                                  "one-dword",
                                                  "other"};
    static_assert(std::size(kNames) == static_cast<size_t>(NopMarker::Count));
    return marker < NopMarker::Count ? kNames[static_cast<size_t>(marker)] : "?";
}

} // namespace SecondSon::Gpu::Pm4
