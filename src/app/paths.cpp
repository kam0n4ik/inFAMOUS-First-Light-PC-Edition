// SecondSonPC — install layout. Called from a pre-main initializer: no logging, no other statics.
#include "app/paths.h"

#include <string>
#include <windows.h>

namespace SecondSon::App {

namespace {

std::filesystem::path ExecutableDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return std::filesystem::current_path();
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

[[noreturn]] void Fatal(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"SecondSonPC", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

} // namespace

const std::filesystem::path& RootDir() {
    static const std::filesystem::path root = [] {
        const auto exe_dir = ExecutableDir();
        auto dir = exe_dir;
        for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(dir / "SecondSonPC.toml", ec)) {
                return dir;
            }
            if (dir == dir.parent_path()) {
                break;
            }
            dir = dir.parent_path();
        }
        return exe_dir;
    }();
    return root;
}

void PrepareBridgeWorkingDirectory() {
    const auto work = RootDir() / "runtime" / "sspc";
    std::error_code ec;
    std::filesystem::create_directories(work / "user", ec);
    if (ec || !std::filesystem::is_directory(work / "user")) {
        Fatal(L"Cannot create " + (work / "user").wstring());
    }
    std::filesystem::current_path(work, ec);
    if (ec) {
        Fatal(L"Cannot enter " + work.wstring());
    }
}

} // namespace SecondSon::App
