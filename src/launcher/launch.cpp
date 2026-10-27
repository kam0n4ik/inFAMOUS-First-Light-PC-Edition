// SecondSonPC — starting the game from the launcher (launch.h). Own code.
#include "launcher/launch.h"

#include <windows.h>

#include <tlhelp32.h>

#include <fmt/format.h>

#include <cstdlib>
#include <ctime>
#include <fstream>

#include "app/product.h"
#include "settings/user_store.h"

namespace SecondSon::Launcher {

namespace {

using Var = std::pair<std::string_view, std::string_view>;

#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light's play.cmd, in its order: Second Son's without what acts on Second Son's engine (NATIVE_GBUFFER,
// CITY_PERFORMANCE's engine parts, early submission) and without R11_EARLY_SUBMIT (not measured on First Light yet)
constexpr Var kStandard[] = {
    {"SSPC_CP_MODE", "native"},
    {"SSPC_CITY_PERFORMANCE", "1"},
    {"SSPC_CITY_ENGINE_OPT", "0"},
    {"SSPC_CITY_ENGINE_GBUFFER_RUNS", "0"},
    {"SSPC_P54_EARLY_SUBMIT", "0"},
    {"SSPC_REA_MERGE_RUNS", "1"},
    {"SSPC_CP_ROUND_NODRAIN", "1"},
    {"SSPC_P411_ASYNC_ACQUIRE", "1"},
    {"SSPC_ASYNC_PIPELINES", "1"},
    {"SSPC_P53_FRONTEND_THREAD", "1"},
    {"SSPC_CP_ASYNC_WRITES", "1"},
    {"SSPC_CP_ASYNC_FLIP", "1"},
    {"SSPC_VK_EMIT_THREAD", "1"},
    {"SSPC_VK_EMIT_BATCH", "1"},
    {"SSPC_CP_RING", "128"},
    {"SSPC_R9_PCACHE", "1"},
    {"SSPC_R9_HEAP_WINDOW", "1"},
    {"SSPC_R10_AJM", "native"},
    {"SSPC_R10_SYSRT", "native"},
};
// the asynchronous compute lane (REA_DEFER_WAIT_B is a Second Son engine hook)
constexpr Var kExperimental[] = {
    {"SSPC_FB_ASYNC_COMPUTE", "1"},
    {"SSPC_R10_AQ_MAIN_FREE", "1"},
    {"SSPC_R11_AQ_IMAGES", "1"},
    {"SSPC_R11_AQ_ORDER_EPOCH", "1"},
};
#else
// play.cmd, in its order
constexpr Var kStandard[] = {
    {"SSPC_CP_MODE", "native"},
    {"SSPC_CITY_PERFORMANCE", "1"},
    {"SSPC_REA_MERGE_RUNS", "1"},
    {"SSPC_CP_ROUND_NODRAIN", "1"},
    {"SSPC_P411_ASYNC_ACQUIRE", "1"},
    {"SSPC_ASYNC_PIPELINES", "1"},
    {"SSPC_P53_FRONTEND_THREAD", "1"},
    {"SSPC_CP_ASYNC_WRITES", "1"},
    {"SSPC_CP_ASYNC_FLIP", "1"},
    {"SSPC_VK_EMIT_THREAD", "1"},
    {"SSPC_VK_EMIT_BATCH", "1"},
    {"SSPC_CP_RING", "128"},
    {"SSPC_NATIVE_GBUFFER", "native"},
    {"SSPC_R9_PCACHE", "1"},
    {"SSPC_R9_HEAP_WINDOW", "1"},
    {"SSPC_R10_AJM", "native"},
    {"SSPC_R10_SYSRT", "native"},
    {"SSPC_R11_EARLY_SUBMIT", "256"},
};
// play.cmd under SSPC_PLAY_EXPERIMENTAL (play_experimental.cmd)
constexpr Var kExperimental[] = {
    {"SSPC_REA_DEFER_WAIT_B", "1"},
    {"SSPC_FB_ASYNC_COMPUTE", "1"},
    {"SSPC_R10_AQ_MAIN_FREE", "1"},
    {"SSPC_R11_AQ_IMAGES", "1"},
    {"SSPC_R11_AQ_ORDER_EPOCH", "1"},
};
#endif

void PutEnv(std::string_view name, std::string_view value) {
    // _putenv_s changes the CRT's copy and the process's (the child's environment); "" removes the variable
    _putenv_s(std::string(name).c_str(), std::string(value).c_str());
}

std::wstring Wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::filesystem::path ExePath() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) {
        return a;
    }
    std::wstring out = L"\"";
    for (const wchar_t ch : a) {
        if (ch == L'"') {
            out += L'\\';
        }
        out += ch;
    }
    return out + L"\"";
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\"");
    const auto e = s.find_last_not_of(" \t\r\"");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

} // namespace

