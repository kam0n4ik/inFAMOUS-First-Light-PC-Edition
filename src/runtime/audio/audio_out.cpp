// SecondSonPC — libSceAudioOut on WASAPI (NATIVE, group "audio"; `--bridge audio` gives it back to the oracle,
// third_party/shadPS4/src/core/libraries/audio/audioout.cpp + sdl_audio_out.cpp). The six functions the game imports:
// Init, Open, Close, Output, Outputs, SetVolume, with the oracle's checks, handles ((type << 16) | port | 0x20000000),
// port ranges, channel orders and its PS4 7.1 -> stereo fold for a stereo device (front 1, centre and surrounds
// 0.7071). The game's pace is the console's: an output returns once the device has room for it - a port keeps about
// three of the game's buffers (or two device periods) queued, so it blocks for one buffer's time per call, as
// sceAudioOutOutput blocks on the console. The volume of a port is its loudest channel's (as the oracle's SDL gain).
// A port follows the default device; a lost device (unplugged) is reopened, and without any the pace is kept.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <vector>

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Audio {

namespace {

constexpr s32 kOk = 0;
constexpr s32 kErrNotOpened = static_cast<s32>(0x80260001);
constexpr s32 kErrInvalidPort = static_cast<s32>(0x80260003);
constexpr s32 kErrInvalidPointer = static_cast<s32>(0x80260004);
constexpr s32 kErrPortFull = static_cast<s32>(0x80260005);
constexpr s32 kErrInvalidSize = static_cast<s32>(0x80260006);
constexpr s32 kErrInvalidFormat = static_cast<s32>(0x80260007);
constexpr s32 kErrInvalidSampleFreq = static_cast<s32>(0x80260008);
constexpr s32 kErrInvalidVolume = static_cast<s32>(0x80260009);
constexpr s32 kErrInvalidPortType = static_cast<s32>(0x8026000A);
constexpr s32 kErrAlreadyInit = static_cast<s32>(0x8026000E);
constexpr s32 kErrNotInit = static_cast<s32>(0x8026000F);
constexpr s32 kNumPorts = 25;
constexpr s32 kVolume0dB = 32768;
constexpr u32 kAttrRestricted = 0x00010000, kAttrMixToMain = 0x00020000;
constexpr u32 kRate = 48000;
constexpr DWORD kSpeaker71 = 0x63F; // KSAUDIO_SPEAKER_7POINT1_SURROUND: FL FR FC LFE BL BR SL SR
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (without ksuser.lib)
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct Range {
    s32 start, end;
};
constexpr Range kRanges[] = {{0, 7}, {8, 8}, {9, 12}, {13, 16}, {17, 20}, {21, 21}, {22, 22}, {23, 23}, {24, 24}};

int RangeOf(s32 type) {
    switch (type) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
        return type;
    case 5:
    case 6:
    case 7:
    case 8:
        return 5;
    case 126:
        return 6;
    case 127:
        return 7;
    case 125:
        return 8;
    default:
        return -1;
    }
}

struct FormatInfo {
    bool is_float;
    u32 channels;
    std::array<u8, 8> layout; // the source channel of each WAVE-order channel
};

FormatInfo InfoOf(u32 format) {
    static constexpr FormatInfo kInfo[8] = {
        {false, 1, {0}},
        {false, 2, {0, 1}},
        {false, 8, {0, 1, 2, 3, 4, 5, 6, 7}},
        {true, 1, {0}},
        {true, 2, {0, 1}},
        {true, 8, {0, 1, 2, 3, 4, 5, 6, 7}},
        {false, 8, {0, 1, 2, 3, 6, 7, 4, 5}}, // S16_8CH_Std: FL FR FC LFE SL SR BL BR
        {true, 8, {0, 1, 2, 3, 6, 7, 4, 5}},
    };
    return kInfo[format];
}

template <typename T>
void SafeRelease(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

void EnsureCom() {
    thread_local bool done = false;
    if (!done) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); // (S_FALSE / RPC_E_CHANGED_MODE: COM is there already)
        done = true;
    }
}

class Port {
public:
    Port(s32 type, FormatInfo info, u32 frames) : type_(type), info_(info), frames_(frames) {
        volume_.fill(kVolume0dB);
    }
    ~Port() {
        Close();
    }

    u32 Frames() const {
        return frames_;
    }
    u32 Channels() const {
        return info_.channels;
    }

    void Open() {
        std::scoped_lock lock{mutex_};
        OpenLocked();
    }

    void Close() {
        std::scoped_lock lock{mutex_};
        closed_ = true;
        CloseDevice();
    }

