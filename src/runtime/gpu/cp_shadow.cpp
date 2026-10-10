// SecondSonPC — the command processor in the shadow of the bridge's (P4.2). See cp_shadow.h.
#include "runtime/gpu/cp_shadow.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <intrin.h>

#include <fmt/format.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/cp_validation.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/shader/shader_capture.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/vulkan/vk_foundation.h"
#include "runtime/kernel/clock.h"

// Records are filled naming only the fields that matter.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::CpShadow {

namespace GS = Gpu::Shader; // SecondSonPC's shader frontend (P4.3)

std::atomic<bool> g_enabled{false};

namespace {

using namespace Cp;
using Clock = std::chrono::steady_clock;

enum class DivKind : u8 {
    MissingSubmission, ///< the bridge executes a packet the shadow has no submission for
    Address,           ///< the bridge's packet is not the shadow's next one
    ShadowFault,       ///< the shadow's command processor refused a packet the bridge executed
    Register,          ///< a register differs before a draw or dispatch
    Compute,           ///< a compute register differs before a dispatch
    IndexSize,         ///< INDEX_BUFFER_SIZE
    IndirectBase,      ///< SET_BASE's draw-indirect base
    Extent,            ///< a target size hint
    ShaderContent,     ///< a program address seen before holds another program now
    QueueFull,
    Content,           ///< the bridge's packet has other dwords than the shadow's copy of the submission
    Count,
};
constexpr std::string_view kDivNames[] = {"missing submission", "packet address",     "shadow fault",
                                          "register",           "compute register",   "index buffer size",
                                          "draw-indirect base", "target size hint",   "shader content changed",
                                          "shadow queue full",  "packet content"};
static_assert(std::size(kDivNames) == static_cast<size_t>(DivKind::Count));

constexpr size_t kMaxDetails = 64;
constexpr u32 kMaxDiffs = 4096;
constexpr u32 kTokenMovVcchi = 0xBEEB03FFu; ///< the first instruction of a PS4 shader binary, before its footer
constexpr char kOrbShdr[7] = {'O', 'r', 'b', 'S', 'h', 'd', 'r'};

struct Submission {
    u32 queue = 0;
    u64 addr = 0;
    std::vector<u32> data;
};

struct Feed {
    std::deque<std::unique_ptr<Submission>> fed; ///< handed to the command processor, oldest first
    u64 fed_count = 0;
    u64 recorded = 0;    ///< submissions written to the trace (absolute count)
    bool seen = false;   ///< compute registers taken from the bridge
    bool traced = false; ///< the trace has this queue from a submission's start
};

struct Footer {
    u64 hash = 0;
    u32 length = 0;
};

struct State;

class Reader final : public CommandMemory {
public:
    bool Read(u64 addr, void* dst, u64 bytes) override;
    bool Write(u64, const void*, u64) override {
        return false; // never: the bridge executes the stream
    }
    TraceWriter* trace = nullptr;
};

class Work final : public GpuWork {
public:
    void Draw(const DrawDescriptor& d) override;
    void Dispatch(const DispatchDescriptor& d) override;
    void CacheAction(const CacheActionOp&) override;
    void Flip(const FlipMarker&) override;
    void Fence(const FenceOp& f) override;
    void Dma(const DmaOp& d) override;
    State* s = nullptr;
};

enum class TraceStage : u8 { Off, Armed, Pending, Recording, Done };

struct State {
    Options options;
    Clock::time_point start = Clock::now();
    Reader reader;
    Work work;
    std::unique_ptr<CommandProcessor> cp;
    std::unique_ptr<BridgeMirror> mirror;

    std::mutex inbox_mutex;
    std::vector<std::unique_ptr<Submission>> inbox, drain, free_list;
    std::atomic<u32> inbox_count{0};
    std::array<Feed, kNumQueues> feeds;
    bool synced = false;

    // the GPU thread's counts
    u64 packets = 0, checkpoints = 0, draw_checks = 0, dispatch_checks = 0, flips = 0, ticks = 0;
    u64 checkpoints_with_diff = 0, resynced = 0;
    u64 semantic_draws = 0; ///< draws whose VS/PS registers differ between the hardware's semantics and the bridge's
    u64 semantic_loaded = 0; ///< ... in a program register or a user data slot the shader loads
    std::vector<std::string> semantic_examples;
    std::array<u64, static_cast<size_t>(DivKind::Count)> div_counts{};
    std::vector<std::string> details;
    std::map<u32, u64> reg_counts;
    std::vector<RegDiff> diffs;
    std::array<u64, static_cast<size_t>(Group::Count)> group_changes{};
    u64 draws = 0, dispatches = 0, cache_actions = 0;
    std::array<u64, 3> fences{};
    std::array<u64, 64> fence_events{};
    std::array<u64, 6> dma{};
    u64 dma_patches = 0;
    std::unordered_set<u64> frame_keys, all_keys;
    u64 frame_keys_sum = 0;
    std::unordered_map<u64, Footer> footers;
    std::unordered_set<u64> frame_checked, no_footer;
    std::vector<u32> scan;
    u64 shader_checks = 0, shader_no_footer = 0, shader_changed = 0;
    std::array<u64, kNumQueues> last_compute_program{};

    // trace
    TraceWriter trace;
    TraceStage trace_stage = TraceStage::Off;
    Clock::time_point trace_at;
    u32 trace_frames_done = 0;
    u32 trace_checkpoints = 0;
    std::filesystem::path trace_path;
    std::vector<u32> flat;

    // P4.3: SecondSonPC's shader frontend and its comparison with the bridge's pipelines (--gpu-shaders)
    std::unique_ptr<GS::ShaderFrontend> frontend;
    u32 last_pipeline_id = 0;
    GS::DeviceProfile profile;
    std::string profile_error;
    u64 frontend_ticks = 0;
    struct ShaderCompare {
        u64 bridge_pipelines = 0, compared_stages = 0, ours_waiting = 0, program_diff = 0;
        u64 flat_equal = 0, flat_diff = 0, flat_diff_walked = 0, runtime_equal = 0, runtime_diff = 0,
            runtime_skipped = 0, same_content_other_footer = 0;
        u64 module_coarser = 0, pipeline_coarser = 0, bridge_pipeline_without_draw = 0;
        u64 ud_differ_draws = 0, ud_differ_loaded = 0, ud_differ_read = 0, ud_differ_unknown = 0;
        /// (SecondSonPC's module id, the bridge's footer hash) -> the bridge's module: one program by content may be
        /// several of the bridge's (one per footer hash); a coarser key is two bridge modules for one pair
        std::map<std::pair<u32, u64>, u64> our_module_to_bridge;
        std::unordered_map<u32, u64> our_module_first_footer;
        std::unordered_map<u64, std::unordered_set<u32>> bridge_module_to_ours;
        std::unordered_map<u32, u64> our_pipeline_to_bridge;
        std::unordered_map<u64, std::unordered_set<u32>> bridge_pipeline_to_ours;
        std::array<std::pair<u32, u64>, 6> last_pair{}; ///< per logical stage: the last (our module, bridge module)
        std::vector<std::string> examples;
        bool draw_pending = false; ///< a draw/dispatch was prepared and the bridge has not reported its pipeline
        bool dispatch = false;
    } cmp;
    std::string profile_diff;
    std::unordered_set<u64> bridge_footers; ///< programs the bridge has translated (footer hashes it reported)

