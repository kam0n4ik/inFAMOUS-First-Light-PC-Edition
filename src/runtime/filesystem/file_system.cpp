// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — FileSystem (NATIVE, Phase 1). Semantics (flag handling, errno values and their
// order, stat fields, PFS/normal directories, the bounce buffer + GPU invalidation of reads) are
// ported from third_party/shadPS4/src/core/libraries/kernel/file_system.cpp and
// src/core/file_sys/fs.cpp @ 0fe263a; paths go through SecondSonPC's Vfs, files are Win32 handles
// with positioned I/O. Descriptors SecondSonPC did not open go to the BRIDGE implementation.
#include "runtime/filesystem/file_system.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "base/log.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/r9/gprof.h"
#include "runtime/exports.h"
#include "runtime/filesystem/content_patch.h"
#include "runtime/filesystem/directory.h"
#include "runtime/filesystem/fs_host.h"
#include "runtime/filesystem/fs_observer.h"
#include "runtime/filesystem/vfs.h"

namespace SecondSon::Filesystem {

namespace {

FsHost* g_host = nullptr;

// FreeBSD errno values (the console's), not the host's.
constexpr s32 kEPERM = 1;
constexpr s32 kENOENT = 2;
constexpr s32 kEIO = 5;
constexpr s32 kEBADF = 9;
constexpr s32 kEACCES = 13;
constexpr s32 kEFAULT = 14;
constexpr s32 kEEXIST = 17;
constexpr s32 kENOTDIR = 20;
constexpr s32 kEISDIR = 21;
constexpr s32 kEINVAL = 22;
constexpr s32 kENOTTY = 25;
constexpr s32 kEROFS = 30;
constexpr s32 kENAMETOOLONG = 63;
constexpr s32 kENOTEMPTY = 66;
constexpr s32 kOrbisKernelErrorBase = static_cast<s32>(0x80020000); // errno + this
constexpr s32 ORBIS_OK = 0;

constexpr s32 O_WRONLY = 0x1;
constexpr s32 O_RDWR = 0x2;
constexpr s32 O_NONBLOCK = 0x4;
constexpr s32 O_APPEND = 0x8;
constexpr s32 O_FSYNC = 0x80;
constexpr s32 O_CREAT = 0x200;
constexpr s32 O_TRUNC = 0x400;
constexpr s32 O_EXCL = 0x800;
constexpr s32 O_DSYNC = 0x1000;
constexpr s32 O_DIRECT = 0x10000;
constexpr s32 O_DIRECTORY = 0x20000;

s32 SetErrno(s32 e) {
    *g_host->Errno() = e;
    return -1;
}

s32 Errno() {
    return *g_host->Errno();
}

/// sceKernel* variant of a POSIX result: errno + 0x80020000 on failure (errno stays set).
template <typename T>
T ToOrbis(T result, const char* what) {
    if (result < 0) {
        SSPC_LOG(Filesystem, Debug, "{}: errno {}", what, Errno());
        return static_cast<T>(Errno() + kOrbisKernelErrorBase);
    }
    return result;
}

enum class Access { Read, Write, Append, ReadWrite, ReadAppend };

struct File {
    bool is_dir = false;
    std::string guest;
    std::filesystem::path host;
    HANDLE handle = INVALID_HANDLE_VALUE;
    Access access = Access::Read;
    u64 pos = 0;
    std::unique_ptr<Directory> dir;
    ContentPatch::FilePatch patch; // the PC Edition's replacements in what the game reads (read only)
    std::mutex mutex;
    // a movie (.bsf): where its reads go (diagnostics: a video that repeats a moment or jumps elsewhere)
    bool movie = false;
    u64 movie_next = 0, movie_reads = 0, movie_jumps = 0, movie_bytes = 0;

