// SecondSonPC — the game's GCN shader programs (P4.3). See gcn_program.h.
#include "runtime/gpu/shader/gcn_program.h"

#include <algorithm>
#include <cstring>

#include <xxhash.h>

namespace SecondSon::Gpu::Shader {

namespace {

constexpr char kSignature[7] = {'O', 'r', 'b', 'S', 'h', 'd', 'r'};
constexpr u64 kGpuSpace = 1ull << 40;

bool InGpuSpace(u64 addr, u64 bytes) {
    return addr < kGpuSpace && bytes <= kGpuSpace - addr;
}

} // namespace

std::string_view HwStageName(HwStage stage) {
    static constexpr std::string_view kNames[] = {"ps", "vs", "gs", "es", "hs", "ls", "cs"};
    const auto i = static_cast<size_t>(stage);
    return i < std::size(kNames) ? kNames[i] : "?";
}

std::string_view SwStageName(SwStage stage) {
    static constexpr std::string_view kNames[] = {"fragment", "tess-control", "tess-eval", "vertex", "geometry",
                                                  "compute"};
    const auto i = static_cast<size_t>(stage);
    return i < std::size(kNames) ? kNames[i] : "?";
}

std::string_view ProgramErrorName(ProgramError error) {
    static constexpr std::string_view kNames[] = {"none",        "null address", "outside the GPU address space",
                                                  "unreadable",  "no footer",    "bad length",
                                                  "too large",   "hash collision"};
    static_assert(std::size(kNames) == static_cast<size_t>(ProgramError::Count));
    const auto i = static_cast<size_t>(error);
    return i < std::size(kNames) ? kNames[i] : "?";
}

bool ParseFooter(const u8* b, ProgramFooter& out) {
    if (!std::equal(std::begin(kSignature), std::end(kSignature), b)) {
        return false;
    }
    out = {};
    out.version = b[7];
    u32 bits = 0;
    std::memcpy(&bits, b + 8, 4);
    out.pssl_or_cg = (bits & 1) != 0;
    out.cached = ((bits >> 1) & 1) != 0;
    out.type = static_cast<u8>((bits >> 2) & 0xF);
    out.source_type = static_cast<u8>((bits >> 6) & 0x3);
    out.length = bits >> 8;
    out.usage_base_dw = b[12];
    out.num_usage_slots = b[13];
    out.is_srt = (b[14] & 1) != 0;
    out.srt_used_info_valid = (b[14] & 2) != 0;
    out.extended_usage = (b[14] & 4) != 0;
    std::memcpy(&out.shader_hash, b + 16, 8);
    std::memcpy(&out.crc32, b + 24, 4);
    return true;
}

ProgramError FindFooter(GuestReader& memory, u64 addr, ProgramFooter& out, std::vector<u32>& scan) {
    if (addr == 0) {
        return ProgramError::NullAddress;
    }
    if (!InGpuSpace(addr, 8)) {
        return ProgramError::OutOfRange;
    }
    u32 head[2] = {};
    if (!memory.Read(addr, head, sizeof(head))) {
        return ProgramError::Unreadable;
    }
    if (head[0] == kTokenMovVcchi) {
        // The first instruction's immediate: the footer is (imm + 1) qwords from the start (the bridge's reading).
        const u64 offset = (u64{head[1]} + 1) * 8;
        u8 bytes[kFooterBytes] = {};
        if (offset < kMaxProgramBytes + 8 && InGpuSpace(addr + offset, kFooterBytes) &&
            memory.Read(addr + offset, bytes, sizeof(bytes)) && ParseFooter(bytes, out)) {
            out.offset = static_cast<u32>(offset);
            out.by_token = true;
            return ProgramError::None;
        }
    }
    // The bridge's fallback: the first dword-aligned signature in the first 64 KiB (the footer may end past it).
    const u64 span = kFooterSearchBytes + kFooterBytes;
    if (!InGpuSpace(addr, span)) {
        return ProgramError::OutOfRange;
    }
    scan.resize(span / 4);
    if (!memory.Read(addr, scan.data(), span)) {
        return ProgramError::Unreadable;
    }
    const auto* bytes = reinterpret_cast<const u8*>(scan.data());
    for (u32 off = 0; off < kFooterSearchBytes; off += 4) {
        if (bytes[off] == 'O' && ParseFooter(bytes + off, out)) {
            out.offset = off;
            out.by_token = false;
            return ProgramError::None;
        }
    }
    return ProgramError::NoFooter;
}

ContentHash HashCode(std::span<const u32> code) {
    const XXH128_hash_t h = XXH3_128bits(code.data(), code.size_bytes());
    return {h.low64, h.high64};
}

ProgramSettings DecodeSettings(HwStage stage, u32 rsrc1, u32 rsrc2) {
    ProgramSettings s;
    s.vgpr_blocks = rsrc1 & 0x3F;
    s.sgpr_blocks = (rsrc1 >> 6) & 0xF;
    s.float_round32 = (rsrc1 >> 12) & 3;
    s.float_round64 = (rsrc1 >> 14) & 3;
    s.float_denorm32 = (rsrc1 >> 16) & 3;
    s.float_denorm64 = (rsrc1 >> 18) & 3;
    s.dx10_clamp = ((rsrc1 >> 21) & 1) != 0;
    s.ieee = ((rsrc1 >> 23) & 1) != 0;
    s.scratch = (rsrc2 & 1) != 0;
    s.user_sgprs = (rsrc2 >> 1) & 0x1F;
    if (stage == HwStage::Cs) {
        s.tgid_enable = (rsrc2 >> 7) & 7;
        s.lds_blocks = (rsrc2 >> 15) & 0x1FF;
    } else {
        s.vgpr_comp_cnt = (rsrc1 >> 24) & 3;
        s.oc_lds = ((rsrc2 >> 7) & 1) != 0;
    }
    return s;
}

u32 Rsrc1TranslationMask(HwStage stage) {
    // VGPRS (allocated VGPRs), the float modes, DX10_CLAMP; graphics: VGPR_COMP_CNT
    const u32 common = 0x3Fu | (0xFFu << 12) | (1u << 21);
    return stage == HwStage::Cs ? common : common | (3u << 24);
}

u32 Rsrc2TranslationMask(HwStage stage) {
    // USER_SGPR; compute: TGID_X/Y/Z_EN and LDS_SIZE; hull: OC_LDS_EN
    const u32 user = 0x1Fu << 1;
    switch (stage) {
    case HwStage::Cs:
        return user | (7u << 7) | (0x1FFu << 15);
    case HwStage::Hs:
        return user | (1u << 7);
    default:
        return user;
    }
}

ProgramError ReadProgram(GuestReader& memory, u64 addr, HwStage stage, ProgramImage& out, std::vector<u32>& scan) {
    out.address = addr;
    out.stage = stage;
    out.code.clear();
    out.hash = {};
    const ProgramError e = FindFooter(memory, addr, out.footer, scan);
    if (e != ProgramError::None) {
        return e;
    }
    const u32 length = out.footer.length;
    if (length == 0 || length % 4 != 0) {
        return ProgramError::BadLength;
    }
    if (length > kMaxProgramBytes) {
        return ProgramError::TooLarge;
    }
    if (!InGpuSpace(addr, length)) {
        return ProgramError::OutOfRange;
    }
    out.code.resize(length / 4);
    if (!memory.Read(addr, out.code.data(), length)) {
        out.code.clear();
        return ProgramError::Unreadable;
    }
    out.hash = HashCode(out.code);
    return ProgramError::None;
}

} // namespace SecondSon::Gpu::Shader
