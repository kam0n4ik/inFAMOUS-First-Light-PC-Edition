// SecondSonPC — reading a context's draw command buffer for the render-pass trace (see dcb_scan.h).
#include "engine/render/dcb_scan.h"

#include <algorithm>

#include "runtime/gpu/pm4.h"

namespace SecondSon::Engine::Render {

namespace {

namespace Op = Gpu::Pm4::Op;

template <typename T>
void AddUnique(std::vector<T>& list, const T& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

/// SPI_SHADER_PGM_LO_* (SH register offsets from 0x2C00); the HI register follows each.
constexpr u32 kPgmLoPs = 0x008;
constexpr u32 kPgmLoVs = 0x048;
constexpr u32 kPgmLoGs = 0x088;
constexpr u32 kPgmLoEs = 0x0C8;
constexpr u32 kPgmLoHs = 0x108;
constexpr u32 kPgmLoLs = 0x148;
constexpr u32 kComputePgmLo = 0x20C; ///< COMPUTE_PGM_LO (0x2E0C)

void SetProgramWord(u64& program, bool hi, u32 value) {
    // The register holds the address >> 8: LO bits 0..31, HI bits 32..39.
    u64 shifted = program >> 8;
    if (hi) {
        shifted = (shifted & 0xFFFFFFFFull) | (u64{value & 0xFF} << 32);
    } else {
        shifted = (shifted & ~0xFFFFFFFFull) | value;
    }
    program = shifted << 8;
}

void WriteSh(ScanState& s, u32 reg, u32 value) {
    const auto program = [&](u32 lo, u64& field) {
        if (reg == lo || reg == lo + 1) {
            SetProgramWord(field, reg == lo + 1, value);
        }
    };
    program(kPgmLoPs, s.ps);
    program(kPgmLoVs, s.vs);
    program(kPgmLoGs, s.gs);
    program(kPgmLoEs, s.es);
    program(kPgmLoHs, s.hs);
    program(kPgmLoLs, s.ls);
    program(kComputePgmLo, s.cs);
}

void RecordDraw(const ScanState& s, ScanResult& r) {
    const u32* c = s.context;
    const u32 mask = c[Reg::CbTargetMask];
    const u32 mode = (c[Reg::CbColorControl] >> 4) & 0x7; // CB_COLOR_CONTROL.MODE: 0 = colour disabled
    bool color = false;
    if (mode != 0) {
        for (u32 i = 0; i < 8; ++i) {
            if (((mask >> (4 * i)) & 0xF) == 0) {
                continue;
            }
            const u32* t = c + Reg::CbColor0Base + i * Reg::CbColorStride;
            const u32 info = t[4];
            const u32 format = (info >> 2) & 0x1F;
            if (t[0] == 0 || format == 0) {
                continue;
            }
            const u32 width = ((t[1] & 0x7FF) + 1) * 8;
            const u32 height = static_cast<u32>((u64{(t[2] & 0x3FFFFF)} + 1) * 64 / width);
            AddUnique(r.targets, TargetDesc{u64{t[0]} << 8, width, height, format | ((info >> 8) & 0x7) << 8, false});
            color = true;
        }
    }
    const u32 zinfo = c[Reg::DbZInfo];
    const u32 control = c[Reg::DbDepthControl];
    const bool test = (zinfo & 0x3) != 0 && (control & 0x2) != 0;
    const bool write = test && (control & 0x4) != 0;
    if (test) {
        const u32 size = c[Reg::DbDepthSize];
        const TargetDesc depth{u64{write ? c[Reg::DbZWriteBase] : c[Reg::DbZReadBase]} << 8, ((size & 0x7FF) + 1) * 8,
                               (((size >> 11) & 0x7FF) + 1) * 8, zinfo & 0x3, true};
        ++r.draws_depth_test;
        if (write) {
            ++r.draws_depth_write;
            AddUnique(r.targets, depth);
        } else {
            AddUnique(r.depth_read, depth);
        }
    }
    if (!color) {
        ++r.draws_no_color;
    }
    for (const u64 program : {s.vs, s.ps}) {
        if (program != 0) {
            AddUnique(r.shaders, program);
        }
    }
}

} // namespace

void ScanCommands(std::span<const u32> stream, ScanState& s, ScanResult& r) {
    r.dwords += static_cast<u32>(stream.size());
    while (!stream.empty()) {
        Gpu::Pm4::Packet p;
        Gpu::Pm4::DecodeError error{};
        if (!Gpu::Pm4::Decode(stream, p, error)) {
            ++r.malformed;
            return;
        }
        ++r.packets;
        const std::span<const u32> body = stream.subspan(1, p.dwords - 1);
        stream = stream.subspan(p.dwords);
        if (p.type != 3) {
            continue;
        }
        switch (p.opcode) {
        case Op::SetContextReg:
            if (!body.empty()) {
                const u32 first = body[0] & 0xFFFF;
                for (u32 k = 1; k < body.size() && first + k - 1 < 0x400; ++k) {
                    s.context[first + k - 1] = body[k];
                }
            }
            break;
        case Op::SetShReg:
            if (!body.empty()) {
                const u32 first = body[0] & 0xFFFF;
                for (u32 k = 1; k < body.size(); ++k) {
                    WriteSh(s, first + k - 1, body[k]);
                }
            }
            break;
        case Op::NumInstances:
            if (!body.empty()) {
                s.num_instances = std::max(body[0], 1u);
            }
            break;
        case Op::DrawIndex2:
        case Op::DrawIndexOffset2:
        case Op::DrawIndexAuto: {
            const size_t count_at = p.opcode == Op::DrawIndex2 ? 3 : p.opcode == Op::DrawIndexOffset2 ? 2 : 0;
            if (count_at < body.size()) {
                r.indices += u64{body[count_at]} * s.num_instances;
            }
            ++r.draws;
            if (s.num_instances > 1) {
                ++r.draws_instanced;
            }
            RecordDraw(s, r);
            break;
        }
        case Op::DrawIndirect:
        case Op::DrawIndexIndirect:
        case Op::DrawIndirectMulti:
        case Op::DrawIndexIndirectMulti:
        case Op::DrawIndexIndirectCountMulti:
            ++r.draws_indirect;
            RecordDraw(s, r);
            break;
        case Op::DispatchDirect:
        case Op::DispatchIndirect:
            ++r.dispatches;
            if (s.cs != 0) {
                AddUnique(r.shaders, s.cs);
            }
            break;
        case Op::EventWrite:
        case Op::EventWriteEop:
        case Op::EventWriteEos:
        case Op::ReleaseMem:
            ++r.events;
            break;
        case Op::AcquireMem:
        case Op::SurfaceSync:
            ++r.cache_actions;
            break;
        case Op::WaitRegMem:
            ++r.waits;
            break;
        case Op::DmaData:
        case Op::CpDma:
        case Op::WriteData:
            ++r.dma;
            break;
        case Op::IndirectBuffer:
        case Op::IndirectBufferConst:
            ++r.indirect_buffers;
            break;
        default:
            break;
        }
    }
}

} // namespace SecondSon::Engine::Render
