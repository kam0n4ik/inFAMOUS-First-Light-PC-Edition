// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project (sequence from src/emulator.cpp)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY Phase 0 bridge (removed in Phase 5).
// Startup sequence adapted from shadPS4 Core::Emulator::Run()
// (third_party/shadPS4 @ 0fe263a4760dfbfa973366890061749b4af0de97). Differences:
//  * configuration comes from SecondSonPC.toml, applied in memory (shadPS4's config.json in
//    runtime\sspc\user is never saved by SecondSonPC);
//  * /savedata0 lives under SecondSonPC [Paths] userData (shadPS4 "home dir"), not in any shadPS4 install;
//  * the HLE symbol table is rebuilt: SecondSonPC exports first, shadPS4 HLE behind the call counter;
//  * no game patches/cheats, Discord RPC, launcher IPC or play-time tracking.

#include <WinSock2.h>
#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/bootstrap.h"
#include "runtime/exports.h"
#include "trace/import_counter.h"

#include "common/elf_info.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/ntapi.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/aerolib/aerolib.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/libs.h"
#include "core/libraries/save_data/save_backup.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/user_settings.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/cache_storage.h"
#include "video_core/renderdoc.h"

extern Frontend::WindowSDL* g_window; // defined in shadPS4 src/emulator.cpp

namespace Libraries::Kernel {
extern char const* g_environment[64];
}

namespace Core { // defined in shadPS4 src/emulator.cpp
s32 ReadCompiledSdkVersion(const std::string& guest_or_host_path);
std::map<s32, std::string> ExtractTrophies(std::string_view npbind_guest,
                                           std::string_view trophy_dir_guest);
} // namespace Core

namespace Common {
// ElfInfo's fields are private (only Core::Emulator may fill them); this class is made a friend by
// patches/shadps4/0002-elfinfo-friend-secondson-bootstrap.patch. Phase 0-4 only.
class SecondSonBootstrap {
public:
    struct GameInfo {
        std::string serial;
        std::string title;
        std::string app_version;
        u32 firmware = 0;
        u32 sdk = 0;
        PSFAttributes attributes{};
        std::vector<u8> splash;
        std::filesystem::path folder;
    };

    static void Fill(GameInfo info) {
        auto& e = ElfInfo::Instance();
        e.initialized = true;
        e.game_serial = std::move(info.serial);
        e.title = std::move(info.title);
        e.app_ver = std::move(info.app_version);
        e.firmware_ver = info.firmware & 0xFFF00000;
        e.raw_firmware_ver = info.firmware;
        e.sdk_ver = info.sdk;
        e.psf_attributes = info.attributes;
        e.splash_data = std::move(info.splash);
        e.game_folder = std::move(info.folder);
    }

    static void SetTrophyIndexMap(std::map<s32, std::string> map) {
        ElfInfo::Instance().trophy_index_map = std::move(map);
    }
};
} // namespace Common

