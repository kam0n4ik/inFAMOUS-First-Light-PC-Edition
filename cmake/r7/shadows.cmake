# SecondSonPC R7: the sources of R7-S (feature/r7-native-shadows), owned by that branch alone
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_SHADOWS_SOURCES go into sspc_runtime (/arch:AVX etc. with
# set_source_files_properties here).
set(SSPC_R7_SHADOWS_SOURCES
    src/native_engine/reconstructed/shadow_casters.cpp
    src/native_engine/r7/shadows/shadow_stats.cpp
    src/native_engine/r7/shadows/shadow_draw_command.cpp
    src/native_engine/r7/shadows/shadow_params.cpp
)
# the instance records: the original's AVX instructions (vpermilps / vmulps / vaddps / vpblendw)
set_source_files_properties(src/native_engine/reconstructed/shadow_casters.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")
