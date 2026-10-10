// SecondSonPC — .eh_frame_hdr decoding (DWARF exception-handling pointer encodings).
#include "loader/eh_frame.h"

#include <algorithm>
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

namespace {

bool ReadUleb(u64& cursor, u64 end, u64& out) {
    out = 0;
    for (u32 shift = 0; cursor < end && shift < 64; shift += 7) {
        const u8 b = Get<u8>(cursor++);
        out |= u64{b & 0x7Fu} << shift;
        if ((b & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

/// Content [begin, end) of the CIE/FDE record at `record` (after its length field). false if malformed.
bool RecordBounds(u64 record, u64 limit, u64& begin, u64& end) {
    if (record + 4 > limit) {
        return false;
    }
    u64 length = Get<u32>(record);
    begin = record + 4;
    if (length == 0xFFFFFFFFu) {
        if (record + 12 > limit) {
            return false;
        }
        length = Get<u64>(record + 4);
        begin = record + 12;
    }
    if (length == 0 || length > limit - begin) {
        return false;
    }
    end = begin + length;
    return true;
}

/// The FDE pointer encoding of the CIE at `cie` ('R' augmentation; absptr without one).
bool CiePointerEncoding(u64 cie, u64 limit, u8& encoding, std::string& error) {
    u64 begin = 0, end = 0;
    if (!RecordBounds(cie, limit, begin, end) || end - begin < 5 || Get<u32>(begin) != 0) {
        error = fmt::format("malformed CIE at {:#x}", cie);
        return false;
    }
    u64 cursor = begin + 4;
    const u8 version = Get<u8>(cursor++);
    const u64 aug = cursor;
    while (cursor < end && Get<u8>(cursor) != 0) {
        ++cursor;
    }
    if (cursor >= end) {
        error = fmt::format("CIE at {:#x}: unterminated augmentation", cie);
        return false;
    }
    const u64 aug_end = cursor++;
    if (version >= 4) {
        cursor += 2; // address size, segment selector size
    }
    u64 ignored = 0;
    if (!ReadUleb(cursor, end, ignored) || !ReadUleb(cursor, end, ignored)) { // code / data alignment
        error = fmt::format("CIE at {:#x}: bad alignment fields", cie);
        return false;
    }
    if (version == 1) {
        ++cursor; // return address register
    } else if (!ReadUleb(cursor, end, ignored)) {
        error = fmt::format("CIE at {:#x}: bad return address register", cie);
        return false;
    }
    encoding = DW_EH_PE_absptr;
    if (aug == aug_end || Get<u8>(aug) != 'z') {
        return true; // no augmentation data: absolute pointers
    }
    if (!ReadUleb(cursor, end, ignored)) {
        error = fmt::format("CIE at {:#x}: bad augmentation length", cie);
        return false;
    }
    for (u64 a = aug + 1; a < aug_end; ++a) {
        switch (Get<u8>(a)) {
        case 'R':
            if (cursor >= end) {
                error = fmt::format("CIE at {:#x}: 'R' past the record", cie);
                return false;
            }
            encoding = Get<u8>(cursor);
            return true;
        case 'L':
            ++cursor; // LSDA pointer encoding
            break;
        case 'P': {
            if (cursor >= end) {
                error = fmt::format("CIE at {:#x}: 'P' past the record", cie);
                return false;
            }
            const u8 personality = Get<u8>(cursor++);
            const size_t size = EncodedSize(personality);
            if (size == 0) {
                error = fmt::format("CIE at {:#x}: personality encoding {:#x}", cie, personality);
                return false;
            }
            cursor += size;
            break;
        }
        case 'S':
        case 'B':
            break;
        default:
            error = fmt::format("CIE at {:#x}: unknown augmentation before 'R'", cie);
            return false;
        }
    }
    return true;
}

} // namespace

bool DecodeEhFrameFunctions(const EhFrameHeader& header, u64 hdr_end, u64 eh_frame_end,
                            std::vector<CodeRange>& out, std::string& error) {
    out.clear();
    if (header.fde_count == 0 || header.table_enc == DW_EH_PE_omit) {
        return true;
    }
    const size_t entry = EncodedSize(header.table_enc) * 2;
    if (entry == 0 || header.table > hdr_end || header.fde_count > (hdr_end - header.table) / entry) {
        error = "malformed .eh_frame_hdr search table";
        return false;
    }
    const u64 frame_begin = header.eh_frame_ptr;
    struct CachedCie {
        u64 address;
        u8 encoding;
    };
    std::vector<CachedCie> cies;
    out.reserve(header.fde_count);
    u64 cursor = header.table;
    for (u64 i = 0; i < header.fde_count; ++i) {
        u64 initial_location = 0;
        u64 fde = 0;
        if (!ReadEncoded(cursor, hdr_end, header.table_enc, header.datarel_base, initial_location, error) ||
            !ReadEncoded(cursor, hdr_end, header.table_enc, header.datarel_base, fde, error)) {
            return false;
        }
        u64 begin = 0, end = 0;
        if (fde < frame_begin || !RecordBounds(fde, eh_frame_end, begin, end) || end - begin < 4) {
            error = fmt::format("FDE {} at {:#x} is outside .eh_frame or malformed", i, fde);
            return false;
        }
        const u32 cie_offset = Get<u32>(begin);
        if (cie_offset == 0 || cie_offset > begin - frame_begin) {
            error = fmt::format("FDE at {:#x}: CIE pointer {:#x} outside .eh_frame", fde, cie_offset);
            return false;
        }
        const u64 cie = begin - cie_offset;
        u8 encoding = 0;
        const auto cached =
            std::find_if(cies.begin(), cies.end(), [cie](const CachedCie& c) { return c.address == cie; });
        if (cached != cies.end()) {
            encoding = cached->encoding;
        } else {
            if (!CiePointerEncoding(cie, eh_frame_end, encoding, error)) {
                return false;
            }
            cies.push_back({cie, encoding});
        }
        u64 pc = begin + 4;
        u64 pc_begin = 0, pc_range = 0;
        if (!ReadEncoded(pc, end, encoding, 0, pc_begin, error) ||
            !ReadEncoded(pc, end, static_cast<u8>(encoding & DW_EH_PE_format_mask), 0, pc_range, error)) {
            error = fmt::format("FDE at {:#x}: {}", fde, error);
            return false;
        }
        if (pc_begin != initial_location) {
            error = fmt::format("FDE at {:#x}: pc_begin {:#x} differs from the search table's {:#x}", fde, pc_begin,
                                initial_location);
            return false;
        }
        out.push_back({pc_begin, pc_begin + pc_range});
    }
    return true;
}

} // namespace SecondSon::Loader
