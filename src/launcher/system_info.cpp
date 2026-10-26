// SecondSonPC — the launcher's view of this PC (system_info.h). Own code.
#include "launcher/system_info.h"

#include <windows.h>

#include <dxgi.h>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <regex>

namespace SecondSon::Launcher {

namespace {

std::string Utf8(const wchar_t* w) {
    if (w == nullptr || *w == 0) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(const std::string& s) {
    // the registry pads some names with runs of spaces
    std::string out;
    for (const char ch : s) {
        if (ch == '\t' || ch == ' ') {
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
        } else {
            out += ch;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

void QueryGpu(SystemInfo& s) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return;
    }
    SIZE_T best = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        a->Release();
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || d.DedicatedVideoMemory < best) {
            continue;
        }
        best = d.DedicatedVideoMemory;
        s.gpu = Trim(Utf8(d.Description));
        s.gpu_vendor = d.VendorId;
    }
    factory->Release();
    if (s.gpu_vendor == 0x10DE) {
        std::smatch m;
        static const std::regex kRtx("RTX\\s*([0-9])[0-9]{2,3}");
        if (std::regex_search(s.gpu, m, kRtx)) {
            s.rtx = true;
            const int series = m[1].str()[0] - '0';
            s.rtx40 = series >= 4;
            s.rtx50 = series >= 5;
        }
    }
}

void QueryCpu(SystemInfo& s) {
    wchar_t name[256] = {};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) {
        s.cpu = Trim(Utf8(name));
    }
    ULONGLONG kib = 0;
    if (GetPhysicallyInstalledSystemMemory(&kib) && kib != 0) {
        s.ram = fmt::format("{} ГБ", static_cast<u64>(std::llround(static_cast<double>(kib) / (1024.0 * 1024.0))));
    } else {
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof(m);
        if (GlobalMemoryStatusEx(&m)) {
            s.ram = fmt::format("{} ГБ",
                                static_cast<u64>(std::ceil(static_cast<double>(m.ullTotalPhys) / (1ull << 30))));
        }
    }
}

/// The displays as the in-game menu lists them (ui/menu_controller.cpp Displays, DisplayLabel).
void QueryDisplays(SystemInfo& s) {
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; ids != nullptr && i < n; ++i) {
        const char* dn = SDL_GetDisplayName(ids[i]);
        const std::string name = dn != nullptr ? dn : "?";
        const auto same = std::ranges::count_if(s.displays, [&](const DisplayInfo& d) {
            return d.name == name || d.name.starts_with(name + " (");
        });
        DisplayInfo d;
        d.name = same == 0 ? name : fmt::format("{} ({})", name, same + 1);
        d.primary = ids[i] == primary;
        std::string label = d.name;
        if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(ids[i]); m != nullptr && m->w > 0 && m->h > 0) {
            const float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
            d.width = static_cast<u32>(std::lround(m->w * density));
            d.height = static_cast<u32>(std::lround(m->h * density));
            d.hz = m->refresh_rate;
            label += d.hz > 0 ? fmt::format(" — {} × {}, {:.0f} Гц", d.width, d.height, d.hz)
                              : fmt::format(" — {} × {}", d.width, d.height);
        }
        if (d.primary) {
            s.primary_label = label;
            label += " (основной)";
        }
        d.label = std::move(label);
        s.displays.push_back(std::move(d));
    }
    SDL_free(ids);
}

void QueryAudio(SystemInfo& s) {
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; ids != nullptr && i < n; ++i) {
        if (const char* dn = SDL_GetAudioDeviceName(ids[i]); dn != nullptr) {
            s.audio_devices.emplace_back(dn);
        }
    }
    SDL_free(ids);
}

void QueryGamepad(SystemInfo& s) {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids != nullptr && n > 0) {
        const char* dn = SDL_GetGamepadNameForID(ids[0]);
        s.controller = dn != nullptr ? dn : "Gamepad";
    }
    SDL_free(ids);
}

} // namespace

SystemInfo QuerySystem() {
    SystemInfo s;
    QueryGpu(s);
    QueryCpu(s);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::error_code ec;
    s.fsr_dll =
        std::filesystem::is_regular_file(std::filesystem::path(exe).parent_path() / L"amd_fidelityfx_vk.dll", ec);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        QueryDisplays(s);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        QueryAudio(s);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        QueryGamepad(s);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    SDL_Quit();
    return s;
}

} // namespace SecondSon::Launcher
