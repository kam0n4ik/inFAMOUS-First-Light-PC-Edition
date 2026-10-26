// SecondSonPC — what the launcher shows about this PC: the video card, the processor, the memory, the displays and
// sound outputs under the names the game's [Display] monitor / [Audio] device take (SDL's, as the in-game menu lists
// them), a connected controller, and what the video card can do for the upscaling settings. Own code.
#pragma once

#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Launcher {

struct DisplayInfo {
    std::string name;  // SDL's name ("<name> (2)" for a second of one model): the [Display] monitor value
    std::string label; // "<name> — 1920 × 1080, 165 Гц (основной)"
    u32 width = 0, height = 0;
    double hz = 0;
    bool primary = false;
};

struct SystemInfo {
    std::string gpu, cpu, ram; // "NVIDIA GeForce RTX 2060", "AMD Ryzen 5 5500", "32 ГБ"
    u32 gpu_vendor = 0;        // PCI vendor (0x10DE NVIDIA)
    bool rtx = false;          // DLSS Super Resolution
    bool rtx40 = false;        // DLSS Frame Generation (RTX 40 / 50)
    bool rtx50 = false;        // DLSS Multi Frame Generation (x3 / x4)
    bool fsr_dll = false;      // amd_fidelityfx_vk.dll next to SecondSonPC.exe (FSR 3.1 frame generation)
    std::vector<DisplayInfo> displays;
    std::string primary_label; // the primary display without " (основной)"
    std::vector<std::string> audio_devices;
    std::string controller; // the first connected gamepad's name ("" none)
};

/// Asks Windows (DXGI, the registry) and SDL (displays, audio outputs, gamepads); SDL is shut down again.
SystemInfo QuerySystem();

} // namespace SecondSon::Launcher
