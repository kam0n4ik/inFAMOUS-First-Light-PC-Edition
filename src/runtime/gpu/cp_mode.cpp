// SecondSonPC - SecondSon Native CP: the command processor mode (cp_mode.h).
#include <windows.h>

#include "runtime/gpu/cp_mode.h"

#include <utility>
#include <vector>
#include <fmt/format.h>
#include "base/log.h"

namespace SecondSon::Gpu {
namespace {

std::string Env(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string{};
}

} // namespace

std::string ApplyCpMode(std::string mode, bool native_backend) {
    if (mode.empty()) {
        mode = Env("SSPC_CP_MODE");
    }
    for (char& c : mode) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (mode.empty()) {
        mode = kDefaultCpMode;
    }
    if (mode != "legacy" && mode != "parallel" && mode != "native") {
        SSPC_LOG(GPU, Error, "command processor mode '{}': want legacy, parallel or native - {} runs", mode,
                 kDefaultCpMode);
        mode = kDefaultCpMode;
    }
    if (!native_backend) {
        return mode; // (the bridge's renderer has its own command processor)
    }
    std::vector<std::pair<const char*, const char*>> preset;
    if (mode == "legacy") {
        preset = {{"SSPC_P410_PARALLEL_CP", "0"}};
    } else if (mode == "parallel") {
        preset = {{"SSPC_P410_PARALLEL_CP", "1"}};
    } else {
        preset = {{"SSPC_P410_PARALLEL_CP", "1"},
                  {"SSPC_P52_CODE_TRUST", "sampled"},
                  {"SSPC_CP_JOURNAL_CHANGES", "1"},
                  {"SSPC_CP_SRT_LINES", "1"},
                  // the frame handed to the processor at the game's sceGnmSubmitDone (P5.4; docs/NATIVE_CP.md, GPU-1)
                  {"SSPC_P54_EARLY_SUBMIT", "1"}};
    }
    std::string set, kept;
    for (const auto& [name, value] : preset) {
        if (Env(name).empty()) {
            SetEnvironmentVariableA(name, value);
            set += fmt::format(" {}={}", name, value);
        } else {
            kept += fmt::format(" {}={}", name, Env(name));
        }
    }
    SetEnvironmentVariableA("SSPC_CP_MODE", mode.c_str());
    SSPC_LOG(GPU, Info, "command processor mode: {} (docs/NATIVE_CP.md) -{}{}", mode,
             set.empty() ? "" : " set:" + set, kept.empty() ? "" : "; given explicitly:" + kept);
    return mode;
}

} // namespace SecondSon::Gpu
