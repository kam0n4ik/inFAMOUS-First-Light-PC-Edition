// SecondSonPC — module list, relocation, TLS and program start (Phase 1 Loader).
// Replaces shadPS4's Core::Linker. The sequence (load order, relocation passes, TLS layout,
// main-thread start-up) reproduces the Phase 0 oracle so both paths reach the eboot entry with the
// same image.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "loader/module.h"

namespace SecondSon::Loader {

class Bindings;
class Host;

/// Guest thread control block (FS base on the console). Same layout as libkernel's.
union DtvEntry {
    u64 counter;
    u8* pointer;
};
struct GuestTcb {
    GuestTcb* self;
    DtvEntry* dtv;
    void* thread;
    void* spare[2];
    u64 canary;
    void* fiber;
};
static_assert(sizeof(GuestTcb) == 0x38);

struct KernelMemParam {
    u64 size;
    u64* extended_page_table;
    u64* flexible_memory_size;
    u8* extended_memory_1;
    u64* extended_gpu_page_table;
    u8* extended_memory_2;
    u64* extended_cpu_page_table;
};
static_assert(sizeof(KernelMemParam) == 0x38);

/// SceProcParam (PT_SCE_PROCPARAM of the executable).
struct ProcParam {
    u64 size;
    u32 magic;
    u32 entry_count;
    u64 sdk_version;
    char* process_name;
    char* main_thread_name;
    u32* main_thread_prio;
    u32* main_thread_stack_size;
    void* libc_param;
    KernelMemParam* mem_param;
    void* fs_param;
    u32* process_preload_enable;
    u64 unknown1;
};
static_assert(sizeof(ProcParam) == 0x60);

using GuestPathResolver = std::function<std::filesystem::path(std::string_view guest_path)>;

class Linker {
public:
    Linker(Host& host, const Bindings& bindings, GuestPathResolver resolver);
    ~Linker();
    Linker(const Linker&) = delete;
    Linker& operator=(const Linker&) = delete;

    /// The instance serving this session, nullptr when the Phase 0 (shadPS4) loader runs.
    static Linker* Active();

    /// Loads (maps, patches, parses) a module. Returns its handle or -1.
    s32 LoadModule(std::string_view guest_path, bool is_dynamic);
    /// Loads, relocates and starts (DT_INIT) a module once; returns its handle or -1.
    s32 LoadAndStartModule(std::string_view guest_path, u64 args, const void* argp, s32* result);

    Module* GetModule(s32 handle) const;
    size_t ModuleCount() const;
    Module* FindByAddress(u64 address) const;
    ProcParam* GetProcParam() const;
    u64 SdkVersion() const;

    void RelocateAll();

    // TLS (guest threads; the bridge's pthread code calls CreateTcb/DestroyTcb)
    GuestTcb* CreateTcb(void* thread, bool initial);
    void DestroyTcb(GuestTcb* tcb);
    void* TlsGetAddr(u64 modid, u64 offset);
    void SetHeapApi(void** functions);

    /// Program start (console order): static TLS, relocation, memory regions, then thread
    /// "Game:Main": `preload` (libSceSysmodule loads the game's PRX), libSceGnmDriver's mapping,
    /// `before_entry` (diagnostics, may be empty), jump to the eboot entry. Returns immediately.
    void Execute(std::vector<std::string> args, std::function<void()> preload,
                 std::function<void()> before_entry);

private:
    struct HeapApi {
        SSPC_SYSV_ABI void* (*heap_malloc)(u64);
        SSPC_SYSV_ABI void (*heap_free)(void*);
    };

    void Relocate(Module& m);
    void RelocateAnyImports(Module& m);
    bool Resolve(const Module& m, std::string_view name, u8 sym_type, u64& address);
    void* AllocateTls(bool primary);
    void FreeTls(void* pointer);
    Module* ModuleByTlsId(u64 modid) const;
    [[noreturn]] void MainThread();

    Host& host_;
    const Bindings& bindings_;
    GuestPathResolver resolver_;
    mutable std::recursive_mutex mutex_; // module list + relocation + dynamic TLS
    std::mutex tcb_mutex_;               // TCB construction/destruction (libkernel's rtld lock)
    std::vector<std::unique_ptr<Module>> modules_;
    u32 num_static_modules_ = 0;
    u32 max_tls_index_ = 0;
    u64 static_tls_size_ = 0;
    u64 generation_ = 1;
    HeapApi* heap_api_ = nullptr;
    std::vector<std::string> args_;
    std::function<void()> preload_;
    std::function<void()> before_entry_;
};

} // namespace SecondSon::Loader
