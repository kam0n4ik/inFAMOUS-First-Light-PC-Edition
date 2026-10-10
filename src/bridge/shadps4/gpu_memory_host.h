// SecondSonPC — Gpu::GuestMemory over the bridge's Core::GpuMemory (P4.1.3; gpu_memory_host.cpp).
#pragma once

namespace SecondSon::Bridge {

/// Installs the adapter as Gpu::GetGuestMemory(). After the guest memory manager is selected (its first use closes
/// the selection); once.
void InstallGpuGuestMemory();

} // namespace SecondSon::Bridge
