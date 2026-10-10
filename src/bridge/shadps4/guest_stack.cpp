// SecondSonPC — guest stacks of the native thread manager (P3.2). See guest_stack.h.
#include "bridge/shadps4/guest_stack.h"

#include "base/log.h"
#include "core/guest_memory.h"

namespace SecondSon::Bridge {

void* MapGuestStack(VAddr hint, u64 size, u64 guard) {
    auto& memory = Core::GetGuestVirtualMemory();
    // The whole range first, reserved: the search happens once, and nothing else can take the stack part
    // before it is mapped over the reservation.
    void* addr = nullptr;
    if (memory.Map(&addr, {.addr = hint,
                           .size = size + guard,
                           .prot = Core::MemoryProt::NoAccess,
                           .flags = Core::MemoryMapFlags::NoFlags,
                           .type = Core::VMAType::Reserved,
                           .name = "stack guard"}) != 0) {
        SSPC_LOG(Kernel, Error, "guest stack: no free range of {:#x} bytes", size + guard);
        return nullptr;
    }
    const VAddr base = reinterpret_cast<VAddr>(addr);
    void* stack = nullptr;
    const s32 r = memory.Map(&stack, {.addr = base + guard,
                                      .size = size,
                                      .prot = Core::MemoryProt::CpuReadWrite,
                                      .flags = Core::MemoryMapFlags::Fixed,
                                      .type = Core::VMAType::Stack,
                                      .name = "stack"});
    if (r != 0 || reinterpret_cast<VAddr>(stack) != base + guard) {
        SSPC_LOG(Kernel, Error, "guest stack: {:#x} bytes at {:#x} could not be mapped ({:#x})", size, base + guard,
                 static_cast<u32>(r));
        if (r == 0) {
            memory.Unmap(reinterpret_cast<VAddr>(stack), size);
        }
        memory.Unmap(base, size + guard);
        return nullptr;
    }
    return stack;
}

bool UnmapGuestStack(void* stack, u64 size, u64 guard) {
    const VAddr base = reinterpret_cast<VAddr>(stack) - guard;
    const s32 r = Core::GetGuestVirtualMemory().Unmap(base, size + guard);
    if (r != 0) {
        SSPC_LOG(Kernel, Error, "guest stack {:#x}+{:#x} (guard {:#x}): unmap refused ({:#x})", base + guard, size,
                 guard, static_cast<u32>(r));
        return false;
    }
    return true;
}

} // namespace SecondSon::Bridge
