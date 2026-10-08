// SecondSonPC — subsystem self-test (--selftest): every NATIVE subsystem of Phase 1 is run next to
// its BRIDGE implementation (the shadPS4 code the Phase 0 oracle uses for the same import) on the
// same inputs, and the results are compared. Functions are taken from the binding tables by NID,
// exactly as the game gets them. The game is not started; the report goes to logs\selftest.txt.
//   FileSystem: two scratch folders (userdata\selftest\{native,bridge}) mounted as /sspctest,
//               plus read-only /app0 (the game folder);
//   PlayGo, AppContent, SystemService, UserService: return values, output buffers, error paths;
//   ParamSfo: SecondSonPC's param.sfo reader against shadPS4's PSF (not an import: start-up data).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <cmath>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <fmt/format.h>

#include "app/config.h"
#include "input/keyboard_mouse.h"
#include "base/log.h"
#include "bridge/shadps4/bridge_exports.h"
#include "bridge/shadps4/loader_host.h"
#include "common/singleton.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/kernel.h"
#include "core/user_settings.h"
#include "runtime/exports.h"
#include "runtime/filesystem/file_system.h"
#include "runtime/filesystem/vfs.h"
#include "runtime/psf/param_sfo.h"
#include "runtime/user/user_service.h"

namespace SecondSon::Bridge {

namespace {

// ---------------------------------------------------------------------------- plumbing

template <typename R, typename... A>
R Call(const void* f, A... a) {
    using Fn = R(SSPC_SYSV_ABI*)(A...);
    return reinterpret_cast<Fn>(const_cast<void*>(f))(a...);
}

const void* NativeOf(std::string_view nid) {
    for (const auto& e : Runtime::Exports()) {
        if (nid == e.nid && e.original == nullptr) {
            return e.function;
        }
    }
    return nullptr;
}

const void* BridgeOf(std::string_view nid) {
    for (const auto& e : BridgeExports()) {
        if (nid == e.nid) {
            return e.address;
        }
    }
    return nullptr;
}

/// One implementation of a subsystem: resolves NIDs from its table and records result lines.
struct Side {
    const char* name;
    bool native;
    std::vector<std::string> lines;

    /// `oracle_stub`: what the Phase 0 oracle runs for an import shadPS4 does not implement
    /// (COMMON_STUB: returns 0, does nothing).
    const void* F(const char* nid, const void* oracle_stub = nullptr) const {
        const void* f = native ? NativeOf(nid) : BridgeOf(nid);
        if (f == nullptr && oracle_stub != nullptr) {
            return oracle_stub;
        }
        if (f == nullptr) {
            SSPC_LOG(App, Critical, "selftest: {} has no function for {}", name, nid);
            std::quick_exit(3);
        }
        return f;
    }
    template <typename... Args>
    void Line(fmt::format_string<Args...> f, Args&&... args) {
        lines.push_back(fmt::format(f, std::forward<Args>(args)...));
    }
};

std::string Code(s64 r) {
    return r < 0 ? fmt::format("{:#x}", static_cast<u32>(r)) : std::to_string(r);
}

std::string Hex(const void* p, size_t n) {
    std::string s;
    const auto* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) {
        s += fmt::format("{:02x}", b[i]);
    }
    return s;
}

s32 Errno() {
    return *Libraries::Kernel::__Error();
}

struct Totals {
    int checks = 0;
    int failures = 0;
    std::vector<std::string> report;
};

/// Compares the two sides line by line; `expected` lists line prefixes whose difference is known
/// and documented (they are reported but not counted as failures).
void Compare(Totals& t, const char* subsystem, const Side& a, const Side& b,
             std::initializer_list<const char*> expected = {}) {
    int failures = 0;
    int known = 0;
    const size_t n = std::max(a.lines.size(), b.lines.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string la = i < a.lines.size() ? a.lines[i] : "<missing>";
        const std::string lb = i < b.lines.size() ? b.lines[i] : "<missing>";
        ++t.checks;
        if (la == lb) {
            continue;
        }
        const bool is_known = std::any_of(expected.begin(), expected.end(),
                                          [&](const char* p) { return la.starts_with(p); });
        t.report.push_back(fmt::format("  {} {}:\n      {:7} {}\n      {:7} {}",
                                       is_known ? "KNOWN" : "DIFF ", subsystem, a.name, la, b.name, lb));
        if (is_known) {
            ++known;
        } else {
            ++failures;
        }
    }
    t.failures += failures;
    t.report.push_back(fmt::format("{:14} {:4} checks, {} different, {} known differences - {}",
                                   subsystem, n, failures, known, failures == 0 ? "PASS" : "FAIL"));
}

// ---------------------------------------------------------------------------- FileSystem

SSPC_SYSV_ABI s32 CommonStubTruncate(const char*, s64) {
    return 0;
}

constexpr s32 O_RDONLY = 0x0, O_WRONLY = 0x1, O_RDWR = 0x2, O_CREAT = 0x200, O_TRUNC = 0x400,
              O_EXCL = 0x800, O_DIRECTORY = 0x20000;

void StatLine(Side& s, const char* what, s32 r, const Filesystem::OrbisKernelStat& st, bool size) {
    if (r < 0) {
        s.Line("{} -> {}", what, Code(r));
        return;
    }
    s.Line("{} -> {} mode {:o} nlink {} size {} blocks {} blksize {}", what, r, st.st_mode,
           st.st_nlink, size ? std::to_string(st.st_size) : "-", size ? std::to_string(st.st_blocks) : "-",
           st.st_blksize);
}

// Directory entry formats (FreeBSD dirent; PFS dirent of the package file system).
#pragma pack(push, 1)
struct NormalDirent {
    u32 d_fileno;
    u16 d_reclen;
    u8 d_type;
    u8 d_namlen;
    char d_name[256];
};
struct PfsDirent {
    u32 d_fileno;
    u32 d_type;
    u32 d_namlen;
    u32 d_reclen;
    char d_name[256];
};
#pragma pack(pop)

/// Directory entries as names/types/record lengths (d_fileno differs by design: inode numbers).
std::string Entries(const char* buf, s64 len, bool pfs) {
    std::string s;
    s64 at = 0;
    while (at < len) {
        if (pfs) {
            PfsDirent d{};
            std::memcpy(&d, buf + at, std::min<s64>(sizeof(d), len - at));
            if (d.d_reclen == 0) {
                break;
            }
            s += fmt::format("[{} t{} r{}]", std::string(d.d_name, std::min<u32>(d.d_namlen, 255)), d.d_type, d.d_reclen);
            at += d.d_reclen;
        } else {
            NormalDirent d{};
            std::memcpy(&d, buf + at, std::min<s64>(sizeof(d), len - at));
            if (d.d_reclen == 0) {
                break;
            }
            s += fmt::format("[{} t{} r{}]", std::string(d.d_name, d.d_namlen), d.d_type, d.d_reclen);
            at += d.d_reclen;
        }
    }
    return s;
}

void FileSystemScript(Side& s) {
    using Stat = Filesystem::OrbisKernelStat;
    const auto open = s.F("1G3lF1Gg1k8"), close = s.F("UK2Tl2DWUns"), read = s.F("Cg4srZ6TKbU"),
               pread = s.F("+r3rMFwItV4"), write = s.F("4wSze92BhLI"), pwrite = s.F("nKWi-N2HBV4"),
               lseek = s.F("oib76F-12fk"), stat = s.F("eV9wAD2riIA"), fstat = s.F("kBwCPsYX-m4"),
               fsync = s.F("fTx66l5iWIA"), ftruncate = s.F("VW3TVZiM4-E"), truncate = s.F("WlyEA-sLDf0", reinterpret_cast<const void*>(&CommonStubTruncate)),
               getdirentries = s.F("taRWhTJFTgE"), mkdir = s.F("1-LFLmRFxxM"), rmdir = s.F("naInUjYt3so"),
               rename = s.F("52NcYU9+lEo"), unlink = s.F("AUXVxWeJU-A"), posix_read = s.F("DRuBt2pvICk"),
               posix_write = s.F("FxVZqBAA7ks");
    const auto Open = [&](const std::string& p, s32 flags, u16 mode) { return Call<s32>(open, p.c_str(), flags, mode); };
    const auto Close = [&](s32 fd) { return Call<s32>(close, fd); };
    const auto StatOf = [&](const std::string& p, Stat& st) { return Call<s32>(stat, p.c_str(), &st); };

    std::vector<char> pattern(1000);
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<char>(i * 7 + 3);
    }
    std::vector<char> buf(4096);
    Stat st{};
    const std::string d = "/sspctest/d", f = d + "/f.bin", g = d + "/g.bin";

