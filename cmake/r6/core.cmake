# SecondSonPC R6: the sources of R6-Core (feature/r6-core), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md §3).
# SSPC_R6_CORE_SOURCES go into sspc_runtime, SSPC_R6_CORE_TEST_SOURCES into sspc_native_engine_tests (list a
# reconstruction in both; /arch:AVX etc. with set_source_files_properties here).
set(SSPC_R6_CORE_SOURCES
    src/native_engine/r6/cue_dedup.cpp
)
set(SSPC_R6_CORE_TEST_SOURCES
    src/native_engine/r6/cue_dedup.cpp
    tools/native_engine_tests/r6_core_tests.cpp
)
