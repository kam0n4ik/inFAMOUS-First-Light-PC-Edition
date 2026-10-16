// SecondSonPC - the hash of the GPU path's maps keyed by u64 (docs/GPU_OPTIMIZATION.md, GPU-5). MSVC's
// std::hash<u64> is FNV-1a over the key's 8 bytes (8 dependent multiplications); the lookups per draw (programs by
// address, fetch shaders, pipelines, textures, image views, samplers) showed up in the processor's and the recording
// thread's samples. MurmurHash3's 64-bit finalizer: two multiplications, every key bit reaches the low bits.
#pragma once

#include <cstddef>

#include "base/abi.h"

namespace SecondSon::Gpu {

struct U64Hash {
    size_t operator()(u64 x) const noexcept {
        x ^= x >> 33;
        x *= 0xFF51AFD7ED558CCDull;
        x ^= x >> 33;
        x *= 0xC4CEB9FE1A85EC53ull;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};

} // namespace SecondSon::Gpu
