// SecondSonPC — one loaded PS4 module (eboot.bin or a .prx): mapping, dynamic info, exports.
// Behaviour follows the console loader as observed through the Phase 0 oracle (shadPS4 0fe263a,
// src/core/module.cpp): same load bases, same segment order, same TLS/proc-param handling.
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "loader/elf.h"

namespace SecondSon::Loader {

class Host;
class ImageFile;

/// DT_SCE_NEEDED_MODULE / DT_SCE_MODULE_INFO
struct ModuleRef {
    std::string name;
    u16 id = 0;
    u8 version_major = 0;
    u8 version_minor = 0;
    std::string enc_id;

    bool SameAs(const ModuleRef& o) const {
        return name == o.name && version_major == o.version_major && version_minor == o.version_minor;
    }
};

/// DT_SCE_IMPORT_LIB / DT_SCE_EXPORT_LIB
struct LibraryRef {
    std::string name;
    u16 id = 0;
    u16 version = 0;
    std::string enc_id;

    bool SameAs(const LibraryRef& o) const {
        return name == o.name && version == o.version;
    }
};

struct TlsImage {
    u32 align = 0;
    u32 image_size = 0; // p_memsz aligned to p_align
    u32 offset = 0;     // static TLS offset (main module only)
    u32 modid = 0;      // 1-based TLS module index, 0 = no TLS
    u64 image_vaddr = 0;
    u32 init_image_size = 0; // p_filesz
};

/// Layout of OrbisKernelModuleSegmentInfo.
struct SegmentInfo {
    u64 address;
    u32 size;
    s32 prot;
};
static_assert(sizeof(SegmentInfo) == 16);

struct ExportedSymbol {
    std::string nid;
    std::string library;
    std::string module;
    u64 address = 0;
    bool is_object = false;
};

class Module {
public:
    static constexpr u64 kTrampolineSize = 8ull << 20;

    /// Parses `host_path`, maps it into guest memory through `host`, patches its code and reads
    /// its dynamic section. Relocation is the Linker's job. Returns nullptr on error.
    static std::unique_ptr<Module> Load(Host& host, const std::filesystem::path& host_path,
                                        std::string guest_path, s32 id, u32& max_tls_index,
                                        std::string& error);

    /// Calls the module's DT_INIT entry with the console's (args, argp, param) convention.
    s32 Start(u64 args, const void* argp, void* param) const;

    u64 Base() const {
        return base_;
    }
    u64 EntryAddress() const {
        return base_ + entry_;
    }
    bool Contains(u64 address) const {
        return address >= base_ && address < base_ + aligned_base_size;
    }
    bool IsSharedLibrary() const {
        return e_type_ == Elf::ET_SCE_DYNAMIC;
    }

    /// Symbol name "nid#lib#mod" components: library/module by encoded id (import tables first,
    /// then export tables; an empty id means the first export entry).
    const LibraryRef* FindLibrary(std::string_view enc_id) const;
    const ModuleRef* FindModule(std::string_view enc_id) const;

    /// First export with this key (versions are ignored, like on the console). 0 if none.
    u64 FindExport(std::string_view nid, std::string_view library, std::string_view module) const;
    bool ExportsLibrary(const LibraryRef& lib) const;
    bool ExportsModule(const ModuleRef& mod) const;

    std::span<const Elf::Rela> Relocations() const {
        return {rela_, rela_count_};
    }
    std::span<const Elf::Rela> PltRelocations() const {
        return {jmprel_, jmprel_count_};
    }
    const Elf::Symbol* Symbols() const {
        return symtab_;
    }
    u64 SymbolCount() const {
        return symtab_count_;
    }
    const char* String(u32 offset) const {
        return offset < strtab_size_ ? strtab_ + offset : "";
    }

    bool TestRelaBit(u32 index) const {
        return (rela_bits_[index >> 3] >> (index & 7)) & 1;
    }
    void SetRelaBit(u32 index) {
        rela_bits_[index >> 3] |= static_cast<u8>(1u << (index & 7));
    }

    std::span<const ModuleRef> ImportModules() const {
        return import_modules_;
    }
    std::span<const ModuleRef> ExportModules() const {
        return export_modules_;
    }
    std::span<const ExportedSymbol> Exports() const {
        return exports_;
    }

    // Public state read by the Linker/rtld (mirrors OrbisKernelModuleInfoEx).
    std::string name;        // file name, e.g. "libc.prx"
    std::string guest_path;  // e.g. "/app0/sce_module/libc.prx"
    std::filesystem::path host_path;
    std::array<char, 256> info_name{}; // "<export module>.sprx", as the console reports it
    s32 id = 0;
    u64 aligned_base_size = 0;
    u64 base_size = 0;
    u64 proc_param = 0; // absolute address of SceProcParam / SceModuleParam, 0 if none
    TlsImage tls;
    std::array<SegmentInfo, 4> segments{};
    u32 segment_count = 0;
    u64 eh_frame_hdr_addr = 0; // relative to the base
    u64 eh_frame_addr = 0;     // relative to the base
    u32 eh_frame_hdr_size = 0;
    u32 eh_frame_size = 0;
    u64 init_addr = 0; // DT_INIT, relative
    u64 fini_addr = 0; // DT_FINI, relative

private:
    bool Map(Host& host, const ImageFile& file, u32& max_tls_index, std::string& error);
    bool ParseDynamic(std::string& error);
    void CollectExports();

    u64 base_ = 0;
    u64 entry_ = 0;
    u16 e_type_ = 0;
    std::vector<u8> dynamic_;      // PT_DYNAMIC
    std::vector<u8> dynamic_data_; // PT_SCE_DYNLIBDATA
    const char* strtab_ = nullptr;
    u64 strtab_size_ = 0;
    const Elf::Symbol* symtab_ = nullptr;
    u64 symtab_count_ = 0;
    const Elf::Rela* rela_ = nullptr;
    u64 rela_count_ = 0;
    const Elf::Rela* jmprel_ = nullptr;
    u64 jmprel_count_ = 0;
    std::vector<u8> rela_bits_;
    std::vector<ModuleRef> import_modules_;
    std::vector<ModuleRef> export_modules_;
    std::vector<LibraryRef> import_libs_;
    std::vector<LibraryRef> export_libs_;
    std::vector<ExportedSymbol> exports_;
};

/// Same as the console's module/library id encoding in symbol names ("nid#<lib>#<mod>").
std::string EncodeId(u64 value);

} // namespace SecondSon::Loader
