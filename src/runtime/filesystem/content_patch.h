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

/// What a file the game opens for reading gets: byte replacements, and a range whose reads are reported (where in
/// guest memory the game put those bytes: the button icon atlas, rewritten there when the input device changes).
struct FilePatch {
    std::vector<Edit> edits;
    u64 watch_offset = 0, watch_size = 0;
};

/// The patch of a file the game opens for reading (`guest` path, `host` file); empty for most files.
FilePatch ForFile(std::string_view guest, const std::filesystem::path& host);

/// The game's language tables (lang_<language>_text.xpps, not the _p ones): the button icons (ui/prompt_service.h).
/// `open`: the table's bytes as on disk -> its patch (any thread: a guest thread opens files). `read`: `size` bytes of
/// the watched range at file offset `offset` landed at guest address `address` (the reading guest thread).
struct LanguageTableHook {
    FilePatch (*open)(std::string_view guest, const std::string& bytes) = nullptr;
    void (*read)(std::string_view guest, u64 offset, u64 address, u64 size) = nullptr;
    /// Any other pack (/app0/art/cache/*.xpps): its patch from its file (`host`); the same `read` reports.
    FilePatch (*pack_open)(std::string_view guest, const std::filesystem::path& host) = nullptr;
};
void SetLanguageTableHook(LanguageTableHook hook);

/// A read of `n` bytes at file offset `offset` into guest memory `address`: the watched part reported.
void Observe(const FilePatch& patch, std::string_view guest, u64 offset, u64 address, u64 n);

/// The edits of a language table's bytes (tests): `russian` picks the replacement texts' language.
std::vector<Edit> ForLanguageTable(const std::string& bytes, bool russian);

/// Applies `edits` to `n` bytes read at file offset `offset` into `buf`.
void Apply(const std::vector<Edit>& edits, u64 offset, u8* buf, u64 n);

} // namespace SecondSon::Filesystem::ContentPatch
