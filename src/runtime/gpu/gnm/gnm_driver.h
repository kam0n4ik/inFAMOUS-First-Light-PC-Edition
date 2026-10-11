// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — SecondSonPC's own libSceGnmDriver (Phase 4, P4.4, EXPERIMENTAL: export group "gnm", bound together with
// SecondSonPC's VideoOut only with --gpu-backend sspc; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md). Ported from
// shadPS4's gnmdriver.cpp (GPL-2.0-or-later), the bridge's driver, which stays the oracle (src/bridge/shadps4/
// selftest_gnm.cpp compares every command-building function with it).
//
// The game's 85 libSceGnmDriver imports. Command-building functions write the same PM4 dwords as the bridge's into the
// game's own command buffers (guest addresses are host addresses) and need nothing else. Submissions, compute queues
// and flips go through Gnm::Host (gnm_host.h), installed by SecondSonPC's backend; before that (or after it stopped)
// those functions log an error and refuse. Event queue registrations (sceGnmAddEqEvent) go to the queues' owner
// (Kernel::EventQueues) and are woken by RaiseInterrupt. Base PS4 (non-Neo) only: with Neo mode on the driver logs an
// error when a host is installed and still builds the base PS4's packets.
//
// Kept from the bridge: argument checks, results and error codes; the submission lock (sceGnmSubmitDone takes it while
// the GPU is busy, Interrupt::GpuIdle releases it, every submission and DingDong waits for it); the initialisation
// sequence once before the first submission after each SubmitDone; the flip marker patch of sceGnmSubmitAndFlip*;
// compute queue ids. Where the bridge asserts (or would crash on the game's input) this driver logs an error and
// returns an error code instead (gnm_driver.cpp lists each case); where the bridge is a stub the result is the same
// and a warning says once that the function does nothing.
#pragma once

#include <span>

#include "base/abi.h"
#include "runtime/gpu/gnm/gnm_host.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Gpu::Gnm {

/// The game's libSceGnmDriver imports (group "gnm", experimental), keyed exactly like the game imports them.
std::span<const Runtime::Export> GnmExports();

namespace Test {
/// The driver as at start-up: no submission lock, the initialisation sequence pending, no compute queue mapped, no
/// event registration, SDK version 0. Self-tests only: no host installed and no guest thread inside the driver.
void Reset();
/// sceGnmSubmitDone took the submission lock and no Interrupt::GpuIdle has released it yet.
bool SubmissionLocked();
} // namespace Test

} // namespace SecondSon::Gpu::Gnm