    void SetVolume(u32 flags, s32 vol) {
        std::scoped_lock lock{mutex_};
        for (u32 c = 0; c < 8; ++c) {
            if ((flags & (1u << c)) != 0) {
                volume_[c] = vol;
            }
        }
        float loudest = 0.0f;
        for (u32 c = 0; c < std::min(info_.channels, 8u); ++c) {
            loudest = std::max(loudest, static_cast<float>(volume_[c]) / kVolume0dB);
        }
        gain_ = loudest;
    }

    /// Blocks until the device has room, then queues one buffer; false: the port was closed meanwhile.
    bool Output(const void* data) {
        std::unique_lock lock{mutex_};
        if (closed_) {
            return false;
        }
        if (render_ == nullptr) {
            ReopenLocked();
        }
        if (render_ == nullptr) {
            lock.unlock();
            PaceWithoutDevice(); // no device: keep the pace of one buffer per call
            return true;
        }
        const u32 target = std::max(3 * frames_, 2 * period_frames_);
        for (;;) {
            UINT32 padding = 0;
            const HRESULT hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                SSPC_LOG(Audio, Warning, "audio port {}: the device is gone ({:#x}); reopening", type_,
                         static_cast<u32>(hr));
                CloseDevice();
                return true;
            }
            if (padding <= target && buffer_frames_ - padding >= frames_) {
                break;
            }
            const u32 over = padding > target ? padding - target : frames_;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(1000000ull * over / kRate / 2 + 200));
            lock.lock();
            if (closed_) {
                return false;
            }
            if (render_ == nullptr) {
                return true;
            }
        }
        BYTE* out = nullptr;
        if (FAILED(render_->GetBuffer(frames_, &out))) {
            CloseDevice();
            return true;
        }
        Convert(data, reinterpret_cast<float*>(out));
        render_->ReleaseBuffer(frames_, 0);
        return true;
    }

private:
    void OpenLocked() {
        EnsureCom();
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        last_try_ = std::chrono::steady_clock::now();
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&client_)))) {
            SafeRelease(device);
            SafeRelease(enumerator);
            SafeRelease(client_);
            if (!warned_) {
                SSPC_LOG(Audio, Warning, "audio port {}: no output device", type_);
                warned_ = true;
            }
            return;
        }
        SafeRelease(device);
        SafeRelease(enumerator);
        WAVEFORMATEX* mix = nullptr;
        u32 device_channels = 2;
        if (SUCCEEDED(client_->GetMixFormat(&mix)) && mix != nullptr) {
            device_channels = mix->nChannels;
            CoTaskMemFree(mix);
        }
        fold_ = info_.channels >= 6 && device_channels <= 2; // the oracle's PS4 fold for a stereo device
        out_channels_ = fold_ ? 2 : info_.channels;
        WAVEFORMATEXTENSIBLE f{};
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = static_cast<WORD>(out_channels_);
        f.Format.nSamplesPerSec = kRate;
        f.Format.wBitsPerSample = 32;
        f.Format.nBlockAlign = static_cast<WORD>(out_channels_ * 4);
        f.Format.nAvgBytesPerSec = kRate * f.Format.nBlockAlign;
        f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f.Samples.wValidBitsPerSample = 32;
        f.dwChannelMask = out_channels_ == 1   ? SPEAKER_FRONT_CENTER
                          : out_channels_ == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                               : kSpeaker71;
        f.SubFormat = kSubtypeFloat;
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        REFERENCE_TIME period = 0;
        client_->GetDevicePeriod(&period, nullptr);
        HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0,
                                         reinterpret_cast<WAVEFORMATEX*>(&f), nullptr);
        UINT32 buffer = 0;
        if (SUCCEEDED(hr)) {
            hr = client_->GetBufferSize(&buffer);
        }
        if (SUCCEEDED(hr)) {
            hr = client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_));
        }
        if (SUCCEEDED(hr)) {
            hr = client_->Start();
        }
        if (FAILED(hr)) {
            SSPC_LOG(Audio, Error, "audio port {}: WASAPI stream ({} ch float, 48 kHz) failed: {:#x}", type_,
                     out_channels_, static_cast<u32>(hr));
            CloseDevice();
            return;
        }
        buffer_frames_ = buffer;
        period_frames_ = static_cast<u32>(period * kRate / 10000000);
        warned_ = false;
        SSPC_LOG(Audio, Info, "audio port {}: {} ch {} -> device {} ch{}, buffer {} frames, period {} frames", type_,
                 info_.channels, info_.is_float ? "float" : "s16", device_channels,
                 fold_ ? " (PS4 stereo fold)" : "", buffer_frames_, period_frames_);
    }

    void ReopenLocked() {
        if (std::chrono::steady_clock::now() - last_try_ < std::chrono::seconds(1)) {
            return;
        }
        OpenLocked();
    }

    void CloseDevice() {
        if (client_ != nullptr) {
            client_->Stop();
        }
        SafeRelease(render_);
        SafeRelease(client_);
    }

    void PaceWithoutDevice() {
        const auto period = std::chrono::nanoseconds(1000000000ull * frames_ / kRate);
        const auto now = std::chrono::steady_clock::now();
        if (next_ < now - period * 4) {
            next_ = now;
        }
        next_ += period;
        std::this_thread::sleep_until(next_);
    }

    float Sample(const void* data, u32 frame, u32 channel) const {
        const u32 at = frame * info_.channels + channel;
        if (info_.is_float) {
            return static_cast<const float*>(data)[at];
        }
        return static_cast<const s16*>(data)[at] * (1.0f / 32768.0f);
    }

    void Convert(const void* data, float* out) const {
        const float g = gain_;
        if (fold_) {
            constexpr float kFront = 1.0f, kCentre = 0.7071f, kSurround = 0.7071f;
            for (u32 i = 0; i < frames_; ++i) {
                const float c = kCentre * Sample(data, i, 2);
                out[i * 2 + 0] =
                    g * (kFront * Sample(data, i, 0) + c + kSurround * (Sample(data, i, 4) + Sample(data, i, 6)));
                out[i * 2 + 1] =
                    g * (kFront * Sample(data, i, 1) + c + kSurround * (Sample(data, i, 5) + Sample(data, i, 7)));
            }
            return;
        }
        for (u32 i = 0; i < frames_; ++i) {
            for (u32 c = 0; c < out_channels_; ++c) {
                out[i * out_channels_ + c] = g * Sample(data, i, info_.layout[c]);
            }
        }
    }

    const s32 type_;
    const FormatInfo info_;
    const u32 frames_;
    std::mutex mutex_;
    std::array<s32, 8> volume_{};
    float gain_ = 1.0f;
    bool closed_ = false;
    bool fold_ = false;
    bool warned_ = false;
    u32 out_channels_ = 2;
    u32 buffer_frames_ = 0;
    u32 period_frames_ = 480;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    std::chrono::steady_clock::time_point last_try_{};
    std::chrono::steady_clock::time_point next_{};
};

