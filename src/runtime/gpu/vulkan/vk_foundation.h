// SecondSonPC — the Vulkan foundation of SecondSonPC's own graphics backend (Phase 4, P4.1.4;
// docs/PHASE4_P41_GPU_FOUNDATION.md §6). What it is and is not:
//   level A — the system Vulkan loader, an instance, the physical devices with their capabilities against what the
//             backend needs, the chosen device and its queues, resources created/used/destroyed, an orderly shutdown;
//   level B — a test window of its own, cleared to a diagnostic colour through its own swapchain;
//   NOT level C/D — it executes none of the game's GPU commands and shows no game frame (the bridge renders the game).
// The same vulkan-1.dll the bridge loads, through SecondSonPC's own table of functions (C API): no second loader, no
// vulkan-hpp dispatcher, no VMA — nothing that could clash with the bridge's copies in the same executable.
// The Vulkan headers stay inside the .cpp; this header has no Vulkan types.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu::Shader {
struct DeviceProfile;
}

namespace SecondSon::Gpu::Vk {

struct DeviceInfo {
    u32 index = 0;
    std::string name;
    u32 vendor_id = 0, device_id = 0, driver_version = 0, api_version = 0;
    std::string type;          ///< discrete, integrated, virtual, cpu, other
    u64 device_local_bytes = 0;
    u32 graphics_family = ~0u; ///< graphics + compute (+ present on Windows)
    u32 compute_family = ~0u;  ///< compute without graphics (asynchronous compute), if any
    u32 transfer_family = ~0u; ///< transfer only, if any
    std::vector<std::string> missing;  ///< required capabilities the device lacks (empty: suitable)
    std::vector<std::string> optional; ///< optional capabilities present (reported, not enabled)
    bool suitable = false;
};

/// Self-test fault injection: the step fails as if the driver had refused it.
enum class FailPoint : u8 { None, Instance, Device, Resources };

struct Options {
    s32 device_index = -1;      ///< -1: the policy's choice (discrete, then integrated, then the most video memory)
    bool validation = false;    ///< the Khronos validation layer, if installed
    bool presentation = false;  ///< the swapchain extension too (the test window)
    std::vector<std::string> extra_device_extensions; ///< more required device extensions (tests: one no device has)
    FailPoint fail_at = FailPoint::None;
};

struct Report {
    std::string loader;                 ///< where vulkan-1.dll came from
    u32 loader_api = 0;                 ///< vkEnumerateInstanceVersion
    std::vector<std::string> instance_extensions;
    std::vector<std::string> layers;
    std::vector<DeviceInfo> devices;
    s32 selected = -1;
    std::vector<std::string> queues;    ///< "graphics family N", ...
    std::vector<std::string> enabled_device_extensions;
};

class Foundation {
public:
    /// Level A. nullptr and `error` on any failure; then nothing it created is left alive.
    static std::unique_ptr<Foundation> Create(const Options& options, Report& report, std::string& error);
    ~Foundation(); ///< waits for the device, destroys everything, unloads the loader

    Foundation(const Foundation&) = delete;
    Foundation& operator=(const Foundation&) = delete;

    const DeviceInfo& Selected() const;

    /// A buffer and an image, the image cleared to `rgba` (R in the low byte) on the graphics queue, copied to the
    /// buffer and read back; all destroyed. true if the pixel read back is the colour.
    bool CheckResources(u32 rgba, std::string& detail);

    /// Level B (needs Options::presentation): a window, its surface and swapchain, cleared to a pulsing `rgba` for
    /// `seconds` (or until closed); all destroyed. `frames`: presented.
    bool ShowTestWindow(u32 seconds, u32 rgba, u32& frames, std::string& detail);

    /// P4.3: what the device supports that a shader translation depends on.
    void QueryShaderProfile(Gpu::Shader::DeviceProfile& out) const;
    /// P4.3: each SPIR-V module through vkCreateShaderModule (then destroyed): accepted / refused by the driver.
    void CreateShaderModules(const std::vector<std::vector<u32>>& spirv, u32& accepted, u32& refused,
                             std::string& detail);

    struct Impl;

private:
    explicit Foundation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Vulkan objects SecondSonPC created and has not destroyed yet (instance, device, memory, buffers, images, pools,
/// fences, semaphores, surfaces, swapchains): 0 when nothing of the foundation is alive.
s64 LiveObjects();

/// --gpu-probe: Create (level A), CheckResources, and with window_seconds > 0 the test window (level B); the report
/// in <logs_dir>\gpu_probe.txt and on stdout. 0 if every level asked for passed.
int RunProbe(const std::filesystem::path& logs_dir, s32 device_index, u32 window_seconds, bool validation);

/// P4.3: a level-A foundation on the chosen device (-1: the policy's), its shader profile, everything destroyed.
bool QueryDeviceProfile(s32 device_index, Gpu::Shader::DeviceProfile& out, std::string& error);
/// P4.3: the modules through vkCreateShaderModule on the policy's device. false: no device (`detail`).
bool CheckShaderModules(const std::vector<std::vector<u32>>& spirv, u32& accepted, u32& refused, std::string& detail);

} // namespace SecondSon::Gpu::Vk
