// SecondSonPC — .eh_frame_hdr decoding (DWARF exception-handling pointer encodings).
#include "loader/eh_frame.h"

#include <cstring>

#include <fmt/format.h>

namespace SecondSon::Loader {

namespace {

constexpr u8 DW_EH_PE_omit = 0xFF;
constexpr u8 DW_EH_PE_format_mask = 0x0F;
constexpr u8 DW_EH_PE_application_mask = 0x70;
constexpr u8 DW_EH_PE_indirect = 0x80;

constexpr u8 DW_EH_PE_ptr = 0x00;
constexpr u8 DW_EH_PE_udata2 = 0x02;
constexpr u8 DW_EH_PE_udata4 = 0x03;
constexpr u8 DW_EH_PE_udata8 = 0x04;
constexpr u8 DW_EH_PE_sdata2 = 0x0A;
constexpr u8 DW_EH_PE_sdata4 = 0x0B;
constexpr u8 DW_EH_PE_sdata8 = 0x0C;

constexpr u8 DW_EH_PE_absptr = 0x00;
constexpr u8 DW_EH_PE_pcrel = 0x10;
constexpr u8 DW_EH_PE_datarel = 0x30;

template <typename T>
T Get(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

size_t EncodedSize(u8 encoding) {
    switch (encoding & DW_EH_PE_format_mask) {
    case DW_EH_PE_ptr:
        return 8;
    case DW_EH_PE_udata2:
    case DW_EH_PE_sdata2:
        return 2;
    case DW_EH_PE_udata4:
    case DW_EH_PE_sdata4:
        return 4;
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8:
        return 8;
    default:
        return 0;
    }
}

/// Reads one encoded pointer at `cursor` (advanced past it).
bool ReadEncoded(u64& cursor, u64 end, u8 encoding, u64 datarel_base, u64& out, std::string& error) {
    const u64 start = cursor;
    const size_t size = EncodedSize(encoding);
    if (size == 0) {
        error = fmt::format("unsupported pointer encoding {:#x}", encoding);
        return false;
    }
    if (cursor + size > end) {
        error = fmt::format("encoded pointer at {:#x} runs past {:#x}", cursor, end);
        return false;
    }
    u64 value = 0;
    switch (encoding & DW_EH_PE_format_mask) {
    case DW_EH_PE_ptr:
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8:
        value = Get<u64>(cursor);
        break;
    case DW_EH_PE_udata2:
        value = Get<u16>(cursor);
        break;
    case DW_EH_PE_udata4:
        value = Get<u32>(cursor);
        break;
    case DW_EH_PE_sdata2:
        value = static_cast<u64>(static_cast<s64>(Get<s16>(cursor)));
        break;
    case DW_EH_PE_sdata4:
        value = static_cast<u64>(static_cast<s64>(Get<s32>(cursor)));
        break;
    }
    cursor += size;

    switch (encoding & DW_EH_PE_application_mask) {
    case DW_EH_PE_absptr:
        break;
    case DW_EH_PE_pcrel:
        value += start;
        break;
    case DW_EH_PE_datarel:
        if (datarel_base == 0) {
            error = "datarel pointer without a base";
            return false;
        }
        value += datarel_base;
        break;
    default:
        error = fmt::format("unsupported pointer application {:#x}", encoding);
        return false;
    }
    if ((encoding & DW_EH_PE_indirect) != 0) {
        value = Get<u64>(value);
    }
    out = value;
    return true;
}

} // namespace

bool DecodeEhFrameHeader(u64 start, u64 end, EhFrameHeader& out, std::string& error) {
    if (end - start < 4) {
        error = end == start ? "" : fmt::format(".eh_frame_hdr at {:#x} is too small", start);
        return false;
    }
    u64 cursor = start;
    const u8 version = Get<u8>(cursor++);
    if (version != 1) {
        error = fmt::format("unsupported .eh_frame_hdr version {} at {:#x}", version, start);
        return false;
    }
    const u8 eh_frame_ptr_enc = Get<u8>(cursor++);
    const u8 fde_count_enc = Get<u8>(cursor++);
    out.table_enc = Get<u8>(cursor++);
    if (!ReadEncoded(cursor, end, eh_frame_ptr_enc, start, out.eh_frame_ptr, error)) {
        return false;
    }
    out.fde_count = 0;
    if (fde_count_enc != DW_EH_PE_omit &&
        !ReadEncoded(cursor, end, fde_count_enc, start, out.fde_count, error)) {
        return false;
    }
    out.table = cursor;
    out.datarel_base = start;
    return true;
}

bool DecodeEhFrameTable(const EhFrameHeader& header, u64 end, std::vector<uintptr_t>& starts,
                        std::string& error) {
    starts.clear();
    if (header.fde_count == 0 || header.table_enc == DW_EH_PE_omit) {
        return true;
    }
    const size_t entry = EncodedSize(header.table_enc) * 2;
    if (entry == 0 || header.table > end || header.fde_count > (end - header.table) / entry) {
        error = "malformed .eh_frame_hdr search table";
        return false;
    }
    starts.reserve(header.fde_count);
    u64 cursor = header.table;
    for (u64 i = 0; i < header.fde_count; ++i) {
        u64 initial_location = 0;
        u64 fde = 0;
        if (!ReadEncoded(cursor, end, header.table_enc, header.datarel_base, initial_location, error) ||
            !ReadEncoded(cursor, end, header.table_enc, header.datarel_base, fde, error)) {
            return false;
        }
        starts.push_back(static_cast<uintptr_t>(initial_location));
    }
    return true;
}

} // namespace SecondSon::Loader