std::atomic<bool> g_init{false};
std::mutex g_alloc_mutex;
std::shared_mutex g_table_mutex;
std::array<std::shared_ptr<Port>, kNumPorts> g_ports;

/// The handle's form (the oracle's checks, in its order): 0 or the error.
s32 CheckHandle(s32 handle) {
    const s32 id = handle & 0xFF;
    if (id >= kNumPorts || (handle & 0x3F000000) != 0x20000000) {
        return kErrInvalidPort;
    }
    const s32 type = (handle >> 16) & 0xFF;
    if ((type >= 5 && type <= 13) || !((type >= 0 && type <= 4) || type == 14 || type == 126 || type == 127)) {
        return kErrInvalidPortType;
    }
    return kOk;
}

s32 Lookup(s32 handle, std::shared_ptr<Port>& port) {
    if (const s32 r = CheckHandle(handle); r != kOk) {
        return r;
    }
    std::shared_lock lock{g_table_mutex};
    port = g_ports[handle & 0xFF];
    return port != nullptr ? kOk : kErrNotOpened;
}

SSPC_SYSV_ABI s32 Init() {
    bool expected = false;
    if (!g_init.compare_exchange_strong(expected, true)) {
        return kErrAlreadyInit;
    }
    SSPC_LOG(Audio, Info, "audio out: SecondSonPC's (WASAPI)");
    return kOk;
}

SSPC_SYSV_ABI s32 Open(s32 /*user*/, s32 type, s32 index, u32 length, u32 rate, u32 param) {
    if (!g_init.load()) {
        return kErrNotInit;
    }
    if (length == 0 || length > 2048 || (length & 0xFF) != 0) {
        return kErrInvalidSize;
    }
    if (type != 3 && (param & kAttrMixToMain) != 0) {
        return kErrInvalidFormat;
    }
    if (type != 0 && (param & 0x70000000) != 0) {
        return kErrInvalidFormat;
    }
    if ((type < 0 || type > 4) && type != 126 && type != 127) {
        return kErrInvalidPortType;
    }
    if (rate != kRate) {
        return kErrInvalidSampleFreq;
    }
    if (index != 0) {
        SSPC_LOG(Audio, Warning, "sceAudioOutOpen: index {} (not 0)", index);
    }
    const u32 format = param & 0xFF;
    const u32 attr = (param >> 16) & 0xF;
    if (format > 7 || attr > 2) {
        return kErrInvalidFormat;
    }
    std::scoped_lock alloc{g_alloc_mutex};
    const int range = RangeOf(type);
    s32 id = -1;
    {
        std::shared_lock lock{g_table_mutex};
        for (s32 i = kRanges[range].start; i <= kRanges[range].end; ++i) {
            if (g_ports[i] == nullptr) {
                id = i;
                break;
            }
        }
    }
    if (id < 0) {
        return kErrPortFull;
    }
    auto port = std::make_shared<Port>(type, InfoOf(format), length);
    port->Open();
    {
        std::unique_lock lock{g_table_mutex};
        g_ports[id] = port;
    }
    const s32 handle = (type << 16) | id | 0x20000000;
    SSPC_LOG(Audio, Info, "sceAudioOutOpen type {} format {} length {}{} -> {:#x}", type, format, length,
             (param & kAttrRestricted) != 0 ? " restricted" : "", handle);
    return handle;
}

