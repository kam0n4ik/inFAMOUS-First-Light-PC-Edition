// SecondSonPC — libSceVideodec on FFmpeg's H.264 decoder (NATIVE, group "videodec"; `--bridge videodec` gives it back
// to the oracle, third_party/shadPS4/src/core/libraries/videodec/videodec.cpp + videodec_impl.cpp + video_utils.cpp).
// The six functions the game imports (the movies: .bsf H.264 access units in, NV12 pictures out), with the oracle's
// results: one access unit in, at most one picture out (FFmpeg's own delay, one decoding thread, as the oracle), the
// NV12 picture at a 64-aligned pitch and a 16-aligned height (the last rows repeated into the crop space), the same
// resource sizes. One deliberate difference: a decoder handle the game no longer owns (deleted, or garbage) is
// refused with ERROR_HANDLE instead of being used.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <span>
#include <unordered_set>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/filesystem/fs_host.h"

namespace SecondSon::Media {

namespace {

constexpr s32 kOk = 0;
constexpr s32 kErrApiFail = static_cast<s32>(0x80C10000);
constexpr s32 kErrStructSize = static_cast<s32>(0x80C10002);
constexpr s32 kErrHandle = static_cast<s32>(0x80C10003);
constexpr s32 kErrAuSize = static_cast<s32>(0x80C10009);
constexpr s32 kErrAuPointer = static_cast<s32>(0x80C1000A);
constexpr s32 kErrArgumentPointer = static_cast<s32>(0x80C1000F);
constexpr u64 kMiB = 1024ull * 1024;

struct ConfigInfo {
    u64 this_size;
    u32 codec_type;
    u32 profile;
    u32 max_level;
    s32 max_frame_width;
    s32 max_frame_height;
    s32 max_dpb_frame_count;
    u64 flags;
};
struct ResourceInfo {
    u64 this_size;
    u64 cpu_memory_size;
    void* cpu_memory;
    u64 cpu_gpu_memory_size;
    void* cpu_gpu_memory;
    u64 max_frame_buffer_size;
    u32 frame_buffer_alignment;
};
struct Ctrl {
    u64 this_size;
    void* handle;
    u64 version;
};
struct FrameBuffer {
    u64 this_size;
    void* frame_buffer;
    u64 frame_buffer_size;
};
struct AvcInfo {
    u32 num_units_in_tick;
    u32 time_scale;
    u8 fixed_frame_rate_flag;
    u8 aspect_ratio_idc;
    u16 sar_width;
    u16 sar_height;
    u8 colour_primaries;
    u8 transfer_characteristics;
    u8 matrix_coefficients;
    u8 video_full_range_flag;
    u32 crop_left;
    u32 crop_right;
    u32 crop_top;
    u32 crop_bottom;
};
union CodecInfo {
    u8 reserved[64];
    AvcInfo avc;
};
struct PictureInfo {
    u64 this_size;
    u32 is_valid;
    u32 codec_type;
    u32 frame_width;
    u32 frame_pitch;
    u32 frame_height;
    u32 is_error_pic;
    u64 pts;
    u64 attached;
    CodecInfo codec;
};
struct InputData {
    u64 this_size;
    void* au_data;
    u64 au_size;
    u64 pts;
    u64 dts;
    u64 attached;
};

u32 AlignUp(u32 v, u32 a) {
    return (v + a - 1) / a * a;
}
u64 AlignUp64(u64 v, u64 a) {
    return (v + a - 1) / a * a;
}

class Decoder {
public:
    explicit Decoder(const ConfigInfo& cfg) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) {
            return;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (ctx_ == nullptr) {
            return;
        }
        ctx_->width = cfg.max_frame_width;
        ctx_->height = cfg.max_frame_height;
        ctx_->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            avcodec_free_context(&ctx_);
        }
    }
    ~Decoder() {
        SSPC_LOG(Audio, Info,
                 "videodec: decoder {:#x} done: {} access units in, {} pictures out, {} flushes, {} resets, {} pictures "
                 "with a pts at or before the previous one, {} without a pts, {} errors",
                 reinterpret_cast<u64>(this), units_, pictures_, flushes_, resets_, pts_back_, no_pts_, errors_);
        avcodec_free_context(&ctx_);
        sws_freeContext(sws_);
    }

    bool Ok() const {
        return ctx_ != nullptr;
    }

