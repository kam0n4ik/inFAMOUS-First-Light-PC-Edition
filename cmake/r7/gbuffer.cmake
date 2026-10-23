# SecondSonPC R7: the sources of R7-G (feature/r7-native-gbuffer), owned by that branch alone
# (docs/NATIVE_ENGINE_R7_CORE.md §3). SSPC_R7_GBUFFER_SOURCES go into sspc_runtime (/arch:AVX etc. with
# set_source_files_properties here).
set(SSPC_R7_GBUFFER_SOURCES
    src/native_engine/r7/gbuffer/geometry_pass.cpp
    src/native_engine/r7/gbuffer/arg_liveness.cpp
    src/native_engine/r7/gbuffer/draw_command.cpp
    src/native_engine/reconstructed/gbuffer_mesh_pass.cpp
)
# the instance records and the light rows: AVX as the eboot's code (vpermilps, vblendps), no FMA (nothing to contract into)
set_source_files_properties(src/native_engine/r7/gbuffer/geometry_pass.cpp PROPERTIES COMPILE_OPTIONS
                            "/arch:AVX")
set_source_files_properties(src/native_engine/reconstructed/gbuffer_mesh_pass.cpp PROPERTIES COMPILE_OPTIONS
                            "/arch:AVX")
