// SecondSonPC — the GCN (Sea Islands) instruction decoder (gcn_decoder.h).
#include "runtime/gpu/gcn/gcn_decoder.h"

#include <array>
#include <unordered_map>

#include <fmt/format.h>

namespace SecondSon::Gpu::Gcn {

namespace {

struct NameRow {
    Format format;
    u32 op;
    const char* name;
};
constexpr NameRow kNames[] = {
#include "runtime/gpu/gcn/gcn_opcode_names.inc"
};

const std::unordered_map<u32, std::string_view>& NameMap() {
    static const auto map = [] {
        std::unordered_map<u32, std::string_view> m;
        for (const NameRow& r : kNames) {
            m.emplace((static_cast<u32>(r.format) << 16) | r.op, r.name);
        }
        return m;
    }();
    return map;
}

constexpr u32 kLiteral = 255;
constexpr u32 kSopEndpgm = 1;

u32 Bits(u32 w, u32 lo, u32 n) {
    return (w >> lo) & ((1u << n) - 1);
}

} // namespace

std::string_view FormatName(Format f) {
    static constexpr std::array<std::string_view, static_cast<size_t>(Format::Count)> kFormatNames = {
        "SOP2", "SOPK", "SOP1", "SOPC", "SOPP", "SMRD", "VOP2", "VOP3", "VOP1", "VOPC",
        "VINTRP", "DS", "MUBUF", "MTBUF", "MIMG", "EXP", "?"};
    return kFormatNames[static_cast<size_t>(f)];
}

std::string_view Mnemonic(Format f, u32 op) {
    const auto& m = NameMap();
    const auto it = m.find((static_cast<u32>(f) << 16) | op);
    return it != m.end() ? it->second : std::string_view{};
}

bool Decode(std::span<const u32> code, std::vector<Inst>& out, std::string& error) {
    size_t i = 0;
    while (i < code.size()) {
        Inst in;
        in.offset = static_cast<u32>(i);
        const u32 w = code[i];
        in.w0 = w;
        bool wide = false;
        bool lit = false;
        if ((w >> 23) == 0x17D) { // SOP1
            in.format = Format::Sop1;
            in.op = static_cast<u16>(Bits(w, 8, 8));
            lit = Bits(w, 0, 8) == kLiteral;
        } else if ((w >> 23) == 0x17E) { // SOPC
            in.format = Format::Sopc;
            in.op = static_cast<u16>(Bits(w, 16, 7));
            lit = Bits(w, 0, 8) == kLiteral || Bits(w, 8, 8) == kLiteral;
        } else if ((w >> 23) == 0x17F) { // SOPP
            in.format = Format::Sopp;
            in.op = static_cast<u16>(Bits(w, 16, 7));
        } else if ((w >> 28) == 0xB) { // SOPK
            in.format = Format::Sopk;
            in.op = static_cast<u16>(Bits(w, 23, 5));
            lit = in.op == 21; // s_setreg_imm32_b32
        } else if ((w >> 30) == 0x2) { // SOP2
            in.format = Format::Sop2;
            in.op = static_cast<u16>(Bits(w, 23, 7));
            lit = Bits(w, 0, 8) == kLiteral || Bits(w, 8, 8) == kLiteral;
        } else if ((w >> 27) == 0x18) { // SMRD
            in.format = Format::Smrd;
            in.op = static_cast<u16>(Bits(w, 22, 5));
            lit = Bits(w, 8, 1) == 0 && Bits(w, 0, 8) == kLiteral; // CI: a 32-bit literal offset
        } else if ((w >> 31) == 0) {
            if ((w >> 25) == 0x3F) { // VOP1
                in.format = Format::Vop1;
                in.op = static_cast<u16>(Bits(w, 9, 8));
            } else if ((w >> 25) == 0x3E) { // VOPC
                in.format = Format::Vopc;
                in.op = static_cast<u16>(Bits(w, 17, 8));
            } else { // VOP2
                in.format = Format::Vop2;
                in.op = static_cast<u16>(Bits(w, 25, 6));
            }
            lit = Bits(w, 0, 9) == kLiteral || (in.format == Format::Vop2 && (in.op == 32 || in.op == 33));
        } else {
            switch (w >> 26) {
            case 0x32:
                in.format = Format::Vintrp;
                in.op = static_cast<u16>(Bits(w, 16, 2));
                break;
            case 0x34:
                in.format = Format::Vop3;
                in.op = static_cast<u16>(Bits(w, 17, 9));
                wide = true;
                break;
            case 0x36:
                in.format = Format::Ds;
                in.op = static_cast<u16>(Bits(w, 18, 8));
                wide = true;
                break;
            case 0x38:
                in.format = Format::Mubuf;
                in.op = static_cast<u16>(Bits(w, 18, 7));
                wide = true;
                break;
            case 0x3A:
                in.format = Format::Mtbuf;
                in.op = static_cast<u16>(Bits(w, 16, 3));
                wide = true;
                break;
            case 0x3C:
                in.format = Format::Mimg;
                in.op = static_cast<u16>(Bits(w, 18, 7));
                wide = true;
                break;
            case 0x3E:
                in.format = Format::Exp;
                in.op = 0;
                wide = true;
                break;
            default:
                error = fmt::format("dword {}: {:#010x} is no GCN encoding", i, w);
                return false;
            }
        }
        in.dwords = static_cast<u8>(1 + (wide ? 1 : 0) + (lit ? 1 : 0));
        if (i + in.dwords > code.size()) {
            error = fmt::format("dword {}: {} {} runs past the code ({} dwords)", i, FormatName(in.format), in.op,
                                code.size());
            return false;
        }
        if (wide) {
            in.w1 = code[i + 1];
        }
        if (lit) {
            in.literal = true;
            in.lit = code[i + (wide ? 2 : 1)];
        }
        out.push_back(in);
        i += in.dwords;
        if (in.format == Format::Sopp && in.op == kSopEndpgm) {
            break;
        }
    }
    return true;
}

} // namespace SecondSon::Gpu::Gcn