    s.Line("open missing -> {}", Code(Open("/sspctest/missing.bin", O_RDONLY, 0)));
    s.Line("stat missing -> {}", Code(StatOf("/sspctest/missing.bin", st)));
    s.Line("mkdir -> {}", Code(Call<s32>(mkdir, d.c_str(), u16{0777})));
    s.Line("mkdir again -> {}", Code(Call<s32>(mkdir, d.c_str(), u16{0777})));
    s.Line("open dir as file O_WRONLY -> {}", Code(Open(d, O_WRONLY, 0)) );

    s32 fd = Open(f, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    s.Line("create -> {}", fd >= 0 ? "fd" : Code(fd));
    s.Line("open O_CREAT|O_EXCL existing -> {}", Code(Open(f, O_WRONLY | O_CREAT | O_EXCL, 0666)));
    s.Line("write 1000 -> {}", Code(Call<s64>(write, fd, static_cast<const void*>(pattern.data()), u64{1000})));
    s.Line("lseek 0 CUR -> {}", Code(Call<s64>(lseek, fd, s64{0}, s32{1})));
    s.Line("lseek -10 END -> {}", Code(Call<s64>(lseek, fd, s64{-10}, s32{2})));
    s.Line("lseek -1 SET -> {}", Code(Call<s64>(lseek, fd, s64{-1}, s32{0})));
    s.Line("lseek whence 7 -> {}", Code(Call<s64>(lseek, fd, s64{0}, s32{7})));
    s.Line("pwrite 16 @2000 -> {}", Code(Call<s64>(pwrite, fd, static_cast<const void*>(pattern.data()), u64{16}, s64{2000})));
    s.Line("pwrite @-1 -> {}", Code(Call<s64>(pwrite, fd, static_cast<const void*>(pattern.data()), u64{16}, s64{-1})));
    StatLine(s, "fstat", Call<s32>(fstat, fd, &st), st, true);
    s.Line("read on O_WRONLY -> {}", Code(Call<s64>(read, fd, static_cast<void*>(buf.data()), u64{10})));
    s.Line("fsync -> {}", Code(Call<s32>(fsync, fd)));
    s.Line("close -> {}", Code(Close(fd)));
    s.Line("close again -> {}", Code(Close(fd)));

    fd = Open(f, O_RDONLY, 0);
    s.Line("open O_RDONLY -> {}", fd >= 0 ? "fd" : Code(fd));
    s64 r = Call<s64>(read, fd, static_cast<void*>(buf.data()), u64{4096});
    s.Line("read 4096 -> {} {}", Code(r), r > 0 ? Hex(buf.data() + 990, 30) : "");
    r = Call<s64>(pread, fd, static_cast<void*>(buf.data()), u64{20}, s64{990});
    s.Line("pread 20 @990 -> {} {}", Code(r), r > 0 ? Hex(buf.data(), static_cast<size_t>(r)) : "");
    s.Line("pread @-5 -> {}", Code(Call<s64>(pread, fd, static_cast<void*>(buf.data()), u64{20}, s64{-5})));
    s.Line("read at EOF -> {}", Code(Call<s64>(read, fd, static_cast<void*>(buf.data()), u64{16})));
    *Libraries::Kernel::__Error() = 0; // the oracle leaves errno as it is here
    s.Line("write on O_RDONLY -> {}", Code(Call<s64>(write, fd, static_cast<const void*>(pattern.data()), u64{4})));
    s.Line("lseek 5000 SET -> {}", Code(Call<s64>(lseek, fd, s64{5000}, s32{0})));
    s.Line("read past EOF -> {}", Code(Call<s64>(read, fd, static_cast<void*>(buf.data()), u64{16})));
    s.Line("close -> {}", Code(Close(fd)));

    StatLine(s, "stat file", StatOf(f, st), st, true);
    StatLine(s, "stat dir", StatOf(d, st), st, false);
    s.Line("truncate 100 -> {}", Code(Call<s32>(truncate, f.c_str(), s64{100})));
    StatLine(s, "stat after truncate", StatOf(f, st), st, true);
    s.Line("truncate -1 -> {}", Code(Call<s32>(truncate, f.c_str(), s64{-1})));
    s.Line("truncate missing -> {}", Code(Call<s32>(truncate, "/sspctest/missing.bin", s64{1})));
    fd = Open(f, O_RDWR, 0);
    s.Line("ftruncate 50 -> {}", Code(Call<s32>(ftruncate, fd, s64{50})));
    StatLine(s, "fstat after ftruncate", Call<s32>(fstat, fd, &st), st, true);
    s.Line("close -> {}", Code(Close(fd)));

    s.Line("rename -> {}", Code(Call<s32>(rename, f.c_str(), g.c_str())));
    s.Line("stat old name -> {}", Code(StatOf(f, st)));
    StatLine(s, "stat new name", StatOf(g, st), st, true);
    s.Line("rename missing -> {}", Code(Call<s32>(rename, f.c_str(), g.c_str())));

    fd = Open(d, O_RDONLY | O_DIRECTORY, 0);
    s.Line("open dir -> {}", fd >= 0 ? "fd" : Code(fd));
    s64 base = -1;
    r = Call<s64>(getdirentries, fd, buf.data(), u64{4096}, &base);
    s.Line("getdirentries -> {} base {} {}", Code(r), base, r > 0 ? Entries(buf.data(), r, false) : "");
    r = Call<s64>(getdirentries, fd, buf.data(), u64{4096}, &base);
    s.Line("getdirentries again -> {} base {}", Code(r), base);
    s.Line("getdirentries small buffer -> {}", Code(Call<s64>(getdirentries, fd, buf.data(), u64{8}, &base)));
    s.Line("close -> {}", Code(Close(fd)));

    s.Line("rmdir non-empty -> {}", Code(Call<s32>(rmdir, d.c_str())));
    s.Line("unlink -> {}", Code(Call<s32>(unlink, g.c_str())));
    s.Line("unlink again -> {}", Code(Call<s32>(unlink, g.c_str())));
    s.Line("unlink dir -> {}", Code(Call<s32>(unlink, d.c_str())));
    s.Line("rmdir -> {}", Code(Call<s32>(rmdir, d.c_str())));
    s.Line("rmdir again -> {}", Code(Call<s32>(rmdir, d.c_str())));

    // Read-only /app0 (PFS directory format).
    const std::string sfo = "/app0/sce_sys/param.sfo";
    fd = Open(sfo, O_RDONLY, 0);
    r = Call<s64>(read, fd, static_cast<void*>(buf.data()), u64{64});
    s.Line("app0 read 64 -> {} {}", Code(r), r > 0 ? Hex(buf.data(), static_cast<size_t>(r)) : "");
    StatLine(s, "app0 fstat", Call<s32>(fstat, fd, &st), st, true);
    s.Line("app0 close -> {}", Code(Close(fd)));
    StatLine(s, "app0 stat", StatOf(sfo, st), st, true);
    StatLine(s, "app0 stat dir", StatOf("/app0/sce_sys", st), st, false);
    s.Line("app0 open O_WRONLY -> {}", Code(Open(sfo, O_WRONLY, 0)));
    s.Line("app0 create -> {}", Code(Open("/app0/sspc_new.bin", O_WRONLY | O_CREAT, 0666)));
    s.Line("app0 mkdir -> {}", Code(Call<s32>(mkdir, "/app0/sspc_dir", u16{0777})));
    s.Line("app0 unlink -> {}", Code(Call<s32>(unlink, sfo.c_str())));
    fd = Open("/app0//sce_sys//param.sfo", O_RDONLY, 0);
    s.Line("app0 double slash -> {}", fd >= 0 ? "fd" : Code(fd));
    if (fd >= 0) {
        Close(fd);
    }
    fd = Open("/app0/sce_sys", O_RDONLY | O_DIRECTORY, 0);
    base = -1;
    r = Call<s64>(getdirentries, fd, buf.data(), u64{4096}, &base);
    s.Line("app0 getdirentries -> {} base {} {}", Code(r), base, r > 0 ? Entries(buf.data(), r, false) : "");
    s.Line("app0 lseek dir 0 SET -> {}", Code(Call<s64>(lseek, fd, s64{0}, s32{0})));
    r = Call<s64>(read, fd, static_cast<void*>(buf.data()), u64{4096});
    s.Line("app0 read dir -> {} {}", Code(r), r > 0 ? Entries(buf.data(), r, true) : "");
    s.Line("app0 close dir -> {}", Code(Close(fd)));

    s.Line("open long path -> {}", Code(Open("/sspctest/" + std::string(300, 'x'), O_RDONLY, 0)));
    s.Line("stat unmounted -> {}", Code(StatOf("/nowhere/file", st)));
    *Libraries::Kernel::__Error() = 0;
    s.Line("_read bad fd -> {} errno {}", Call<s64>(posix_read, s32{-5}, static_cast<void*>(buf.data()), u64{1}), Errno());
    *Libraries::Kernel::__Error() = 0;
    s.Line("_write bad fd -> {} errno {}", Call<s64>(posix_write, s32{9999}, static_cast<const void*>(buf.data()), u64{1}), Errno());
    s.Line("close bad fd -> {}", Code(Close(9999)));
}

void TestFileSystem(Totals& t, const App::Config& cfg) {
    const auto root = cfg.user_data_dir / "selftest";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "native", ec);
    std::filesystem::create_directories(root / "bridge", ec);
    // /app0 is a read-only copy of sce_sys for the duration of the test: a write that slipped
    // through can never reach the game folder.
    std::filesystem::create_directories(root / "app0", ec);
    std::filesystem::copy(cfg.game_dir / "sce_sys", root / "app0" / "sce_sys",
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
        SSPC_LOG(App, Critical, "selftest: cannot copy sce_sys: {}", ec.message());
        std::quick_exit(3);
    }
    auto& vfs = Filesystem::GetVfs();
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    vfs.Unmount("/app0");
    mnt->Unmount("/app0");
    vfs.Mount("/app0", root / "app0", true);
    mnt->Mount(root / "app0", "/app0", true);
    vfs.Mount("/sspctest", root / "native", false);
    mnt->Mount(root / "bridge", "/sspctest", false);

