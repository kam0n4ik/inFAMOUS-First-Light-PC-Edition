// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — libScePlayGo (NATIVE). The package is fully installed: every chunk is local
// (LocalFast), nothing is left to download, progress = total. Behaviour (handle, error order,
// chunk sizes from /app0/sce_sys/playgo-chunk.dat) follows the Phase 0 oracle,
// third_party/shadPS4/src/core/libraries/playgo/playgo.cpp @ 0fe263a.
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/filesystem/vfs.h"
#include "runtime/system/system_service.h"

namespace SecondSon::Runtime::PlayGo {

namespace {

#include "runtime/playgo/playgo_chunk.inc"

constexpr s32 ORBIS_OK = 0;
constexpr s32 ORBIS_PLAYGO_ERROR_INVALID_ARGUMENT = static_cast<s32>(0x80B20004);
constexpr s32 ORBIS_PLAYGO_ERROR_NOT_INITIALIZED = static_cast<s32>(0x80B20005);
constexpr s32 ORBIS_PLAYGO_ERROR_ALREADY_INITIALIZED = static_cast<s32>(0x80B20006);
constexpr s32 ORBIS_PLAYGO_ERROR_BAD_HANDLE = static_cast<s32>(0x80B20009);
constexpr s32 ORBIS_PLAYGO_ERROR_BAD_POINTER = static_cast<s32>(0x80B2000A);
constexpr s32 ORBIS_PLAYGO_ERROR_BAD_SIZE = static_cast<s32>(0x80B2000B);
constexpr s32 ORBIS_PLAYGO_ERROR_BAD_CHUNK_ID = static_cast<s32>(0x80B2000C);
constexpr s32 ORBIS_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO = static_cast<s32>(0x80B2000E);

constexpr u32 kHandle = 1;
constexpr s8 kLocusNotDownloaded = 0;
constexpr s8 kLocusLocalFast = 3;

struct InitParams {
    const void* buf_addr;
    u32 buf_size;
    u32 reserved;
};
struct Progress {
    u64 progress_size;
    u64 total_size;
};

struct Chunk {
    u8 req_locus = 0;
    u64 language_mask = 0;
    std::string label;
    u64 total_size = 0;
};

struct State {
    PlaygoHeader header{};
    std::vector<Chunk> chunks;
    u64 language_mask = 0;
};
std::mutex g_mutex;
std::unique_ptr<State> g_state;

bool Parse(std::span<const u8> data, State& s) {
    if (data.size() < sizeof(PlaygoHeader)) {
        return false;
    }
    std::memcpy(&s.header, data.data(), sizeof(PlaygoHeader));
    if (s.header.magic != PLAYGO_MAGIC) {
        return false;
    }
    const auto part = [&](const chunk_t c) -> std::span<const u8> {
        if (static_cast<u64>(c.offset) + c.length > data.size()) {
            return {};
        }
        return data.subspan(c.offset, c.length);
    };
    const auto attrs = part(s.header.chunk_attrs);
    const auto mchunks = part(s.header.chunk_mchunks);
    const auto labels = part(s.header.chunk_labels);
    const auto mattrs = part(s.header.mchunk_attrs);
    if (attrs.size() < s.header.chunk_count * sizeof(playgo_chunk_attr_entry_t)) {
        return false;
    }
    s.chunks.resize(s.header.chunk_count);
    for (u16 i = 0; i < s.header.chunk_count; ++i) {
        playgo_chunk_attr_entry_t a;
        std::memcpy(&a, attrs.data() + i * sizeof(a), sizeof(a));
        auto& c = s.chunks[i];
        c.req_locus = a.req_locus;
        c.language_mask = a.language_mask;
        if (a.label_offset < labels.size()) {
            c.label = std::string(reinterpret_cast<const char*>(labels.data() + a.label_offset),
                                  strnlen(reinterpret_cast<const char*>(labels.data() + a.label_offset),
                                          labels.size() - a.label_offset));
        }
        for (u16 j = 0; j < a.mchunk_count; ++j) {
            const u64 at = static_cast<u64>(a.mchunks_offset) + j * sizeof(u16);
            if (at + sizeof(u16) > mchunks.size()) {
                break;
            }
            u16 id;
            std::memcpy(&id, mchunks.data() + at, sizeof(id));
            if ((id + 1) * sizeof(playgo_mchunk_attr_entry_t) > mattrs.size()) {
                continue;
            }
            playgo_mchunk_attr_entry_t m;
            std::memcpy(&m, mattrs.data() + id * sizeof(m), sizeof(m));
            c.total_size += m.size.size;
        }
    }
    return true;
}

SSPC_SYSV_ABI s32 Initialize(InitParams* param) {
    SSPC_LOG(SystemService, Info, "scePlayGoInitialize(bufSize {:#x})", param->buf_size);
    if (param->buf_addr == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (param->buf_size < 0x200000) {
        return ORBIS_PLAYGO_ERROR_BAD_SIZE;
    }
    std::scoped_lock lock{g_mutex};
    if (g_state) {
        return ORBIS_PLAYGO_ERROR_ALREADY_INITIALIZED;
    }
    g_state = std::make_unique<State>();
    if (const auto r = Filesystem::GetVfs().Resolve("/app0/sce_sys/playgo-chunk.dat")) {
        std::ifstream file(r->host, std::ios::binary);
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (!Parse(bytes, *g_state)) {
            SSPC_LOG(SystemService, Warning, "PlayGo: cannot parse playgo-chunk.dat");
        }
    }
    s32 lang = 0;
    SystemService::ParamGetInt(static_cast<s32>(SystemService::ParamId::Lang), &lang);
    g_state->language_mask = lang >= 0 && lang < 48 ? 1ull << (63 - lang) : 0;
    SSPC_LOG(SystemService, Info, "PlayGo: {} chunks, file size {:#x}, language mask {:#018x}",
             g_state->chunks.size(), g_state->header.file_size, g_state->language_mask);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 Open(u32* out_handle, const void* param) {
    if (out_handle == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (param != nullptr) {
        return ORBIS_PLAYGO_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_state) {
        return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    if (g_state->header.file_size == 0) {
        return ORBIS_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO;
    }
    *out_handle = kHandle;
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 Close(u32 handle) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    std::scoped_lock lock{g_mutex};
    return g_state ? ORBIS_OK : ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
}

SSPC_SYSV_ABI s32 GetEta(u32 handle, const u16* chunk_ids, u32 count, s64* out_eta) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    if (chunk_ids == nullptr || out_eta == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (count == 0) {
        return ORBIS_PLAYGO_ERROR_BAD_SIZE;
    }
    *out_eta = 0; // everything is installed
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetLocus(u32 handle, const u16* chunk_ids, u32 count, s8* out_loci) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    if (chunk_ids == nullptr || out_loci == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (count == 0) {
        return ORBIS_PLAYGO_ERROR_BAD_SIZE;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_state) {
        return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    if (g_state->header.file_size == 0) {
        return ORBIS_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO;
    }
    for (u32 i = 0; i < count; ++i) {
        if (chunk_ids[i] < g_state->chunks.size()) {
            out_loci[i] = kLocusLocalFast;
        } else {
            out_loci[i] = kLocusNotDownloaded;
            return ORBIS_PLAYGO_ERROR_BAD_CHUNK_ID;
        }
    }
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetProgress(u32 handle, const u16* chunk_ids, u32 count, Progress* out) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    if (chunk_ids == nullptr || out == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (count == 0) {
        return ORBIS_PLAYGO_ERROR_BAD_SIZE;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_state) {
        return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    if (g_state->header.file_size == 0) {
        return ORBIS_PLAYGO_ERROR_BAD_CHUNK_ID;
    }
    out->progress_size = 0;
    out->total_size = 0;
    u64 total = 0;
    for (u32 i = 0; i < count; ++i) {
        if (chunk_ids[i] >= g_state->chunks.size()) {
            return ORBIS_PLAYGO_ERROR_BAD_CHUNK_ID;
        }
        total += g_state->chunks[chunk_ids[i]].total_size;
    }
    out->progress_size = total;
    out->total_size = total;
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetToDoList(u32 handle, void* out_todo, u32 count, u32* out_entries) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    if (out_todo == nullptr || out_entries == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (count == 0) {
        return ORBIS_PLAYGO_ERROR_BAD_SIZE;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_state) {
        return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    *out_entries = 0; // nothing left to install
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 SetLanguageMask(u32 handle, u64 mask) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_state) {
        return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    g_state->language_mask = mask;
    SSPC_LOG(Language, Info, "scePlayGoSetLanguageMask({:#018x})", mask);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 SetToDoList(u32 handle, const void* todo, u32 count) {
    if (handle != kHandle) {
        return ORBIS_PLAYGO_ERROR_BAD_HANDLE;
    }
    if (todo == nullptr) {
        return ORBIS_PLAYGO_ERROR_BAD_POINTER;
    }
    if (count == 0) {
        return ORBIS_PLAYGO_ERROR_BAD_SIZE;
    }
    std::scoped_lock lock{g_mutex};
    return g_state ? ORBIS_OK : ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
}

SSPC_SYSV_ABI s32 Terminate() {
    std::scoped_lock lock{g_mutex};
    if (!g_state) {
        return ORBIS_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    g_state.reset();
    return ORBIS_OK;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"ts6GlZOKRrE", "libScePlayGo", "libScePlayGo", "scePlayGoInitialize", Fn(&Initialize), nullptr, "playgo"},
    {"M1Gma1ocrGE", "libScePlayGo", "libScePlayGo", "scePlayGoOpen", Fn(&Open), nullptr, "playgo"},
    {"Uco1I0dlDi8", "libScePlayGo", "libScePlayGo", "scePlayGoClose", Fn(&Close), nullptr, "playgo"},
    {"v6EZ-YWRdMs", "libScePlayGo", "libScePlayGo", "scePlayGoGetEta", Fn(&GetEta), nullptr, "playgo"},
    {"uWIYLFkkwqk", "libScePlayGo", "libScePlayGo", "scePlayGoGetLocus", Fn(&GetLocus), nullptr, "playgo"},
    {"-RJWNMK3fC8", "libScePlayGo", "libScePlayGo", "scePlayGoGetProgress", Fn(&GetProgress), nullptr, "playgo"},
    {"Nn7zKwnA5q0", "libScePlayGo", "libScePlayGo", "scePlayGoGetToDoList", Fn(&GetToDoList), nullptr, "playgo"},
    {"LosLlHOpNqQ", "libScePlayGo", "libScePlayGo", "scePlayGoSetLanguageMask", Fn(&SetLanguageMask), nullptr, "playgo"},
    {"gUPGiOQ1tmQ", "libScePlayGo", "libScePlayGo", "scePlayGoSetToDoList", Fn(&SetToDoList), nullptr, "playgo"},
    {"MPe0EeBGM-E", "libScePlayGo", "libScePlayGo", "scePlayGoTerminate", Fn(&Terminate), nullptr, "playgo"},
};

} // namespace

std::span<const Export> PlayGoExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::PlayGo
