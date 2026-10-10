// SecondSonPC — the game's GCN shader programs as SecondSonPC's shader frontend reads them (Phase 4, P4.3;
// docs/PHASE4_P43_SHADER_TRANSLATION.md §5).
//
// A program is found by the address its stage's PGM_LO/PGM_HI registers hold. What is there is the compiler's output:
// GCN code, then a 28-byte "OrbShdr" footer (signature, the code's length, the compiler's hash of the source). The
// footer is where the first instruction (s_mov_b32 vcc_hi, imm) points, or — as the bridge searches it — the first
// signature in the first 64 KiB. Everything is read through a GuestReader (bounded copies; nothing here dereferences a
// guest address), and the code is copied into a buffer the frontend owns: no pointer into guest memory is kept.
//
// The identity of a program is its content: the stage it runs as and a 128-bit hash of the code (XXH3-128), with the
// length. The footer's compiler hash is recorded (it is the bridge's cache key) but never trusted alone: a program
// rewritten in place keeps whatever footer it is given. The address is not part of the identity (the same code at two
// addresses is one program); it matters only to the resources (an inline constant buffer's address is relative to it).
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu::Shader {

/// The hardware stage a program runs as: the SPI_SHADER_PGM_* register block of a graphics stage (the order of
/// Cp::Stage) or the compute registers.
enum class HwStage : u8 { Ps, Vs, Gs, Es, Hs, Ls, Cs, Count };
/// The logical stage the translator emits (the order of the bridge's Shader::SwStage).
enum class SwStage : u8 { Fragment, TessControl, TessEval, Vertex, Geometry, Compute, Count };

std::string_view HwStageName(HwStage stage);
std::string_view SwStageName(SwStage stage);

/// Guest memory as the frontend reads it: the GPU memory contract in the game (CopySparse: never faults), a capture's
/// recorded blocks offline, a test memory in the self-test.
class GuestReader {
public:
    virtual ~GuestReader() = default;
    /// [addr, addr + bytes) into `dst`. false: some part is not readable (outside the 40-bit GPU space, or not in the
    /// capture); nothing is guaranteed in `dst` then.
    virtual bool Read(u64 addr, void* dst, u64 bytes) = 0;
};

/// The footer the PS4 shader compiler appends to a program (the bridge's AmdGpu::BinaryInfo layout).
struct ProgramFooter {
    u8 version = 0;
    u8 type = 0;        ///< the compiler's shader type
    u8 source_type = 0;
    bool pssl_or_cg = false;
    bool cached = false;
    u32 length = 0;     ///< bytes of code from the program's start
    u8 usage_base_dw = 0;
    u8 num_usage_slots = 0;
    bool is_srt = false;
    bool srt_used_info_valid = false;
    bool extended_usage = false;
    u64 shader_hash = 0; ///< the compiler's hash (the bridge's program cache key)
    u32 crc32 = 0;
    u32 offset = 0;      ///< bytes from the program's start to the footer
    bool by_token = false; ///< found where the first instruction points (not by the 64 KiB search)
};

inline constexpr u32 kFooterBytes = 28;
inline constexpr u32 kFooterSearchBytes = 0x10000;   ///< as the bridge: the first 64 KiB
inline constexpr u32 kMaxProgramBytes = 1u << 20;    ///< longer programs are refused
inline constexpr u32 kTokenMovVcchi = 0xBEEB03FFu;   ///< s_mov_b32 vcc_hi, imm: the first instruction of a program

enum class ProgramError : u8 {
    None,
    NullAddress, ///< PGM_LO/HI hold 0
    OutOfRange,  ///< the program would leave the 40-bit GPU address space
    Unreadable,  ///< the reader refused a part of it
    NoFooter,    ///< no "OrbShdr" signature where the first instruction points nor in the first 64 KiB
    BadLength,   ///< the footer's length is 0, not a multiple of 4, or runs past the footer
    TooLarge,    ///< longer than kMaxProgramBytes
    Collision,   ///< another program's code with the same 128-bit hash (refused, never merged)
    Count,
};
std::string_view ProgramErrorName(ProgramError error);

/// A footer from its 28 bytes; false if they do not start with "OrbShdr".
bool ParseFooter(const u8* bytes, ProgramFooter& out);

/// The footer of the program at `addr`. `scan`: a buffer the caller keeps (the 64 KiB search reuses it).
ProgramError FindFooter(GuestReader& memory, u64 addr, ProgramFooter& out, std::vector<u32>& scan);

/// 128-bit content hash of code.
struct ContentHash {
    u64 lo = 0;
    u64 hi = 0;
    bool operator==(const ContentHash&) const = default;
};
ContentHash HashCode(std::span<const u32> code);

/// The settings of SPI_SHADER_PGM_RSRC1/RSRC2 (COMPUTE_PGM_RSRC1/RSRC2 for compute) a translation depends on.
struct ProgramSettings {
    u32 vgpr_blocks = 0;      ///< RSRC1.VGPRS: (allocated VGPRs / 4) - 1
    u32 sgpr_blocks = 0;      ///< RSRC1.SGPRS
    u32 float_round32 = 0, float_round64 = 0, float_denorm32 = 0, float_denorm64 = 0;
    bool dx10_clamp = false;
    bool ieee = false;
    u32 vgpr_comp_cnt = 0;    ///< graphics: input VGPRs the hardware initialises (RSRC1 bits 24-25)
    bool scratch = false;     ///< RSRC2.SCRATCH_EN
    u32 user_sgprs = 0;       ///< RSRC2.USER_SGPR: user data registers loaded into s0..s(n-1)
    bool oc_lds = false;      ///< graphics: RSRC2.OC_LDS_EN (hull/vertex for tessellation)
    u32 tgid_enable = 0;      ///< compute: RSRC2.TGID_X/Y/Z_EN
    u32 lds_blocks = 0;       ///< compute: RSRC2.LDS_SIZE (128-dword blocks)
};
ProgramSettings DecodeSettings(HwStage stage, u32 rsrc1, u32 rsrc2);

/// The bits of RSRC1/RSRC2 that DecodeSettings reads (everything else — priority, debug mode, CU limits — does not
/// change a translation and is left out of keys).
u32 Rsrc1TranslationMask(HwStage stage);
u32 Rsrc2TranslationMask(HwStage stage);

/// A program read from guest memory: its code (a copy), footer and identity.
struct ProgramImage {
    u64 address = 0;
    HwStage stage = HwStage::Vs;
    ProgramFooter footer;
    std::vector<u32> code; ///< footer.length / 4 dwords
    ContentHash hash;
};

/// Reads the program at `addr`: footer, code, hash. `scan`: as FindFooter.
ProgramError ReadProgram(GuestReader& memory, u64 addr, HwStage stage, ProgramImage& out, std::vector<u32>& scan);

/// The identity of a program for the caches: what it is, not where it is.
struct ProgramIdentity {
    ContentHash hash;
    u32 length = 0; ///< bytes
    HwStage stage = HwStage::Vs;
    bool operator==(const ProgramIdentity&) const = default;
};

struct ProgramIdentityHash {
    size_t operator()(const ProgramIdentity& id) const noexcept {
        return static_cast<size_t>(id.hash.lo ^ (id.hash.hi * 0x9E3779B97F4A7C15ull) ^ id.length ^
                                   (u64{static_cast<u8>(id.stage)} << 56));
    }
};

} // namespace SecondSon::Gpu::Shader
