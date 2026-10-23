# SecondSonPC R9: the gameplay profiler (docs/R9_GAMEPLAY_PERFORMANCE.md §2) - every thread's CPU time per tick, code
# samples weighted by cycles, subsystem counters and marks (SSPC_R9_GPROF=1|2, off by default). SSPC_R9_PROFILE_SOURCES
# go into sspc_runtime.
set(SSPC_R9_PROFILE_SOURCES
    src/runtime/gpu/r9/gprof.cpp
)
