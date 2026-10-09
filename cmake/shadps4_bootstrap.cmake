# TEMPORARY (Phase 0-4) bootstrap on the shadPS4 core. Removed in Phase 5 (docs/AUDIT.md, section 12).
#
# The stock `shadps4` executable target is reused as-is (all its sources, generated files and the
# link options the core depends on: image base 0x700000000000, no ASLR, 2 MiB stack), except that
# its src/main.cpp is replaced by SecondSonPC's main and the output is named SecondSonPC.exe.
# No file under third_party/shadPS4 is modified.

# Temporary source patches for the pinned checkout (each one is listed in docs/PATCHES.md).
# Whether they are applied is decided for the whole chain, not patch by patch: a later patch may change
# the context of an earlier one (0008 does for 0007), and then the earlier one no longer reverse-applies
# to the fully patched tree. The tree "HEAD + every patch in order" (built in a temporary git index) is
# compared with the checkout's src/: equal = applied; src/ as in HEAD = apply them all in order; anything
# else stops the configure (local changes, or a patch out of date).
find_package(Git REQUIRED)
file(GLOB _sspc_shadps4_patches ${CMAKE_SOURCE_DIR}/patches/shadps4/*.patch)
list(SORT _sspc_shadps4_patches)
list(LENGTH _sspc_shadps4_patches _sspc_patch_count)
set(_sspc_shad_dir ${CMAKE_SOURCE_DIR}/third_party/shadPS4)
function(_sspc_git out_var)
    execute_process(COMMAND ${GIT_EXECUTABLE} ${ARGN} WORKING_DIRECTORY ${_sspc_shad_dir}
                    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _result EQUAL 0)
        unset(ENV{GIT_INDEX_FILE})
        message(FATAL_ERROR "git ${ARGN} failed: ${_error}")
    endif()
    set(${out_var} "${_output}" PARENT_SCOPE)
endfunction()
set(ENV{GIT_INDEX_FILE} ${CMAKE_BINARY_DIR}/sspc_patches_expected.index)
_sspc_git(_ignored read-tree HEAD)
foreach(_patch IN LISTS _sspc_shadps4_patches)
    _sspc_git(_ignored apply --cached ${_patch})
endforeach()
_sspc_git(_sspc_expected write-tree --prefix=src/)
set(ENV{GIT_INDEX_FILE} ${CMAKE_BINARY_DIR}/sspc_patches_checkout.index)
_sspc_git(_ignored read-tree HEAD)
_sspc_git(_ignored add -A -- src)
_sspc_git(_sspc_checkout write-tree --prefix=src/)
unset(ENV{GIT_INDEX_FILE})
_sspc_git(_sspc_head rev-parse HEAD:src)
if(_sspc_checkout STREQUAL _sspc_expected)
    message(STATUS "SecondSonPC: the ${_sspc_patch_count} shadPS4 patches are applied")
elseif(_sspc_checkout STREQUAL _sspc_head)
    foreach(_patch IN LISTS _sspc_shadps4_patches)
        _sspc_git(_ignored apply ${_patch})
        message(STATUS "SecondSonPC: applied ${_patch}")
    endforeach()
else()
    message(FATAL_ERROR "third_party/shadPS4/src is neither HEAD nor HEAD + patches/shadps4 (local changes, "
                        "or a patch out of date: regenerate it, see docs/PATCHES.md)")
endif()

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
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/selftest_vmm.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/selftest_native.cpp
        ${CMAKE_SOURCE_DIR}/src/bridge/shadps4/native_memory.cpp
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
