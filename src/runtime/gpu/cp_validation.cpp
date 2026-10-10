// SecondSonPC — validation of the command processor against the bridge's (P4.2). See cp_validation.h.
#include "runtime/gpu/cp_validation.h"

#include <cstring>
#include <iterator>

namespace SecondSon::Gpu::Cp {

namespace {

u32 FlatReg(u32 reg) {
    u32 i = 0;
    FlatIndex(reg, i);
    return i;
}

constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;
constexpr u64 kFnvPrime = 0x100000001b3ull;

void Put(std::vector<u8>& b, const void* p, size_t n) {
    const auto* c = static_cast<const u8*>(p);
    b.insert(b.end(), c, c + n);
}
template <typename T>
void Put(std::vector<u8>& b, const T& v) {
    Put(b, &v, sizeof(v));
}

} // namespace

BridgeMirror::BridgeMirror(CommandProcessor& cp) : cp_(cp) {
    std::copy(cp.Registers().Values().begin(), cp.Registers().Values().end(), mirror_.begin());
    cp.MutableRegisters().SetMirror(mirror_.data());
    cp.SetObserver(this);
}

BridgeMirror::~BridgeMirror() {
    cp_.MutableRegisters().SetMirror(nullptr);
    cp_.SetObserver(nullptr);
}

u32 BridgeMirror::Get(u32 reg) const {
    u32 i = 0;
    return FlatIndex(reg, i) ? mirror_[i] : 0;
}

void BridgeMirror::Resync(u32 reg, u32 value) {
    cp_.MutableRegisters().WriteReg(reg, value); // the mirror follows the write
}

void BridgeMirror::Load(const u32* flat) {
    cp_.MutableRegisters().Load(std::span<const u32, kNumRegs>(flat, kNumRegs)); // and the mirror
}

void BridgeMirror::BeforePacket(u32, u64, std::span<const u32> packet) {
    if ((packet[0] >> 30) != 3) {
        return;
    }
    switch (static_cast<u8>(packet[0] >> 8)) {
    case Pm4::Op::IndexBufferSize:
        saved_dma_size_ = mirror_[FlatReg(Reg::VgtDmaSize)];
        break;
    case Pm4::Op::DrawIndirect:
    case Pm4::Op::DrawIndexIndirect:
        saved_initiator_ = mirror_[FlatReg(Reg::VgtDrawInitiator)];
        break;
    default:
        break;
    }
}

void BridgeMirror::AfterPacket(u32, u64, std::span<const u32> packet) {
    if ((packet[0] >> 30) != 3) {
        return;
    }
    switch (static_cast<u8>(packet[0] >> 8)) {
    case Pm4::Op::IndexBufferSize: // the bridge keeps the size apart; its VGT_DMA_SIZE register stays
        mirror_[FlatReg(Reg::VgtDmaSize)] = saved_dma_size_;
        break;
    case Pm4::Op::DrawIndirect: // the bridge does not write the initiator of an indirect draw
    case Pm4::Op::DrawIndexIndirect:
        mirror_[FlatReg(Reg::VgtDrawInitiator)] = saved_initiator_;
        break;
    case Pm4::Op::ClearState: { // the bridge clears every register, not only the context
        const u32 ctx = kSpaceFirst[static_cast<u32>(Space::Context)];
        const u32 ucfg = kSpaceFirst[static_cast<u32>(Space::Uconfig)];
        std::fill(mirror_.begin(), mirror_.begin() + ctx, 0u);
        std::fill(mirror_.begin() + ucfg, mirror_.end(), 0u);
        break;
    }
    default:
        break;
    }
}

u64 HashBridgeRegs(const u32* regs, std::span<const RegRange> ranges) {
    u64 h = kFnvBasis;
    for (const auto& r : ranges) {
        for (u32 i = 0; i < r.count; ++i) {
            h = (h ^ regs[r.first + i]) * kFnvPrime;
        }
    }
    return h;
}

u64 HashFlatRegs(const u32* flat, std::span<const RegRange> ranges) {
    u64 h = kFnvBasis;
    for (const auto& r : ranges) {
        const u32* p = flat + FlatReg(r.first);
        for (u32 i = 0; i < r.count; ++i) {
            h = (h ^ p[i]) * kFnvPrime;
        }
    }
    return h;
}

u64 HashCompute(const u32* cs) {
    u64 h = kFnvBasis;
    for (u32 i = 4; i < kComputeCount; ++i) {
        h = (h ^ cs[i]) * kFnvPrime;
    }
    return h;
}

void BridgeToFlat(const u32* regs, u32* flat) {
    for (const auto& r : kAllRanges) {
        std::memcpy(flat + FlatReg(r.first), regs + r.first, size_t{r.count} * sizeof(u32));
    }
}

u32 DiffRegs(const BridgeMirror& mirror, const u32* bridge_regs, std::span<const RegRange> ranges,
             std::vector<RegDiff>& out, u32 max) {
    u32 n = 0;
    const u32* flat = mirror.Flat();
    for (const auto& r : ranges) {
        const u32* ours = flat + FlatReg(r.first);
        const u32* theirs = bridge_regs + r.first;
        if (std::memcmp(ours, theirs, size_t{r.count} * sizeof(u32)) == 0) {
            continue;
        }
        for (u32 i = 0; i < r.count; ++i) {
            if (ours[i] != theirs[i]) {
                if (out.size() < max) {
                    out.push_back({r.first + i, ours[i], theirs[i]});
                }
                ++n;
            }
        }
    }
    return n;
}

u32 DiffFlat(const BridgeMirror& mirror, const u32* bridge_flat, std::span<const RegRange> ranges,
             std::vector<RegDiff>& out, u32 max) {
    u32 n = 0;
    for (const auto& r : ranges) {
        const u32 f = FlatReg(r.first);
        for (u32 i = 0; i < r.count; ++i) {
            const u32 ours = mirror.Flat()[f + i], theirs = bridge_flat[f + i];
            if (ours != theirs) {
                if (out.size() < max) {
                    out.push_back({r.first + i, ours, theirs});
                }
                ++n;
            }
        }
    }
    return n;
}

// --- trace files ---

bool TraceWriter::Open(const std::filesystem::path& path, const std::string& info) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        return false;
    }
    out_.write(kTraceMagic, sizeof(kTraceMagic));
    const u32 version = kTraceVersion, reserved = 0;
    out_.write(reinterpret_cast<const char*>(&version), 4);
    out_.write(reinterpret_cast<const char*>(&reserved), 4);
    bytes_ = 16;
    Record(TraceRecord::Info, info.data(), info.size());
    return static_cast<bool>(out_);
}

