// SecondSonPC — the guest TCB slot (see tcb_slot.h).
#include "runtime/cpu/tcb_slot.h"

#include <mutex>

#include <windows.h>

#include "base/log.h"

namespace SecondSon::Cpu {

namespace {

DWORD g_slot = TLS_OUT_OF_INDEXES;
std::once_flag g_once;

} // namespace

bool InitTcbSlot() {
    std::call_once(g_once, [] {
        g_slot = TlsAlloc();
        if (g_slot == TLS_OUT_OF_INDEXES) {
            SSPC_LOG(Cpu, Critical, "guest TCB: no Windows TLS slot left ({})", GetLastError());
            return;
        }
        SSPC_LOG(Cpu, Info, "guest TCB: Windows TLS slot {} ({}), owned by SecondSonPC", g_slot,
                 g_slot < 64 ? "TEB TlsSlots" : "TLS expansion slots");
    });
    return g_slot != TLS_OUT_OF_INDEXES;
}

u32 TcbSlot() {
    return g_slot;
}

void SetCurrentTcb(void* tcb) {
    // Cannot fail for an allocated slot (TlsSetValue only checks the index).
    TlsSetValue(g_slot, tcb);
}

void* CurrentTcb() {
    return TlsGetValue(g_slot);
}

} // namespace SecondSon::Cpu
