// SecondSonPC — P2.5 performance changes in the temporary shadPS4 GPU bridge, each behind its own
// switch so one build can be measured with and without it (docs/PHASE2_P25_PERFORMANCE.md).
//
// Set once from the command line (--perf-opt <name> / --no-perf-opt <name>) before the bridge starts,
// read-only afterwards. A change becomes on by default only after its A/B and correctness checks.
#pragma once

namespace SecondSon::PerfOpts {

// Image::GetBarriers (patch 0015): a partial, read-only transition request that already found every
// subresource of its range in the requested state is not walked again while the image's states are
// unchanged (Image::BackingImage::state_gen). Same barriers, same states; skips the per-subresource
// walk (up to 12288 states per request for Second Son's largest texture array). On by default since
// its A/B (Salmon Bay, 7 runs: 18.2 -> 24.3 FPS); --no-perf-opt barrier-memo turns it off.
inline bool barrier_memo = true;

// TextureCache::FindImage (patch 0018): while no image was registered or unregistered
// (TextureCache::sspc_registry_gen), a request whose perfect match was found before - same address,
// sizes, format, type, mips/layers - returns that image again without walking the page table (the
// same image and the same LRU touch). On by default since its A/B (beach scene, 4 runs ABBA: open
// view 55.6-58.0 -> 60 FPS, dense foliage 32.4-35.2 -> 38.5 FPS); --no-perf-opt image-memo turns it off.
inline bool image_memo = true;

// BufferCache::stream_buffer (patch 0019): the stream buffer, which receives a copy of every small
// read-only buffer a draw binds (BufferCache::ObtainBuffer: ~27 000 copies, ~28 MiB per frame looking
// at dense foliage), in system memory (MemoryType::HostUncached, the GPU reads it over PCIe) instead
// of CPU-visible video memory (MemoryType::Stream), where each copy is a write over PCIe. Read when the
// buffer cache is created. On by default since its A/B (beach scene, ABBA: dense foliage 37.9-38.4 ->
// 40.8-41.1 FPS, median frame 32.5 -> 18 ms; open view 1 % low 43 -> 53; GPU busy +~1 ms per frame,
// 128 MiB moved from VRAM to shared memory); --no-perf-opt stream-host turns it off.
inline bool stream_host = true;

} // namespace SecondSon::PerfOpts