    s32 Decode(const InputData& in, FrameBuffer& fb, PictureInfo& pic) {
        pic.is_valid = 0;
        if (in.au_data == nullptr) {
            return kErrAuPointer;
        }
        if (in.au_size == 0) {
            return kErrAuSize;
        }
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            return kErrApiFail;
        }
        packet->data = static_cast<u8*>(in.au_data);
        packet->size = static_cast<int>(in.au_size);
        packet->pts = static_cast<s64>(in.pts);
        packet->dts = static_cast<s64>(in.dts);
        packet->opaque = reinterpret_cast<void*>(in.attached);
        ++units_;
        if (units_ <= 3) {
            SSPC_LOG(Audio, Info, "videodec: decoder {:#x}: access unit {} ({} bytes): pts {:#x}, dts {:#x}, attached {:#x}",
                     reinterpret_cast<u64>(this), units_, in.au_size, in.pts, in.dts, in.attached);
        }
        if (units_ > 1 && in.pts < last_in_pts_ && back_logged_ < 16) {
            ++back_logged_;
            SSPC_LOG(Audio, Warning, "videodec: decoder {:#x}: access unit {} goes back: pts {} after {} ({} pictures out)",
                     reinterpret_cast<u64>(this), units_, in.pts, last_in_pts_, pictures_);
        }
        last_in_pts_ = in.pts;
        int ret = avcodec_send_packet(ctx_, packet);
        if (ret == AVERROR_EOF) {
            avcodec_flush_buffers(ctx_); // (after a flush: decoding again, as the oracle)
            ret = avcodec_send_packet(ctx_, packet);
        }
        av_packet_free(&packet);
        if (ret < 0) {
            ++errors_;
            SSPC_LOG(Audio, Error, "videodec: sending an access unit failed ({})", ret);
            return kErrApiFail;
        }
        return Receive(fb, pic);
    }

    s32 Flush(FrameBuffer& fb, PictureInfo& pic) {
        pic.is_valid = 0;
        ++flushes_;
        SSPC_LOG(Audio, Info, "videodec: decoder {:#x} flushed after {} access units, {} pictures",
                 reinterpret_cast<u64>(this), units_, pictures_);
        // A flush can hold several pictures but returns one (the oracle: some games hang when one is skipped);
        // a second flush in a row is refused by FFmpeg, which is ignored.
        avcodec_send_packet(ctx_, nullptr);
        return Receive(fb, pic);
    }

    void Reset() {
        ++resets_;
        SSPC_LOG(Audio, Info, "videodec: decoder {:#x} reset after {} access units, {} pictures (last pts {})",
                 reinterpret_cast<u64>(this), units_, pictures_, last_out_pts_);
        has_out_pts_ = false;
        avcodec_flush_buffers(ctx_);
    }

