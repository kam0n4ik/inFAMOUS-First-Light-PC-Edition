# SecondSonPC R6: the sources of R6-S (feature/r6-shadows), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md §3).
# SSPC_R6_SHADOWS_SOURCES go into sspc_runtime, SSPC_R6_SHADOWS_TEST_SOURCES into sspc_native_engine_tests (list a
# reconstruction in both; /arch:AVX etc. with set_source_files_properties here).
set(SSPC_R6_SHADOWS_SOURCES
)
set(SSPC_R6_SHADOWS_TEST_SOURCES
    tools/native_engine_tests/r6_shadows_tests.cpp
)
