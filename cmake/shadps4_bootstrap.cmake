# TEMPORARY (Phase 0-4) bootstrap on the shadPS4 core. Removed in Phase 5 (docs/AUDIT.md, section 12).
#
# The stock `shadps4` executable target is reused as-is (all its sources, generated files and the
# link options the core depends on: image base 0x700000000000, no ASLR, 2 MiB stack), except that
# its src/main.cpp is replaced by SecondSonPC's main and the output is named SecondSonPC.exe.
# No file under third_party/shadPS4 is modified.

# Temporary source patches for the pinned checkout (each one is listed in docs/PATCHES.md).
find_package(Git REQUIRED)
file(GLOB _sspc_shadps4_patches ${CMAKE_SOURCE_DIR}/patches/shadps4/*.patch)
list(SORT _sspc_shadps4_patches)
foreach(_patch IN LISTS _sspc_shadps4_patches)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${_patch}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/shadPS4
                    RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already EQUAL 0)
        execute_process(COMMAND ${GIT_EXECUTABLE} apply ${_patch}
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/shadPS4
                        RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "cannot apply ${_patch}")
        endif()
        message(STATUS "SecondSonPC: applied ${_patch}")
    endif()
endforeach()

set(ENABLE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
set(ENABLE_UPDATER OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/shadPS4 ${CMAKE_BINARY_DIR}/shadps4 EXCLUDE_FROM_ALL)

function(sspc_attach_bootstrap runtime_target)
    get_target_property(_sources shadps4 SOURCES)
    list(FILTER _sources EXCLUDE REGEX "(^|/)src/main\\.cpp$")
    # Phase 1: the TCB/TLS glue of shadPS4's pthreads is replaced by a dual-mode copy that takes
    # TLS from SecondSonPC's Loader (src/bridge/shadps4/rtld_tcb.cpp).
    list(FILTER _sources EXCLUDE REGEX "(^|/)src/core/libraries/kernel/threads/tcb\\.cpp$")
    set_property(TARGET shadps4 PROPERTY SOURCES ${_sources})

    target_sources(shadps4 PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/main.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/early_init.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/bootstrap.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/fs_host.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/user_host.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/input_host.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/kernel_host.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/selftest.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/loader_host.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/memory_host.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/rtld_tcb.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/gpu_alloc_diag.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/generated/bridge_exports.cpp
    )
    target_link_libraries(shadps4 PRIVATE ${runtime_target})
    set_target_properties(shadps4 PROPERTIES
        OUTPUT_NAME SecondSonPC
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        EXCLUDE_FROM_ALL OFF
    )
endfunction()
