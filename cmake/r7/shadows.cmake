# SecondSonPC R7: the sources of R7-S (feature/r7-native-shadows), owned by that branch alone
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_SHADOWS_SOURCES go into sspc_runtime, SSPC_R7_SHADOWS_TEST_SOURCES into
# sspc_native_engine_tests (list a reconstruction in both; /arch:AVX etc. with set_source_files_properties here).
set(SSPC_R7_SHADOWS_SOURCES
    src/native_engine/reconstructed/shadow_casters.cpp
    src/native_engine/r7/shadows/shadow_stats.cpp
    src/native_engine/r7/shadows/shadow_draw_command.cpp
    src/native_engine/r7/shadows/shadow_params.cpp
)
set(SSPC_R7_SHADOWS_TEST_SOURCES
    tests/native_engine_tests/r7_shadows_tests.cpp
    src/native_engine/reconstructed/shadow_casters.cpp
    src/native_engine/r7/shadows/shadow_stats.cpp
    src/native_engine/r7/shadows/shadow_draw_command.cpp
    src/native_engine/r7/shadows/shadow_params.cpp
)
# the instance records: the original's AVX instructions (vpermilps / vmulps / vaddps / vpblendw)
set_source_files_properties(src/native_engine/reconstructed/shadow_casters.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")