namespace SecondSon::Bridge {

namespace {

Input::GameControllers* g_controllers = nullptr;

void OnQuickExit() {
    Trace::FinalDump();
    SSPC_LOG(Bridge, Info, "session end");
    Log::Flush();
    Common::Log::Flush();
    Libraries::SaveData::Backup::StopThread();
    Storage::DataBase::Instance().Close();
    if (g_controllers != nullptr) {
        g_controllers->ResetLightbarColors();
        Sleep(10);
    }
}

[[noreturn]] void Fatal(std::string_view message) {
    SSPC_LOG(Bridge, Critical, "{}", message);
    Log::Flush();
    MessageBoxA(nullptr, std::string(message).c_str(), "SecondSonPC", MB_ICONERROR | MB_OK);
    std::quick_exit(1);
}

std::string NidName(const std::string& nid) {
    const auto* entry = Core::AeroLib::FindByNid(nid.c_str());
    return entry != nullptr ? entry->name : "?";
}

bool IsUnder(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto rel = std::filesystem::weakly_canonical(path).lexically_relative(
        std::filesystem::weakly_canonical(root));
    return !rel.empty() && *rel.begin() != "..";
}

void BuildBindingTable(Core::Linker& linker, const App::Config& cfg) {
    using namespace Core::Loader;

    SymbolsResolver shad; // temporary shadPS4 HLE table (BRIDGE)
    Libraries::InitHLELibs(&shad);
    // Linker::Execute() registers the libSceLibcInternal HLE itself when the LLE sprx is absent;
    // registering it here first puts these functions (used by the game's libSceFios2.prx) behind
    // the counter too. Execute()'s later duplicate registration loses to these (first match wins).
    const auto libc_internal_sprx = EmulatorSettings.GetSysModulesDir() / "libSceLibcInternal.sprx";
    if (!std::filesystem::exists(libc_internal_sprx)) {
        Libraries::LibcInternal::RegisterLib(&shad);
    }

    auto& table = linker.GetHLESymbols();
    if (cfg.import_counter && !Trace::Init(shad.GetSize() + 64)) {
        SSPC_LOG(Trace, Error, "import counter disabled (allocation failed)");
    }

    // 1) SecondSonPC exports first: SymbolsResolver::FindSymbol returns the first match.
    for (const auto& e : Runtime::Exports()) {
        SymbolResolver key{};
        key.name = e.nid;
        key.nidName = e.name;
        key.library = e.library;
        key.library_version = 1;
        key.module = e.module;
        key.type = SymbolType::Function;

        auto owner = Trace::Owner::Native;
        if (e.original != nullptr) {
            const auto* rec = shad.FindSymbol(key);
            if (rec == nullptr || rec->virtual_address == 0) {
                SSPC_LOG(Bridge, Error, "observer {}: nothing to observe, not bound", e.name);
                continue;
            }
            *e.original = reinterpret_cast<void*>(rec->virtual_address);
            owner = Trace::Owner::Observed;
        }
        void* fn = e.function;
        if (cfg.import_counter) {
            if (void* thunk = Trace::Wrap(e.nid, e.library, e.module, e.name, owner, fn)) {
                fn = thunk;
            }
        }
        table.AddSymbol(key, reinterpret_cast<u64>(fn));
        SSPC_LOG(Bridge, Info, "bind {:<8} {} ({}/{}) -> SecondSonPC",
                 owner == Trace::Owner::Native ? "NATIVE" : "OBSERVED", e.name, e.library, e.module);
    }

    // 2) shadPS4 HLE: temporary BRIDGE, every function behind the call counter.
    size_t functions = 0;
    size_t objects = 0;
    for (const auto& rec : shad.GetSymbols()) {
        u64 address = rec.virtual_address;
        if (rec.symbol.type == SymbolType::Function) {
            ++functions;
            if (cfg.import_counter && address != 0) {
                if (void* thunk = Trace::Wrap(rec.symbol.name, rec.symbol.library, rec.symbol.module,
                                              NidName(rec.symbol.name), Trace::Owner::Bridge,
                                              reinterpret_cast<void*>(address))) {
                    address = reinterpret_cast<u64>(thunk);
                }
            }
        } else {
            ++objects;
        }
        table.AddSymbol(rec.symbol, address);
    }
    if (cfg.import_counter) {
        Trace::Seal();
    }
    SSPC_LOG(Bridge, Info,
             "binding table: {} SecondSonPC exports, {} shadPS4 HLE functions (BRIDGE) + {} objects, "
             "{} counted thunks",
             Runtime::Exports().size(), functions, objects, Trace::WrappedCount());
}

} // namespace

void RunGame(const App::Config& cfg) {
    Common::SetCurrentThreadName("SecondSonPC:Main");

    // Equivalent of Core::Emulator::Emulator().
    Common::NtApi::Initialize();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::at_quick_exit(OnQuickExit);

    // The shadPS4 core keeps its own log file; SecondSonPC.log is written by SecondSonPC only.
    std::error_code ec;
    std::filesystem::create_directories(cfg.logs_dir, ec);
    Common::FS::SetUserPath(Common::FS::PathType::LogDir, cfg.logs_dir);
    Common::Log::Setup("bridge_shadps4.log");

    const auto& shad_user = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    if (!IsUnder(shad_user, cfg.root)) {
        Fatal(fmt::format("shadPS4 core user dir {} is outside {} — refusing to run",
                          shad_user.string(), cfg.root.string()));
    }
    SSPC_LOG(Bridge, Info, "shadPS4 core {} (temporary bootstrap), user dir {}", Common::g_scm_rev,
             shad_user.string());

    KeyManager::GetInstance()->LoadFromFile();
    EmulatorSettings.Load();
    UserSettings.Load();

    // Mount /app0 and read param.sfo.
    const auto& game_folder = cfg.game_dir;
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    mnt->Mount(game_folder, "/app0", true);
    mnt->Mount(game_folder, "/hostapp", true);

    std::string id;
    std::string title = "Unknown title";
    std::string app_version = "Unknown version";
    u32 fw_version = 0x4700000;
    Common::PSFAttributes psf_attributes{};
    if (auto psf_handle = mnt->Open("/app0/sce_sys/param.sfo", /*writable=*/false)) {
        auto* param_sfo = Common::Singleton<PSF>::Instance();
        if (!param_sfo->Open(psf_handle)) {
            Fatal("cannot parse /app0/sce_sys/param.sfo");
        }
        const auto content_id = param_sfo->GetString("CONTENT_ID");
        const auto title_id = param_sfo->GetString("TITLE_ID");
        if (content_id.has_value() && !content_id->empty()) {
            id = std::string(*content_id, 7, 9);
        } else if (title_id.has_value()) {
            id = *title_id;
        }
        title = param_sfo->GetString("TITLE").value_or(title);
        fw_version = param_sfo->GetInteger("SYSTEM_VER").value_or(fw_version);
        app_version = param_sfo->GetString("APP_VER").value_or(app_version);
        if (const auto raw_attributes = param_sfo->GetInteger("ATTRIBUTE")) {
            psf_attributes.raw = *raw_attributes;
        }
    } else {
        Fatal("/app0/sce_sys/param.sfo not found");
    }

    // Per-game overrides of the bridge core (runtime\sspc\user\custom_configs\<id>.json, normally
    // absent), then SecondSonPC.toml on top. Nothing is written back.
    EmulatorSettings.Load(id);
    EmulatorSettings.SetConsoleLanguage(cfg.language_id); // shadPS4-internal users only (trophy
                                                          // XML, save locale); the game asks ours
    std::filesystem::create_directories(cfg.user_data_dir / "home", ec);
    EmulatorSettings.SetHomeDir(cfg.user_data_dir / "home");
    EmulatorSettings.SetFullScreen(cfg.fullscreen);
    EmulatorSettings.SetWindowWidth(cfg.window_width);
    EmulatorSettings.SetWindowHeight(cfg.window_height);
    EmulatorSettings.SetVblankFrequency(cfg.vblank_hz);
    EmulatorSettings.SetConnectedToNetwork(false);
    EmulatorSettings.SetLogFilter(cfg.bridge_log_filter);
    Common::Log::Switch("bridge_shadps4.log", false); // applies the filter

    SSPC_LOG(Bridge, Info, "game {} \"{}\" v{} fw {:#x}", id, title, app_version, fw_version);
    SSPC_LOG(SaveData, Info, "/savedata0 root: {}\\<user>\\savedata\\{}\\<dir>",
             (cfg.user_data_dir / "home").string(), id);

    const std::string guest_eboot_path = "/app0/eboot.bin";
    Common::SecondSonBootstrap::GameInfo info{
        .serial = id,
        .title = title,
        .app_version = app_version,
        .firmware = fw_version,
        .sdk = static_cast<u32>(Core::ReadCompiledSdkVersion(guest_eboot_path)),
        .attributes = psf_attributes,
        .splash = {},
        .folder = game_folder,
    };
    if (auto splash = mnt->ReadFile("/app0/sce_sys/pic1.png")) {
        info.splash = std::move(*splash);
    }
    SSPC_LOG(Loader, Info, "eboot SDK version {:#x}", info.sdk);
    Common::SecondSonBootstrap::Fill(std::move(info));

    Common::Singleton<Core::FileSys::HandleTable>::Instance()->CreateStdHandles();

    // Reserves the guest address space. Must precede Trace::Init (thunks are host allocations).
    auto* memory = Core::Memory::Instance();
    (void)memory;
    g_controllers = Common::Singleton<Input::GameControllers>::Instance();
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    VideoCore::LoadRenderDoc();

    Common::SecondSonBootstrap::SetTrophyIndexMap(
        Core::ExtractTrophies("/app0/sce_sys/npbind.dat", "/app0/sce_sys/trophy"));

    const auto window_title =
        fmt::format("SecondSonPC (Phase 0 bootstrap) | {} - {} <{}>", id, title, app_version);
    auto window = std::make_unique<Frontend::WindowSDL>(
        static_cast<s32>(EmulatorSettings.GetWindowWidth()),
        static_cast<s32>(EmulatorSettings.GetWindowHeight()), g_controllers, window_title);
    g_window = window.get();
    if (auto icon = mnt->ReadFile("/app0/sce_sys/icon0.png")) {
        window->SetIcon(*icon);
    } else {
        window->SetIcon({});
    }

    using Common::FS::GetUserPath;
    using Common::FS::PathType;
    mnt->Mount(GetUserPath(PathType::GameDataDir), "/data");
    const auto temp_dir = GetUserPath(PathType::TempDataDir) / id;
    std::filesystem::remove_all(temp_dir, ec); // temp0 is cleared on each boot
    std::filesystem::create_directory(temp_dir, ec);
    mnt->Mount(temp_dir, "/temp0");
    mnt->Mount(temp_dir, "/temp");
    const auto download_dir = GetUserPath(PathType::DownloadDir) / id;
    std::filesystem::create_directory(download_dir, ec);
    mnt->Mount(download_dir, "/download0");
    const auto captures_dir = GetUserPath(PathType::CapturesDir);
    std::filesystem::create_directory(captures_dir, ec);
    VideoCore::SetOutputDir(captures_dir, id);

    const auto fonts_dir = EmulatorSettings.GetFontsDir();
    std::filesystem::create_directory(fonts_dir, ec);
    std::string guest_font_dir = "/";
    guest_font_dir.append(Libraries::Kernel::sceKernelGetFsSandboxRandomWord()).append("/common/font");
    std::filesystem::create_directory(fonts_dir / "font", ec);
    std::filesystem::create_directory(fonts_dir / "font2", ec);
    mnt->Mount(fonts_dir / "font", guest_font_dir);
    mnt->Mount(fonts_dir / "font2", guest_font_dir + "2");

    Libraries::Kernel::g_environment[0] = nullptr;

    BuildBindingTable(*linker, cfg);
    if (cfg.import_counter) {
        Trace::StartPeriodicDump(cfg.logs_dir / "calls.json", cfg.dump_interval_s);
    }

    if (linker->LoadModule(guest_eboot_path) == -1) {
        Fatal(fmt::format("failed to load {}", guest_eboot_path));
    }
    SSPC_LOG(Loader, Info, "loaded {}, starting guest", guest_eboot_path);

    const std::vector<std::string> args{guest_eboot_path};
    linker->Execute(args);

    window->InitTimers();
    while (window->IsOpen()) {
        window->WaitEvent();
    }
    SSPC_LOG(App, Info, "window closed");
    std::quick_exit(0);
}

} // namespace SecondSon::Bridge
