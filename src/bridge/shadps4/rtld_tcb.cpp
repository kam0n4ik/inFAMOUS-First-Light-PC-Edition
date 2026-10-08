// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY Phase 1-2 bridge. Replaces, at build time,
// third_party/shadPS4/src/core/libraries/kernel/threads/tcb.cpp (@ 0fe263a; see
// cmake/shadps4_bootstrap.cmake), the only place where shadPS4's pthreads (BRIDGE until Phase 2)
// touch the module loader:
//   * with the SecondSonPC Loader (Loader::Linker::Active()), TCB/TLS come from it;
//   * otherwise (Phase 0 path, --loader shadps4) the original code below runs unchanged.
#include "common/assert.h"
#include "common/singleton.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/libs.h"
#include "core/linker.h"
#include "core/tls.h"

#include "loader/linker.h"

namespace Libraries::Kernel {

static_assert(sizeof(Core::Tcb) == sizeof(SecondSon::Loader::GuestTcb));
static_assert(offsetof(Core::Tcb, tcb_dtv) == offsetof(SecondSon::Loader::GuestTcb, dtv));
static_assert(offsetof(Core::Tcb, tcb_thread) == offsetof(SecondSon::Loader::GuestTcb, thread));
static_assert(offsetof(Core::Tcb, tcb_fiber) == offsetof(SecondSon::Loader::GuestTcb, fiber));

static constexpr size_t TlsTcbSize = 0x40;
static constexpr size_t TlsTcbAlign = 0x20;

static std::shared_mutex RtldLock;

Core::Tcb* TcbCtor(Pthread* thread, int initial) {
    if (auto* loader = SecondSon::Loader::Linker::Active()) {
        return reinterpret_cast<Core::Tcb*>(loader->CreateTcb(thread, initial != 0));
    }

    std::scoped_lock lk{RtldLock};

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    auto* addr_out = linker->AllocateTlsForThread(initial);
    ASSERT_MSG(addr_out, "Unable to allocate guest TCB");

    // Initialize allocated memory and allocate DTV table.
    const u32 num_dtvs = linker->MaxTlsIndex();
    const auto static_tls_size = linker->StaticTlsSize();
    auto* dtv_table = new Core::DtvEntry[num_dtvs + 2]{};

    // Initialize thread control block
    u8* addr = reinterpret_cast<u8*>(addr_out);
    auto* tcb = reinterpret_cast<Core::Tcb*>(addr + static_tls_size);
    memset(addr_out, 0, static_tls_size);
    tcb->tcb_self = tcb;
    tcb->tcb_dtv = dtv_table;

    // Dtv[0] is the generation counter. libkernel puts their number into dtv[1]
    dtv_table[0].counter = linker->GenerationCounter();
    dtv_table[1].counter = num_dtvs;

    // Copy init image of main module.
    auto* module = linker->GetModule(0);
    u8* dest = reinterpret_cast<u8*>(addr + static_tls_size - module->tls.offset);

    if (module->tls.image_size != 0) {
        if (module->tls.image_virtual_addr != 0) {
            const u8* src = reinterpret_cast<const u8*>(module->tls.image_virtual_addr);
            memcpy(dest, src, module->tls.init_image_size);
        }
        ASSERT_MSG(module->tls.modid > 0 && module->tls.modid <= num_dtvs);
        tcb->tcb_dtv[module->tls.modid + 1].pointer = dest;
    }

    if (tcb) {
        tcb->tcb_thread = thread;
        tcb->tcb_fiber = nullptr;
    }
    return tcb;
}

void TcbDtor(Core::Tcb* oldtls) {
    if (auto* loader = SecondSon::Loader::Linker::Active()) {
        loader->DestroyTcb(reinterpret_cast<SecondSon::Loader::GuestTcb*>(oldtls));
        return;
    }

    std::scoped_lock lk{RtldLock};
    auto* dtv_table = oldtls->tcb_dtv;

    auto* linker = Common::Singleton<Core::Linker>::Instance();
    const u32 max_tls_index = linker->MaxTlsIndex();
    const u32 num_dtvs = dtv_table[1].counter;
    ASSERT_MSG(num_dtvs <= max_tls_index, "Out of bounds DTV access");

    const u32 static_tls_size = linker->StaticTlsSize();
    u8* tls_base = (u8*)oldtls - static_tls_size;

    for (int i = 1; i < num_dtvs; i++) {
        u8* dtv_ptr = dtv_table[i + 1].pointer;
        if (dtv_ptr && (dtv_ptr < tls_base || (const u8*)oldtls < dtv_ptr)) {
            linker->FreeTlsForNonPrimaryThread(dtv_ptr);
        }
    }
    delete[] dtv_table;

    linker->FreeTlsForNonPrimaryThread(tls_base);
}

struct TlsIndex {
    u64 ti_module;
    u64 ti_offset;
};

void* PS4_SYSV_ABI __tls_get_addr(TlsIndex* index) {
    if (auto* loader = SecondSon::Loader::Linker::Active()) {
        return loader->TlsGetAddr(index->ti_module, index->ti_offset);
    }
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    return linker->TlsGetAddr(index->ti_module, index->ti_offset);
}

void RegisterRtld(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("vNe1w4diLCs", "libkernel", 1, "libkernel", __tls_get_addr);
}

} // namespace Libraries::Kernel
