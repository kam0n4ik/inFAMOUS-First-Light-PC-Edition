// SecondSonPC — .eh_frame_hdr decoding (DWARF exception-handling pointer encodings).
// Used for the module info returned to the game's unwinder and for the function boundaries the
// (temporary, Phase 0-2 bridge) static CPU patcher needs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Loader {

struct EhFrameHeader {
    u64 eh_frame_ptr = 0; // absolute address of .eh_frame
    u64 fde_count = 0;
    u8 table_enc = 0xFF;
    u64 table = 0;        // absolute address of the binary search table
    u64 datarel_base = 0; // start of .eh_frame_hdr
};

/// Decodes the header at [start, end) (already mapped guest memory).
bool DecodeEhFrameHeader(u64 start, u64 end, EhFrameHeader& out, std::string& error);

/// Initial locations of every FDE in the search table (function starts, table order).
bool DecodeEhFrameTable(const EhFrameHeader& header, u64 end, std::vector<uintptr_t>& starts,
                        std::string& error);

/// A function's code [begin, end) as its FDE describes it (pc_begin, pc_begin + pc_range).
struct CodeRange {
    u64 begin;
    u64 end;
};

/// The code range of every FDE in the search table (table order), read from the FDEs and their CIEs
/// (pointer encoding of the 'R' augmentation). `hdr_end`: end of .eh_frame_hdr; every FDE and CIE must
/// lie in [header.eh_frame_ptr, eh_frame_end). Used by SecondSonPC's FS patcher (P3.3): decoding stops
/// at a function's real end, not at the next function's start (gaps may hold data).
bool DecodeEhFrameFunctions(const EhFrameHeader& header, u64 hdr_end, u64 eh_frame_end,
                            std::vector<CodeRange>& out, std::string& error);

} // namespace SecondSon::Loader
