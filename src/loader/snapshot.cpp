// SecondSonPC — relocation snapshot.
#include "loader/snapshot.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include <fmt/format.h>

#include "loader/bindings.h"
#include "loader/elf.h"
#include "loader/image_file.h"
#include "trace/import_counter.h"

namespace SecondSon::Loader {

namespace {

struct Parsed {
    std::vector<u8> data; // PT_SCE_DYNLIBDATA
    u64 strtab = 0, strsz = 0, symtab = 0, symsz = 0, rela = 0, relasz = 0, jmprel = 0, jmprelsz = 0;
    u64 image_size = 0;
};

bool Parse(const std::filesystem::path& path, Parsed& out, std::string& error) {
    ImageFile f;
    if (!f.Open(path, error)) {
        return false;
    }
    std::vector<u8> dyn;
    for (const auto& p : f.ProgramHeaders()) {
        if (p.type == Elf::PT_DYNAMIC) {
            dyn = f.ReadSegment(p);
        } else if (p.type == Elf::PT_SCE_DYNLIBDATA) {
            out.data = f.ReadSegment(p);
        }
        if ((p.type == Elf::PT_LOAD || p.type == Elf::PT_SCE_RELRO) && p.memsz != 0) {
            const u64 size = p.align ? (p.memsz + p.align - 1) & ~(p.align - 1) : p.memsz;
            out.image_size = std::max(out.image_size, p.vaddr + size);
        }
    }
    for (size_t i = 0; i + sizeof(Elf::Dynamic) <= dyn.size(); i += sizeof(Elf::Dynamic)) {
        Elf::Dynamic d;
        std::memcpy(&d, dyn.data() + i, sizeof(d));
        if (d.tag == Elf::DT_NULL) {
            break;
        }
        switch (d.tag) {
        case Elf::DT_SCE_STRTAB: out.strtab = d.value; break;
        case Elf::DT_SCE_STRSZ: out.strsz = d.value; break;
        case Elf::DT_SCE_SYMTAB: out.symtab = d.value; break;
        case Elf::DT_SCE_SYMTABSZ: out.symsz = d.value; break;
        case Elf::DT_SCE_RELA: out.rela = d.value; break;
        case Elf::DT_SCE_RELASZ: out.relasz = d.value; break;
        case Elf::DT_SCE_JMPREL: out.jmprel = d.value; break;
        case Elf::DT_SCE_PLTRELSZ: out.jmprelsz = d.value; break;
        default: break;
        }
    }
    const auto fits = [&](u64 off, u64 size) { return off <= out.data.size() && out.data.size() - off >= size; };
    if (!fits(out.strtab, out.strsz) || !fits(out.symtab, out.symsz) || !fits(out.rela, out.relasz) ||
        !fits(out.jmprel, out.jmprelsz)) {
        error = fmt::format("{}: dynamic tables out of range", path.string());
        return false;
    }
    return true;
}

const char* TypeName(u32 type) {
    switch (type) {
    case Elf::R_X86_64_64: return "R64";
    case Elf::R_X86_64_GLOB_DAT: return "GLOB_DAT";
    case Elf::R_X86_64_JUMP_SLOT: return "JUMP_SLOT";
    case Elf::R_X86_64_RELATIVE: return "RELATIVE";
    case Elf::R_X86_64_DTPMOD64: return "DTPMOD64";
    default: return "?";
    }
}

} // namespace

bool WriteRelocationSnapshot(const std::filesystem::path& file, std::span<const SnapshotModule> modules,
                             const Bindings* bindings, std::string& error) {
    std::vector<Parsed> parsed(modules.size());
    std::vector<SnapshotModule> ranges(modules.begin(), modules.end());
    for (size_t i = 0; i < modules.size(); ++i) {
        if (!Parse(modules[i].host_path, parsed[i], error)) {
            return false;
        }
        if (ranges[i].size == 0) {
            ranges[i].size = (parsed[i].image_size + 0xFFF) & ~u64{0xFFF};
        }
    }
    const auto classify = [&](u64 value) -> std::string {
        if (value == 0) {
            return "0";
        }
        for (const auto& r : ranges) {
            if (value >= r.base && value < r.base + r.size) {
                return fmt::format("{}+{:#x}", r.name, value - r.base);
            }
        }
        u64 target = value;
        if (const void* t = Trace::ThunkTarget(value)) {
            target = reinterpret_cast<u64>(t);
        }
        if (bindings != nullptr) {
            if (const auto* b = bindings->FindByAddress(target)) {
                return fmt::format("{} {}", OwnerName(b->owner), b->info->name);
            }
        }
        return target != value ? fmt::format("thunk->{:#x}", target) : fmt::format("raw {:#x}", value);
    };

    std::string out;
    out.reserve(8 << 20);
    for (size_t i = 0; i < modules.size(); ++i) {
        const auto& m = modules[i];
        const auto& p = parsed[i];
        out += fmt::format("# {} base {:#x} size {:#x}\n", m.name, m.base, ranges[i].size);
        const auto* strtab = reinterpret_cast<const char*>(p.data.data() + p.strtab);
        const auto* syms = reinterpret_cast<const Elf::Symbol*>(p.data.data() + p.symtab);
        const u64 nsyms = p.symsz / sizeof(Elf::Symbol);
        for (const auto [off, size] : {std::pair{p.rela, p.relasz}, std::pair{p.jmprel, p.jmprelsz}}) {
            const auto* rel = reinterpret_cast<const Elf::Rela*>(p.data.data() + off);
            for (u64 r = 0; r < size / sizeof(Elf::Rela); ++r) {
                u64 value = 0;
                std::memcpy(&value, reinterpret_cast<const void*>(m.base + rel[r].offset), sizeof(value));
                std::string_view sym = "-";
                if (rel[r].Type() != Elf::R_X86_64_RELATIVE && rel[r].SymbolIndex() < nsyms) {
                    const u32 name = syms[rel[r].SymbolIndex()].name;
                    sym = name < p.strsz ? strtab + name : "?";
                }
                out += fmt::format("{}+{:#x}\t{}\t{}\t{}\n", m.name, rel[r].offset, TypeName(rel[r].Type()),
                                   sym, classify(value));
            }
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary);
    if (!f.write(out.data(), static_cast<std::streamsize>(out.size()))) {
        error = fmt::format("cannot write {}", file.string());
        return false;
    }
    return true;
}

} // namespace SecondSon::Loader