    // reports
    Clock::time_point next_report, next_publish;
    std::mutex report_mutex, file_mutex;
    std::string published;
    std::atomic<bool> publish_request{false};
};

State* g_state = nullptr;

/// Whether a divergence's text is still wanted (the report keeps the first kMaxDetails): no formatting otherwise.
bool Detail(const State& s) {
    return s.details.size() < kMaxDetails;
}

void Diverge(State& s, DivKind kind, std::string text) {
    ++s.div_counts[static_cast<size_t>(kind)];
    if (s.details.size() < kMaxDetails) {
        s.details.push_back(fmt::format("[{}] {} (frame {}, packet {})", kDivNames[static_cast<size_t>(kind)],
                                        text, s.flips, s.packets));
    }
}

bool Reader::Read(u64 addr, void* dst, u64 bytes) {
    GuestMemory* gm = GetGuestMemory();
    if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
        return false;
    }
    gm->CopySparse(addr, static_cast<u8*>(dst), bytes); // never faults: unmapped pages read as zeros
    if (trace != nullptr) {
        trace->Memory(addr, std::span<const u8>(static_cast<const u8*>(dst), bytes));
    }
    return true;
}

bool ReadFooter(State& s, u64 addr, Footer& out) {
    GuestMemory* gm = GetGuestMemory();
    if (gm == nullptr || addr == 0 || !GuestMemory::IsValidGpuMapping(addr, 0x10000)) {
        return false; // the contract's reads stay inside the 40-bit GPU space
    }
    u8 info[28] = {};
    u32 head[2] = {};
    gm->CopySparse(addr, reinterpret_cast<u8*>(head), sizeof(head));
    bool found = false;
    if (head[0] == kTokenMovVcchi) {
        gm->CopySparse(addr + (u64{head[1]} + 1) * 8, info, sizeof(info));
        found = std::equal(std::begin(kOrbShdr), std::end(kOrbShdr), info);
    }
    if (!found) { // the footer anywhere in the first 64 KiB (as the bridge searches it)
        s.scan.resize(0x4000);
        gm->CopySparse(addr, reinterpret_cast<u8*>(s.scan.data()), s.scan.size() * 4);
        const auto* bytes = reinterpret_cast<const u8*>(s.scan.data());
        const size_t total = s.scan.size() * 4;
        for (size_t i = 0; i + sizeof(info) <= total && !found; i += 4) {
            if (std::equal(std::begin(kOrbShdr), std::end(kOrbShdr), bytes + i)) {
                std::copy_n(bytes + i, sizeof(info), info);
                found = true;
            }
        }
    }
    if (!found) {
        return false;
    }
    u32 bits = 0;
    std::memcpy(&bits, info + 8, 4);
    out.length = bits >> 8;
    std::memcpy(&out.hash, info + 16, 8);
    return true;
}

void CheckShader(State& s, u64 addr) {
    if (addr == 0 || !s.frame_checked.insert(addr).second || s.no_footer.contains(addr)) {
        return; // once per address and frame: a program rewritten in place shows within the frame it is used
    }
    ++s.shader_checks;
    Footer f;
    const bool ok = ReadFooter(s, addr, f);
    const auto it = s.footers.find(addr);
    if (it == s.footers.end()) {
        if (ok) {
            s.footers.emplace(addr, f);
        } else {
            ++s.shader_no_footer;
            s.no_footer.insert(addr); // not searched again (a 64 KiB scan)
        }
    } else if (ok && (it->second.hash != f.hash || it->second.length != f.length)) {
        ++s.shader_changed;
        Diverge(s, DivKind::ShaderContent,
                fmt::format("program at {:#x}: hash {:#018x} length {} -> hash {:#018x} length {}", addr,
                            it->second.hash, it->second.length, f.hash, f.length));
        it->second = f;
    }
}

constexpr GroupMask kProgramGroups = Bit(Group::PsProgram) | Bit(Group::VsProgram) | Bit(Group::GsProgram) |
                                     Bit(Group::EsProgram) | Bit(Group::HsProgram) | Bit(Group::LsProgram);

void Work::Draw(const DrawDescriptor& d) {
    ++s->draws;
    for (GroupMask m = d.changed; m != 0; m &= m - 1) {
        ++s->group_changes[std::countr_zero(m)];
    }
    if ((d.changed & kProgramGroups) != 0) {
        for (u32 st = 0; st < static_cast<u32>(Stage::Count); ++st) {
            if ((d.active_stages & (1u << st)) != 0) {
                CheckShader(*s, d.shaders[st].address);
            }
        }
    }
    s->frame_keys.insert(d.pipeline_key);
    if (s->all_keys.size() < (1u << 20)) {
        s->all_keys.insert(d.pipeline_key);
    }
    if (s->frontend) {
        const u64 t0 = __rdtsc();
        const GS::DrawPreparation& prep = s->frontend->PrepareDraw(d, s->cp->Registers(), GS::GameMemory());
        s->last_pipeline_id = prep.ready ? prep.pipeline : 0;
        s->cmp.draw_pending = true;
        s->cmp.dispatch = false;
        // CLEAR_STATE (docs §8): the PS user data slots where the hardware's value and the bridge's differ — does the
        // hardware load the slot (RSRC2.USER_SGPR) and does the program read it (its IR after optimisation)?
        const GS::StageView& ps = s->frontend->Stage(GS::HwStage::Ps);
        bool differ = false, loaded = false, read = false, unknown = false;
        const u32 user_sgprs = (s->cp->Registers().Get(Reg::SpiShaderPgmLoPs + 3) >> 1) & 0x1F;
        for (u32 slot = 0; slot < 16; ++slot) {
            const u32 r = Reg::SpiShaderPgmLoPs + 4 + slot;
            if (s->cp->Registers().Get(r) == s->mirror->Get(r)) {
                continue;
            }
            differ = true;
            loaded |= slot < user_sgprs;
            if (ps.status == GS::StageStatus::Ready && ps.module != nullptr) {
                read |= ((ps.module->result.reflection.user_data_used >> slot) & 1) != 0;
            } else {
                unknown = true;
            }
        }
        s->cmp.ud_differ_draws += differ ? 1 : 0;
        s->cmp.ud_differ_loaded += loaded ? 1 : 0;
        s->cmp.ud_differ_read += read ? 1 : 0;
        s->cmp.ud_differ_unknown += differ && unknown ? 1 : 0;
        s->frontend_ticks += __rdtsc() - t0;
    }
}

