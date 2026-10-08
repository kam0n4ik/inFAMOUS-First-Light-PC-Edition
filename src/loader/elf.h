// SecondSonPC — PS4 SELF/ELF format definitions (written from the format; see tools/ps4elf.py).
#pragma once

#include <cstddef>

#include "base/abi.h"

namespace SecondSon::Loader::Elf {

// SELF container (fake-signed dumps: segments are stored as plain blocks).
constexpr u32 kSelfMagic = 0x1D3D154F;
constexpr size_t kSelfSegmentCountOffset = 0x18; // u16
constexpr size_t kSelfSegmentTableOffset = 0x20; // SelfSegment[count], ELF header follows

struct SelfSegment {
    u64 flags;
    u64 file_offset;
    u64 file_size;
    u64 memory_size;

    bool IsBlocked() const {
        return (flags & 0x800) != 0;
    }
    u32 PhdrIndex() const {
        return static_cast<u32>((flags >> 20) & 0xFFF);
    }
};
static_assert(sizeof(SelfSegment) == 32);

struct Header {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};
static_assert(sizeof(Header) == 64);

struct ProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};
static_assert(sizeof(ProgramHeader) == 56);

struct Dynamic {
    s64 tag;
    u64 value;
};
static_assert(sizeof(Dynamic) == 16);

struct Symbol {
    u32 name;
    u8 info;
    u8 other;
    u16 shndx;
    u64 value;
    u64 size;

    u8 Bind() const {
        return info >> 4;
    }
    u8 Type() const {
        return info & 0xF;
    }
    u8 Visibility() const {
        return other & 3;
    }
};
static_assert(sizeof(Symbol) == 24);

struct Rela {
    u64 offset;
    u64 info;
    s64 addend;

    u32 Type() const {
        return static_cast<u32>(info & 0xFFFFFFFF);
    }
    u32 SymbolIndex() const {
        return static_cast<u32>(info >> 32);
    }
};
static_assert(sizeof(Rela) == 24);

// e_type
constexpr u16 ET_SCE_EXEC = 0xFE00;
constexpr u16 ET_SCE_DYNEXEC = 0xFE10;
constexpr u16 ET_SCE_DYNAMIC = 0xFE18;

// p_type
constexpr u32 PT_LOAD = 0x1;
constexpr u32 PT_DYNAMIC = 0x2;
constexpr u32 PT_INTERP = 0x3;
constexpr u32 PT_TLS = 0x7;
constexpr u32 PT_SCE_DYNLIBDATA = 0x61000000;
constexpr u32 PT_SCE_PROCPARAM = 0x61000001;
constexpr u32 PT_SCE_MODULE_PARAM = 0x61000002;
constexpr u32 PT_SCE_RELRO = 0x61000010;
constexpr u32 PT_GNU_EH_FRAME = 0x6474E550;
constexpr u32 PT_SCE_COMMENT = 0x6FFFFF00;
constexpr u32 PT_SCE_LIBVERSION = 0x6FFFFF01;

// p_flags
constexpr u32 PF_EXEC = 0x1;
constexpr u32 PF_WRITE = 0x2;
constexpr u32 PF_READ = 0x4;

// d_tag
constexpr s64 DT_NULL = 0;
constexpr s64 DT_NEEDED = 0x00000001;
constexpr s64 DT_RELA = 0x00000007;
constexpr s64 DT_INIT = 0x0000000C;
constexpr s64 DT_FINI = 0x0000000D;
constexpr s64 DT_DEBUG = 0x00000015;
constexpr s64 DT_TEXTREL = 0x00000016;
constexpr s64 DT_INIT_ARRAY = 0x00000019;
constexpr s64 DT_FINI_ARRAY = 0x0000001A;
constexpr s64 DT_INIT_ARRAYSZ = 0x0000001B;
constexpr s64 DT_FINI_ARRAYSZ = 0x0000001C;
constexpr s64 DT_FLAGS = 0x0000001E;
constexpr s64 DT_PREINIT_ARRAY = 0x00000020;
constexpr s64 DT_PREINIT_ARRAYSZ = 0x00000021;
constexpr s64 DT_SCE_FINGERPRINT = 0x61000007;
constexpr s64 DT_SCE_ORIGINAL_FILENAME = 0x61000009;
constexpr s64 DT_SCE_MODULE_INFO = 0x6100000D;
constexpr s64 DT_SCE_NEEDED_MODULE = 0x6100000F;
constexpr s64 DT_SCE_MODULE_ATTR = 0x61000011;
constexpr s64 DT_SCE_EXPORT_LIB = 0x61000013;
constexpr s64 DT_SCE_IMPORT_LIB = 0x61000015;
constexpr s64 DT_SCE_IMPORT_LIB_ATTR = 0x61000019;
constexpr s64 DT_SCE_HASH = 0x61000025;
constexpr s64 DT_SCE_PLTGOT = 0x61000027;
constexpr s64 DT_SCE_JMPREL = 0x61000029;
constexpr s64 DT_SCE_PLTREL = 0x6100002B;
constexpr s64 DT_SCE_PLTRELSZ = 0x6100002D;
constexpr s64 DT_SCE_RELA = 0x6100002F;
constexpr s64 DT_SCE_RELASZ = 0x61000031;
constexpr s64 DT_SCE_RELAENT = 0x61000033;
constexpr s64 DT_SCE_STRTAB = 0x61000035;
constexpr s64 DT_SCE_STRSZ = 0x61000037;
constexpr s64 DT_SCE_SYMTAB = 0x61000039;
constexpr s64 DT_SCE_SYMENT = 0x6100003B;
constexpr s64 DT_SCE_HASHSZ = 0x6100003D;
constexpr s64 DT_SCE_SYMTABSZ = 0x6100003F;

// relocation types
constexpr u32 R_X86_64_64 = 1;
constexpr u32 R_X86_64_GLOB_DAT = 6;
constexpr u32 R_X86_64_JUMP_SLOT = 7;
constexpr u32 R_X86_64_RELATIVE = 8;
constexpr u32 R_X86_64_DTPMOD64 = 16;

// symbol binding / type
constexpr u8 STB_LOCAL = 0;
constexpr u8 STB_GLOBAL = 1;
constexpr u8 STB_WEAK = 2;
constexpr u8 STT_NOTYPE = 0;
constexpr u8 STT_OBJECT = 1;
constexpr u8 STT_FUNC = 2;

} // namespace SecondSon::Loader::Elf