    Side native{"NATIVE", true}, bridge{"BRIDGE", false};
    FileSystemScript(native);
    FileSystemScript(bridge);
    // sceKernelTruncate is COMMON_STUB in the oracle (shadPS4 has none): it returns 0 and keeps
    // the file as it is; SecondSonPC implements it (docs/MIGRATION.md, M2).
    Compare(t, "FileSystem", native, bridge,
            {"truncate -1", "truncate missing", "stat after truncate",
             // The oracle's File::Write fails without setting errno (the code is whatever errno
             // held before, 0 here); SecondSonPC returns EBADF like FreeBSD.
             "write on O_RDONLY"});

    vfs.Unmount("/sspctest");
    mnt->Unmount("/sspctest");
    vfs.Unmount("/app0");
    mnt->Unmount("/app0");
    vfs.Mount("/app0", cfg.game_dir, true);
    mnt->Mount(cfg.game_dir, "/app0", true);
    std::filesystem::remove_all(root, ec);
}

// ---------------------------------------------------------------------------- PlayGo

struct PlayGoInit {
    const void* buf;
    u32 size;
    u32 reserved;
};

void PlayGoScript(Side& s) {
    const auto init = s.F("ts6GlZOKRrE"), open = s.F("M1Gma1ocrGE"), close = s.F("Uco1I0dlDi8"),
               eta = s.F("v6EZ-YWRdMs"), locus = s.F("uWIYLFkkwqk"), progress = s.F("-RJWNMK3fC8"),
               todo = s.F("Nn7zKwnA5q0"), set_mask = s.F("LosLlHOpNqQ"), set_todo = s.F("gUPGiOQ1tmQ"),
               terminate = s.F("MPe0EeBGM-E");
    static std::vector<u8> work(0x200000);
    u32 h = 0xFFFF;
    s.Line("open before init -> {}", Code(Call<s32>(open, &h, static_cast<const void*>(nullptr))));
    PlayGoInit p{nullptr, 0x200000, 0};
    s.Line("init null buf -> {}", Code(Call<s32>(init, &p)));
    p = {work.data(), 0x1000, 0};
    s.Line("init small buf -> {}", Code(Call<s32>(init, &p)));
    p = {work.data(), 0x200000, 0};
    s.Line("init -> {}", Code(Call<s32>(init, &p)));
    s.Line("init again -> {}", Code(Call<s32>(init, &p)));
    s.Line("open param -> {}", Code(Call<s32>(open, &h, static_cast<const void*>(&p))));
    s.Line("open null out -> {}", Code(Call<s32>(open, static_cast<u32*>(nullptr), static_cast<const void*>(nullptr))));
    s32 r = Call<s32>(open, &h, static_cast<const void*>(nullptr));
    s.Line("open -> {} handle {}", Code(r), h);

    u16 last_valid = 0;
    for (u16 id = 0; id < 512; ++id) {
        s8 l = -1;
        r = Call<s32>(locus, h, &id, u32{1}, &l);
        u64 pr[2] = {~0ull, ~0ull};
        const s32 rp = Call<s32>(progress, h, &id, u32{1}, static_cast<void*>(pr));
        s64 e = -1;
        const s32 re = Call<s32>(eta, h, &id, u32{1}, &e);
        s.Line("chunk {} locus {} {} progress {} {}/{} eta {} {}", id, Code(r), l, Code(rp), pr[0], pr[1], Code(re), e);
        if (r < 0) {
            break;
        }
        last_valid = id;
    }
    std::vector<u16> ids;
    for (u16 i = 0; i <= last_valid; ++i) {
        ids.push_back(i);
    }
    std::vector<s8> loci(ids.size(), -1);
    s.Line("locus all {} -> {}", ids.size(), Code(Call<s32>(locus, h, ids.data(), static_cast<u32>(ids.size()), loci.data())));
    u64 pr[2] = {};
    r = Call<s32>(progress, h, ids.data(), static_cast<u32>(ids.size()), static_cast<void*>(pr));
    s.Line("progress all -> {} {}/{}", Code(r), pr[0], pr[1]);
    s8 l = 0;
    s.Line("locus count 0 -> {}", Code(Call<s32>(locus, h, ids.data(), u32{0}, &l)));
    s.Line("locus null -> {}", Code(Call<s32>(locus, h, static_cast<const u16*>(nullptr), u32{1}, &l)));
    s.Line("locus bad handle -> {}", Code(Call<s32>(locus, h + 1, ids.data(), u32{1}, &l)));
    u8 list[8 * 4] = {};
    u32 entries = 77;
    r = Call<s32>(todo, h, static_cast<void*>(list), u32{8}, &entries);
    s.Line("todo -> {} entries {}", Code(r), entries);
    s.Line("todo count 0 -> {}", Code(Call<s32>(todo, h, static_cast<void*>(list), u32{0}, &entries)));
    s.Line("set todo -> {}", Code(Call<s32>(set_todo, h, static_cast<const void*>(list), u32{1})));
    s.Line("set todo null -> {}", Code(Call<s32>(set_todo, h, static_cast<const void*>(nullptr), u32{1})));
    s.Line("set mask -> {}", Code(Call<s32>(set_mask, h, u64{0x0080000000000000ull})));
    s.Line("set mask bad handle -> {}", Code(Call<s32>(set_mask, u32{5}, u64{1})));
    s.Line("close bad handle -> {}", Code(Call<s32>(close, u32{5})));
    s.Line("close -> {}", Code(Call<s32>(close, h)));
    s.Line("terminate -> {}", Code(Call<s32>(terminate)));
    s.Line("terminate again -> {}", Code(Call<s32>(terminate)));
}