void Work::Dispatch(const DispatchDescriptor& d) {
    ++s->dispatches;
    if (d.queue < kNumQueues && s->last_compute_program[d.queue] != u64{d.program_version} + 1) {
        s->last_compute_program[d.queue] = u64{d.program_version} + 1;
        CheckShader(*s, d.shader.address);
    }
    if (s->frontend) {
        const u64 t0 = __rdtsc();
        if (const ComputeRegisters* cr = s->cp->Compute(d.queue)) {
            const GS::DrawPreparation& prep = s->frontend->PrepareDispatch(
                d, std::span<const u32, kComputeCount>(cr->values), GS::GameMemory());
            s->last_pipeline_id = prep.ready ? prep.pipeline : 0;
            s->cmp.draw_pending = true;
            s->cmp.dispatch = true;
        }
        s->frontend_ticks += __rdtsc() - t0;
    }
}

/// The bridge picked its pipeline for the packet the shadow processed last (patch 0031, the bridge's GPU thread).
void OnBridgePipeline(const GS::BridgePipelineView& v) {
    State* s = g_state;
    if (s == nullptr || !s->frontend) {
        return;
    }
    const u64 t0 = __rdtsc();
    auto& c = s->cmp;
    ++c.bridge_pipelines;
    if (!c.draw_pending || c.dispatch != v.compute) {
        ++c.bridge_pipeline_without_draw; // the bridge drew something the frontend did not prepare (or vice versa)
        s->frontend_ticks += __rdtsc() - t0;
        return;
    }
    c.draw_pending = false;
    const auto example = [&](std::string text) {
        if (c.examples.size() < 24) {
            c.examples.push_back(fmt::format("frame {}: {}", s->flips, std::move(text)));
        }
    };
    static constexpr std::pair<GS::HwStage, GS::SwStage> kGraphics[] = {
        {GS::HwStage::Ps, GS::SwStage::Fragment}, {GS::HwStage::Vs, GS::SwStage::Vertex}};
    static constexpr std::pair<GS::HwStage, GS::SwStage> kCompute[] = {
        {GS::HwStage::Cs, GS::SwStage::Compute}};
    const auto stages = v.compute ? std::span<const std::pair<GS::HwStage, GS::SwStage>>(kCompute)
                                  : std::span<const std::pair<GS::HwStage, GS::SwStage>>(kGraphics);
    bool all_ready = true;
    size_t mi = 0; // stages compared ready
    for (const auto& [hw, sw] : stages) {
        const GS::StageView& ours = s->frontend->Stage(hw);
        const GS::BridgeStageView& br = v.stages[static_cast<size_t>(sw)];
        if (!br.present) {
            continue;
        }
        s->bridge_footers.insert(br.footer_hash);
        ++c.compared_stages;
        if (ours.program == nullptr || ours.footer_hash != br.footer_hash) {
            ++c.program_diff;
            example(fmt::format("{}: program at {:#x}: footer {:#018x} (ours) vs {:#018x} (bridge)",
                                GS::HwStageName(hw), ours.address, ours.footer_hash, br.footer_hash));
            all_ready = false;
            continue;
        }
        if (ours.status != GS::StageStatus::Ready || ours.module == nullptr) {
            ++c.ours_waiting;
            all_ready = false;
            continue;
        }
        // the flattened user data: SecondSonPC's SRT walk against the bridge's walker, for this draw
        if (ours.plan.flat.size() == br.flat.size() && std::equal(br.flat.begin(), br.flat.end(), ours.plan.flat.begin())) {
            ++c.flat_equal;
        } else {
            ++c.flat_diff;
            size_t at = 0;
            while (at < br.flat.size() && at < ours.plan.flat.size() && br.flat[at] == ours.plan.flat[at]) {
                ++at;
            }
            // past the 16 registers: what the SRT walks read (the registers' own differences are CLEAR_STATE's)
            bool walked = ours.plan.flat.size() != br.flat.size();
            for (size_t k = 16; !walked && k < br.flat.size(); ++k) {
                walked = br.flat[k] != ours.plan.flat[k];
            }
            c.flat_diff_walked += walked ? 1 : 0;
            example(fmt::format("{} {:#018x}: flattened user data {} dwords (ours) vs {} (bridge), first difference at "
                                "{}: {:#x} vs {:#x}",
                                GS::HwStageName(hw), br.footer_hash, ours.plan.flat.size(), br.flat.size(), at,
                                at < ours.plan.flat.size() ? ours.plan.flat[at] : 0, at < br.flat.size() ? br.flat[at] : 0));
        }
        // modules: one of ours never stands for two of the bridge's (a key coarser than the bridge's)
        const u32 id = ours.module->id;
        const auto [ff, ffresh] = c.our_module_first_footer.try_emplace(id, br.footer_hash);
        c.same_content_other_footer += !ffresh && ff->second != br.footer_hash ? 1 : 0;
        const auto [it, fresh] = c.our_module_to_bridge.try_emplace({id, br.footer_hash}, br.module);
        if (!fresh && it->second != br.module) {
            ++c.module_coarser;
            example(fmt::format("{} {:#018x}: SecondSonPC's module {} for two of the bridge's modules",
                                GS::HwStageName(hw), br.footer_hash, id));
            it->second = br.module;
        }
        c.bridge_module_to_ours[br.module].insert(id);
        // runtime information: compared whenever the pair of modules changes
        auto& last = c.last_pair[static_cast<size_t>(sw)];
        if (last.first != id || last.second != br.module) {
            last = {id, br.module};
            std::string diff;
            GS::ShaderTranslator* tr = GS::GetShaderTranslator();
            if (tr == nullptr) {
                ++c.runtime_skipped;
            } else if (tr->CompareRuntime(ours.runtime, s->profile, br.runtime, diff)) {
                ++c.runtime_equal;
            } else {
                ++c.runtime_diff;
                example(fmt::format("{} {:#018x}: runtime information differs: {}", GS::HwStageName(hw),
                                    br.footer_hash, diff));
            }
        } else {
            ++c.runtime_skipped;
        }
        ++mi;
    }
    if (all_ready && mi > 0) {
        // pipelines: the frontend's pipeline of this draw
        const u32 pid = s->last_pipeline_id;
        if (pid != 0) {
            const auto [it, fresh] = c.our_pipeline_to_bridge.try_emplace(pid, v.pipeline);
            if (!fresh && it->second != v.pipeline) {
                ++c.pipeline_coarser;
                example(fmt::format("SecondSonPC's pipeline {} for two of the bridge's pipelines", pid));
                it->second = v.pipeline;
            }
            c.bridge_pipeline_to_ours[v.pipeline].insert(pid);
        }
    }
    s->frontend_ticks += __rdtsc() - t0;
}