std::span<const Var> StandardEnv() {
    return kStandard;
}

std::span<const Var> ExperimentalEnv() {
    return kExperimental;
}

void ApplyProfileEnv(Profile profile) {
    for (const auto& [k, v] : kStandard) {
        PutEnv(k, v);
    }
    for (const auto& [k, v] : kExperimental) {
        PutEnv(k, profile == Profile::Experimental ? v : std::string_view{});
    }
    PutEnv("SSPC_PLAY_EXPERIMENTAL", {}); // (play.cmd's switch; the variables above are the profile)
}

std::filesystem::path LauncherFile() {
    const auto user = Settings::DefaultUserFile();
    return user.empty() ? std::filesystem::path{} : user.parent_path() / L"launcher.toml";
}

bool LoadLauncherState(LauncherState& out) {
    const auto file = LauncherFile();
    std::ifstream in(file);
    if (file.empty() || !in) {
        if (const char* e = std::getenv("SSPC_PLAY_EXPERIMENTAL"); e != nullptr && *e != 0) {
            out.profile = Profile::Experimental;
        }
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "profile") {
            out.profile = value == "experimental" ? Profile::Experimental : Profile::Standard;
        } else if (key == "closeAfterLaunch") {
            out.close_after_launch = value == "true";
        }
    }
    return true;
}

bool SaveLauncherState(const LauncherState& s, std::string& error) {
    const auto file = LauncherFile();
    if (file.empty()) {
        error = "LOCALAPPDATA is not set";
        return false;
    }
    const std::string text = fmt::format(
        "# {} launcher: the launch profile and what the launcher does after a start (written by the "
        "launcher)\nprofile = \"{}\"\ncloseAfterLaunch = {}\n",
        App::kProduct.name, s.profile == Profile::Experimental ? "experimental" : "standard", s.close_after_launch ? "true" : "false");
    return Settings::WriteFileAtomic(file, text, error);
}

std::filesystem::path GameDir() {
    return ExePath().parent_path();
}

std::string CheckGame(const std::filesystem::path& game_dir) {
    std::error_code ec;
    if (game_dir.empty() || !std::filesystem::is_regular_file(game_dir / L"eboot.bin", ec)) {
        return fmt::format("Не найден eboot.bin игры — положите {0} в папку игры или укажите её в {0}.toml, "
                           "[Game] path (README, «Установка»).",
                           App::kProduct.name);
    }
    const std::wstring self = ExePath().filename().wstring();
    const DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        bool running = false;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ProcessID != me && _wcsicmp(pe.szExeFile, self.c_str()) == 0) {
                running = true;
                break;
            }
        }
        CloseHandle(snap);
        if (running) {
            return fmt::format("{}.exe уже запущен.", App::kProduct.name);
        }
    }
    return {};
}

std::string BackupSaves(const std::filesystem::path& user_data_dir) {
    if (user_data_dir.empty()) {
        return {};
    }
    const auto home = user_data_dir / L"home";
    std::error_code ec;
    if (!std::filesystem::is_directory(home, ec)) {
        return {}; // no saves yet
    }
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    const auto dst = user_data_dir / L"savedata_backups" / stamp;
    std::filesystem::create_directories(dst, ec);
    std::filesystem::copy(home, dst,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        return "не удалось скопировать сохранения в userdata\\savedata_backups: " + ec.message();
    }
    return {};
}

std::string StartGame(std::span<const std::string> args, GameProcess& out) {
    const auto exe = ExePath();
    std::wstring cmd = Quote(exe.wstring()) + L" --no-launcher";
    for (const auto& a : args) {
        cmd += L" " + Quote(Wide(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = GameDir().wstring();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(),
                        &si, &pi)) {
        return fmt::format("Windows не запустил {}.exe (ошибка {}).", App::kProduct.name, GetLastError());
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    return {};
}

bool GameRunning(GameProcess& p) {
    if (p.process == nullptr) {
        return false;
    }
    if (WaitForSingleObject(p.process, 0) == WAIT_TIMEOUT) {
        return true;
    }
    CloseHandle(p.process);
    p.process = nullptr;
    p.pid = 0;
    return false;
}

} // namespace SecondSon::Launcher