void TestPlayGo(Totals& t) {
    Side native{"NATIVE", true}, bridge{"BRIDGE", false};
    PlayGoScript(native);
    PlayGoScript(bridge);
    Compare(t, "PlayGo", native, bridge);
}

// ---------------------------------------------------------------------------- AppContent

void AppContentScript(Side& s) {
    const auto init = s.F("R9lA82OraNs"), info = s.F("m47juOmH0VE");
    u8 init_param[32] = {}, boot_param[32] = {};
    s.Line("init -> {}", Code(Call<s32>(init, static_cast<const void*>(init_param), static_cast<void*>(boot_param))));
    s.Line("boot param after init {}", Hex(boot_param, sizeof(boot_param)));
    char label[17] = "SSPCTESTLABEL000";
    u8 out[64] = {};
    s.Line("addcont info -> {} {}", Code(Call<s32>(info, u32{0}, static_cast<const void*>(label), static_cast<void*>(out))), Hex(out, 24));
    s.Line("addcont info null -> {}", Code(Call<s32>(info, u32{0}, static_cast<const void*>(nullptr), static_cast<void*>(out))));
}

void TestAppContent(Totals& t) {
    Side native{"NATIVE", true}, bridge{"BRIDGE", false};
    AppContentScript(native);
    AppContentScript(bridge);
    Compare(t, "AppContent", native, bridge);
}

// ---------------------------------------------------------------------------- SystemService

void SystemServiceScript(Side& s) {
    const auto get_int = s.F("fZo48un7LK4"), get_string = s.F("SsC-m-S9JTA"), status = s.F("rPo6tV8D9bM"),
               receive = s.F("656LMQSrg6U"), hide = s.F("Vo5V8KAwCmk"), safe_area = s.F("1n37q1Bvc5Y");
    for (const s32 id : {1, 2, 3, 4, 5, 7, 1000, 0, 999}) {
        s32 v = 0x5A5A5A5A;
        const s32 r = Call<s32>(get_int, id, &v);
        s.Line("ParamGetInt {} -> {} value {}", id, Code(r), v);
    }
    s.Line("ParamGetInt null -> {}", Code(Call<s32>(get_int, s32{1}, static_cast<s32*>(nullptr))));
    char name[0x41];
    std::memset(name, 'Z', sizeof(name));
    const s32 r = Call<s32>(get_string, s32{6}, name, u64{sizeof(name)});
    s.Line("ParamGetString SystemName -> {} \"{}\"", Code(r), std::string(name, strnlen(name, sizeof(name))));
    u8 st[64];
    std::memset(st, 0xEE, sizeof(st));
    s.Line("GetStatus -> {} {}", Code(Call<s32>(status, static_cast<void*>(st))), Hex(st, 9));
    s.Line("GetStatus null -> {}", Code(Call<s32>(status, static_cast<void*>(nullptr))));
    static u8 event[8196 + 64];
    s.Line("ReceiveEvent -> {}", Code(Call<s32>(receive, static_cast<void*>(event))));
    s.Line("ReceiveEvent null -> {}", Code(Call<s32>(receive, static_cast<void*>(nullptr))));
    float ratio = -1.0f;
    u8 area[132] = {};
    s32 ra = Call<s32>(safe_area, static_cast<void*>(area));
    std::memcpy(&ratio, area, sizeof(ratio));
    s.Line("GetDisplaySafeAreaInfo -> {} ratio {}", Code(ra), ratio);
    s.Line("GetDisplaySafeAreaInfo null -> {}", Code(Call<s32>(safe_area, static_cast<void*>(nullptr))));
    s.Line("HideSplashScreen -> {}", Code(Call<s32>(hide)));
}

