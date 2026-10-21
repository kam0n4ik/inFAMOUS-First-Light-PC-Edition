# SecondSonPC R8: the sources of R8-A1 CP Profiler (feature/r8-cp-profile): the low-overhead profiler of the command processor
# (docs/NATIVE_ENGINE_R8_CORE.md §3). SSPC_R8_PROFILE_SOURCES go into sspc_runtime, SSPC_R8_PROFILE_TEST_SOURCES into
# sspc_native_engine_tests.
set(SSPC_R8_PROFILE_SOURCES
    src/runtime/gpu/r8/cp_profile.cpp
)
set(SSPC_R8_PROFILE_TEST_SOURCES
    tools/native_engine_tests/r8_profile_tests.cpp
)