SSPC_SYSV_ABI s32 Close(s32 handle) {
    if (!g_init.load()) {
        return kErrNotInit;
    }
    if (const s32 r = CheckHandle(handle); r != kOk) {
        return r;
    }
    std::shared_ptr<Port> port;
    {
        std::scoped_lock alloc{g_alloc_mutex};
        std::unique_lock lock{g_table_mutex};
        port = std::move(g_ports[handle & 0xFF]);
    }
    if (port == nullptr) {
        return kErrNotOpened;
    }
    port->Close(); // (a thread blocked in an output leaves it: NOT_OPENED)
    return kOk;
}

SSPC_SYSV_ABI s32 Output(s32 handle, const void* ptr) {
    if (!g_init.load()) {
        return kErrNotInit;
    }
    std::shared_ptr<Port> port;
    if (const s32 r = Lookup(handle, port); r != kOk) {
        return r;
    }
    if (ptr == nullptr) {
        return 0; // (the oracle: nothing sent)
    }
    if (!port->Output(ptr)) {
        return kErrNotOpened;
    }
    return static_cast<s32>(port->Frames() * port->Channels()); // samples sent, as the oracle
}

struct OutputParam {
    s32 handle;
    const void* ptr;
};

SSPC_SYSV_ABI s32 Outputs(const OutputParam* param, u32 num) {
    if (!g_init.load()) {
        return kErrNotInit;
    }
    if (num == 0 || num > 25) {
        return kErrPortFull;
    }
    if (param == nullptr) {
        return kErrInvalidPointer;
    }
    std::vector<std::shared_ptr<Port>> ports(num);
    u32 frames = 0;
    for (u32 i = 0; i < num; ++i) {
        if (const s32 r = Lookup(param[i].handle, ports[i]); r != kOk) {
            return r;
        }
        for (u32 j = 0; j < i; ++j) {
            if (param[i].handle == param[j].handle) {
                return kErrInvalidPort;
            }
        }
        if (i == 0) {
            frames = ports[i]->Frames();
        } else if (ports[i]->Frames() != frames) {
            return kErrInvalidSize;
        }
    }
    for (u32 i = 0; i < num; ++i) {
        if (param[i].ptr != nullptr && !ports[i]->Output(param[i].ptr)) {
            return kErrNotOpened;
        }
    }
    return static_cast<s32>(frames);
}

SSPC_SYSV_ABI s32 SetVolume(s32 handle, s32 flag, const s32* vol) {
    if (!g_init.load()) {
        return kErrNotInit;
    }
    if (const s32 r = CheckHandle(handle); r != kOk) {
        return r;
    }
    if (vol == nullptr) {
        return kErrInvalidPointer;
    }
    if (*vol > kVolume0dB) {
        return kErrInvalidVolume;
    }
    std::shared_ptr<Port> port;
    if (const s32 r = Lookup(handle, port); r != kOk) {
        return r;
    }
    port->SetVolume(static_cast<u32>(flag), *vol);
    return kOk;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define AO(nid, name, fn) Runtime::Export{nid, "libSceAudioOut", "libSceAudioOut", name, Fn(&fn), nullptr, "audio"}
const Runtime::Export kExports[] = {
    AO("JfEPXVxhFqA", "sceAudioOutInit", Init),
    AO("ekNvsT22rsY", "sceAudioOutOpen", Open),
    AO("s1--uE9mBFw", "sceAudioOutClose", Close),
    AO("QOQtbeDqsT4", "sceAudioOutOutput", Output),
    AO("w3PdaSTSwGE", "sceAudioOutOutputs", Outputs),
    AO("b+uAV89IlxE", "sceAudioOutSetVolume", SetVolume),
};
#undef AO

} // namespace

std::span<const Runtime::Export> AudioOutExports() {
    return kExports;
}

} // namespace SecondSon::Audio
