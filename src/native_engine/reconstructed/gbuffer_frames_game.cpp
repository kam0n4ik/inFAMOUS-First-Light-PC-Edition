// SecondSonPC — R6-G: the frame counter of the geometry draws' measurement in the game (gbuffer_draw.h R6GFrameId).
#include "native_engine/reconstructed/gbuffer_draw.h"
#include "runtime/gpu/native_videoout.h"

namespace SecondSon::NativeEngine::Reconstructed {

u64 R6GFrameId() {
    return Gpu::VideoOut::NextFlipId();
}

} // namespace SecondSon::NativeEngine::Reconstructed
