// SecondSonPC — what SecondSonPC's own libSceGnmDriver (gnm_driver.cpp, --gpu-backend sspc) needs from the GPU backend
// and from SecondSonPC's VideoOut (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md).
//
// The driver builds the PM4 the game asks for in the game's own command buffers (pure CPU work), validates and patches
// submissions (the flip marker), and hands them on through this interface. The backend owns the command processor
// (P4.2, Mode::Isolated), executes the work on its Vulkan device and raises the interrupts the hardware would. GnmDriver
// and VideoOut belong to one backend together (backend.h: Resource::CommandStream and Resource::Presentation): this
// host is installed only by SecondSonPC's backend, and the driver refuses every call before that.
#pragma once

#include "base/abi.h"

namespace SecondSon::Gpu::Gnm {

/// The hardware's interrupt ids as Gnm names them (sceGnmAddEqEvent's `id`, the event's data).
enum class Interrupt : u32 {
    Compute0RelMem = 0x00, ///< a compute pipe's RELEASE_MEM with an interrupt (pipe 0..6)
    Compute6RelMem = 0x06,
    GfxFlip = 0x08,
    GpuIdle = 0x09, ///< every submission processed and executed after a SubmitDone
    GfxEop = 0x40,  ///< the graphics ring's end-of-pipe event with an interrupt
};

class Host {
public:
    virtual ~Host() = default;

    /// Graphics command buffer: `dwords` at guest `addr`. `data`: the dwords when they are not guest memory (the
    /// driver's own initialisation sequence; static), else nullptr. false: refused (the backend logged why).
    virtual bool SubmitGraphics(u64 addr, u32 dwords, const u32* data) = 0;
    /// A compute queue (Gnm vqid 1..64; pipe = what sceGnmMapComputeQueue named) and its ring.
    virtual bool MapComputeQueue(u32 vqid, u32 pipe, u64 ring_addr, u32 ring_dwords, u64 read_ptr_addr) = 0;
    virtual void UnmapComputeQueue(u32 vqid) = 0;
    /// sceGnmDingDong: the queue's ring up to `next_offset_dw` is ready.
    virtual bool DingDong(u32 vqid, u32 next_offset_dw) = 0;
    /// sceGnmSubmitDone: the frame's submissions are complete; Interrupt::GpuIdle follows once all of it executed.
    virtual void SubmitDone() = 0;
    /// Every submission processed and every GPU work item complete.
    virtual bool IsGpuIdle() = 0;

    /// VideoOut (SecondSonPC's): a flip of `buffer_index` requested by a submission's flip marker; it happens once the
    /// GPU work before the marker is complete. 0 or the VideoOut error (0x80290012: the flip queue is full).
    virtual s32 SubmitEopFlip(s32 video_out_handle, u32 buffer_index, u32 flip_mode, s64 flip_arg) = 0;
    /// VideoOut: the guest address of the port's buffer labels (one u64 per buffer). false: no such port.
    virtual bool BufferLabelAddress(s32 video_out_handle, u64& address) = 0;
};

/// Installed by SecondSonPC's backend when it starts with --gpu-backend sspc (and removed when it stops); nullptr before.
void SetHost(Host* host);
Host* GetHost();

/// The backend raises an interrupt (its GPU thread): the driver wakes the event queues registered for it with
/// sceGnmAddEqEvent and its own waiters (GpuIdle releases a submission lock taken by SubmitDone).
void RaiseInterrupt(Interrupt id);

/// The game's SDK version as the driver uses it to choose its initialisation sequence (sceKernelGetCompiledSdkVersion).
void SetSdkVersion(s32 version);

} // namespace SecondSon::Gpu::Gnm
