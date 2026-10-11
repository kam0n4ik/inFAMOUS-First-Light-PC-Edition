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
    src/base/lifecycle.cpp
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
    src/runtime/gpu/backend.cpp
    src/runtime/gpu/command_processor.cpp
    src/runtime/gpu/cp_registers.cpp
    src/runtime/gpu/cp_replay.cpp
    src/runtime/gpu/cp_shadow.cpp
    src/runtime/gpu/cp_validation.cpp
    src/runtime/gpu/frame_pacing.cpp
    src/runtime/gpu/gnm/gnm_driver.cpp
    src/runtime/gpu/pm4.cpp
    src/runtime/gpu/pm4_stats.cpp
    src/runtime/gpu/shader/gcn_program.cpp
    src/runtime/gpu/shader/pipeline_state.cpp
    src/runtime/gpu/shader/runtime_params.cpp
    src/runtime/gpu/shader/shader_cache.cpp
    src/runtime/gpu/shader/shader_capture.cpp
    src/runtime/gpu/shader/shader_frontend.cpp
    src/runtime/gpu/shader/shader_resources.cpp
    src/runtime/gpu/shader/shader_translator.cpp
    src/runtime/gpu/vulkan/vk_buffer_cache.cpp
    src/runtime/gpu/vulkan/vk_device_context.cpp
    src/runtime/gpu/vulkan/vk_diagnostics.cpp
    src/runtime/gpu/vulkan/vk_executor.cpp
    src/runtime/gpu/vulkan/vk_formats.cpp
    src/runtime/gpu/vulkan/vk_image_cache.cpp
    src/runtime/gpu/vulkan/vk_pipelines.cpp
    src/runtime/gpu/vulkan/vk_presenter.cpp
    src/runtime/gpu/vulkan/vk_resources.cpp
    src/runtime/gpu/host_shaders.cpp
    src/runtime/gpu/image_layout.cpp
    src/runtime/gpu/native_backend.cpp
    src/runtime/gpu/native_videoout.cpp
    src/runtime/gpu/page_tracker.cpp
    src/runtime/gpu/png_writer.cpp
    src/runtime/gpu/vulkan/vk_foundation.cpp
    src/runtime/kernel/clock.cpp
    src/runtime/kernel/condvar.cpp
    src/runtime/kernel/event_flag.cpp
    src/runtime/kernel/event_queues.cpp
    src/runtime/kernel/guest_errno.cpp
    src/runtime/kernel/kernel_memory.cpp
    src/runtime/kernel/kernel_util.cpp
    src/runtime/kernel/memory.cpp
    src/runtime/kernel/mutex.cpp
    src/runtime/kernel/process.cpp
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
# P4.1: the Vulkan headers (header-only, from the bridge's externals) for SecondSonPC's Vulkan foundation
# (src/runtime/gpu/vulkan). No loader is linked: the system vulkan-1.dll is loaded at run time, as the bridge does.
target_link_libraries(sspc_runtime PRIVATE Vulkan::Headers)
# P4.3: xxHash (BSD-2, from the bridge's externals like Zydis): programs' content identity, cache keys.
target_link_libraries(sspc_runtime PRIVATE xxHash::xxhash)
# P3.4: the commit in fatal exception reports (as of the last configure; "-dirty" with local changes then).
execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=10
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE SSPC_GIT_REV
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set_source_files_properties(src/runtime/cpu/exceptions.cpp PROPERTIES
    COMPILE_DEFINITIONS "SSPC_GIT_REV=\"${SSPC_GIT_REV}\"")

sspc_attach_bootstrap(sspc_runtime)

# P3.6.6: the SecondSonPC <-> bridge boundary, checked whenever a source, a patch or the allowlist changes
# (cmake/sspc_boundary_check.cmake; report in the build directory). A violation fails the build.
file(GLOB_RECURSE _sspc_boundary_inputs CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h ${CMAKE_SOURCE_DIR}/src/*.inc)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/sspc_boundary.stamp
    COMMAND ${CMAKE_COMMAND} -DSSPC_SRC=${CMAKE_SOURCE_DIR}/src
            -DSSPC_SHAD=${CMAKE_SOURCE_DIR}/third_party/shadPS4/src
            -DSSPC_PATCHES=${CMAKE_SOURCE_DIR}/patches/shadps4
            -DSSPC_ALLOWLIST=${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_allowlist.txt
            -DSSPC_REPORT=${CMAKE_BINARY_DIR}/sspc_boundary_report.txt
            -DSSPC_STAMP=${CMAKE_BINARY_DIR}/sspc_boundary.stamp
            -P ${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_check.cmake
    DEPENDS ${_sspc_boundary_inputs} ${_sspc_shadps4_patches} ${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_allowlist.txt
            ${CMAKE_SOURCE_DIR}/cmake/sspc_boundary_check.cmake
    COMMENT "SecondSonPC: checking the runtime/bridge boundary"
    VERBATIM)
add_custom_target(sspc_boundary_check DEPENDS ${CMAKE_BINARY_DIR}/sspc_boundary.stamp)
add_dependencies(sspc_runtime sspc_boundary_check)
