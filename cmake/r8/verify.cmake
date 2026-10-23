# SecondSonPC R8: the sources of R8-A5 Verification (feature/r8-verify): differential, state and frame checks
# (docs/NATIVE_ENGINE_R8_CORE.md §3). SSPC_R8_VERIFY_SOURCES go into sspc_runtime.
set(SSPC_R8_VERIFY_SOURCES
    src/runtime/gpu/r8/r8_verify.cpp
    src/runtime/gpu/r8/r8_verify_stage.cpp
    src/runtime/gpu/r8/r8_verify_frame.cpp
)
