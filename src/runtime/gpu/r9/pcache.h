// SecondSonPC R9-F1 - the driver's pipeline cache saved without stopping the game (SSPC_R9_PCACHE=1, off by default;
// docs/R9_GAMEPLAY_PERFORMANCE.md §F1).
//
// Measured (tools/r9 traverse runs, waiting samples of the gameplay profiler): the periodic save of the driver's cache
// (Backend::MaybeSavePipelineCache, at most once a minute when pipelines were created) ran under the shader disk
// cache's I/O lock for the whole save - the driver's data copied twice and written, 866 MiB, 1.3-4.5 s. A frontend
// that needed a module from the disk cache meanwhile (a new view: a camera turn, fast traversal) waited in
// ShaderDiskCache::ReadRecord for the lock: the command processor and the game stood for the rest of the save (frames
// of 1.1-4.5 s, once a minute while new pipelines keep coming).
//
//   0 (default)  as before
//   1            the save holds a writer lease of its own (Close still waits for it; module reads and appends do not),
//                the driver's data is written as it comes (no zero-filled buffer, no second copy), and a save whose
//                data has the size of the last one loaded or saved is skipped (the driver's cache only grows when it
//                takes a new pipeline; a pipeline created from the cache adds nothing)
#pragma once

#include <windows.h>

#include "base/abi.h"

namespace SecondSon::Gpu::R9 {

inline u32 PipelineCacheMode() {
    static const u32 mode = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("SSPC_R9_PCACHE", v, sizeof(v)) != 0 && v[0] == '1' ? 1u : 0u;
    }();
    return mode;
}

} // namespace SecondSon::Gpu::R9