void Work::CacheAction(const CacheActionOp&) {
    ++s->cache_actions;
}

void Work::Fence(const FenceOp& f) {
    ++s->fences[static_cast<size_t>(f.kind)];
    ++s->fence_events[f.event_type & 63];
}

void Work::Dma(const DmaOp& d) {
    ++s->dma[static_cast<size_t>(d.kind)];
    s->dma_patches += d.command_patch ? 1 : 0;
}

void Work::Flip(const FlipMarker&) {
    ++s->flips;
    if (s->frontend) {
        s->frontend->NewFrame();
    }
    s->frame_keys_sum += s->frame_keys.size();
    s->frame_keys.clear();
    s->frame_checked.clear();
    if (s->trace_stage == TraceStage::Armed && Clock::now() >= s->trace_at) {
        s->trace_stage = TraceStage::Pending; // from the next graphics submission
    } else if (s->trace_stage == TraceStage::Recording) {
        s->trace.Flip(++s->trace_frames_done);
        if (s->trace_frames_done >= s->options.trace_frames) {
            s->trace.Close(fmt::format("frames {}, checkpoints {}, bytes {}", s->trace_frames_done,
                                       s->trace_checkpoints, s->trace.Bytes()));
            s->reader.trace = nullptr;
            s->trace_stage = TraceStage::Done;
            SSPC_LOG(GPU, Info, "--gpu-trace: {} frames recorded to {} ({} checkpoints, {:.1f} MiB)",
                     s->trace_frames_done, s->trace_path.string(), s->trace_checkpoints,
                     static_cast<double>(s->trace.Bytes()) / (1 << 20));
        }
    }
}

void Drain(State& s) {
    if (s.inbox_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    {
        std::scoped_lock lock{s.inbox_mutex};
        std::swap(s.inbox, s.drain);
        s.inbox_count.store(0, std::memory_order_relaxed);
    }
    for (auto& sub : s.drain) {
        Feed& f = s.feeds[sub->queue];
        const u32 dw = static_cast<u32>(sub->data.size());
        const bool ok = sub->queue == 0 ? s.cp->SubmitGraphics(sub->addr, dw, sub->data.data())
                                        : s.cp->SubmitCompute(sub->queue, sub->addr, dw, sub->data.data());
        if (!ok) {
            Diverge(s, DivKind::QueueFull, fmt::format("queue {}: {}", sub->queue, s.cp->LastError().Text()));
            std::scoped_lock lock{s.inbox_mutex};
            s.free_list.push_back(std::move(sub));
            continue;
        }
        f.fed.push_back(std::move(sub));
        ++f.fed_count;
    }
    s.drain.clear();
}

void Recycle(State& s, u32 queue) {
    Feed& f = s.feeds[queue];
    const u32 pending = s.cp->PendingSubmissions(queue);
    while (f.fed.size() > size_t{pending} + 1) { // keep the current one and the queued ones
        std::scoped_lock lock{s.inbox_mutex};
        s.free_list.push_back(std::move(f.fed.front()));
        f.fed.pop_front();
    }
}

void TraceSubmissions(State& s, u32 queue, u64 started_before, u64 started_now) {
    Feed& f = s.feeds[queue];
    if (started_now == started_before) {
        return;
    }
    if (!f.traced) {
        if (s.cp->StartedMidPacket(queue)) {
            return; // its first dwords end a packet the trace does not have: from the next span on
        }
        f.traced = true;
        f.recorded = started_now - 1; // from the submission starting at this packet
        if (queue != 0) {
            s.trace.Compute(queue, s.cp->Compute(queue)->values.data());
        }
    }
    const u64 first_kept = f.fed_count - f.fed.size();
    for (u64 i = std::max(f.recorded, first_kept); i < started_now; ++i) {
        const Submission& sub = *f.fed[static_cast<size_t>(i - first_kept)];
        s.trace.Submission(queue, queue != 0, sub.addr, sub.data);
    }
    f.recorded = started_now;
}

void BeginTrace(State& s, const BridgeView& v) {
    s.trace_path = s.options.dir / "gpu_trace" /
                   fmt::format("trace_{}.sspctrace",
                               std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - s.start).count());
    const std::string info = fmt::format("SecondSonPC PM4 trace (P4.2): {} frames from {} s after start",
                                         s.options.trace_frames, s.options.trace_start_s);
    if (!s.trace.Open(s.trace_path, info)) {
        SSPC_LOG(GPU, Error, "--gpu-trace: cannot write {}", s.trace_path.string());
        s.trace_stage = TraceStage::Done;
        return;
    }
    auto st = std::make_unique<TraceState>();
    st->index_buffer_size = v.index_buffer_size;
    st->indirect_args = v.indirect_args;
    std::copy_n(v.cb_extent, 8, st->cb_extent.begin());
    st->db_extent = v.db_extent;
    BridgeToFlat(v.regs, st->flat.data());
    s.trace.State(*st);
    s.trace.Compute(0, v.cs); // the graphics queue's own compute registers (dispatches in the DCB)
    s.reader.trace = &s.trace;
    s.trace_stage = TraceStage::Recording;
    for (auto& f : s.feeds) {
        f.traced = false;
    }
}