    bool WriteOnly() const {
        return access == Access::Write || access == Access::Append;
    }
    bool Appends() const {
        return access == Access::Append || access == Access::ReadAppend;
    }
    ~File() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

std::shared_mutex g_table_mutex;
std::unordered_map<s32, std::shared_ptr<File>> g_files;

std::shared_ptr<File> Get(s32 fd) {
    std::shared_lock lock{g_table_mutex};
    const auto it = g_files.find(fd);
    return it != g_files.end() ? it->second : nullptr;
}

// ---- host file helpers (shadPS4's IOFile modes: Read "rb", Write/ReadWrite "r+b", Append "ab",
// ReadAppend "a+b", Create "wb") ------------------------------------------------------------
HANDLE OpenHost(const std::filesystem::path& path, Access access) {
    DWORD rights = GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    switch (access) {
    case Access::Read:
        break;
    case Access::Write:
    case Access::ReadWrite:
        rights = GENERIC_READ | GENERIC_WRITE;
        break;
    case Access::Append:
        rights = GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        break;
    case Access::ReadAppend:
        rights = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        break;
    }
    return CreateFileW(path.c_str(), rights, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool CreateEmpty(const std::filesystem::path& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

u64 HostSize(HANDLE h) {
    LARGE_INTEGER size{};
    return GetFileSizeEx(h, &size) ? static_cast<u64>(size.QuadPart) : 0;
}

bool SetHostSize(HANDLE h, u64 size) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof(info)) != 0;
}

u64 ReadAt(HANDLE h, u64 offset, void* buf, u64 n) {
    u64 total = 0;
    while (total < n) {
        OVERLAPPED ov{};
        const u64 off = offset + total;
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        const DWORD chunk = static_cast<DWORD>(std::min<u64>(n - total, 0x40000000));
        DWORD got = 0;
        if (!ReadFile(h, static_cast<u8*>(buf) + total, chunk, &got, &ov) || got == 0) {
            break;
        }
        total += got;
    }
    return total;
}

u64 WriteAt(HANDLE h, u64 offset, const void* buf, u64 n) {
    u64 total = 0;
    while (total < n) {
        OVERLAPPED ov{};
        const u64 off = offset + total;
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        const DWORD chunk = static_cast<DWORD>(std::min<u64>(n - total, 0x40000000));
        DWORD put = 0;
        if (!WriteFile(h, static_cast<const u8*>(buf) + total, chunk, &put, &ov) || put == 0) {
            break;
        }
        total += put;
    }
    return total;
}

s64 UnixSeconds(const FILETIME& ft) {
    const u64 t = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t == 0 ? 0 : static_cast<s64>(t / 10000000ull) - 11644473600ll;
}

thread_local std::vector<u8> t_bounce;

/// Read into guest memory: invalidate the GPU-tracked range, read into a bounce buffer, copy.
s64 ReadRegular(File& f, void* buf, u64 nbytes, u64 offset) {
    const u64 size = HostSize(f.handle);
    const u64 remaining = size > offset ? size - offset : 0;
    g_host->InvalidateGuestMemory(reinterpret_cast<u64>(buf), std::min(nbytes, remaining));
    if (t_bounce.size() < nbytes) {
        t_bounce.resize(nbytes);
    }
    const u64 got = ReadAt(f.handle, offset, t_bounce.data(), nbytes);
    if (!f.patch.edits.empty()) {
        ContentPatch::Apply(f.patch.edits, offset, t_bounce.data(), got);
    }
    std::memcpy(buf, t_bounce.data(), got);
    if (f.patch.watch_size != 0) {
        ContentPatch::Observe(f.patch, f.guest, offset, reinterpret_cast<u64>(buf), got);
    }
    // DIAGNOSTIC (SSPC_DIAG_PACK_READS=1): where each read of a pack landed (logs\pack_reads.csv: path, file offset,
    // guest address, bytes) - a texture's address in a frame back to its file
    static std::FILE* pack_log = [] {
        char v[4] = {};
        std::FILE* out = nullptr;
        if (GetEnvironmentVariableA("SSPC_DIAG_PACK_READS", v, sizeof(v)) == 1 && v[0] == '1') {
            _wfopen_s(&out, L"logs\\pack_reads.csv", L"w");
        }
        return out;
    }();
    if (pack_log != nullptr && f.guest.find("/art/cache/") != std::string::npos) {
        static std::mutex m;
        std::scoped_lock lock{m};
        std::fprintf(pack_log, "%s,%llu,%llu,%llu\n", f.guest.c_str(), static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(reinterpret_cast<u64>(buf)), static_cast<unsigned long long>(got));
        std::fflush(pack_log);
    }
    return static_cast<s64>(got);
}

s64 WriteRegular(File& f, const void* buf, u64 nbytes, u64 offset) {
    if (f.Appends()) {
        offset = HostSize(f.handle);
    }
    return static_cast<s64>(WriteAt(f.handle, offset, buf, nbytes));
}

void StatTimes(OrbisKernelStat& sb, const WIN32_FILE_ATTRIBUTE_DATA& a) {
    sb.st_atim.tv_sec = UnixSeconds(a.ftLastAccessTime);
    sb.st_mtim.tv_sec = UnixSeconds(a.ftLastWriteTime);
    sb.st_ctim.tv_sec = UnixSeconds(a.ftCreationTime);
}

// ------------------------------------------------------------------------------- POSIX layer

s32 PosixOpen(const char* raw_path, s32 flags, u16 mode) {
    const bool read = (flags & 0x3) == 0;
    const bool write = (flags & 0x3) == O_WRONLY;
    const bool rdwr = (flags & 0x3) == O_RDWR;
    if (!read && !write && !rdwr) {
        return SetErrno(kEINVAL);
    }
    if (std::strlen(raw_path) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    const bool append = (flags & O_APPEND) != 0;
    const bool create = (flags & O_CREAT) != 0;
    const bool truncate = (flags & O_TRUNC) != 0;
    const bool excl = (flags & O_EXCL) != 0;
    bool directory = (flags & O_DIRECTORY) != 0;
    if ((flags & (O_FSYNC | O_DIRECT | O_DSYNC | O_NONBLOCK)) != 0) {
        SSPC_LOG(Filesystem, Debug, "open {}: flags {:#x} not fully handled", raw_path, flags);
    }

    const std::string_view path{raw_path};
    if (path.starts_with("/dev/")) {
        return g_host->ForeignOpen(raw_path, flags, mode); // devices stay BRIDGE
    }

    const auto r = GetVfs().Resolve(path);
    const bool read_only = r && r->read_only;
    std::error_code ec;
    const bool exists = r && std::filesystem::exists(r->host, ec);

    if (create) {
        if (excl && exists) {
            return SetErrno(kEEXIST);
        }
        if (!exists) {
            if (read_only) {
                return SetErrno(kEROFS);
            }
            if (r) {
                CreateEmpty(r->host);
            }
        }
    } else if (!exists) {
        return SetErrno(kENOENT);
    }

    const bool is_dir = r && std::filesystem::is_directory(r->host, ec);
    directory = directory || is_dir;

    auto file = std::make_shared<File>();
    file->guest = r ? r->guest : std::string(path);
    file->host = r ? r->host : std::filesystem::path{};
    if (directory) {
        if (!is_dir) {
            return SetErrno(kENOTDIR);
        }
        if (write || rdwr || truncate) {
            return SetErrno(kEISDIR);
        }
        file->is_dir = true;
        file->dir = Directory::Create(r->host, r->guest.starts_with("/app0"));
    } else {
        if ((write || rdwr || truncate) && read_only) {
            return SetErrno(kEROFS);
        }
        Access access = Access::Read;
        if (truncate) {
            access = Access::ReadWrite;
        } else if (write) {
            access = append ? Access::Append : Access::Write;
        } else if (rdwr) {
            access = append ? Access::ReadAppend : Access::ReadWrite;
        }
        file->access = access;
        if (r) {
            file->handle = OpenHost(r->host, access);
            if (access == Access::Read && file->handle != INVALID_HANDLE_VALUE) {
                file->patch = ContentPatch::ForFile(file->guest, r->host);
                file->movie = file->guest.ends_with(".bsf");
            }
        }
        if (file->handle == INVALID_HANDLE_VALUE) {
            return SetErrno(r ? ((write || rdwr || truncate) && read_only ? kEROFS : kEIO) : kEINVAL);
        }
        if (truncate) {
            SetHostSize(file->handle, 0);
        }
    }

    const s32 fd = g_host->ReserveDescriptor();
    {
        std::unique_lock lock{g_table_mutex};
        g_files[fd] = std::move(file);
    }
    return fd;
}

s32 PosixClose(s32 fd) {
    std::shared_ptr<File> file;
    {
        std::unique_lock lock{g_table_mutex};
        const auto it = g_files.find(fd);
        if (it == g_files.end()) {
            return g_host->ForeignClose(fd);
        }
        file = std::move(it->second);
        g_files.erase(it);
    }
    if (file->movie) {
        SSPC_LOG(Filesystem, Info, "close {} ({}): {} reads, {} MiB, {} not where the previous one ended", fd,
                 file->guest, file->movie_reads, file->movie_bytes >> 20, file->movie_jumps);
    } else {
        SSPC_LOG(Filesystem, Info, "close {} ({})", fd, file->guest);
    }
    g_host->ReleaseDescriptor(fd);
    return ORBIS_OK;
}

void NoteMovieRead(File& f, u64 offset, s64 got) {
    if (f.movie_reads != 0 && offset != f.movie_next && ++f.movie_jumps <= 32) {
        SSPC_LOG(Filesystem, Warning, "movie {}: read {} at {:#x}, the previous read ended at {:#x}", f.guest,
                 f.movie_reads + 1, offset, f.movie_next);
    }
    ++f.movie_reads;
    if (got > 0) {
        f.movie_bytes += static_cast<u64>(got);
    }
    f.movie_next = offset + static_cast<u64>(std::max<s64>(got, 0));
}

s64 PosixRead(s32 fd, void* buf, u64 nbytes) {
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignRead(fd, buf, nbytes);
    }
    std::scoped_lock lock{f->mutex};
    if (f->is_dir) {
        return f->dir->Read(buf, nbytes);
    }
    if (f->WriteOnly()) {
        return SetErrno(kEBADF);
    }
    const s64 got = ReadRegular(*f, buf, nbytes, f->pos);
    if (f->movie) {
        NoteMovieRead(*f, f->pos, got);
    }
    f->pos += got;
    return got;
}

s64 PosixPread(s32 fd, void* buf, u64 nbytes, s64 offset) {
    if (offset < 0) {
        return SetErrno(kEINVAL);
    }
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignPread(fd, buf, nbytes, offset);
    }
    std::scoped_lock lock{f->mutex};
    if (f->is_dir) {
        const s64 old = f->dir->Tell();
        f->dir->SetOffset(offset);
        const s64 got = f->dir->Read(buf, nbytes);
        f->dir->SetOffset(old);
        return got;
    }
    if (f->WriteOnly()) {
        return SetErrno(kEBADF);
    }
    const s64 got = ReadRegular(*f, buf, nbytes, static_cast<u64>(offset));
    if (f->movie) {
        NoteMovieRead(*f, static_cast<u64>(offset), got);
    }
    return got;
}

s64 PosixWrite(s32 fd, const void* buf, u64 nbytes) {
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignWrite(fd, buf, nbytes);
    }
    std::scoped_lock lock{f->mutex};
    if (f->is_dir || f->access == Access::Read) {
        return SetErrno(kEBADF);
    }
    const s64 put = WriteRegular(*f, buf, nbytes, f->pos);
    f->pos = f->Appends() ? HostSize(f->handle) : f->pos + put;
    return put;
}

s64 PosixPwrite(s32 fd, const void* buf, u64 nbytes, s64 offset) {
    if (offset < 0) {
        return SetErrno(kEINVAL);
    }
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignPwrite(fd, buf, nbytes, offset);
    }
    std::scoped_lock lock{f->mutex};
    if (f->is_dir || f->access == Access::Read) {
        return SetErrno(kEBADF);
    }
    return WriteRegular(*f, buf, nbytes, static_cast<u64>(offset));
}

s64 PosixLseek(s32 fd, s64 offset, s32 whence) {
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignLseek(fd, offset, whence);
    }
    std::scoped_lock lock{f->mutex};
    if (f->is_dir) {
        const s64 r = f->dir->Lseek(offset, whence);
        if (r < 0) {
            return SetErrno(static_cast<s32>(r - kOrbisKernelErrorBase));
        }
        return r;
    }
    s64 base = 0;
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = static_cast<s64>(f->pos);
    } else if (whence == 2) {
        base = static_cast<s64>(HostSize(f->handle));
    } else if (whence == 3 || whence == 4) {
        return SetErrno(kENOTTY); // SEEK_DATA / SEEK_HOLE
    } else {
        return SetErrno(kEINVAL);
    }
    if (base + offset < 0) {
        return SetErrno(kEINVAL);
    }
    f->pos = static_cast<u64>(base + offset);
    return static_cast<s64>(f->pos);
}

