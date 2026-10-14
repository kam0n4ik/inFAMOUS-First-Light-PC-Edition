// SecondSonPC — native render operations: the engine's draw as SecondSonPC reconstructs it, carried to the command
// processor IN the game's command buffer (P5.3B/C; docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md).
//
// An operation travels as a PM4 NOP whose payload starts with kMarkerMagic, written into the context's DCB at the place
// the game's own draw packet goes. The command buffers keep their order, are submitted, chained and replayed as before,
// and the processor meets the operation exactly where the game's draw is - so the order of state changes, draws,
// barriers and fences stays the game's by construction. What the processor does with it:
//   kVerify (shadow, P5.3B)  remembers the operation; the next draw packet of the queue is compared with it (the draw
//                            the PM4 path built: its parameters, the shader registers it sees, the resource tables its
//                            user data point at). Counted as a match or a mismatch per field; nothing is executed twice.
//   kApply  (P5.3C)          the operation replaces the game's PM4 for its draw (see NATIVE_RENDER_INTERFACE.md §4).
//
// The engine side (native_draw.cpp) builds DrawOp from the engine's own objects: the constant update engine's shader
// pointers and stage resource shadows, the user-data layout tables, the draw wrapper's arguments. No PM4 is read there.
#pragma once

#include <atomic>
#include <span>

#include "base/abi.h"

namespace SecondSon::Engine::Render {

inline constexpr u32 kMarkerMagic = 0x43505353u; ///< "SSPC" (little endian) - no driver NOP payload starts with it

enum class OpKind : u16 { Draw = 1 };

enum OpFlags : u16 {
    kVerify = 1,
    kApply = 2,
};

/// Registers sceGnmSetVsShader / sceGnmSetPsShader350 write from the shader object's register block (gnm_driver.cpp
/// SetVsRegs/SetPsRegs), in the block's order: the values a draw sees for its VS and PS.
inline constexpr u32 kVsRegs = 7;  ///< PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_VS_OUT_CONFIG, SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL
inline constexpr u32 kPsRegs = 12; ///< PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_SHADER_Z_FORMAT, SPI_SHADER_COL_FORMAT,
                                   ///< SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, SPI_PS_IN_CONTROL, SPI_BARYC_CNTL,
                                   ///< DB_SHADER_CONTROL, CB_SHADER_MASK
inline constexpr u32 kVsRegIndex[kVsRegs] = {0x2C48, 0x2C49, 0x2C4A, 0x2C4B, 0xA1B1, 0xA1C3, 0xA207};
inline constexpr u32 kPsRegIndex[kPsRegs] = {0x2C08, 0x2C09, 0x2C0A, 0x2C0B, 0xA1C4, 0xA1C5,
                                             0xA1B3, 0xA1B4, 0xA1B6, 0xA1B8, 0xA203, 0xA08F};

/// A user-data SGPR pair that holds a pointer into the stage's resource snapshot (the copy of the CUE's stage shadow the
/// commit 0x14ad0 makes), `offset` dwords past its start.
struct TableRef {
    u8 sgpr = 0;
    u16 offset = 0;
};

/// A stage's resource table as the engine holds it: the shadow's first `dwords` dwords and their FNV-1a 64.
struct StageTable {
    bool present = false;
    u32 dwords = 0;
    u64 hash = 0;
    u32 refs = 0;
    TableRef ref[7];
};

enum DrawStage : u32 { kStageVs = 0, kStagePs = 1, kDrawStages = 2 };

struct DrawOp {
    s32 pass = -1; ///< 0..40; -2 pre, -3 post, -4 deformation, -1 other context
    u32 sub = 0;
    bool canary = false;   ///< spoiled on purpose (index count + 1): the processor must report it
    bool indexed = false;
    u32 index_count = 0;
    u64 index_base = 0;    ///< indexed: the index buffer's address
    bool shaders = false;  ///< vs/ps below are known (the commit's ordinary shader path)
    bool has_ps = false;
    u32 vs[kVsRegs] = {};
    u32 ps[kPsRegs] = {};
    StageTable table[kDrawStages];
};

/// Dwords EncodeDrawMarker writes (header included).
inline constexpr u32 kDrawMarkerDwords = 1 + 27 + 2 * 11;

/// Writes the NOP: header, {magic, kind | flags << 16, the operation}. Returns kDrawMarkerDwords.
u32 EncodeDrawMarker(const DrawOp& op, u16 flags, u32* out);
/// True when `packet` (a NOP, header included) carries a SecondSonPC operation.
bool IsMarker(std::span<const u32> packet);
/// The marker's draw operation; false if it is not a well-formed one (the caller counts it).
bool DecodeDrawMarker(std::span<const u32> packet, DrawOp& out, u16& flags);

/// What the command processor built for the draw that follows the marker.
struct DrawView {
    bool indexed = false;
    u32 index_count = 0;
    u64 index_base = 0;
    u32 vs[kVsRegs] = {};
    u32 ps[kPsRegs] = {};
    const u32* user_data[kDrawStages] = {}; ///< 16 dwords each (USER_DATA_VS_0.., USER_DATA_PS_0..)
};

enum DrawMismatch : u32 {
    kMismatchKind = 1u << 0,       ///< indexed vs auto
    kMismatchIndexCount = 1u << 1,
    kMismatchIndexBase = 1u << 2,
    kMismatchVsRegs = 1u << 3,
    kMismatchPsRegs = 1u << 4,
    kMismatchVsTable = 1u << 5,    ///< the VS user data do not point at a copy of the engine's VS table
    kMismatchPsTable = 1u << 6,
    kMismatchFields = 7,
};

/// FNV-1a 64 of `bytes` guest bytes at `address`; false if they cannot be read.
using HashGuestFn = bool (*)(u64 address, u64 bytes, u64& hash);

/// The fields of `view` that differ from `op` (DrawMismatch bits; 0 = the PM4 path drew exactly the engine's draw).
u32 VerifyDraw(const DrawOp& op, const DrawView& view, HashGuestFn hash_guest);

/// Guest memory as the processor's process sees it (the eboot's memory is the process's); readable ranges cached.
bool HashGuestMemory(u64 address, u64 bytes, u64& hash);

/// What the command processor did with the markers (all queues, since start).
inline constexpr u32 kPassSlots = 48; ///< 0..40 passes, 41 pre, 42 post, 43 deformation, 44 other
u32 PassSlot(s32 pass);
struct DrawCounters {
    std::atomic<u64> markers{0};
    std::atomic<u64> malformed{0};
    std::atomic<u64> orphaned{0};  ///< a marker not followed by a draw before the next marker / end of buffer
    std::atomic<u64> verified{0};
    std::atomic<u64> matched{0};
    std::atomic<u64> mismatched{0};
    std::atomic<u64> field[kMismatchFields] = {};
    std::atomic<u64> shaders_unknown{0};
    std::atomic<u64> tables_checked{0}; ///< stage tables compared by content (references and dwords present)
    std::atomic<u64> canaries{0};       ///< markers the engine side spoiled on purpose (SSPC_P53_NATIVE_DRAW_CANARY)
    std::atomic<u64> canaries_caught{0};
    std::atomic<u64> pass_verified[kPassSlots] = {};
    std::atomic<u64> pass_mismatched[kPassSlots] = {};
};
DrawCounters& Counters();

} // namespace SecondSon::Engine::Render
