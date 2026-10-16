// SecondSonPC - SecondSon Native CP (docs/NATIVE_CP.md): the native backend's command processor mode.
#pragma once

#include <string>

namespace SecondSon::Gpu {

/// The command processors that exist (chosen once, before the native backend starts: a switch while the game runs
/// would mix the processors' states - a restart changes it):
///   legacy   - one thread: processor, shader frontend and Vulkan recording (SSPC_P410_PARALLEL_CP=0)
///   parallel - the processor + shader frontend on one thread, the Vulkan recording on a second (P4.10; the default)
///   native   - parallel with the Native CP's measured changes: shader code trust "sampled" (SSPC_P52_CODE_TRUST=
///              sampled), register writes that change nothing not journaled (SSPC_CP_JOURNAL_CHANGES=1), SRT walks
///              reading guest memory by 64-byte lines (SSPC_CP_SRT_LINES=1)
/// `mode` empty: SSPC_CP_MODE, else kDefaultCpMode. A variable the mode presets keeps a value given explicitly. Returns
/// the mode that runs (logged; SSPC_CP_MODE then holds it). Without the native backend nothing is set.
std::string ApplyCpMode(std::string mode, bool native_backend);

inline constexpr const char* kDefaultCpMode = "parallel";

} // namespace SecondSon::Gpu
