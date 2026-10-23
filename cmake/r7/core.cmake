# SecondSonPC R7: the sources of R7-Core (feature/r7-native-core): the Native Render Layer and the integration
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_CORE_SOURCES go into sspc_runtime, SSPC_R7_CORE_TEST_SOURCES into
# sspc_native_engine_tests.
set(SSPC_R7_CORE_SOURCES
    src/engine/render/native_render_layer.cpp
)
set(SSPC_R7_CORE_TEST_SOURCES
    tests/native_engine_tests/r7_core_tests.cpp
)