s32 PosixStat(const char* path, OrbisKernelStat* sb) {
    if (std::strlen(path) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    std::memset(sb, 0, sizeof(OrbisKernelStat));
    const auto r = GetVfs().Resolve(path);
    std::error_code ec;
    const bool is_dir = r && std::filesystem::is_directory(r->host, ec);
    const bool is_file = !is_dir && r && std::filesystem::exists(r->host, ec);
    const bool is_root = std::strcmp(path, "/") == 0;
    if (!is_dir && !is_file && !is_root) {
        return SetErrno(kENOENT);
    }
    if (is_root) {
        sb->st_mode = 0000777u | 0040000u;
        sb->st_size = 65536;
        sb->st_blksize = 65536;
        sb->st_blocks = 128;
        return ORBIS_OK;
    }
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const bool have = GetFileAttributesExW(r->host.c_str(), GetFileExInfoStandard, &a) != 0;
    if (is_dir) {
        sb->st_mode = 0000777u | 0040000u;
        sb->st_size = 65536;
        sb->st_blksize = 65536;
        sb->st_blocks = 128;
        if (have) {
            sb->st_mtim.tv_sec = UnixSeconds(a.ftLastWriteTime);
        }
    } else {
        sb->st_mode = 0000777u | 0100000u;
        if (have) {
            sb->st_size = static_cast<s64>((static_cast<u64>(a.nFileSizeHigh) << 32) | a.nFileSizeLow);
            StatTimes(*sb, a);
        }
        sb->st_blksize = 512;
        sb->st_blocks = (sb->st_size + 511) / 512;
    }
    return ORBIS_OK;
}

s32 PosixFstat(s32 fd, OrbisKernelStat* sb) {
    if (sb == nullptr) {
        return SetErrno(kEFAULT);
    }
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignFstat(fd, sb);
    }
    std::scoped_lock lock{f->mutex};
    std::memset(sb, 0, sizeof(OrbisKernelStat));
    if (f->is_dir) {
        f->dir->Fstat(*sb);
        return ORBIS_OK;
    }
    sb->st_mode = 0000777u | 0100000u;
    sb->st_blksize = 512;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(f->handle, &info)) {
        sb->st_size = static_cast<s64>((static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
        sb->st_atim.tv_sec = UnixSeconds(info.ftLastAccessTime);
        sb->st_mtim.tv_sec = UnixSeconds(info.ftLastWriteTime);
        sb->st_ctim.tv_sec = UnixSeconds(info.ftCreationTime);
    }
    sb->st_blocks = (sb->st_size + 511) / 512;
    return ORBIS_OK;
}

s32 PosixFsync(s32 fd) {
    const auto f = Get(fd);
    if (!f) {
        return g_host->ForeignFsync(fd);
    }
    std::scoped_lock lock{f->mutex};
    if (!f->is_dir && f->access != Access::Read) {
        FlushFileBuffers(f->handle);
    }
    return ORBIS_OK;
}

s32 PosixFtruncate(s32 fd, s64 length) {
    const auto f = Get(fd);
    if (!f) {
        return SetErrno(kEBADF);
    }
    std::scoped_lock lock{f->mutex};
    if (f->host.empty()) {
        return SetErrno(kEACCES);
    }
    if (f->is_dir || f->access == Access::Read) {
        return SetErrno(kEINVAL); // not open for writing
    }
    if (length < 0 || !SetHostSize(f->handle, static_cast<u64>(length))) {
        return SetErrno(kEINVAL);
    }
    return ORBIS_OK;
}

s32 PosixTruncate(const char* path, s64 length) {
    if (std::strlen(path) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    const auto r = GetVfs().Resolve(path);
    std::error_code ec;
    if (!r || !std::filesystem::exists(r->host, ec)) {
        return SetErrno(kENOENT);
    }
    if (r->read_only) {
        return SetErrno(kEROFS);
    }
    if (std::filesystem::is_directory(r->host, ec)) {
        return SetErrno(kEISDIR);
    }
    if (length < 0) {
        return SetErrno(kEINVAL);
    }
    HANDLE h = OpenHost(r->host, Access::ReadWrite);
    if (h == INVALID_HANDLE_VALUE) {
        return SetErrno(kEIO);
    }
    const bool ok = SetHostSize(h, static_cast<u64>(length));
    CloseHandle(h);
    return ok ? ORBIS_OK : SetErrno(kEIO);
}

s32 PosixMkdir(const char* path, u16 mode) {
    if (path == nullptr) {
        return SetErrno(kENOTDIR);
    }
    SSPC_LOG(Filesystem, Info, "mkdir {} mode {:#o}", path, mode);
    if (std::strlen(path) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    const auto r = GetVfs().Resolve(path);
    std::error_code ec;
    if (r && std::filesystem::exists(r->host, ec)) {
        return SetErrno(kEEXIST);
    }
    if (r && r->read_only) {
        return SetErrno(kEROFS);
    }
    if (!r || !std::filesystem::create_directory(r->host, ec)) {
        return SetErrno(kEIO);
    }
    if (!std::filesystem::exists(r->host, ec)) {
        return SetErrno(kENOENT);
    }
    return ORBIS_OK;
}

s32 PosixRmdir(const char* path) {
    if (std::strlen(path) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    const auto r = GetVfs().Resolve(path);
    if (r && r->read_only) {
        return SetErrno(kEROFS);
    }
    std::error_code ec;
    if (!r || !std::filesystem::is_directory(r->host, ec)) {
        return SetErrno(kENOTDIR);
    }
    std::filesystem::remove_all(r->host, ec);
    return ec ? SetErrno(kEIO) : ORBIS_OK;
}

s32 PosixRename(const char* from, const char* to) {
    const auto src = GetVfs().Resolve(from);
    if (std::strlen(from) > 255 || std::strlen(to) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    std::error_code ec;
    if (!src || !std::filesystem::exists(src->host, ec)) {
        return SetErrno(kENOENT);
    }
    if (src->read_only) {
        return SetErrno(kEROFS);
    }
    const auto dst = GetVfs().Resolve(to);
    if (!dst || dst->read_only) {
        return SetErrno(kEROFS);
    }
    const bool src_is_dir = std::filesystem::is_directory(src->host, ec);
    const bool dst_is_dir = std::filesystem::is_directory(dst->host, ec);
    if (std::filesystem::exists(dst->host, ec)) {
        if (src_is_dir && !dst_is_dir) {
            return SetErrno(kENOTDIR);
        }
        if (!src_is_dir && dst_is_dir) {
            return SetErrno(kEISDIR);
        }
        if (dst_is_dir && !std::filesystem::is_empty(dst->host, ec)) {
            return SetErrno(kENOTEMPTY);
        }
        if (dst_is_dir) {
            std::filesystem::remove(dst->host, ec);
        }
    }
    // Open handles were created with FILE_SHARE_DELETE, so the file can move while open.
    if (!MoveFileExW(src->host.c_str(), dst->host.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        SSPC_LOG(Filesystem, Error, "rename {} -> {} failed: {}", from, to, GetLastError());
        return SetErrno(kEIO);
    }
    std::shared_lock lock{g_table_mutex};
    for (auto& [fd, f] : g_files) {
        if (f->host == src->host) {
            f->host = dst->host;
            f->guest = dst->guest;
        }
    }
    return ORBIS_OK;
}

s32 PosixUnlink(const char* path) {
    if (path == nullptr) {
        return SetErrno(kEINVAL);
    }
    if (std::strlen(path) > 255) {
        return SetErrno(kENAMETOOLONG);
    }
    const auto r = GetVfs().Resolve(path);
    if (!r) {
        return SetErrno(kENOENT);
    }
    if (r->read_only) {
        return SetErrno(kEROFS);
    }
    std::error_code ec;
    if (std::filesystem::is_directory(r->host, ec)) {
        return SetErrno(kEPERM);
    }
    if (!DeleteFileW(r->host.c_str())) {
        // The oracle reports success here as well.
        SSPC_LOG(Filesystem, Warning, "unlink {}: host delete failed ({})", path, GetLastError());
    }
    SSPC_LOG(Filesystem, Info, "unlinked {}", path);
    return ORBIS_OK;
}

s64 Getdents(s32 fd, char* buf, u64 nbytes, s64* basep) {
    if (buf == nullptr) {
        return SetErrno(kEFAULT);
    }
    const auto f = Get(fd);
    if (!f) {
        return SetErrno(kEBADF);
    }
    if (nbytes < 512) {
        return SetErrno(kEINVAL);
    }
    std::scoped_lock lock{f->mutex};
    if (!f->is_dir) {
        return SetErrno(kEINVAL);
    }
    return f->dir->Getdents(buf, nbytes, basep);
}

// ------------------------------------------------------------------------- guest entry points

/// Name of the calling guest thread (set by the pthread bridge as the host thread description).
std::string ThreadName() {
    PWSTR desc = nullptr;
    std::string out = "?";
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc != nullptr) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            out.resize(static_cast<size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, out.data(), n, nullptr, nullptr);
        }
        LocalFree(desc);
    }
    return out;
}

/// A read the Performance Inspector counts (P4.8; off: the call alone).
template <typename F>
s64 CountedRead(F&& read) {
    const bool insp = Gpu::Inspector::On();
    if (!insp && !Gpu::Gprof::On()) {
        return read();
    }
    const auto t0 = std::chrono::steady_clock::now();
    const s64 r = read();
    const u64 bytes = r > 0 ? static_cast<u64>(r) : 0;
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (insp) {
        Gpu::Inspector::FileRead(bytes, ns);
    }
    if (Gpu::Gprof::On()) { // (R9: the gameplay profiler's counters)
        Gpu::Gprof::Add(Gpu::Gprof::Ctr::FileReadBytes, bytes);
        Gpu::Gprof::Add(Gpu::Gprof::Ctr::FileReadNs, ns);
        Gpu::Gprof::Add(Gpu::Gprof::Ctr::FileReads, 1);
    }
    return r;
}

SSPC_SYSV_ABI s32 KernelOpen(const char* path, s32 flags, u16 mode) {
    const s32 r = PosixOpen(path, flags, mode);
    if (r < 0) {
        SSPC_LOG(Filesystem, Info, "open [{}] {} flags {:#x} mode {:#o} -> errno {}", ThreadName(), path,
                 flags, mode, Errno());
    } else {
        SSPC_LOG(Filesystem, Info, "open [{}] {} flags {:#x} mode {:#o} -> fd {}", ThreadName(), path,
                 flags, mode, r);
    }
    const s32 result = ToOrbis(r, "sceKernelOpen");
    NoteLanguageFile(path, "opened", result);
    return result;
}

SSPC_SYSV_ABI s32 KernelClose(s32 fd) {
    return ToOrbis(PosixClose(fd), "sceKernelClose");
}

SSPC_SYSV_ABI s64 KernelRead(s32 fd, void* buf, u64 nbytes) {
    return ToOrbis(CountedRead([&] { return PosixRead(fd, buf, nbytes); }), "sceKernelRead");
}

SSPC_SYSV_ABI s64 KernelPread(s32 fd, void* buf, u64 nbytes, s64 offset) {
    return ToOrbis(CountedRead([&] { return PosixPread(fd, buf, nbytes, offset); }), "sceKernelPread");
}

SSPC_SYSV_ABI s64 KernelWrite(s32 fd, const void* buf, u64 nbytes) {
    return ToOrbis(PosixWrite(fd, buf, nbytes), "sceKernelWrite");
}

SSPC_SYSV_ABI s64 KernelPwrite(s32 fd, const void* buf, u64 nbytes, s64 offset) {
    return ToOrbis(PosixPwrite(fd, buf, nbytes, offset), "sceKernelPwrite");
}

SSPC_SYSV_ABI s64 KernelLseek(s32 fd, s64 offset, s32 whence) {
    return ToOrbis(PosixLseek(fd, offset, whence), "sceKernelLseek");
}

SSPC_SYSV_ABI s32 KernelStat(const char* path, OrbisKernelStat* sb) {
    const s32 r = PosixStat(path, sb);
    SSPC_LOG(Filesystem, Debug, "stat {} -> {}", path, r < 0 ? Errno() : 0);
    return ToOrbis(r, "sceKernelStat");
}

SSPC_SYSV_ABI s32 KernelFstat(s32 fd, OrbisKernelStat* sb) {
    return ToOrbis(PosixFstat(fd, sb), "sceKernelFstat");
}

SSPC_SYSV_ABI s32 KernelFsync(s32 fd) {
    return ToOrbis(PosixFsync(fd), "sceKernelFsync");
}

SSPC_SYSV_ABI s32 KernelFtruncate(s32 fd, s64 length) {
    return ToOrbis(PosixFtruncate(fd, length), "sceKernelFtruncate");
}

SSPC_SYSV_ABI s32 KernelTruncate(const char* path, s64 length) {
    SSPC_LOG(Filesystem, Info, "truncate {} {}", path, length);
    return ToOrbis(PosixTruncate(path, length), "sceKernelTruncate");
}

SSPC_SYSV_ABI s64 KernelGetdirentries(s32 fd, char* buf, u64 nbytes, s64* basep) {
    return ToOrbis(Getdents(fd, buf, nbytes, basep), "sceKernelGetdirentries");
}

SSPC_SYSV_ABI s32 KernelMkdir(const char* path, u16 mode) {
    return ToOrbis(PosixMkdir(path, mode), "sceKernelMkdir");
}

SSPC_SYSV_ABI s32 KernelRmdir(const char* path) {
    return ToOrbis(PosixRmdir(path), "sceKernelRmdir");
}

SSPC_SYSV_ABI s32 KernelRename(const char* from, const char* to) {
    SSPC_LOG(Filesystem, Info, "rename {} -> {}", from, to);
    return ToOrbis(PosixRename(from, to), "sceKernelRename");
}

SSPC_SYSV_ABI s32 KernelUnlink(const char* path) {
    return ToOrbis(PosixUnlink(path), "sceKernelUnlink");
}

// libc.prx's _read/_write: POSIX results (-1 + errno).
SSPC_SYSV_ABI s64 Read(s32 fd, void* buf, u64 nbytes) {
    return CountedRead([&] { return PosixRead(fd, buf, nbytes); });
}

SSPC_SYSV_ABI s64 Write(s32 fd, const void* buf, u64 nbytes) {
    return PosixWrite(fd, buf, nbytes);
}

// libSceDiscMap (used by libSceFios2.prx): no disc, every request is served from the HDD package.
constexpr s32 ORBIS_DISC_MAP_ERROR_NO_BITMAP_INFO = static_cast<s32>(0x81100004);

SSPC_SYSV_ABI s32 DiscMapIsRequestOnHdd(char* /*path*/, s64 /*offset*/, s64 /*nbytes*/, s32* /*ret*/) {
    return ORBIS_DISC_MAP_ERROR_NO_BITMAP_INFO;
}

SSPC_SYSV_ABI s32 DiscMapFunc7C980FFB(char* /*path*/, s64 /*offset*/, s64 /*nbytes*/, s32* flags,
                                      s32* ret1, s32* ret2) {
    *flags = 0;
    *ret1 = 0;
    *ret2 = 0;
    return ORBIS_OK;
}

// SaveData (BRIDGE until Phase 3) mounts /savedata0: mirror its mounts into the Vfs.
void* g_mount2_original = nullptr;
void* g_umount_original = nullptr;

SSPC_SYSV_ABI s32 ObserveSaveDataMount2(const void* mount, char* result) {
    using Fn = SSPC_SYSV_ABI s32 (*)(const void*, char*);
    const s32 r = reinterpret_cast<Fn>(g_mount2_original)(mount, result);
    if (r == 0 && result != nullptr) {
        const std::string point(result, strnlen(result, 16)); // OrbisSaveDataMountResult::mount_point
        if (const auto m = g_host->BridgeMountOf(point)) {
            if (GetVfs().IsMounted(point)) {
                GetVfs().Unmount(point);
            }
            GetVfs().Mount(point, m->host, m->read_only);
        } else {
            SSPC_LOG(Filesystem, Error, "sceSaveDataMount2 -> {} but SaveData reports no host folder", point);
        }
    }
    SSPC_LOG(SaveData, Info, "sceSaveDataMount2 -> {:#x}{}", static_cast<u32>(r),
             r == 0 && result != nullptr ? fmt::format(" ({})", std::string(result, strnlen(result, 16))) : "");
    return r;
}

SSPC_SYSV_ABI s32 ObserveSaveDataUmount(const char* mount_point) {
    using Fn = SSPC_SYSV_ABI s32 (*)(const char*);
    const s32 r = reinterpret_cast<Fn>(g_umount_original)(mount_point);
    if (r == 0 && mount_point != nullptr) {
        GetVfs().Unmount(std::string(mount_point, strnlen(mount_point, 16)));
    }
    SSPC_LOG(SaveData, Info, "sceSaveDataUmount -> {:#x}", static_cast<u32>(r));
    return r;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"1G3lF1Gg1k8", "libkernel", "libkernel", "sceKernelOpen", Fn(&KernelOpen), nullptr, "fs"},
    {"UK2Tl2DWUns", "libkernel", "libkernel", "sceKernelClose", Fn(&KernelClose), nullptr, "fs"},
    {"Cg4srZ6TKbU", "libkernel", "libkernel", "sceKernelRead", Fn(&KernelRead), nullptr, "fs"},
    {"+r3rMFwItV4", "libkernel", "libkernel", "sceKernelPread", Fn(&KernelPread), nullptr, "fs"},
    {"4wSze92BhLI", "libkernel", "libkernel", "sceKernelWrite", Fn(&KernelWrite), nullptr, "fs"},
    {"nKWi-N2HBV4", "libkernel", "libkernel", "sceKernelPwrite", Fn(&KernelPwrite), nullptr, "fs"},
    {"oib76F-12fk", "libkernel", "libkernel", "sceKernelLseek", Fn(&KernelLseek), nullptr, "fs"},
    {"eV9wAD2riIA", "libkernel", "libkernel", "sceKernelStat", Fn(&KernelStat), nullptr, "fs"},
    {"kBwCPsYX-m4", "libkernel", "libkernel", "sceKernelFstat", Fn(&KernelFstat), nullptr, "fs"},
    {"fTx66l5iWIA", "libkernel", "libkernel", "sceKernelFsync", Fn(&KernelFsync), nullptr, "fs"},
    {"VW3TVZiM4-E", "libkernel", "libkernel", "sceKernelFtruncate", Fn(&KernelFtruncate), nullptr, "fs"},
    {"WlyEA-sLDf0", "libkernel", "libkernel", "sceKernelTruncate", Fn(&KernelTruncate), nullptr, "fs"},
    {"taRWhTJFTgE", "libkernel", "libkernel", "sceKernelGetdirentries", Fn(&KernelGetdirentries), nullptr, "fs"},
    {"1-LFLmRFxxM", "libkernel", "libkernel", "sceKernelMkdir", Fn(&KernelMkdir), nullptr, "fs"},
    {"naInUjYt3so", "libkernel", "libkernel", "sceKernelRmdir", Fn(&KernelRmdir), nullptr, "fs"},
    {"52NcYU9+lEo", "libkernel", "libkernel", "sceKernelRename", Fn(&KernelRename), nullptr, "fs"},
    {"AUXVxWeJU-A", "libkernel", "libkernel", "sceKernelUnlink", Fn(&KernelUnlink), nullptr, "fs"},
    {"DRuBt2pvICk", "libkernel", "libkernel", "_read", Fn(&Read), nullptr, "fs"},
    {"FxVZqBAA7ks", "libkernel", "libkernel", "_write", Fn(&Write), nullptr, "fs"},
    {"lbQKqsERhtE", "libSceDiscMap", "libSceDiscMap", "sceDiscMapIsRequestOnHDD", Fn(&DiscMapIsRequestOnHdd), nullptr, "fs"},
    {"fJgP+wqifno", "libSceDiscMap", "libSceDiscMap", "sceDiscMap (7C980FFB0AA27E7A)", Fn(&DiscMapFunc7C980FFB), nullptr, "fs"},
    {"0z45PIH+SNI", "libSceSaveData", "libSceSaveData", "sceSaveDataMount2", Fn(&ObserveSaveDataMount2), &g_mount2_original, "fs"},
    {"BMR4F-Uek3E", "libSceSaveData", "libSceSaveData", "sceSaveDataUmount", Fn(&ObserveSaveDataUmount), &g_umount_original, "fs"},
};

} // namespace

void NotifyHostWrite(u64 address, u64 size) {
    if (g_host != nullptr && size != 0) {
        g_host->InvalidateGuestMemory(address, size);
    }
}

void SetHost(FsHost* host) {
    g_host = host;
}

s32 ReserveDescriptor() {
    return g_host->ReserveDescriptor();
}

void ReleaseDescriptor(s32 fd) {
    g_host->ReleaseDescriptor(fd);
}

std::span<const Runtime::Export> FileSystemExports() {
    return kExports;
}

} // namespace SecondSon::Filesystem
