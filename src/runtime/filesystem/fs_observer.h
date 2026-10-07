// SecondSonPC — Filesystem observers (Phase 0).
// Logs what the game opens, then forwards to the current implementation (BRIDGE in Phase 0).
// Replaced by the native FileSystem module in Phase 1.
#pragma once

#include "base/abi.h"

namespace SecondSon::Filesystem {

// Filled by the bootstrap with the address of the implementation being observed.
inline void* g_open_original = nullptr;
inline void* g_stat_original = nullptr;

// NID 1G3lF1Gg1k8. s32 sceKernelOpen(const char* path, s32 flags, u16 mode)
SSPC_SYSV_ABI s32 ObserveOpen(const char* path, s32 flags, u16 mode);
// NID eV9wAD2riIA. s32 sceKernelStat(const char* path, OrbisKernelStat* sb)
SSPC_SYSV_ABI s32 ObserveStat(const char* path, void* sb);

} // namespace SecondSon::Filesystem