void CheckDraw(State& s, u32 queue, u64 addr, u8 op, const BridgeView& v, bool traced) {
    ++s.checkpoints;
    ++s.draw_checks;
    const bool full = s.options.full_every != 0 && s.checkpoints % s.options.full_every == 0;
    s.diffs.clear();
    const auto ranges = full ? std::span<const RegRange>(kAllRanges) : std::span<const RegRange>(kDrawRanges);
    const u32 n = DiffRegs(*s.mirror, v.regs, ranges, s.diffs, kMaxDiffs);
    if (n != 0) {
        ++s.checkpoints_with_diff;
        for (const auto& d : s.diffs) {
            ++s.reg_counts[d.reg];
            Diverge(s, DivKind::Register,
                    Detail(s) ? fmt::format("register {:#06x} ({}): ours {:#010x}, bridge {:#010x}, before {} at {:#x}",
                                            d.reg, GroupName(GroupOf(d.reg)), d.ours, d.bridge, Pm4::OpcodeName(op),
                                            addr)
                              : std::string{});
            s.mirror->Resync(d.reg, d.bridge);
            ++s.resynced;
        }
    }
    // Where the hardware's semantics (the processor's registers) and the bridge's (the mirror) differ in what this
    // draw uses: the programs and user data of the active stages. Not a divergence of the shadow: a measure of
    // whether the bridge's differences (CLEAR_STATE clearing SH registers) reach a draw.
    {
        // the program registers, and the user data slots the shader loads (SPI_SHADER_PGM_RSRC2.USER_SGPR)
        static constexpr u32 kStages[] = {Reg::SpiShaderPgmLoPs, Reg::SpiShaderPgmLoVs};
        bool any = false, loaded = false;
        for (const u32 base : kStages) {
            const u32 user_sgprs = (s.cp->Registers().Get(base + 3) >> 1) & 0x1F;
            for (u32 r = base; r < base + 20; ++r) {
                if (s.cp->Registers().Get(r) == s.mirror->Get(r)) {
                    continue;
                }
                any = true;
                const bool used = r < base + 4 || r - (base + 4) < user_sgprs;
                loaded |= used;
                if (s.semantic_examples.size() < 8) {
                    s.semantic_examples.push_back(fmt::format(
                        "register {:#06x} at a draw at {:#x}: hardware {:#x}, bridge {:#x}; {}", r, addr,
                        s.cp->Registers().Get(r), s.mirror->Get(r),
                        used ? fmt::format("loaded by the shader ({} user SGPRs)", user_sgprs)
                             : fmt::format("not loaded ({} user SGPRs)", user_sgprs)));
                }
            }
        }
        s.semantic_draws += any ? 1 : 0;
        s.semantic_loaded += loaded ? 1 : 0;
    }
    const u32 ours_size = s.cp->Registers().Get(Reg::VgtDmaSize);
    if (ours_size != v.index_buffer_size && ours_size != v.regs[Reg::VgtDmaSize]) {
        Diverge(s, DivKind::IndexSize,
                fmt::format("VGT_DMA_SIZE ours {:#x}, bridge {:#x} (register {:#x})", ours_size, v.index_buffer_size,
                            v.regs[Reg::VgtDmaSize]));
    }
    if (s.cp->DrawIndirectBase() != v.indirect_args) {
        Diverge(s, DivKind::IndirectBase,
                fmt::format("ours {:#x}, bridge {:#x}", s.cp->DrawIndirectBase(), v.indirect_args));
        s.cp->SetDrawIndirectBase(v.indirect_args);
    }
    if (!std::equal(s.cp->CbExtents().begin(), s.cp->CbExtents().end(), v.cb_extent) ||
        s.cp->DbExtent() != v.db_extent) {
        Diverge(s, DivKind::Extent,
                fmt::format("cb0 ours {:#x} bridge {:#x}, db ours {:#x} bridge {:#x}", s.cp->CbExtents()[0],
                            v.cb_extent[0], s.cp->DbExtent(), v.db_extent));
        std::array<u32, 8> cb{};
        std::copy_n(v.cb_extent, 8, cb.begin());
        s.cp->SetExtents(cb, v.db_extent);
    }
    if (traced) {
        TraceCheckpoint c{.seq = ++s.trace_checkpoints,
                          .queue = queue,
                          .kind = 0,
                          .addr = addr,
                          .regs_hash = HashBridgeRegs(v.regs, kDrawRanges),
                          .index_buffer_size = v.index_buffer_size,
                          .reg_dma_size = v.regs[Reg::VgtDmaSize],
                          .indirect_args = v.indirect_args,
                          .db_extent = v.db_extent};
        std::copy_n(v.cb_extent, 8, c.cb_extent.begin());
        s.trace.Checkpoint(c);
        if (s.options.trace_full_every != 0 && c.seq % s.options.trace_full_every == 0) {
            s.flat.resize(kNumRegs);
            BridgeToFlat(v.regs, s.flat.data());
            s.trace.Full(c.seq, s.flat.data());
        }
    }
}

void CheckDispatch(State& s, u32 queue, u64 addr, const BridgeView& v, bool traced) {
    ++s.checkpoints;
    ++s.dispatch_checks;
    ComputeRegisters* cr = s.cp->MutableCompute(queue);
    bool differ = false;
    for (u32 i = 4; i < kComputeCount && cr != nullptr; ++i) { // the initiator and the counts: the packet's own
        if (cr->values[i] != v.cs[i]) {
            differ = true;
            Diverge(s, DivKind::Compute,
                    Detail(s) ? fmt::format("queue {} COMPUTE register {:#06x}: ours {:#010x}, bridge {:#010x}, before "
                                            "a dispatch at {:#x}",
                                            queue, kComputeBase + i, cr->values[i], v.cs[i], addr)
                              : std::string{});
            cr->values[i] = v.cs[i];
            ++s.resynced;
        }
    }
    s.checkpoints_with_diff += differ ? 1 : 0;
    if (traced) {
        TraceCheckpoint c{.seq = ++s.trace_checkpoints,
                          .queue = queue,
                          .kind = 1,
                          .addr = addr,
                          .cs_hash = HashCompute(v.cs),
                          .index_buffer_size = v.index_buffer_size,
                          .reg_dma_size = v.regs[Reg::VgtDmaSize],
                          .indirect_args = v.indirect_args,
                          .db_extent = v.db_extent};
        std::copy_n(v.cb_extent, 8, c.cb_extent.begin());
        s.trace.Checkpoint(c);
    }
}

u64 TotalErrors(const Counters& c) {
    u64 e = 0;
    for (const u64 x : c.errors) {
        e += x;
    }
    return e;
}

