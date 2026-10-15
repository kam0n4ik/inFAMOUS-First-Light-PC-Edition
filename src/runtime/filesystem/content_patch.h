// SecondSonPC — the game's files as the PC Edition reads them: a few strings of the game's language tables are replaced
// in what the game reads (the files on disk stay unchanged; docs/PC_EDITION_UI.md).
//
// The game's Settings screen ends with "HDR Rendering - Unavailable" (u_options_opt_hdr / u_options_val_hdr_unavailable
// of lang_<language>_text_p.xpps), an option a PC never has. The PC Edition turns it into its "Additional settings -
// Press (X)" item: the game draws it in its own list with its own font, and SecondSonPC opens its settings when it is
// chosen (src/ui/menu_controller.cpp).
//
// A language table (KCAP): a hash table of entries {u64 hash of the key name, u32 offset of the key name}, then
// "key\0text\0" with zero padding. The text is found after the key name: the replacement writes an empty key name
// ("\0") where the key began and the new text after it, inside the entry's own bytes (key + text + padding).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Filesystem::ContentPatch {

struct Edit {
    u64 offset = 0;    // in the file
    std::string bytes; // what the game reads there instead
};

/// The edits of a file the game opens for reading (`guest` path, `host` file); none for most files.
std::vector<Edit> ForFile(std::string_view guest, const std::filesystem::path& host);

/// The edits of a language table's bytes (tests): `russian` picks the replacement texts' language.
std::vector<Edit> ForLanguageTable(const std::string& bytes, bool russian);

/// Applies `edits` to `n` bytes read at file offset `offset` into `buf`.
void Apply(const std::vector<Edit>& edits, u64 offset, u8* buf, u64 n);

} // namespace SecondSon::Filesystem::ContentPatch