void TraceWriter::Record(TraceRecord type, const void* a, size_t na, const void* b, size_t nb) {
    if (!out_.is_open()) {
        return;
    }
    const u8 head[4] = {static_cast<u8>(type), 0, 0, 0};
    const u32 size = static_cast<u32>(na + nb);
    out_.write(reinterpret_cast<const char*>(head), 4);
    out_.write(reinterpret_cast<const char*>(&size), 4);
    if (na != 0) {
        out_.write(static_cast<const char*>(a), static_cast<std::streamsize>(na));
    }
    if (nb != 0) {
        out_.write(static_cast<const char*>(b), static_cast<std::streamsize>(nb));
    }
    bytes_ += 8 + size;
}

void TraceWriter::State(const TraceState& s) {
    std::vector<u8> b;
    Put(b, s.index_buffer_size);
    Put(b, s.indirect_args);
    Put(b, s.cb_extent.data(), sizeof(s.cb_extent));
    Put(b, s.db_extent);
    Record(TraceRecord::State, b.data(), b.size(), s.flat.data(), sizeof(s.flat));
}

void TraceWriter::Compute(u32 queue, const u32* cs) {
    Record(TraceRecord::Compute, &queue, 4, cs, size_t{kComputeCount} * 4);
}

void TraceWriter::Submission(u32 queue, bool ring, u64 addr, std::span<const u32> data) {
    std::vector<u8> b;
    Put(b, queue);
    Put(b, u32{ring ? 1u : 0u});
    Put(b, addr);
    Put(b, static_cast<u32>(data.size()));
    Record(TraceRecord::Submission, b.data(), b.size(), data.data(), data.size_bytes());
}

void TraceWriter::Memory(u64 addr, std::span<const u8> data) {
    std::vector<u8> b;
    Put(b, addr);
    Put(b, static_cast<u32>(data.size()));
    Record(TraceRecord::Memory, b.data(), b.size(), data.data(), data.size());
}

void TraceWriter::Checkpoint(const TraceCheckpoint& c) {
    std::vector<u8> b;
    b.reserve(96);
    Put(b, c.seq);
    Put(b, c.queue);
    Put(b, c.kind);
    Put(b, c.addr);
    Put(b, c.regs_hash);
    Put(b, c.cs_hash);
    Put(b, c.index_buffer_size);
    Put(b, c.reg_dma_size);
    Put(b, c.indirect_args);
    Put(b, c.cb_extent.data(), sizeof(c.cb_extent));
    Put(b, c.db_extent);
    Record(TraceRecord::Checkpoint, b.data(), b.size());
}

void TraceWriter::Full(u32 seq, const u32* flat) {
    Record(TraceRecord::Full, &seq, 4, flat, size_t{kNumRegs} * 4);
}

void TraceWriter::Flip(u32 frame) {
    Record(TraceRecord::Flip, &frame, 4);
}

void TraceWriter::Close(const std::string& summary) {
    if (!out_.is_open()) {
        return;
    }
    Record(TraceRecord::End, summary.data(), summary.size());
    out_.close();
}

bool TraceReader::Load(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data_.size() < 16 || std::memcmp(data_.data(), kTraceMagic, 8) != 0) {
        error = "not a SecondSonPC PM4 trace (magic)";
        return false;
    }
    u32 version = 0;
    std::memcpy(&version, data_.data() + 8, 4);
    if (version != kTraceVersion) {
        error = "trace version " + std::to_string(version) + ", expected " + std::to_string(kTraceVersion);
        return false;
    }
    items_.clear();
    size_t pos = 16;
    while (pos + 8 <= data_.size()) {
        const auto type = static_cast<TraceRecord>(data_[pos]);
        u32 size = 0;
        std::memcpy(&size, data_.data() + pos + 4, 4);
        if (size > data_.size() - pos - 8) {
            error = "truncated record at byte " + std::to_string(pos);
            return false;
        }
        items_.push_back({type, std::span<const u8>(data_.data() + pos + 8, size)});
        pos += 8 + size;
    }
    if (pos != data_.size()) {
        error = "trailing bytes after the last record";
        return false;
    }
    return true;
}

} // namespace SecondSon::Gpu::Cp
