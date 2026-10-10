# SecondSonPC — game-specific PC compatibility runtime for inFAMOUS: Second Son.
cmake_minimum_required(VERSION 3.24)
project(SecondSonPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "SecondSonPC requires clang-cl: guest code calls the host with the SysV ABI")
endif()

# TEMPORARY (Phase 0-4): build on top of the shadPS4 core. Removed in Phase 5 (see docs/AUDIT.md).
option(SSPC_SHADPS4_BRIDGE "Temporary bootstrap on the shadPS4 core" ON)
if(NOT SSPC_SHADPS4_BRIDGE)
    message(FATAL_ERROR "A build without the shadPS4 bridge is the Phase 6 goal and not available yet")
endif()
include(cmake/shadps4_bootstrap.cmake) # provides fmt::fmt, toml11::toml11 and the bridge target

# Own code. Rule: nothing outside src/bridge/ may include shadPS4 headers.
add_library(sspc_runtime STATIC
    src/app/config.cpp
    src/app/paths.cpp
    src/base/crash.cpp
    src/base/log.cpp
    src/base/vram_monitor.cpp
    src/input/keyboard_mouse.cpp
    src/input/motion.cpp
    src/input/settings.cpp
    src/loader/bindings.cpp
    src/loader/eh_frame.cpp
    src/loader/image_file.cpp
    src/loader/linker.cpp
    src/loader/module.cpp
    src/loader/rtld.cpp
    src/loader/snapshot.cpp
    src/runtime/app_content/app_content.cpp
    src/runtime/cpu/exceptions.cpp
    src/runtime/cpu/fs_patcher.cpp
    src/runtime/cpu/sse4a.cpp
    src/runtime/cpu/tcb_slot.cpp
    src/runtime/exports.cpp
    src/runtime/filesystem/directory.cpp
    src/runtime/filesystem/file_system.cpp
    src/runtime/filesystem/fs_observer.cpp
    src/runtime/filesystem/vfs.cpp
    src/runtime/kernel/condvar.cpp
    src/runtime/kernel/event_flag.cpp
    src/runtime/kernel/guest_errno.cpp
    src/runtime/kernel/kernel_memory.cpp
    src/runtime/kernel/kernel_util.cpp
    src/runtime/kernel/memory.cpp
    src/runtime/kernel/mutex.cpp
    src/runtime/kernel/rwlock.cpp
    src/runtime/kernel/semaphore.cpp
    src/runtime/kernel/signal.cpp
    src/runtime/kernel/thread.cpp
    src/runtime/kernel/time.cpp
    src/runtime/language.cpp
    src/runtime/libc/libc_mem.cpp
    src/runtime/memory/direct_memory.cpp
    src/runtime/memory/host_address_space.cpp
    src/runtime/memory/virtual_memory.cpp
    src/runtime/memory/vma_map.cpp
    src/runtime/net/net_util.cpp
    src/runtime/pad/pad.cpp
    src/runtime/playgo/playgo.cpp
    src/runtime/psf/param_sfo.cpp
    src/runtime/sysmodule/sysmodule.cpp
    src/runtime/system/system_service.cpp
    src/runtime/user/user_service.cpp
    src/trace/import_counter.cpp
)
target_include_directories(sspc_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# PRIVATE: must not leak into the shadPS4 sources of the bridge target.
target_compile_definitions(sspc_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(sspc_runtime PRIVATE /utf-8 /W4)
# SDL3: SecondSonPC's own input layer (src/input) reads SDL events; the library comes from the
# bridge's externals until SecondSonPC has its own window (Phase 4).
target_link_libraries(sspc_runtime PUBLIC fmt::fmt toml11::toml11 dxgi SDL3::SDL3)
# Zydis (x86 decoder, MIT): SecondSonPC's FS patcher (src/runtime/cpu, P3.3). An independent library that
# comes from the bridge's externals until SecondSonPC vendors its own copy (Phase 5).
target_link_libraries(sspc_runtime PRIVATE Zydis::Zydis)
# P3.4: the commit in fatal exception reports (as of the last configure; "-dirty" with local changes then).
execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=10
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE SSPC_GIT_REV
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set_source_files_properties(src/runtime/cpu/exceptions.cpp PROPERTIES
    COMPILE_DEFINITIONS "SSPC_GIT_REV=\"${SSPC_GIT_REV}\"")

sspc_attach_bootstrap(sspc_runtime)
