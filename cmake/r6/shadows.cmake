# SecondSonPC R6: the sources of R6-S (feature/r6-shadows), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md
# §3). SSPC_R6_SHADOWS_SOURCES go into sspc_runtime (/arch:AVX etc. with set_source_files_properties here).
set(SSPC_R6_SHADOWS_SOURCES
    src/native_engine/reconstructed/shadow_trace.cpp
    src/native_engine/reconstructed/shadow_loop.cpp
)
