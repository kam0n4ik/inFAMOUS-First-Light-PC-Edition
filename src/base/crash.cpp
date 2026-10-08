// SecondSonPC — crash report in SecondSonPC.log (unhandled exceptions).
// Symbolize the exe+offset values with:
//   E:\SecondSonPC\toolchain\LLVM\bin\llvm-symbolizer.exe --obj=build\bin\SecondSonPC.exe --relative-address <offset>
#include "base/crash.h"

#include <windows.h>

#include "base/abi.h"
#include "base/log.h"

namespace SecondSon::Base {

namespace {

u64 g_image_base = 0;
u64 g_image_size = 0;

bool InImage(u64 address) {
    return address >= g_image_base && address < g_image_base + g_image_size;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    const auto* ctx = info->ContextRecord;
    const u64 address = reinterpret_cast<u64>(rec->ExceptionAddress);
    SSPC_LOG(App, Critical, "unhandled exception {:#010x} at {:#x}{}", rec->ExceptionCode, address,
             InImage(address) ? fmt::format(" (exe+{:#x})", address - g_image_base) : "");
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        static constexpr const char* kKinds[] = {"read", "write", "?", "?", "?", "?", "?", "?", "execute"};
        const auto kind = rec->ExceptionInformation[0];
        SSPC_LOG(App, Critical, "  access violation: {} of {:#x}", kind < 9 ? kKinds[kind] : "?",
                 rec->ExceptionInformation[1]);
    }
    SSPC_LOG(App, Critical, "  rip {:#x} rsp {:#x} rbp {:#x} rax {:#x} rcx {:#x} rdx {:#x}", ctx->Rip,
             ctx->Rsp, ctx->Rbp, ctx->Rax, ctx->Rcx, ctx->Rdx);
    SSPC_LOG(App, Critical, "  rdi {:#x} rsi {:#x} r8 {:#x} r9 {:#x} r10 {:#x} r11 {:#x}", ctx->Rdi,
             ctx->Rsi, ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    // Return-address candidates: stack qwords that point into the executable image.
    const auto* stack = reinterpret_cast<const u64*>(ctx->Rsp);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(stack, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT) {
        const u64 end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
        int shown = 0;
        for (u64 i = 0; i < 2048 && reinterpret_cast<u64>(stack + i + 1) <= end && shown < 24; ++i) {
            if (InImage(stack[i])) {
                SSPC_LOG(App, Critical, "  stack[{:#x}] exe+{:#x}", i * 8, stack[i] - g_image_base);
                ++shown;
            }
        }
    }
    Log::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallCrashLogger() {
    const auto* base = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_image_base = reinterpret_cast<u64>(base);
    g_image_size = nt->OptionalHeader.SizeOfImage;
    SetUnhandledExceptionFilter(&CrashFilter);
}

} // namespace SecondSon::Base