private:
    s32 Receive(FrameBuffer& fb, PictureInfo& pic) {
        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) {
            return kErrApiFail;
        }
        const int ret = avcodec_receive_frame(ctx_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            return kOk; // no picture yet
        }
        if (ret < 0) {
            av_frame_free(&frame);
            ++errors_;
            SSPC_LOG(Audio, Error, "videodec: decoding failed ({})", ret);
            return kErrApiFail;
        }
        if (frame->format != AV_PIX_FMT_NV12) {
            AVFrame* nv12 = ToNv12(*frame);
            av_frame_free(&frame);
            if (nv12 == nullptr) {
                return kErrApiFail;
            }
            frame = nv12;
        }
        const u32 width = AlignUp(static_cast<u32>(frame->width), 16);
        const u32 pitch = AlignUp(static_cast<u32>(frame->width), 64);
        const u32 height = AlignUp(static_cast<u32>(frame->height), 16);
        if (fb.frame_buffer == nullptr || u64{pitch} * height * 3 / 2 > fb.frame_buffer_size) {
            SSPC_LOG(Audio, Error, "videodec: a {}x{} picture does not fit the frame buffer ({} bytes)",
                     frame->width, frame->height, fb.frame_buffer_size);
            av_frame_free(&frame);
            return kErrArgumentPointer;
        }
        CopyNv12(static_cast<u8*>(fb.frame_buffer), pitch, height, *frame);
        // The GPU backend's copies of the frame buffer (the texture the game draws the picture with) are refreshed: the
        // page tracking does not see this write where the game's memory is imported into Vulkan, and the picture there
        // stayed the one first uploaded - a video frozen on one frame while its sound played on, or an older frame (of
        // this video or another one that used the same buffer) shown again.
        Filesystem::NotifyHostWrite(reinterpret_cast<u64>(fb.frame_buffer), u64{pitch} * height * 3 / 2);
        if (pictures_ == 0) {
            SSPC_LOG(Audio, Info, "videodec: decoder {:#x}: frame buffer {:#x} ({:#x} bytes)", reinterpret_cast<u64>(this),
                     reinterpret_cast<u64>(fb.frame_buffer), fb.frame_buffer_size);
        }
        pic.is_valid = 1;
        pic.codec_type = 0;
        pic.frame_width = width;
        pic.frame_pitch = pitch;
        pic.frame_height = height;
        pic.is_error_pic = 0;
        pic.pts = static_cast<u64>(frame->pts);
        ++pictures_;
        if (pictures_ <= 3) {
            SSPC_LOG(Audio, Info, "videodec: decoder {:#x}: picture {}: pts {:#x}, attached {:#x}",
                     reinterpret_cast<u64>(this), pictures_, pic.pts, reinterpret_cast<u64>(frame->opaque));
        }
        if (pic.pts == ~u64{0}) {
            ++no_pts_;
        } else if (has_out_pts_ && pic.pts <= last_out_pts_ && ++pts_back_ <= 16) {
            SSPC_LOG(Audio, Warning, "videodec: decoder {:#x}: picture {} has pts {} after {}",
                     reinterpret_cast<u64>(this), pictures_, pic.pts, last_out_pts_);
        }
        if (pic.pts != ~u64{0}) {
            last_out_pts_ = pic.pts;
            has_out_pts_ = true;
        }
        pic.attached = reinterpret_cast<u64>(frame->opaque);
        pic.codec.avc.colour_primaries = static_cast<u8>(frame->color_primaries);
        pic.codec.avc.crop_left = 0;
        pic.codec.avc.crop_right = pitch - static_cast<u32>(frame->width);
        pic.codec.avc.crop_top = 0;
        pic.codec.avc.crop_bottom = height - static_cast<u32>(frame->height);
        av_frame_free(&frame);
        return kOk;
    }

    AVFrame* ToNv12(const AVFrame& src) {
        AVFrame* dst = av_frame_alloc();
        if (dst == nullptr) {
            return nullptr;
        }
        dst->pts = src.pts;
        dst->format = AV_PIX_FMT_NV12;
        dst->width = src.width;
        dst->height = src.height;
        dst->opaque = src.opaque;
        dst->color_primaries = src.color_primaries;
        if (av_frame_get_buffer(dst, 0) < 0) {
            av_frame_free(&dst);
            return nullptr;
        }
        if (sws_ == nullptr) {
            sws_ = sws_getContext(src.width, src.height, static_cast<AVPixelFormat>(src.format), src.width,
                                  src.height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }
        if (sws_ == nullptr || sws_scale(sws_, src.data, src.linesize, 0, src.height, dst->data, dst->linesize) < 0) {
            av_frame_free(&dst);
            return nullptr;
        }
        return dst;
    }

    /// The oracle's CopyNV12Data: luma then chroma at `pitch`, the last rows repeated down to `height`.
    static void CopyNv12(u8* dst, u32 pitch, u32 height, const AVFrame& src) {
        const u32 w = static_cast<u32>(src.width);
        const u32 h = static_cast<u32>(src.height);
        u8* luma = dst;
        u8* chroma = dst + u64{pitch} * height;
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(luma + u64{y} * pitch, src.data[0] + u64{std::min(y, h - 1)} * src.linesize[0], w);
        }
        for (u32 y = 0; y < height / 2; ++y) {
            std::memcpy(chroma + u64{y} * pitch, src.data[1] + u64{std::min(y, h / 2 - 1)} * src.linesize[1], w);
        }
    }

    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    // diagnostics (a video that repeats a moment or jumps elsewhere): what the game fed and got back
    u64 units_ = 0, pictures_ = 0, flushes_ = 0, resets_ = 0, pts_back_ = 0, errors_ = 0, back_logged_ = 0, no_pts_ = 0;
    u64 last_in_pts_ = 0, last_out_pts_ = 0;
    bool has_out_pts_ = false;
};

std::mutex g_mutex; // (the set of live decoders; a decoder itself is used by one thread at a time, as on the console)
std::unordered_set<Decoder*> g_live;

Decoder* Live(const Ctrl* ctrl) {
    std::scoped_lock lock{g_mutex};
    auto* d = static_cast<Decoder*>(ctrl->handle);
    return g_live.contains(d) ? d : nullptr;
}

SSPC_SYSV_ABI s32 CreateDecoder(const ConfigInfo* cfg, const ResourceInfo* res, Ctrl* ctrl) {
    if (cfg == nullptr || res == nullptr || ctrl == nullptr) {
        return kErrArgumentPointer;
    }
    if (cfg->this_size != sizeof(ConfigInfo) || res->this_size != sizeof(ResourceInfo)) {
        return kErrStructSize;
    }
    auto* d = new Decoder(*cfg);
    if (!d->Ok()) {
        delete d;
        SSPC_LOG(Audio, Error, "videodec: FFmpeg's H.264 decoder could not be opened");
        return kErrApiFail;
    }
    {
        std::scoped_lock lock{g_mutex};
        g_live.insert(d);
    }
    ctrl->this_size = sizeof(Ctrl);
    ctrl->handle = d;
    ctrl->version = 1;
    SSPC_LOG(Audio, Info, "videodec: decoder {:#x} ({}x{}, level {})", reinterpret_cast<u64>(d),
             cfg->max_frame_width, cfg->max_frame_height, cfg->max_level);
    return kOk;
}