void TestSystemService(Totals& t) {
    Side native{"NATIVE", true}, bridge{"BRIDGE", false};
    SystemServiceScript(native);
    SystemServiceScript(bridge);
    // ParamGetInt/ParamGetString were NATIVE already in Phase 0 (the oracle ran SecondSonPC's;
    // the game asks only for Lang). shadPS4's own versions return constants for TimeZone (+120)
    // and Summertime (1), OK and 0 for unknown ids, and do not write the SystemName buffer.
    // SecondSonPC: host time zone, SCE_SYSTEM_SERVICE_ERROR_PARAMETER for unknown ids, "" for
    // SystemName (SSPC-001 workaround, docs/KNOWN_ISSUES.md).
    Compare(t, "SystemService", native, bridge,
            {"ParamGetInt 4 ", "ParamGetInt 5 ", "ParamGetInt 0 ", "ParamGetInt 999 ",
             "ParamGetString SystemName"});
}

// ---------------------------------------------------------------------------- UserService

void UserServiceScript(Side& s) {
    const auto init = s.F("j3YMu1MVNNo"), initial = s.F("CdWp0oHWGr0"), list = s.F("fPhymKNvK-A"),
               terminate = s.F("bwFjS+bX9mA");
    u8 params[16] = {};
    s.Line("Initialize -> {}", Code(Call<s32>(init, static_cast<const void*>(params))));
    s32 id = 0;
    s.Line("GetInitialUser -> {} user {}", Code(Call<s32>(initial, &id)), id);
    s.Line("GetInitialUser null -> {}", Code(Call<s32>(initial, static_cast<s32*>(nullptr))));
    s32 ids[4] = {7, 7, 7, 7};
    s.Line("GetLoginUserIdList -> {} {} {} {} {}", Code(Call<s32>(list, static_cast<void*>(ids))), ids[0], ids[1], ids[2], ids[3]);
    s.Line("GetLoginUserIdList null -> {}", Code(Call<s32>(list, static_cast<void*>(nullptr))));
    s.Line("Terminate -> {}", Code(Call<s32>(terminate)));
}

void TestUserService(Totals& t) {
    Side native{"NATIVE", true}, bridge{"BRIDGE", false};
    UserServiceScript(native);
    UserServiceScript(bridge);
    Compare(t, "UserService", native, bridge);

    // Events: with the native UserService the input layer's logins come to SecondSonPC (the
    // BRIDGE queue stays empty by design), so the native event is checked against shadPS4's
    // user manager instead: the window's first controller check logged in the user of slot 1.
    struct {
        s32 type;
        s32 user;
    } e{-1, -1};
    const auto get_event = NativeOf("yH17Q6NWtVg");
    const s32 r = Call<s32>(get_event, static_cast<void*>(&e));
    const auto logged = UserManagement.GetLoggedInUsers();
    const s32 expected_user = logged[0] != nullptr ? logged[0]->user_id : -1;
    ++t.checks;
    const bool ok = r == 0 && e.type == 0 && e.user == expected_user;
    t.report.push_back(fmt::format("UserService    event: GetEvent -> {} type {} user {}, shadPS4 slot 1 user {} - {}",
                                   Code(r), e.type, e.user, expected_user, ok ? "PASS" : "FAIL"));
    t.failures += ok ? 0 : 1;
    const s32 r2 = Call<s32>(get_event, static_cast<void*>(&e));
    ++t.checks;
    const bool ok2 = r2 == static_cast<s32>(0x80960007);
    t.report.push_back(fmt::format("UserService    event: second GetEvent -> {} (expected NO_EVENT 0x80960007) - {}",
                                   Code(r2), ok2 ? "PASS" : "FAIL"));
    t.failures += ok2 ? 0 : 1;
}

// ---------------------------------------------------------------------------- Pad

/// Pad data line without the timestamp (bytes 80..87): the two sides read the clock at different
/// moments.
std::string PadDataLine(const u8* d) {
    float f[10];
    std::memcpy(f, d + 12, sizeof(f));
    u32 buttons;
    std::memcpy(&buttons, d, sizeof(buttons));
    return fmt::format("buttons {:#x} sticks {} {} {} {} l2/r2 {} {} q ({:.4f} {:.4f} {:.4f} {:.4f}) "
                       "a ({:.4f} {:.4f} {:.4f}) w ({:.4f} {:.4f} {:.4f}) touch {} connected {} count {} "
                       "rest {}{}",
                       buttons, d[4], d[5], d[6], d[7], d[8], d[9], f[0], f[1], f[2], f[3], f[4], f[5],
                       f[6], f[7], f[8], f[9], d[52], d[76], d[104], Hex(d + 52, 28), Hex(d + 88, 32));
}

void PadScript(Side& s) {
    const auto init = s.F("hv1luiJrqQM"), open = s.F("xk0AcarP3V4"), close = s.F("6ncge5+l5Qs"),
               read = s.F("YndgXqQVV7c"), info = s.F("gjP9-KQzoUk"), light = s.F("RR4novUEENY"),
               vibration = s.F("yFVnOdGxvZY");
    const void* none = nullptr;
    s.Line("Open before Init -> {}", Code(Call<s32>(open, 1000, 0, 0, none)));
    s.Line("Init -> {}", Code(Call<s32>(init)));
    const s32 h = Call<s32>(open, 1000, 0, 0, none);
    s.Line("Open user 1000 standard -> {}", Code(h));
    s.Line("Open again -> {}", Code(Call<s32>(open, 1000, 0, 0, none)));
    s.Line("Open user -1 -> {}", Code(Call<s32>(open, -1, 0, 0, none)));
    s.Line("Open user 5000 -> {}", Code(Call<s32>(open, 5000, 0, 0, none)));
    s.Line("Open system user standard -> {}", Code(Call<s32>(open, 255, 0, 0, none)));
    s.Line("Open user 1000 remote control -> {}", Code(Call<s32>(open, 1000, 16, 0, none)));
    u8 ci[28];
    std::memset(ci, 0xAB, sizeof(ci));
    s.Line("GetControllerInformation -> {} {}", Code(Call<s32>(info, h, static_cast<void*>(ci))), Hex(ci, sizeof(ci)));
    u8 data[120];
    std::memset(data, 0xAB, sizeof(data));
    s.Line("ReadState -> {}", Code(Call<s32>(read, h, static_cast<void*>(data))));
    s.Line("  {}", PadDataLine(data));
    s.Line("ReadState null -> {}", Code(Call<s32>(read, h, static_cast<void*>(nullptr))));
    s.Line("ReadState bad handle -> {}", Code(Call<s32>(read, 9999, static_cast<void*>(data))));
    const u8 dark[4] = {0, 0, 0, 0}, blue[4] = {0, 0, 255, 0};
    s.Line("SetLightBar dark -> {}", Code(Call<s32>(light, h, static_cast<const void*>(dark))));
    s.Line("SetLightBar blue -> {}", Code(Call<s32>(light, h, static_cast<const void*>(blue))));
    s.Line("SetLightBar null -> {}", Code(Call<s32>(light, h, none)));
    s.Line("SetLightBar bad handle -> {}", Code(Call<s32>(light, 9999, static_cast<const void*>(blue))));
    const u8 still[2] = {0, 0};
    s.Line("SetVibration 0 -> {}", Code(Call<s32>(vibration, h, static_cast<const void*>(still))));
    s.Line("SetVibration null -> {}", Code(Call<s32>(vibration, h, none)));
    s.Line("SetVibration bad handle -> {}", Code(Call<s32>(vibration, 9999, static_cast<const void*>(still))));
    s.Line("Close -> {}", Code(Call<s32>(close, h)));
    s.Line("Close again -> {}", Code(Call<s32>(close, h)));
    s.Line("GetControllerInformation closed -> {}", Code(Call<s32>(info, h, static_cast<void*>(ci))));
}

