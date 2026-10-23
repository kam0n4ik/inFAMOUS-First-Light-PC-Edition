# SecondSonPC R7: the sources of R7-R (feature/r7-native-resources), owned by that branch alone
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_RESOURCES_SOURCES go into sspc_runtime (/arch:AVX etc. with
# set_source_files_properties here).
set(SSPC_R7_RESOURCES_SOURCES
    src/native_engine/r7/resources/resource_state.cpp
)
