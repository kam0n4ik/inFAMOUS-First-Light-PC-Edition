# SecondSonPC R7: the sources of R7-R (feature/r7-native-resources), owned by that branch alone
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_RESOURCES_SOURCES go into sspc_runtime, SSPC_R7_RESOURCES_TEST_SOURCES
# into sspc_native_engine_tests (list a reconstruction in both; /arch:AVX etc. with set_source_files_properties here).
set(SSPC_R7_RESOURCES_SOURCES
)
set(SSPC_R7_RESOURCES_TEST_SOURCES
    tools/native_engine_tests/r7_resources_tests.cpp
)
