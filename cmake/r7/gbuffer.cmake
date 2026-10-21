# SecondSonPC R7: the sources of R7-G (feature/r7-native-gbuffer), owned by that branch alone
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_GBUFFER_SOURCES go into sspc_runtime, SSPC_R7_GBUFFER_TEST_SOURCES into
# sspc_native_engine_tests (list a reconstruction in both; /arch:AVX etc. with set_source_files_properties here).
set(SSPC_R7_GBUFFER_SOURCES
)
set(SSPC_R7_GBUFFER_TEST_SOURCES
    tools/native_engine_tests/r7_gbuffer_tests.cpp
)
