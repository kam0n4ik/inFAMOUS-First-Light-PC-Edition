// SecondSonPC — the shader stall trace (Phase 4, P4.14; docs/PHASE4_SHADER_PRECOMPILER.md §2): what made a frame long
// when shaders and pipelines are prepared - GCN -> SPIR-V translations, the disk cache's reads and writes,
// vkCreateShaderModule, vkCreateGraphicsPipelines/vkCreateComputePipelines (with the driver's pipeline cache feedback),
// precompiled pipelines taken or waited for, and the backend's waits (the P4.12 trace's spans, forwarded: the recording
// thread drained, the GPU waited for, submissions, the stream ring, the presenter).
//
// Off unless SSPC_SHADER_STALL_TRACE is set; then each hook is a relaxed load and a branch when off. Value:
//   1[,event=<ms>][,frame=<ms>][,wait=<ms>]
//     event  every traced event at least this long gets its own line (default 10 ms)
//     frame  every frame at least this long (presentation to presentation) gets a breakdown (default 50 ms); 100 ms
//            and 1000 ms are marked
//     wait   forwarded waits shorter than this are not kept (default 1 ms)
// Files (logs directory; written by the trace's own thread every ~250 ms, never on the threads it traces):
//   shader_stall_events.csv  every event >= `event`: start/end (s), ms, thread, role, frame, kind, key, detail
//   shader_stall_frames.csv  every frame >= `frame`: its time, what the threads did in it (ms by kind), the frame log's
//                            breakdown (processor, recording, translations, pipelines, waits)
//   shader_stall_summary.txt at the stop: frames over 50/100/1000 ms, totals by kind, the slowest pipelines and
//                            translations, the driver cache's hits and misses
//
// The hooks only push a fixed-size record under a mutex: they are called at most a few thousand times a session (each
// one is itself a millisecond-scale step), except the forwarded waits, filtered by length before they get here.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::ShaderTrace {

enum class Kind : u8 {
    Translate,       ///< GCN -> SPIR-V (a = program footer hash, b = SPIR-V bytes, c = stage)
    DiskLoad,        ///< a module's translation read from the disk cache instead (a = footer hash, b = bytes)
    DiskWrite,       ///< the disk cache's writer appended records (a = records, b = bytes; its own thread)
    ShaderModule,    ///< vkCreateShaderModule (b = SPIR-V bytes)
    PipelineCreate,  ///< vkCreate*Pipelines on demand (a = description hash, c = flags: kPipe*)
    PipelinePrebuilt,///< a precompiled pipeline taken (a = description hash, c = kPipe*; a span when it was waited for)
    Precompile,      ///< vkCreate*Pipelines by the start-up precompiler (a = description hash, c = kPipe*)
    CacheLoad,       ///< the pipeline cache's / shader cache's files read at start (b = bytes)
    CacheSave,       ///< the pipeline cache's data written (b = bytes)
    Wait,            ///< a wait of the P4.12 trace, forwarded (c = its event)
    Count,
};
constexpr u32 kPipeCompute = 1;   ///< a compute pipeline (else graphics)
constexpr u32 kPipeCacheHit = 2;  ///< the driver reported a hit in the application's pipeline cache
constexpr u32 kPipeFeedback = 4;  ///< the driver gave creation feedback (else the hit bit means nothing)
constexpr u32 kPipeWaited = 8;    ///< PipelinePrebuilt: the recording thread waited for a worker still building it
constexpr u32 kPipeFailed = 16;

const char* KindName(Kind kind);

namespace detail {
extern bool g_on;
void Push(Kind kind, u64 t0, u64 t1, u64 a, u64 b, u32 c);
} // namespace detail

inline bool On() {
    return detail::g_on;
}
/// One event (TSC start/end).
inline void Event(Kind kind, u64 t0, u64 t1, u64 a = 0, u64 b = 0, u32 c = 0) {
    if (detail::g_on) {
        detail::Push(kind, t0, t1, a, b, c);
    }
}

/// This thread's name in the files (before its first event; the thread's description otherwise).
void NameThread(const char* role);

/// Reads SSPC_SHADER_STALL_TRACE; false when not set. Before the backend's threads (after FrameLog::Start).
bool Start(const std::filesystem::path& logs_dir);
/// The files' time origin (the frame log's start: times then match gpu_frames.csv; events before it are negative).
void SetOrigin(u64 tsc);
/// The forwarded waits' minimum in TSC ticks (the P4.12 trace filters with it).
u64 WaitMinTicks();
/// Writes what is left and the summary. After the backend's threads stopped.
void Stop();

} // namespace SecondSon::Gpu::ShaderTrace
