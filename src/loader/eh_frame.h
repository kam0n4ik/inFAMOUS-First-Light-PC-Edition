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

} // namespace SecondSon::Loader
