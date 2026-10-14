// SecondSonPC — native render operations (see native_render_ops.h).
#include "engine/render/native_render_ops.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

#include "engine/guest_hooks.h"

namespace SecondSon::Engine::Render {

namespace {

constexpr u32 kNopHeader = 0xC0001000u; ///< type 3, NOP; count field = body dwords - 1
constexpr u32 kPayload = kDrawMarkerDwords - 1;

enum Bits : u32 { kBitIndexed = 1, kBitShaders = 2, kBitPs = 4, kBitVsTable = 8, kBitPsTable = 16, kBitCanary = 32 };

} // namespace

u32 EncodeDrawMarker(const DrawOp& op, u16 flags, u32* out) {
    u32* p = out;
    *p++ = kNopHeader | ((kPayload - 1) << 16);
    *p++ = kMarkerMagic;
    *p++ = static_cast<u32>(OpKind::Draw) | (u32{flags} << 16);
    *p++ = static_cast<u32>(op.pass);
    *p++ = op.sub;
    *p++ = (op.indexed ? kBitIndexed : 0) | (op.shaders ? kBitShaders : 0) | (op.has_ps ? kBitPs : 0) |
           (op.table[kStageVs].present ? kBitVsTable : 0) | (op.table[kStagePs].present ? kBitPsTable : 0) | (op.canary ? kBitCanary : 0);
    *p++ = op.index_count;
    *p++ = static_cast<u32>(op.index_base);
    *p++ = static_cast<u32>(op.index_base >> 32);
    std::memcpy(p, op.vs, sizeof(op.vs));
    p += kVsRegs;
    std::memcpy(p, op.ps, sizeof(op.ps));
    p += kPsRegs;
    for (const StageTable& t : op.table) {
        *p++ = t.dwords;
        *p++ = static_cast<u32>(t.hash);
        *p++ = static_cast<u32>(t.hash >> 32);
        *p++ = t.refs;
        for (u32 i = 0; i < 7; ++i) {
            *p++ = i < t.refs ? (u32{t.ref[i].sgpr} | (u32{t.ref[i].offset} << 8)) : 0;
        }
    }
    return static_cast<u32>(p - out);
}

bool IsMarker(std::span<const u32> packet) {
    return packet.size() >= 3 && (packet[0] >> 30) == 3 && ((packet[0] >> 8) & 0xFF) == 0x10 &&
           packet[1] == kMarkerMagic;
}

bool DecodeDrawMarker(std::span<const u32> packet, DrawOp& op, u16& flags) {
    if (!IsMarker(packet) || packet.size() != kDrawMarkerDwords ||
        (packet[2] & 0xFFFF) != static_cast<u32>(OpKind::Draw)) {
        return false;
    }
    const u32* p = packet.data() + 2;
    flags = static_cast<u16>(*p++ >> 16);
    op.pass = static_cast<s32>(*p++);
    op.sub = *p++;
    const u32 bits = *p++;
    op.indexed = (bits & kBitIndexed) != 0;
    op.shaders = (bits & kBitShaders) != 0;
    op.has_ps = (bits & kBitPs) != 0;
    op.canary = (bits & kBitCanary) != 0;
    op.index_count = *p++;
    op.index_base = u64{p[0]} | (u64{p[1]} << 32);
    p += 2;
    std::memcpy(op.vs, p, sizeof(op.vs));
    p += kVsRegs;
    std::memcpy(op.ps, p, sizeof(op.ps));
    p += kPsRegs;
    for (u32 s = 0; s < kDrawStages; ++s) {
        StageTable& t = op.table[s];
        t.present = (bits & (s == kStageVs ? kBitVsTable : kBitPsTable)) != 0;
        t.dwords = *p++;
        t.hash = u64{p[0]} | (u64{p[1]} << 32);
        p += 2;
        t.refs = *p++;
        if (t.refs > 7 || t.dwords > 0x600) {
            return false;
        }
        for (u32 i = 0; i < 7; ++i) {
            const u32 v = *p++;
            t.ref[i] = TableRef{static_cast<u8>(v & 0xFF), static_cast<u16>(v >> 8)};
            if (i < t.refs && t.ref[i].sgpr > 14) {
                return false;
            }
        }
    }
    return true;
}

u32 EncodeRegistersMarker(std::span<const RegWrite> writes, u16 flags, u32* out, OpKind kind) {
    const u32 cap = kind == OpKind::State ? kMaxStateWrites : kMaxRegWrites;
    const u32 n = static_cast<u32>(std::min<size_t>(writes.size(), cap));
    const u32 total = RegistersMarkerDwords(n);
    u32* p = out;
    *p++ = kNopHeader | ((total - 2) << 16);
    *p++ = kMarkerMagic;
    *p++ = static_cast<u32>(kind) | (u32{flags} << 16);
    *p++ = n;
    for (u32 i = 0; i < n; ++i) {
        *p++ = writes[i].reg;
        *p++ = writes[i].value;
    }
    return total;
}

bool DecodeRegistersMarker(std::span<const u32> packet, RegWrite* out, u32& count, u16& flags, OpKind kind,
                           u32 capacity) {
    if (!IsMarker(packet) || packet.size() < 4 || MarkerKind(packet) != kind) {
        return false;
    }
    flags = static_cast<u16>(packet[2] >> 16);
    count = packet[3];
    if (count > capacity || packet.size() != RegistersMarkerDwords(count)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out[i] = RegWrite{packet[4 + 2 * i], packet[5 + 2 * i]};
    }
    return true;
}

namespace {

/// The table the user data point at: every reference must agree on the snapshot's start; its content must hash to
/// the engine's shadow.
bool TableMatches(const StageTable& t, const u32* user_data, HashGuestFn hash_guest) {
    u64 base = 0;
    for (u32 i = 0; i < t.refs; ++i) {
        const u32 sgpr = t.ref[i].sgpr;
        const u64 pointer = u64{user_data[sgpr]} | (u64{user_data[sgpr + 1] & 0xFFFF} << 32);
        const u64 start = pointer - u64{t.ref[i].offset} * 4;
        if (i == 0) {
            base = start;
        } else if (start != base) {
            return false;
        }
    }
    if (t.refs == 0 || t.dwords == 0) {
        return true; // nothing reaches the shader through a table
    }
    u64 hash = 0;
    return hash_guest(base, u64{t.dwords} * 4, hash) && hash == t.hash;
}

} // namespace

u32 VerifyDraw(const DrawOp& op, const DrawView& view, HashGuestFn hash_guest) {
    u32 m = 0;
    if (op.indexed != view.indexed) {
        m |= kMismatchKind;
    }
    if (op.index_count != view.index_count) {
        m |= kMismatchIndexCount;
    }
    if (op.indexed && op.index_base != view.index_base) {
        m |= kMismatchIndexBase;
    }
    if (op.shaders) {
        if (std::memcmp(op.vs, view.vs, sizeof(op.vs)) != 0) {
            m |= kMismatchVsRegs;
        }
        if (op.has_ps) {
            if (std::memcmp(op.ps, view.ps, sizeof(op.ps)) != 0) {
                m |= kMismatchPsRegs;
            }
        } else if (view.ps[0] != 0 || view.ps[1] != 0 || view.ps[10] != 0 || view.ps[11] != 0xF) {
            m |= kMismatchPsRegs; // no pixel shader: PGM 0, DB_SHADER_CONTROL 0, CB_SHADER_MASK 0xF
        }
    }
    for (u32 s = 0; s < kDrawStages; ++s) {
        const StageTable& t = op.table[s];
        if (t.present && view.user_data[s] != nullptr && !TableMatches(t, view.user_data[s], hash_guest)) {
            m |= s == kStageVs ? kMismatchVsTable : kMismatchPsTable;
        }
    }
    return m;
}

bool HashGuestMemory(u64 address, u64 bytes, u64& hash) {
    // One readable region remembered per thread: the snapshots of a frame sit in one arena.
    thread_local u64 known_start = 0;
    thread_local u64 known_end = 0;
    if (address == 0 || bytes > (1u << 20) || address + bytes < address) {
        return false;
    }
    u64 at = address;
    while (at < address + bytes) {
        if (at >= known_start && at < known_end) {
            at = known_end;
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
            return false;
        }
        known_start = reinterpret_cast<u64>(mbi.BaseAddress);
        known_end = known_start + mbi.RegionSize;
        at = known_end;
    }
    hash = HashBytes(reinterpret_cast<const void*>(address), bytes);
    return true;
}

u32 PassSlot(s32 pass) {
    if (pass >= 0 && pass <= 40) {
        return static_cast<u32>(pass);
    }
    switch (pass) {
    case -2:
        return 41;
    case -3:
        return 42;
    case -4:
        return 43;
    default:
        return 44;
    }
}

DrawCounters& Counters() {
    static DrawCounters counters;
    return counters;
}

NativeControl& Control() {
    static NativeControl control;
    return control;
}

} // namespace SecondSon::Engine::Render