std::string Report(State& s) {
    const CommandProcessor& cp = *s.cp;
    const Counters& c = cp.Stats();
    const double secs = std::chrono::duration<double>(Clock::now() - s.start).count();
    const u64 freq = Kernel::Clock::TscFrequency();
    const double shadow_ms = freq != 0 ? static_cast<double>(s.ticks) * 1000.0 / static_cast<double>(freq) : 0.0;
    const double frames = static_cast<double>(std::max<u64>(s.flips, 1));
    u64 divs = 0;
    for (const u64 d : s.div_counts) {
        divs += d;
    }
    std::string out = fmt::format(
        "SecondSonPC command processor shadow (--gpu-shadow, P4.2): {:.1f} s, {} frames (flip markers), {} packets, "
        "{} submissions ({} on the graphics queue)\n"
        "Shadow's own time on the bridge's GPU thread: {:.1f} ms total, {:.3f} ms per frame, {:.0f} ns per packet\n\n",
        secs, s.flips, s.packets, c.submissions, s.feeds[0].fed_count, shadow_ms, shadow_ms / frames,
        s.packets != 0 ? shadow_ms * 1e6 / static_cast<double>(s.packets) : 0.0);
    out += fmt::format("Checkpoints: {} ({} before draws, {} before dispatches), with a difference {}, registers "
                       "resynchronised {}\nDivergences: {} total\n",
                       s.checkpoints, s.draw_checks, s.dispatch_checks, s.checkpoints_with_diff, s.resynced, divs);
    for (size_t k = 0; k < s.div_counts.size(); ++k) {
        out += fmt::format("  {:24} {}\n", kDivNames[k], s.div_counts[k]);
    }
    if (!s.reg_counts.empty()) {
        std::vector<std::pair<u32, u64>> regs(s.reg_counts.begin(), s.reg_counts.end());
        std::ranges::sort(regs, [](const auto& a, const auto& b) { return a.second > b.second; });
        out += "Registers that differed (most first):";
        for (size_t i = 0; i < std::min<size_t>(regs.size(), 32); ++i) {
            out += fmt::format("{} {:#06x} {}={}", i % 4 == 0 ? "\n  " : "", regs[i].first,
                               GroupName(GroupOf(regs[i].first)), regs[i].second);
        }
        out += "\n";
    }
    out += fmt::format("Draws whose VS/PS program or user data differ between the hardware's semantics and the "
                       "bridge's (CLEAR_STATE clearing SH registers in the bridge): {}, of them in a program register "
                       "or a user data slot the shader loads: {}\n",
                       s.semantic_draws, s.semantic_loaded);
    for (const auto& e : s.semantic_examples) {
        out += "  " + e + "\n";
    }
    out += fmt::format("First {} differences:\n", s.details.size());
    for (const auto& d : s.details) {
        out += "  " + d + "\n";
    }
    out += fmt::format(
        "\nCommand processor (shadow mode): {} draws, {} dispatches ({} with the shader disabled), {} flips, {} cache "
        "actions, {} waits, fences EOP {} / EOS {} / RELEASE {}, {} GDS fences, {} DMA ({} bytes; prefetch {}, "
        "command patches {}), {} WRITE_DATA ({} bytes), {} indirect buffers, {} ring carries, {} type-2\n",
        c.draws, c.dispatches, c.dispatches_disabled, c.flips, c.cache_actions, c.waits, s.fences[0], s.fences[1],
        s.fences[2], c.gds_fences, c.dma, c.dma_bytes, c.dma_prefetch, c.dma_command_patches, c.writes, c.write_bytes,
        c.indirect_buffers, c.ring_carries, c.type2);
    const double writes = static_cast<double>(cp.Registers().Writes());
    out += fmt::format(
        "Register writes {}, changing a value {} ({:.1f}%); pipeline key rebuilt {} times ({:.1f}% of draws); "
        "distinct pipeline keys {} in the run, {:.0f} per frame on average; extent hints {} (missing {}); SET_*_REG "
        "with the index field set {}; errors {}\n",
        cp.Registers().Writes(), cp.Registers().Changes(),
        writes > 0 ? 100.0 * static_cast<double>(cp.Registers().Changes()) / writes : 0.0, c.pipeline_key_rebuilds,
        c.draws != 0 ? 100.0 * static_cast<double>(c.pipeline_key_rebuilds) / static_cast<double>(c.draws) : 0.0,
        s.all_keys.size(), static_cast<double>(s.frame_keys_sum) / frames, c.extent_hints, c.extent_hints_missing,
        c.reg_index_nonzero, TotalErrors(c));
    out += "Groups changed at a draw (draws whose group changed since the previous draw):";
    for (u32 g = 0; g < static_cast<u32>(Group::Count); ++g) {
        out += fmt::format("{} {}={}", g % 6 == 0 ? "\n  " : "", GroupName(static_cast<Group>(g)), s.group_changes[g]);
    }
    out += "\nFence event types:";
    for (u32 e = 0; e < 64; ++e) {
        if (s.fence_events[e] != 0) {
            out += fmt::format(" {}={}", e, s.fence_events[e]);
        }
    }
    out += "\nDMA kinds:";
    for (u32 k = 0; k < 6; ++k) {
        out += fmt::format(" {}={}", DmaKindName(static_cast<DmaKind>(k)), s.dma[k]);
    }
    out += "\nPackets per opcode (graphics / compute):\n";
    for (u32 op = 0; op < 256; ++op) {
        if (c.graphics_packets[op] != 0 || c.compute_packets[op] != 0) {
            out += fmt::format("  {:#04x} {:30} {:>12} {:>10}\n", op, Pm4::OpcodeName(static_cast<u8>(op)),
                               c.graphics_packets[op], c.compute_packets[op]);
        }
    }
    out += fmt::format("Shaders: {} distinct programs by address, {} footer checks, {} without a footer, {} addresses "
                       "that held another program later\n",
                       s.footers.size(), s.shader_checks, s.shader_no_footer, s.shader_changed);
    if (!s.trace_path.empty()) {
        out += fmt::format("Trace: {} ({} frames, {} checkpoints, {} bytes)\n", s.trace_path.string(),
                           s.trace_frames_done, s.trace_checkpoints, s.trace.Bytes());
    }
    if (cp.LastError().kind != CpErrorKind::None) {
        out += "Last error of the shadow's command processor: " + cp.LastError().Text() + "\n";
    }
    if (s.frontend) {
        const double fms =
            freq != 0 ? static_cast<double>(s.frontend_ticks) * 1000.0 / static_cast<double>(freq) : 0.0;
        out += fmt::format("\nShader frontend (--gpu-shaders, P4.3): {:.1f} ms on the bridge's GPU thread, {:.3f} ms per "
                           "frame (included in the shadow's time); device profile: {}{}\n",
                           fms, fms / frames, s.profile.source.empty() ? "none" : s.profile.source,
                           s.profile_error.empty() ? "" : " (" + s.profile_error + ")");
        out += s.frontend->Report();
        const auto& c = s.cmp;
        u64 finer = 0;
        for (const auto& [m, ours] : c.bridge_module_to_ours) {
            finer += ours.size() > 1 ? 1 : 0;
        }
        u64 finer_p = 0;
        for (const auto& [p, ours] : c.bridge_pipeline_to_ours) {
            finer_p += ours.size() > 1 ? 1 : 0;
        }
        out += fmt::format(
            "Against the bridge's pipeline at the same packet: {} pipelines reported ({} without a prepared draw), "
            "{} stages compared: program differs {}, SecondSonPC still waiting {}; flattened user data equal {} / "
            "different {} (of them past the 16 registers, in what the SRT walks read: {}); runtime information equal {} "
            "/ different {} (not compared again: {})\n",
            c.bridge_pipelines, c.bridge_pipeline_without_draw, c.compared_stages, c.program_diff, c.ours_waiting,
            c.flat_equal, c.flat_diff, c.flat_diff_walked, c.runtime_equal, c.runtime_diff, c.runtime_skipped);
        out += fmt::format(
            "Modules: {} of SecondSonPC's mapped to {} of the bridge's; one of ours with one footer for two of the "
            "bridge's (a coarser key) {}; the same program by content under another footer (one module of ours, several "
            "of the bridge's: expected) {} draws; one of the bridge's for several of ours (a finer key) {}. Pipelines: {} "
            "of ours to {} of the bridge's (the bridge's are per footer hash); one of ours for two of theirs {}, one of "
            "theirs for several of ours {}\n",
            c.our_module_first_footer.size(), c.bridge_module_to_ours.size(), c.module_coarser,
            c.same_content_other_footer, finer,
            c.our_pipeline_to_bridge.size(), c.bridge_pipeline_to_ours.size(), c.pipeline_coarser, finer_p);
        out += fmt::format("CLEAR_STATE / PS user data (hardware vs bridge): {} draws differ in a PS user data slot; "
                           "the hardware loads the slot (RSRC2.USER_SGPR) in {}; the program reads it (IR) in {}; "
                           "program not ready to tell {}\n",
                           c.ud_differ_draws, c.ud_differ_loaded, c.ud_differ_read, c.ud_differ_unknown);
        for (const auto& e : c.examples) {
            out += "  " + e + "\n";
        }
    }
    return out;
}

