# SecondSonPC R6: the sources of R6-G (feature/r6-gbuffer), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md
# §3). SSPC_R6_GBUFFER_SOURCES go into sspc_runtime (/arch:AVX etc. with set_source_files_properties here).
set(SSPC_R6_GBUFFER_SOURCES
    src/native_engine/reconstructed/gbuffer_trace.cpp
    src/native_engine/reconstructed/gbuffer_draw.cpp
    src/native_engine/reconstructed/gbuffer_frames_game.cpp
)