void TestPad(Totals& t) {
    Side native{"NATIVE", true}, bridge{"BRIDGE", false};
    PadScript(native);
    PadScript(bridge);
    Compare(t, "Pad", native, bridge);
}

// ---------------------------------------------------------------------------- Input (native)

/// Behaviour of the keyboard/mouse controller against expected values (there is no BRIDGE
/// counterpart: shadPS4's keyboard layer is a different design).
class InputChecks {
public:
    explicit InputChecks(Totals& t) : t_{t} {}
    void Check(const std::string& what, bool ok, const std::string& detail) {
        ++t_.checks;
        ++checks_;
        if (!ok) {
            ++t_.failures;
            ++failures_;
            t_.report.push_back(fmt::format("  DIFF  Input: {}: {}", what, detail));
        }
    }
    void Summary() {
        t_.report.push_back(fmt::format("{:14} {:4} checks, {} different, 0 known differences - {}",
                                        "Input", checks_, failures_, failures_ == 0 ? "PASS" : "FAIL"));
    }

private:
    Totals& t_;
    int checks_ = 0;
    int failures_ = 0;
};

SDL_Event KeyEvent(SDL_Scancode sc, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}

SDL_Event MotionEvent(float dx, float dy) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    return e;
}

SDL_Event ButtonEvent(u8 button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = button;
    e.button.down = down;
    return e;
}