void Publish(State& s) {
    std::string text = Report(s);
    std::scoped_lock lock{s.report_mutex};
    s.published = std::move(text);
}

void ExitStep() {
    WriteNow();
    if (g_state != nullptr && g_state->frontend) {
        g_state->frontend->Stop(); // the worker ends before the process does (a translation in progress finishes)
    }
}

/// --gpu-shaders: a program is handed to the translator once the bridge has translated it (same thread: the GPU's).
bool BridgeTranslated(void* ctx, u64 footer_hash) {
    return static_cast<State*>(ctx)->bridge_footers.contains(footer_hash);
}

} // namespace

void Enable(const Options& options) {
    if (g_state != nullptr) {
        return;
    }
    auto* s = new State();
    s->options = options;
    s->work.s = s;
    s->cp = std::make_unique<CommandProcessor>(
        Cp::Options{.mode = Mode::Shadow, .memory = &s->reader, .work = &s->work, .resume_after_fault = true});
    s->mirror = std::make_unique<BridgeMirror>(*s->cp);
    s->diffs.reserve(kMaxDiffs);
    s->next_report = s->start + std::chrono::seconds(options.report_seconds);
    if (options.trace_start_s >= 0 && options.trace_frames > 0) {
        s->trace_stage = TraceStage::Armed;
        s->trace_at = s->start + std::chrono::seconds(options.trace_start_s);
    }
    if (options.shaders) {
        // P4.3: the device profile from SecondSonPC's own Vulkan foundation (a level-A device, destroyed at once)
        if (!Vk::QueryDeviceProfile(-1, s->profile, s->profile_error)) {
            SSPC_LOG(Shader, Error, "--gpu-shaders: no device profile ({}); resources only, no translation",
                     s->profile_error);
        }
        GS::FrontendOptions fo;
        fo.async = true;
        fo.translate = options.shaders_translate && s->profile_error.empty();
        fo.validation = options.shaders_per_bind ? GS::Validation::PerBind : GS::Validation::PerFrame;
        fo.profile = &s->profile;
        fo.may_translate = &BridgeTranslated;
        fo.may_translate_ctx = s;
        s->frontend = std::make_unique<GS::ShaderFrontend>(fo);
        GS::SetBridgePipelineCallback(&OnBridgePipeline);
        SSPC_LOG(Shader, Info, "--gpu-shaders: SecondSonPC's shader frontend at every draw/dispatch of the shadow "
                               "(translations {}, programs revalidated {}), compared with the bridge's pipelines",
                 fo.translate ? "on its worker thread" : "off", options.shaders_per_bind ? "at every bind" : "once a frame");
    }
    g_state = s;
    g_enabled.store(true, std::memory_order_release); // after the state exists
    if (options.exit_step) {
        Lifecycle::AddExitStep(Lifecycle::Stage::Reports, "gpu command processor shadow", &ExitStep);
    }
    SSPC_LOG(GPU, Info,
             "--gpu-shadow: SecondSonPC's command processor in the bridge's shadow (state and descriptions only; it "
             "writes nothing, waits for nothing); report {}\\gpu_shadow.txt{}",
             options.dir.string(),
             s->trace_stage == TraceStage::Armed
                 ? fmt::format(", trace of {} frames from {} s", options.trace_frames, options.trace_start_s)
                 : std::string{});
}

void OnSubmit(u32 queue, const u32* data, size_t dwords) {
    State& s = *g_state;
    if (queue >= kNumQueues || data == nullptr) {
        return;
    }
    std::unique_ptr<Submission> sub;
    {
        std::scoped_lock lock{s.inbox_mutex};
        if (!s.free_list.empty()) {
            sub = std::move(s.free_list.back());
            s.free_list.pop_back();
        }
    }
    if (!sub) {
        sub = std::make_unique<Submission>();
    }
    sub->queue = queue;
    sub->addr = reinterpret_cast<u64>(data);
    sub->data.assign(data, data + dwords); // the game's buffer as submitted (after the driver's flip patch)
    std::scoped_lock lock{s.inbox_mutex};
    s.inbox.push_back(std::move(sub));
    s.inbox_count.fetch_add(1, std::memory_order_release);
}

