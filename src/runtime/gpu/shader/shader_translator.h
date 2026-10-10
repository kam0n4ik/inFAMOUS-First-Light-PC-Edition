// SecondSonPC — the GCN -> SPIR-V translator behind SecondSonPC's shader frontend (Phase 4, P4.3;
// docs/PHASE4_P43_SHADER_TRANSLATION.md §2).
//
// SecondSonPC owns the interface; the implementation is whatever translator is installed. For now that is the adapter
// layer over the bridge's shader recompiler (src/bridge/shadps4/shader_translator_host.cpp, patch 0030): the GCN
// decoder, IR, passes and SPIR-V emitter are the bridge's code, driven through this interface only. The runtime never
// includes a header of the bridge and never holds one of its objects; what crosses is SecondSonPC's types.
//
// Guest memory: the translator reads, besides the program's code (passed in), the fetch shader, the vertex buffer
// descriptors and the SRT-walked user data. With SecondSonPC's translator interface those reads go through
// `memory` (bounded copies, never a raw pointer into guest memory), the SRT walk is SecondSonPC's interpreter, and a
// given `flatbuf` (a capture's, offline) replaces the walk altogether.
//
// Threads: Translate may be called from any thread; an implementation serializes what it shares (the bridge's
// recompiler shares one SRT code buffer with the bridge's own translations — patch 0030 locks it).
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Shader {

struct TranslationRequest {
    HwStage hw = HwStage::Vs;
    SwStage sw = SwStage::Vertex;
    u64 address = 0;            ///< the program's guest address (inline constant buffers are relative to it)
    std::span<const u32> code;  ///< the program's code (the caller's copy, valid during the call)
    std::array<u32, 16> user_data{};
    RuntimeParams runtime;
    const DeviceProfile* profile = nullptr;
    BindingStart start;
    GuestReader* memory = nullptr;   ///< fetch shader, V#, SRT memory; required unless every read is given
    std::span<const u32> flatbuf;    ///< non-empty: the flattened user data to use instead of walking the SRT
    std::span<const u32> fetch_code; ///< non-empty: the fetch shader's code to use instead of reading it
    std::span<const std::array<u32, 4>> vertex_sharps; ///< non-empty: the fetch attributes' V#s (else read)
    u64 footer_hash = 0; ///< the program's footer hash (the translator's names and dumps)
};

enum class TranslationError : u8 {
    None,
    NotInstalled,      ///< no translator
    BadRequest,        ///< missing code/profile/memory, unsupported stage
    FetchShader,       ///< the fetch shader is unreadable or not of the form the driver generates
    UnsupportedOpcode, ///< the recompiler does not know an instruction (named in the detail)
    Srt,               ///< the SRT walk failed (unsupported walker instruction, flattened size mismatch)
    Emit,              ///< SPIR-V emission failed
    Count,
};
std::string_view TranslationErrorName(TranslationError error);

struct TranslationResult {
    TranslationError error = TranslationError::None;
    std::string detail;
    std::vector<u32> spirv;
    ShaderReflection reflection;
    std::vector<u32> flatbuf;    ///< the flattened user data the translation saw
    std::vector<u32> fetch_code; ///< the fetch shader's code it read (empty: none)
    std::vector<std::array<u32, 4>> vertex_sharps; ///< the V# of each fetch attribute it read (in attribute order)
    u64 translate_us = 0;        ///< GCN decode, IR, passes
    u64 emit_us = 0;             ///< SPIR-V emission
    bool Ok() const {
        return error == TranslationError::None;
    }
};

class ShaderTranslator {
public:
    virtual ~ShaderTranslator() = default;
    virtual std::string_view Name() const = 0;
    virtual TranslationResult Translate(const TranslationRequest& request) = 0;
    /// Compares `ours` — as the translator's own runtime information with `profile` — with `reference` (that
    /// information's bytes from the translator's owner, e.g. the bridge's at a capture or a draw). true: equal; false
    /// with `diff` naming the fields.
    virtual bool CompareRuntime(const RuntimeParams& ours, const DeviceProfile& profile, std::span<const u8> reference,
                                std::string& diff) = 0;
};

/// The process's translator (installed by the adapter layer at start-up); nullptr before.
ShaderTranslator* GetShaderTranslator();
void InstallShaderTranslator(ShaderTranslator* translator);

} // namespace SecondSon::Gpu::Shader