/// shadPS4's gyro integration (input/controller.cpp, CalculateOrientation): q += q * omega / 2 * dt.
Input::Quat IntegrateLikeBridge(Input::Quat q, const Input::Vec3& w, float dt) {
    const Input::Quat d = Input::Multiply(q, Input::Quat{w.x, w.y, w.z, 0.0f});
    q = {q.x + 0.5f * d.x * dt, q.y + 0.5f * d.y * dt, q.z + 0.5f * d.z * dt, q.w + 0.5f * d.w * dt};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

void TestInput(Totals& t) {
    namespace Button = Input::Button;
    InputChecks c{t};
    Input::KeyboardMouse km;
    km.Configure(Input::DefaultSettings());
    km.AttachWindow(nullptr, true); // logical capture: mouse events count
    u64 now = 1'000'000;
    const auto read = [&](u64 step_us = 10'000) {
        now += step_us;
        return km.Read(now);
    };
    const auto sticks = [](const Input::PadState& s) {
        return fmt::format("l {} {} r {} {}", s.lx, s.ly, s.rx, s.ry);
    };

    auto s = read();
    c.Check("rest", s.buttons == 0 && s.lx == 128 && s.ly == 128 && s.rx == 128 && s.ry == 128 &&
                        s.l2 == 0 && s.r2 == 0 && !s.motion.active && s.motion.acceleration.y == Input::kGravity &&
                        s.motion.orientation.w == 1.0f && !s.touch_active,
            sticks(s));

    km.OnEvent(KeyEvent(SDL_SCANCODE_W, true));
    s = read();
    c.Check("W = left stick up", s.lx == 128 && s.ly == 0, sticks(s));
    km.OnEvent(KeyEvent(SDL_SCANCODE_D, true));
    s = read();
    c.Check("W+D = diagonal on the circle", s.lx == 218 && s.ly == 37, sticks(s));
    km.OnEvent(KeyEvent(SDL_SCANCODE_A, true));
    s = read();
    c.Check("A after D: last pressed wins", s.lx == 37 && s.ly == 37, sticks(s));
    km.OnEvent(KeyEvent(SDL_SCANCODE_A, false));
    s = read();
    c.Check("A released: D again", s.lx == 218, sticks(s));
    km.OnEvent(KeyEvent(SDL_SCANCODE_LCTRL, true));
    s = read();
    c.Check("walk = half deflection", s.lx == 173 && s.ly == 82, sticks(s));
    for (auto sc : {SDL_SCANCODE_LCTRL, SDL_SCANCODE_W, SDL_SCANCODE_D}) {
        km.OnEvent(KeyEvent(sc, false));
    }
    s = read();
    c.Check("all released", s.lx == 128 && s.ly == 128, sticks(s));

    km.OnEvent(KeyEvent(SDL_SCANCODE_SPACE, true));
    s = read();
    c.Check("Space = cross", s.buttons == Button::Cross, fmt::format("{:#x}", s.buttons));
    km.OnEvent(KeyEvent(SDL_SCANCODE_SPACE, false));
    km.OnEvent(KeyEvent(SDL_SCANCODE_SPACE, true, true)); // auto-repeat of a released key
    s = read();
    c.Check("key repeat is not a press", s.buttons == 0, fmt::format("{:#x}", s.buttons));
    km.OnEvent(KeyEvent(SDL_SCANCODE_RETURN, true));
    km.OnEvent(KeyEvent(SDL_SCANCODE_ESCAPE, true));
    km.OnEvent(KeyEvent(SDL_SCANCODE_RETURN, false));
    s = read();
    c.Check("Enter released, Esc held: options stays", s.buttons == Button::Options,
            fmt::format("{:#x}", s.buttons));
    km.OnEvent(KeyEvent(SDL_SCANCODE_ESCAPE, false));

    km.OnEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
    km.OnEvent(ButtonEvent(SDL_BUTTON_RIGHT, true));
    s = read();
    c.Check("mouse buttons = R2 + L2", s.buttons == (Button::R2 | Button::L2) && s.r2 == 255 && s.l2 == 255,
            fmt::format("{:#x} {} {}", s.buttons, s.l2, s.r2));
    km.OnEvent(ButtonEvent(SDL_BUTTON_LEFT, false));
    km.OnEvent(ButtonEvent(SDL_BUTTON_RIGHT, false));

    // Mouse -> right stick: speed over the read interval.
    s = read();
    km.OnEvent(MotionEvent(15.0f, 0.0f)); // 1500 counts/s over 10 ms: full deflection
    s = read();
    c.Check("fast mouse = full right", s.rx == 255 && s.ry == 128, sticks(s));
    km.OnEvent(MotionEvent(3.0f, 0.0f)); // 300 counts/s: 0.2 -> 0.1 + 0.9 * 0.2 = 0.28
    s = read();
    c.Check("slow mouse starts past the dead zone", s.rx == 163, sticks(s));
    s = read();
    c.Check("no movement = centre (no smoothing tail)", s.rx == 128 && s.ry == 128, sticks(s));
    km.OnEvent(MotionEvent(0.0f, -6.0f)); // up, 600 counts/s: 0.46
    s = read();
    c.Check("mouse up = right stick up", s.rx == 128 && s.ry == 69, sticks(s));
    // The same 30 counts at 30 and at 60 frames per second turn the camera equally far.
    const auto turned = [&](int frames, u64 frame_us) {
        double sum = 0.0;
        for (int i = 0; i < frames; ++i) {
            km.OnEvent(MotionEvent(30.0f / frames, 0.0f));
            const auto r = read(frame_us);
            sum += (r.rx - 127.5) / 127.5 * frame_us / 1e6;
        }
        read(frame_us); // let the stick return
        return sum;
    };
    const double at30 = turned(1, 33'333), at60 = turned(2, 16'667);
    c.Check("camera turn independent of frame rate", std::abs(at30 - at60) < 0.01 * at30,
            fmt::format("30 fps {:.5f}, 60 fps {:.5f}", at30, at60));
    // A second read in the same frame does not see a near-zero interval.
    km.OnEvent(MotionEvent(15.0f, 0.0f));
    const auto first = read();
    const auto second = read(100);
    c.Check("two reads in one frame agree", first.rx == second.rx, fmt::format("{} {}", first.rx, second.rx));

    // Touch pad.
    s = read();
    km.OnEvent(KeyEvent(SDL_SCANCODE_TAB, true));
    s = read();
    c.Check("Tab = touch pad click at the centre",
            s.buttons == Button::TouchPad && s.touch_active && s.touch[0].down && s.touch[0].x == 960 &&
                s.touch[0].y == 471,
            fmt::format("{:#x} {} {} {}", s.buttons, s.touch_active, s.touch[0].x, s.touch[0].y));
    const u8 tap_id = s.touch[0].id;
    km.OnEvent(KeyEvent(SDL_SCANCODE_TAB, false));
    km.OnEvent(KeyEvent(SDL_SCANCODE_T, true));
    s = read();
    km.OnEvent(MotionEvent(100.0f, -20.0f));
    s = read();
    c.Check("T + mouse drags a finger",
            s.touch_active && s.touch[0].x == 1160 && s.touch[0].y == 431 && s.touch[0].id != tap_id &&
                s.rx == 128,
            fmt::format("{} {} {} id {} rx {}", s.touch_active, s.touch[0].x, s.touch[0].y, s.touch[0].id, s.rx));
    km.OnEvent(KeyEvent(SDL_SCANCODE_T, false));
    s = read();
    c.Check("finger lifted", !s.touch_active, "");

    // Motion mode: the controller turns sideways (right side up) in 0.25 s.
    km.OnEvent(KeyEvent(SDL_SCANCODE_G, true));
    km.OnEvent(KeyEvent(SDL_SCANCODE_G, false));
    Input::Quat integrated{};
    float max_wz = 0.0f;
    for (int i = 0; i < 40; ++i) { // 0.4 s at 10 ms
        s = read();
        integrated = IntegrateLikeBridge(integrated, s.motion.angular_velocity, 0.01f);
        max_wz = std::max(max_wz, s.motion.angular_velocity.z);
    }
    const auto& a = s.motion.acceleration;
    c.Check("sideways: gravity along +X", std::abs(a.x - Input::kGravity) < 1e-3f && std::abs(a.y) < 1e-3f &&
                                               std::abs(a.z) < 1e-3f,
            fmt::format("({:.4f} {:.4f} {:.4f})", a.x, a.y, a.z));
    const auto& q = s.motion.orientation;
    c.Check("sideways: orientation = 90 deg about Z",
            std::abs(q.z - 0.70711f) < 1e-3f && std::abs(q.w - 0.70711f) < 1e-3f && s.motion.active,
            fmt::format("({:.4f} {:.4f} {:.4f} {:.4f})", q.x, q.y, q.z, q.w));
    c.Check("sideways: turn rate while turning", std::abs(max_wz - 6.2832f) < 0.01f,
            fmt::format("max wz {:.4f}", max_wz));
    // Aim with the mouse while sideways; the bridge's integration of the reported gyro must give the
    // reported orientation (conventions agree).
    for (int i = 0; i < 50; ++i) {
        km.OnEvent(MotionEvent(i < 25 ? 8.0f : -3.0f, i < 25 ? -5.0f : 4.0f));
        s = read();
        integrated = IntegrateLikeBridge(integrated, s.motion.angular_velocity, 0.01f);
    }
    const auto& r = s.motion.orientation;
    const float dot = std::abs(r.x * integrated.x + r.y * integrated.y + r.z * integrated.z + r.w * integrated.w);
    c.Check("gyro integrates to the reported orientation", dot > 0.9995f,
            fmt::format("dot {:.6f} reported ({:.4f} {:.4f} {:.4f} {:.4f}) integrated ({:.4f} {:.4f} {:.4f} {:.4f})",
                        dot, r.x, r.y, r.z, r.w, integrated.x, integrated.y, integrated.z, integrated.w));
    c.Check("motion mode: mouse does not move the right stick", s.rx == 128 && s.ry == 128, sticks(s));
    const Input::Vec3 g = Input::Rotate(r, s.motion.acceleration); // back to the reference frame
    c.Check("gravity stays vertical in the reference frame",
            std::abs(g.y - Input::kGravity) < 1e-3f && std::abs(g.x) < 1e-3f && std::abs(g.z) < 1e-3f,
            fmt::format("({:.4f} {:.4f} {:.4f})", g.x, g.y, g.z));
    // Shake: the vertical acceleration swings by +-1.5 g around 1 g.
    km.OnEvent(KeyEvent(SDL_SCANCODE_X, true));
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 30; ++i) {
        s = read();
        const float up = Input::Rotate(s.motion.orientation, s.motion.acceleration).y;
        lo = std::min(lo, up);
        hi = std::max(hi, up);
    }
    km.OnEvent(KeyEvent(SDL_SCANCODE_X, false));
    c.Check("shake swings the accelerometer", hi - lo > 2.5f, fmt::format("vertical {:.3f}..{:.3f}", lo, hi));
    // Motion mode off: back to rest.
    km.OnEvent(KeyEvent(SDL_SCANCODE_G, true));
    km.OnEvent(KeyEvent(SDL_SCANCODE_G, false));
    for (int i = 0; i < 100; ++i) {
        s = read();
    }
    c.Check("motion mode off: back to rest",
            !s.motion.active && std::abs(s.motion.orientation.w - 1.0f) < 1e-5f &&
                std::abs(s.motion.acceleration.y - Input::kGravity) < 1e-5f,
            fmt::format("active {} w {:.6f}", s.motion.active, s.motion.orientation.w));

    // Focus loss (Alt+Tab) without test mode: nothing stays pressed, no mouse piles up.
    Input::KeyboardMouse real;
    real.Configure(Input::DefaultSettings());
    real.AttachWindow(nullptr, false);
    u64 t2 = 5'000'000;
    real.OnEvent(KeyEvent(SDL_SCANCODE_W, true));
    auto s2 = real.Read(t2 += 10'000);
    c.Check("before focus loss W is held", s2.ly == 0, sticks(s2));
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    real.OnEvent(lost);
    real.OnEvent(MotionEvent(500.0f, 0.0f)); // not captured: ignored
    s2 = real.Read(t2 += 10'000);
    c.Check("after focus loss nothing is held and the mouse is ignored", s2.ly == 128 && s2.rx == 128, sticks(s2));
    c.Summary();
}

} // namespace

// ---------------------------------------------------------------------------- param.sfo (PSF)

/// SecondSonPC's PSF reader against shadPS4's PSF on the game's param.sfo: the same value for
/// every key (each queried as text, integer and binary, plus a missing key). Damaged images are
/// checked on SecondSonPC's side only: shadPS4's reader has no bounds checks.
void TestParamSfo(Totals& t, const App::Config& cfg) {
    const auto path = cfg.game_dir / "sce_sys" / "param.sfo";
    std::ifstream in(path, std::ios::binary);
    const std::vector<u8> image{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    Side own{"NATIVE", true}, shad{"BRIDGE", false};
    Psf::ParamSfo sfo;
    std::string error;
    own.Line("open {}", sfo.Parse(image, error));
    PSF psf;
    shad.Line("open {}", psf.Open(image));
    // shadPS4's getters ASSERT when asked for another format than the key has, so each key is read
    // with the getter of its format on both sides; the missing key with all three.
    const auto hex = [](std::span<const u8> b) { return Hex(b.data(), b.size()); };
    for (const auto& e : sfo.Entries()) {
        switch (e.format) {
        case Psf::Format::Text: {
            const auto a = sfo.GetString(e.key);
            const auto b = psf.GetString(e.key);
            own.Line("{}: text {}", e.key, a ? *a : "<none>");
            shad.Line("{}: text {}", e.key, b ? std::string(*b) : "<none>");
            break;
        }
        case Psf::Format::Integer: {
            const auto a = sfo.GetInteger(e.key);
            const auto b = psf.GetInteger(e.key);
            own.Line("{}: int {}", e.key, a ? fmt::format("{:#x}", *a) : "<none>");
            shad.Line("{}: int {}", e.key, b ? fmt::format("{:#x}", static_cast<u32>(*b)) : "<none>");
            break;
        }
        case Psf::Format::Binary: {
            const auto a = sfo.GetBinary(e.key);
            const auto b = psf.GetBinary(e.key);
            own.Line("{}: binary {}", e.key, a ? hex(*a) : "<none>");
            shad.Line("{}: binary {}", e.key, b ? hex(*b) : "<none>");
            break;
        }
        }
    }
    constexpr std::string_view kMissing = "SSPC_NO_SUCH_KEY";
    own.Line("missing key: {} {} {}", sfo.GetString(kMissing).has_value(),
             sfo.GetInteger(kMissing).has_value(), sfo.GetBinary(kMissing).has_value());
    shad.Line("missing key: {} {} {}", psf.GetString(kMissing).has_value(),
              psf.GetInteger(kMissing).has_value(), psf.GetBinary(kMissing).has_value());
    Compare(t, "ParamSfo", own, shad);

    // SecondSonPC's getters answer "nothing" for a key of another format (shadPS4 asserts).
    int wrong_type_ok = 0;
    for (const auto& e : sfo.Entries()) {
        const bool other_empty =
            (e.format == Psf::Format::Text || !sfo.GetString(e.key).has_value()) &&
            (e.format == Psf::Format::Integer || !sfo.GetInteger(e.key).has_value()) &&
            (e.format == Psf::Format::Binary || !sfo.GetBinary(e.key).has_value());
        ++t.checks;
        if (other_empty) {
            ++wrong_type_ok;
        } else {
            ++t.failures;
            t.report.push_back(fmt::format("  DIFF  ParamSfo: {} answers a getter of another format", e.key));
        }
    }
    t.report.push_back(fmt::format("{:14} {:4} checks, {} keys answer only their own format",
                                   "ParamSfo(type)", sfo.Entries().size(), wrong_type_ok));

    // Damaged copies must be rejected without reading outside the image.
    struct Damage {
        const char* what;
        std::vector<u8> bytes;
    };
    std::vector<Damage> damaged;
    damaged.push_back({"truncated header", std::vector<u8>(image.begin(), image.begin() + 12)});
    auto bad_magic = image;
    bad_magic[1] ^= 0xFF;
    damaged.push_back({"bad magic", bad_magic});
    auto bad_count = image;
    bad_count[16] = 0xFF;
    bad_count[17] = 0xFF;
    damaged.push_back({"entry count past end", bad_count});
    auto bad_data = image;
    bad_data[20 + 12] = 0xFF;
    bad_data[20 + 13] = 0xFF;
    bad_data[20 + 14] = 0xFF;
    damaged.push_back({"data offset past end", bad_data});
    damaged.push_back({"truncated data", std::vector<u8>(image.begin(), image.end() - 64)});
    int rejected = 0;
    for (const auto& d : damaged) {
        Psf::ParamSfo probe;
        std::string why;
        ++t.checks;
        if (probe.Parse(d.bytes, why)) {
            ++t.failures;
            t.report.push_back(fmt::format("  DIFF  ParamSfo: damaged image accepted ({})", d.what));
        } else {
            ++rejected;
        }
    }
    t.report.push_back(fmt::format("{:14} {:4} checks, {} damaged images rejected - {}",
                                   "ParamSfo(bad)", damaged.size(), rejected,
                                   rejected == static_cast<int>(damaged.size()) ? "PASS" : "FAIL"));
}

int RunSelfTest(const App::Config& cfg) {
    Totals t;
    TestParamSfo(t, cfg);
    TestFileSystem(t, cfg);
    TestPlayGo(t);
    TestAppContent(t);
    TestSystemService(t);
    TestUserService(t);
    TestPad(t);
    TestInput(t);

    std::string text = fmt::format("SecondSonPC self-test: NATIVE vs BRIDGE (shadPS4, the oracle's implementation)\n");
    for (const auto& line : t.report) {
        text += line + "\n";
    }
    text += fmt::format("TOTAL {} checks, {} failures - {}\n", t.checks, t.failures, t.failures == 0 ? "PASS" : "FAIL");
    const auto path = cfg.logs_dir / "selftest.txt";
    std::ofstream(path, std::ios::binary) << text;
    std::fwrite(text.data(), 1, text.size(), stdout);
    SSPC_LOG(App, Info, "self-test: {} checks, {} failures, report {}", t.checks, t.failures, path.string());
    Log::Flush();
    return t.failures == 0 ? 0 : 1;
}

} // namespace SecondSon::Bridge