SSPC_SYSV_ABI s32 Decode(const Ctrl* ctrl, const InputData* in, FrameBuffer* fb, PictureInfo* pic) {
    if (ctrl == nullptr || in == nullptr || pic == nullptr || fb == nullptr) {
        return kErrArgumentPointer;
    }
    if (ctrl->this_size != sizeof(Ctrl) || fb->this_size != sizeof(FrameBuffer)) {
        return kErrStructSize;
    }
    Decoder* d = Live(ctrl);
    if (d == nullptr) {
        return kErrHandle;
    }
    return d->Decode(*in, *fb, *pic);
}

SSPC_SYSV_ABI s32 DeleteDecoder(const Ctrl* ctrl) {
    if (ctrl == nullptr) {
        return kErrArgumentPointer;
    }
    Decoder* d = nullptr;
    {
        std::scoped_lock lock{g_mutex};
        auto* h = static_cast<Decoder*>(ctrl->handle);
        if (g_live.erase(h) != 0) {
            d = h;
        }
    }
    if (d == nullptr) {
        return kErrHandle;
    }
    delete d;
    return kOk;
}

SSPC_SYSV_ABI s32 Flush(const Ctrl* ctrl, FrameBuffer* fb, PictureInfo* pic) {
    if (ctrl == nullptr || fb == nullptr || pic == nullptr) {
        return kErrArgumentPointer;
    }
    if (fb->this_size != sizeof(FrameBuffer) || pic->this_size != sizeof(PictureInfo)) {
        return kErrStructSize;
    }
    Decoder* d = Live(ctrl);
    if (d == nullptr) {
        return kErrHandle;
    }
    return d->Flush(*fb, *pic);
}

SSPC_SYSV_ABI s32 QueryResourceInfo(const ConfigInfo* cfg, ResourceInfo* res) {
    if (cfg == nullptr || res == nullptr) {
        return kErrArgumentPointer;
    }
    if (cfg->this_size != sizeof(ConfigInfo) || res->this_size != sizeof(ResourceInfo)) {
        return kErrStructSize;
    }
    s32 w = 1920, h = 1080;
    if (cfg->max_frame_width > 0 && cfg->max_frame_height > 0) {
        w = cfg->max_frame_width;
        h = cfg->max_frame_height;
    } else if (cfg->max_level >= 150) {
        w = 3840;
        h = 2160;
    }
    const u64 frame = u64{AlignUp(static_cast<u32>(w), 256)} * AlignUp(static_cast<u32>(h), 16) * 3 / 2;
    const s32 dpb = cfg->max_dpb_frame_count > 0 ? cfg->max_dpb_frame_count : 8;
    const u64 padded = AlignUp64(frame, 256) + 0x4000;
    res->this_size = sizeof(ResourceInfo);
    res->cpu_memory = nullptr;
    res->cpu_gpu_memory = nullptr;
    res->cpu_gpu_memory_size = padded * (static_cast<u64>(dpb) + 2) + 8 * kMiB;
    res->cpu_memory_size = 16 * kMiB;
    res->max_frame_buffer_size = padded;
    res->frame_buffer_alignment = 0x100;
    return kOk;
}

SSPC_SYSV_ABI s32 Reset(const Ctrl* ctrl) {
    if (ctrl == nullptr) {
        return kErrArgumentPointer;
    }
    Decoder* d = Live(ctrl);
    if (d == nullptr) {
        return kErrHandle;
    }
    d->Reset();
    return kOk;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define VD(nid, name, fn) Runtime::Export{nid, "libSceVideodec", "libSceVideodec", name, Fn(&fn), nullptr, "videodec"}
const Runtime::Export kExports[] = {
    VD("qkgRiwHyheU", "sceVideodecCreateDecoder", CreateDecoder),
    VD("q0W5GJMovMs", "sceVideodecDecode", Decode),
    VD("U0kpGF1cl90", "sceVideodecDeleteDecoder", DeleteDecoder),
    VD("jeigLlKdp5I", "sceVideodecFlush", Flush),
    VD("leCAscipfFY", "sceVideodecQueryResourceInfo", QueryResourceInfo),
    VD("f8AgDv-1X8A", "sceVideodecReset", Reset),
};
#undef VD

} // namespace

std::span<const Runtime::Export> VideodecExports() {
    return kExports;
}

} // namespace SecondSon::Media