void OnPacket(u32 queue, const u32* header, size_t available, bool buffered, const BridgeView& v) {
    State& s = *g_state;
    const u64 t0 = __rdtsc();
    ++s.packets;
    if (queue >= kNumQueues) {
        return;
    }
    Drain(s);
    if (!s.synced) {
        // the shadow starts from the bridge's state (its registers before its first packet)
        std::vector<u32> flat(kNumRegs);
        BridgeToFlat(v.regs, flat.data());
        s.mirror->Load(flat.data());
        std::array<u32, 8> cb{};
        std::copy_n(v.cb_extent, 8, cb.begin());
        s.cp->SetExtents(cb, v.db_extent);
        s.cp->SetDrawIndirectBase(v.indirect_args);
        s.synced = true;
    }
    Feed& f = s.feeds[queue];
    if (!f.seen) {
        if (ComputeRegisters* cr = s.cp->MutableCompute(queue)) {
            std::copy_n(v.cs, kComputeCount, cr->values.begin());
        }
        f.seen = true;
    }
    const u64 started_before = f.fed_count - s.cp->PendingSubmissions(queue);
    u64 addr = 0;
    std::span<const u32> pkt;
    bool have = s.cp->Peek(queue, addr, pkt);
    // the same packet: the same dwords (the bridge reads them now; the shadow decoded its copy of the submission), and
    // for a packet in guest memory the same address
    const auto same_dwords = [&] { return pkt.size() <= available && std::equal(pkt.begin(), pkt.end(), header); };
    const auto same = [&] { return buffered ? same_dwords() : addr == reinterpret_cast<u64>(header); };
    if (!have || !same()) {
        const bool resynced = !buffered && s.cp->Resync(queue, reinterpret_cast<u64>(header)) &&
                              s.cp->Peek(queue, addr, pkt) && same();
        const DivKind kind = have ? DivKind::Address : DivKind::MissingSubmission;
        Diverge(s, kind,
                Detail(s) ? fmt::format("queue {}: the bridge at {:#x} (opcode {:#04x}), the shadow at {:#x}{}",
                                        queue, reinterpret_cast<u64>(header), (header[0] >> 8) & 0xFF,
                                        have ? addr : 0, resynced ? "; resynchronised at a submission's start" : "")
                          : std::string{});
        if (!resynced) {
            s.ticks += __rdtsc() - t0;
            return;
        }
    }
    if (!buffered && !same_dwords()) { // the guest rewrote the packet after submitting it, or the copy is wrong
        Diverge(s, DivKind::Content,
                Detail(s) ? fmt::format("queue {}: the packet at {:#x} (opcode {:#04x}) differs from the submitted "
                                        "copy the shadow decodes",
                                        queue, addr, (header[0] >> 8) & 0xFF)
                          : std::string{});
    }
    const u64 started_now = f.fed_count - s.cp->PendingSubmissions(queue);
    if (s.trace_stage == TraceStage::Pending && queue == 0 && started_now != started_before) {
        BeginTrace(s, v);
    }
    const bool recording = s.trace_stage == TraceStage::Recording;
    if (recording) {
        TraceSubmissions(s, queue, started_before, started_now);
    }
    const bool traced = recording && f.traced;
    if ((pkt[0] >> 30) == 3) {
        const u8 op = static_cast<u8>(pkt[0] >> 8);
        switch (op) {
        case Pm4::Op::DrawIndex2:
        case Pm4::Op::DrawIndexOffset2:
        case Pm4::Op::DrawIndexAuto:
        case Pm4::Op::DrawIndexIndirect:
        case Pm4::Op::DrawIndirect:
            CheckDraw(s, queue, addr, op, v, traced);
            break;
        case Pm4::Op::DispatchDirect:
        case Pm4::Op::DispatchIndirect:
            CheckDispatch(s, queue, addr, v, traced);
            break;
        default:
            break;
        }
    }
    if (s.cp->Step(queue) == StepStatus::Faulted) {
        Diverge(s, DivKind::ShadowFault, s.cp->LastError().Text());
    }
    Recycle(s, queue);
    if ((s.packets & 0xFFF) == 0 || s.publish_request.load(std::memory_order_relaxed)) {
        const auto now = Clock::now();
        const bool periodic = s.options.report_seconds != 0 && now >= s.next_report;
        // published every second: the exit step writes it even when this thread no longer takes packets then
        if (s.publish_request.load(std::memory_order_relaxed) || periodic || now >= s.next_publish) {
            Publish(s);
            s.next_publish = now + std::chrono::seconds(1);
            s.publish_request.store(false, std::memory_order_relaxed);
            if (periodic) {
                s.next_report = now + std::chrono::seconds(s.options.report_seconds);
                u64 divs = 0;
                for (const u64 d : s.div_counts) {
                    divs += d;
                }
                const u64 freq = Kernel::Clock::TscFrequency();
                SSPC_LOG(GPU, Info,
                         "gpu shadow: {} frames, {} packets, {} checkpoints, {} divergences, shadow {:.3f} ms/frame",
                         s.flips, s.packets, s.checkpoints, divs,
                         freq != 0 && s.flips != 0 ? static_cast<double>(s.ticks) * 1000.0 /
                                                         static_cast<double>(freq) / static_cast<double>(s.flips)
                                                   : 0.0);
            }
        }
    }
    s.ticks += __rdtsc() - t0;
}

bool WriteNow() {
    State* s = g_state;
    if (s == nullptr) {
        return false;
    }
    s->publish_request.store(true, std::memory_order_relaxed); // the GPU thread publishes at its next packet
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    while (s->publish_request.load(std::memory_order_relaxed) && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool fresh = !s->publish_request.exchange(false);
    std::string text;
    {
        std::scoped_lock lock{s->report_mutex};
        text = s->published.empty() ? std::string("no packets reached the shadow\n") : s->published;
    }
    if (!fresh) {
        text = "(the GPU thread did not answer within 500 ms: its report of the last second it worked)\n" + text;
    }
    std::scoped_lock file_lock{s->file_mutex};
    std::ofstream out(s->options.dir / "gpu_shadow.txt", std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace SecondSon::Gpu::CpShadow
