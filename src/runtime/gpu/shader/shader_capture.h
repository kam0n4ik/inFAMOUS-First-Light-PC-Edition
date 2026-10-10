// SecondSonPC — the bridge's translations captured for offline comparison, and the bridge's pipeline at a draw
// (Phase 4, P4.3; docs/PHASE4_P43_SHADER_TRANSLATION.md §10, §11).
//
// --gpu-shader-capture: every module the bridge translates (patch 0031: its CompileModule) is written with all its
// inputs — code, user data, the flattened user data it walked, the fetch shader and V#s it read, the registers of the
// draw (SH and context windows, primitive type, compute registers), the start bindings, the device profile — and its
// SPIR-V and runtime information. --gpu-shader-verify <file> replays each one offline through SecondSonPC's frontend
// parts (registers -> runtime parameters, fetch shader parser, SRT walker decoder, descriptors, specialization key)
// and the installed translator, and compares with what the bridge produced.
//
// File: "SSPCSHD1", then records {u32 type, u32 bytes, bytes}: 'I' text, 'M' a module, 'E' the end.
#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Shader {

struct CapturedModule {
    HwStage hw = HwStage::Vs;
    SwStage sw = SwStage::Vertex;
    u32 perm_idx = 0;
    u64 footer_hash = 0;
    u64 address = 0;
    BindingStart start;
    u32 vertex_sgpr_offset = 0, instance_sgpr_offset = 0;
    std::array<u32, 16> user_data{};
    std::vector<u32> code;
    std::vector<u32> flat;       ///< the flattened user data the bridge's translation walked
    bool has_fetch = false;
    u32 fetch_sgpr_base = 0;
    std::vector<u32> fetch_code; ///< up to and with its s_setpc_b64
    std::vector<std::array<u32, 4>> vertex_sharps;
    std::vector<u32> sh;         ///< SH registers 0x2C00..0x2FFF
    std::vector<u32> context;    ///< context registers 0xA000..0xA3FF
    u32 primitive_type = 0;      ///< VGT_PRIMITIVE_TYPE
    std::array<u32, 0x50> cs{};  ///< compute registers 0x2E00..0x2E4F
    std::vector<u32> spirv;
    DeviceProfile profile;       ///< the bridge's profile
    std::vector<u8> bridge_runtime; ///< the bridge's runtime information (bytes only its adapter reads)
};

class CaptureWriter {
public:
    bool Open(const std::filesystem::path& path, std::string_view info);
    void Write(const CapturedModule& module); ///< thread-safe
    void Close();
    bool IsOpen() const { return file_.is_open(); }
    u64 Records() const { return records_; }
    u64 Bytes() const { return bytes_; }

private:
    std::mutex mutex_;
    std::ofstream file_;
    u64 records_ = 0, bytes_ = 0;
};

bool LoadCapture(const std::filesystem::path& path, std::vector<CapturedModule>& out, std::string& info,
                 std::string& error);

/// Serialization of a profile (field by field).
void SerializeProfile(const DeviceProfile& p, std::vector<u8>& out);
bool DeserializeProfile(std::span<const u8> in, size_t& pos, DeviceProfile& p);
/// The fields that differ ("name: a / b, ..."); empty when equal (the source text is not compared).
std::string DiffProfiles(const DeviceProfile& a, const DeviceProfile& b);

/// --gpu-shader-capture: the process's writer (opened once at start-up; nullptr: off).
CaptureWriter* GetCaptureWriter();
bool StartCapture(const std::filesystem::path& path);
void StopCapture();

// ---- the bridge's pipeline at a draw (--gpu-shaders)

struct BridgeStageView {
    bool present = false;
    u64 footer_hash = 0;
    u64 module = 0;               ///< the bridge's module handle (an id of its translation)
    std::span<const u32> flat;    ///< its flattened user data for this draw (valid during the call)
    std::span<const u8> runtime;  ///< its runtime information (bytes only its adapter reads)
    u32 buffers = 0, images = 0, samplers = 0;
    u32 mrt_mask = 0;
    u32 fetch_attributes = 0;
    bool has_fetch = false;
};
struct BridgePipelineView {
    bool compute = false;
    std::array<BridgeStageView, static_cast<size_t>(SwStage::Count)> stages{};
    u64 pipeline = 0;
};
using BridgePipelineCallback = void (*)(const BridgePipelineView& view);
/// The shadow's callback (nullptr: none). Called on the bridge's GPU thread.
void SetBridgePipelineCallback(BridgePipelineCallback callback);
BridgePipelineCallback GetBridgePipelineCallback();

/// --gpu-shader-verify: the offline comparison; report in <logs>\gpu_shader_verify.txt. 0 when every module compared
/// equal (SPIR-V and runtime information) and nothing failed.
int RunShaderVerify(const std::filesystem::path& capture, const std::filesystem::path& logs_dir);

} // namespace SecondSon::Gpu::Shader
