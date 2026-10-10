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

} // namespace SecondSon::PerfOpts
