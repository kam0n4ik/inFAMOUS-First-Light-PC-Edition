// SecondSonPC — the installed GCN -> SPIR-V translator (P4.3). See shader_translator.h.
#include "runtime/gpu/shader/shader_translator.h"

#include <atomic>
#include <iterator>

namespace SecondSon::Gpu::Shader {

namespace {
std::atomic<ShaderTranslator*> g_translator{nullptr};
}

ShaderTranslator* GetShaderTranslator() {
    return g_translator.load(std::memory_order_acquire);
}

void InstallShaderTranslator(ShaderTranslator* translator) {
    g_translator.store(translator, std::memory_order_release);
}

std::string_view TranslationErrorName(TranslationError error) {
    static constexpr std::string_view kNames[] = {"none",         "no translator", "bad request", "fetch shader",
                                                  "unknown opcode", "SRT walk",     "SPIR-V emission"};
    static_assert(std::size(kNames) == static_cast<size_t>(TranslationError::Count));
    const auto i = static_cast<size_t>(error);
    return i < std::size(kNames) ? kNames[i] : "?";
}

} // namespace SecondSon::Gpu::Shader
