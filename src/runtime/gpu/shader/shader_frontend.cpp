// SecondSonPC — the shader frontend (P4.3). See shader_frontend.h.
#include <windows.h>

#include "runtime/gpu/shader/shader_frontend.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <intrin.h>

#include <fmt/format.h>

#include "runtime/gpu/gpu_memory.h"

namespace SecondSon::Gpu::Shader {

// ---- clock and histogram

namespace {

double TicksPerNs() {
    static const double value = [] {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        const u64 c0 = __rdtsc();
        while (Clock::now() - t0 < std::chrono::milliseconds(20)) {
        }
        const auto t1 = Clock::now();
        const u64 c1 = __rdtsc();
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        return ns > 0 ? static_cast<double>(c1 - c0) / ns : 1.0;
    }();
    return value;
}

u32 Bucket(u64 ns) {
    if (ns < 2) {
        return 0;
    }
    const u32 e = 63 - static_cast<u32>(std::countl_zero(ns)); // floor(log2)
    const u32 frac = e >= 3 ? static_cast<u32>((ns >> (e - 3)) & 7) : static_cast<u32>((ns << (3 - e)) & 7);
    return std::min<u32>(e * 8 + frac, 511);
}

u64 BucketUpper(u32 b) {
    const u32 e = b / 8, frac = b % 8;
    const double v = std::ldexp(1.0 + (frac + 1) / 8.0, static_cast<int>(e));
    return static_cast<u64>(v);
}

} // namespace

const bool g_fine_timing = [] {
    char v[4] = {};
    return GetEnvironmentVariableA("SSPC_GPU_TIMINGS", v, sizeof(v)) == 1 && v[0] == '1';
}();

const u32 g_fine_every = [] {
    char v[32] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_GPU_TIMINGS", v, sizeof(v));
    const std::string_view s(v, n < sizeof(v) ? n : 0);
    if (!s.starts_with("sample")) {
        return 0u;
    }
    const u32 every = s.size() > 7 && s[6] == ':' ? static_cast<u32>(std::strtoul(v + 7, nullptr, 10)) : 32u;
    return std::max<u32>(every, 1);
}();

bool g_fine_now = false;

u64 TicksToNs(u64 ticks) {
    return static_cast<u64>(static_cast<double>(ticks) / TicksPerNs());
}

void Histogram::Add(u64 ns) {
    ++buckets_[Bucket(ns)];
    ++count_;
    sum_ += ns;
    max_ = std::max(max_, ns);
}

u64 Histogram::Percentile(double p) const {
    if (count_ == 0) {
        return 0;
    }
    const u64 rank = static_cast<u64>(std::ceil(p * static_cast<double>(count_)));
    u64 seen = 0;
    for (u32 b = 0; b < buckets_.size(); ++b) {
        seen += buckets_[b];
        if (seen >= rank) {
            return std::min(BucketUpper(b), max_);
        }
    }
    return max_;
}

void Histogram::Merge(const Histogram& o) {
    for (size_t i = 0; i < buckets_.size(); ++i) {
        buckets_[i] += o.buckets_[i];
    }
    count_ += o.count_;
    sum_ += o.sum_;
    max_ = std::max(max_, o.max_);
}

std::string Histogram::Text() const {
    if (count_ == 0) {
        return "n 0";
    }
    return fmt::format("n {}, mean {:.0f} ns, P50/P95/P99 {}/{}/{} ns, max {} ns", count_, MeanNs(), Percentile(0.5),
                       Percentile(0.95), Percentile(0.99), max_);
}

std::string_view StageStatusName(StageStatus status) {
    static constexpr std::string_view kNames[] = {"ready",     "pending",     "failed", "no program",
                                                  "fetch shader", "unsupported"};
    static_assert(std::size(kNames) == static_cast<size_t>(StageStatus::Count));
    const auto i = static_cast<size_t>(status);
    return i < std::size(kNames) ? kNames[i] : "?";
}

// ---- guest memory in the game

namespace {

class GameReader final : public GuestReader {
public:
    bool Read(u64 addr, void* dst, u64 bytes) override {
        GuestMemory* gm = GetGuestMemory();
        if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
            return false;
        }
        gm->CopySparse(addr, static_cast<u8*>(dst), bytes); // never faults: unmapped pages read as zeros
        return true;
    }
};

/// SSPC_DIAG_STAGE_MEMO=verify: what a stage's preparation produced - its module, key, flattened user data (every
/// descriptor the SRT walk loaded), buffer and vertex bindings. Never 0.
u64 StageFingerprint(const StageView& s) {
    u64 h = 0xCBF29CE484222325ull;
    const auto mix = [&h](u64 v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(reinterpret_cast<uintptr_t>(s.module));
    mix(s.key.hash);
    mix(s.end.unified | (u64{s.end.buffer} << 32));
    for (const u32 w : s.plan.flat) {
        mix(w);
    }
    for (const BufferBinding& b : s.plan.buffers) {
        mix(b.address);
        mix(b.size);
    }
    for (const VertexBinding& v : s.plan.vertices) {
        mix(v.read[0] | (u64{v.read[1]} << 32));
        mix(v.read[2] | (u64{v.read[3]} << 32));
        mix(v.address);
    }
    mix(s.plan.images.size() | (s.plan.samplers.size() << 32));
    return h | 1;
}

} // namespace

GuestReader& GameMemory() {
    static GameReader reader;
    return reader;
}

// ---- the frontend

ShaderFrontend::ShaderFrontend(const FrontendOptions& options) : options_(options) {
    translator_ = options_.translator != nullptr ? options_.translator : GetShaderTranslator();
    programs_.SetValidation(options_.validation);
    memo_on_ = options_.stage_memo;
    if (char v[4] = {}; memo_on_ && GetEnvironmentVariableA("SSPC_DIAG_MEMO_STATS", v, sizeof(v)) == 1 && v[0] == '1') {
        memo_diag_ = true;
    }
    if (char v[8] = {}; memo_on_ && GetEnvironmentVariableA("SSPC_DIAG_STAGE_MEMO", v, sizeof(v)) == 6 &&
                        std::string_view(v) == "verify") {
        memo_verify_ = true;
    }
    (void)TicksToNs(0); // the TSC calibration (20 ms) now, not at the first draw
    if (options_.async && options_.translate) {
        worker_ = std::thread([this] { WorkerLoop(); });
    }
}

ShaderFrontend::~ShaderFrontend() {
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::string ShaderFrontend::MemoReport() const {
    if (!memo_diag_) {
        return {};
    }
    static constexpr const char* kStage[] = {"ps", "vs", "gs", "es", "hs", "ls", "cs"};
    std::vector<std::pair<u64, u64>> v(memo_ud_masks_.begin(), memo_ud_masks_.end());
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string t = fmt::format("stage memo misses by input (SSPC_DIAG_MEMO_STATS): address {}, binding start {}, "
                                "runtime {}, frame {}, not ready {}; user data dwords that differ (top):",
                                memo_other_by_[0], memo_other_by_[1], memo_other_by_[2], memo_other_by_[3],
                                memo_other_by_[4]);
    t += fmt::format(" misses an 8-entry memo would have had: ps {}, vs {};", memo_would_hit_[0], memo_would_hit_[1]);
    for (size_t i = 0; i < v.size() && i < 16; ++i) {
        const u32 stage = static_cast<u32>(v[i].first >> 16);
        t += fmt::format(" {} {:#06x}: {};", stage < std::size(kStage) ? kStage[stage] : "?", v[i].first & 0xFFFF,
                         v[i].second);
    }
    return t + "\n";
}

const StageView& ShaderFrontend::Stage(HwStage stage) const {
    return stages_[static_cast<size_t>(stage)];
}

void ShaderFrontend::NewFrame() {
    programs_.NewEpoch();
    ++stats_.epochs;
    ++memory_gen_;
    // fold what the worker counted
    std::scoped_lock lock{mutex_};
    stats_.translations += std::exchange(w_translations_, 0);
    stats_.translation_failures += std::exchange(w_failures_, 0);
    stats_.translate_us += std::exchange(w_translate_us_, 0);
    stats_.emit_us += std::exchange(w_emit_us_, 0);
    stats_.spirv_bytes += std::exchange(w_spirv_bytes_, 0);
    times_.translate.Merge(w_translate_);
    w_translate_ = {};
}

void ShaderFrontend::Invalidate(u64 addr, u64 size) {
    programs_.Invalidate(addr, size);
    stage_epoch_.fill(0); // no stage keeps its program without asking the cache
    ++memory_gen_;        // nor its memoized preparation (P4.7)
}

void ShaderFrontend::Stop() {
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    idle_cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void ShaderFrontend::Drain() {
    std::unique_lock lock{mutex_};
    idle_cv_.wait(lock, [&] { return (queue_.empty() && running_ == 0) || stop_; });
}

Module* ShaderFrontend::NewModule(Program& program, const RuntimeParams& runtime, const BindingStart& start,
                                  std::span<const u32, 16> user_data) {
    auto m = std::make_unique<Module>();
    m->id = next_module_++;
    m->runtime = runtime;
    m->start = start;
    std::copy(user_data.begin(), user_data.end(), m->user_data.begin());
    m->queued_tick = Ticks();
    Module* raw = m.get();
    program.modules.push_back(std::move(m));
    return raw;
}

bool ShaderFrontend::Enqueue(Job job) {
    if (translator_ == nullptr) {
        translator_ = options_.translator != nullptr ? options_.translator : GetShaderTranslator();
    }
    if (!options_.translate || translator_ == nullptr || options_.profile == nullptr) {
        return false;
    }
    if (options_.may_translate != nullptr &&
        !options_.may_translate(options_.may_translate_ctx, job.program->footer_hash)) {
        ++stats_.gated; // asked again at a later draw
        return false;
    }
    {
        std::scoped_lock lock{mutex_};
        if (stop_) {
            return false;
        }
    }
    job.module->queued = true;
    ++stats_.queued;
    if (!options_.async) {
        Run(job);
        return true;
    }
    {
        std::scoped_lock lock{mutex_};
        if (queue_.size() >= options_.max_queue) {
            job.module->queued = false;
            --stats_.queued;
            ++stats_.queue_full;
            return false;
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void ShaderFrontend::Run(Job& job) {
    Module& m = *job.module;
    TranslationRequest req;
    req.hw = m.runtime.hw;
    req.sw = m.runtime.sw;
    req.address = job.address;
    req.code = job.program->code;
    req.user_data = m.user_data;
    req.runtime = m.runtime;
    req.profile = options_.profile;
    req.start = m.start;
    req.memory = job.memory;
    req.flatbuf = job.flatbuf;
    req.fetch_code = job.fetch_code;
    req.vertex_sharps = job.vertex_sharps;
    req.footer_hash = job.program->footer_hash;
    const u64 t0 = Ticks();
    TranslationResult result;
    try {
        result = translator_->Translate(req);
    } catch (const std::exception& e) {
        result = {};
        result.error = TranslationError::Emit;
        result.detail = std::string("the translator threw: ") + e.what();
    } catch (...) {
        result = {};
        result.error = TranslationError::Emit;
        result.detail = "the translator threw";
    }
    const u64 ns = TicksToNs(Ticks() - t0);
    const bool ok = result.Ok();
    runs_total_.fetch_add(1, std::memory_order_relaxed);
    run_ns_total_.fetch_add(ns, std::memory_order_relaxed);
    if (ns > run_ns_max_.load(std::memory_order_relaxed)) {
        run_ns_max_.store(ns, std::memory_order_relaxed); // (one writer at a time in the native backend: synchronous)
    }
    {
        std::scoped_lock lock{mutex_};
        ++w_translations_;
        w_failures_ += ok ? 0 : 1;
        w_translate_us_ += result.translate_us;
        w_emit_us_ += result.emit_us;
        w_spirv_bytes_ += result.spirv.size() * 4;
        w_translate_.Add(ns);
    }
    m.result = std::move(result);
    m.state.store(ok ? ModuleState::Ready : ModuleState::Failed, std::memory_order_release);
}

void ShaderFrontend::WorkerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) {
                break;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        Run(job);
        {
            std::scoped_lock lock{mutex_};
            --running_;
        }
        idle_cv_.notify_all();
    }
    idle_cv_.notify_all();
}

void ShaderFrontend::Prepare(Module& m) {
    m.prepared = true;
    const ShaderReflection& r = m.result.reflection;
    if (!r.srt_walker.empty() && !m.srt.Compile(r.srt_walker, m.srt_error)) {
        ++stats_.srt_compile_failures;
    }
    if (!m.keyed) {
        // the first module of a layout: its key from what its translation saw (its flattened user data and V#s)
        ResourceBindingPlan plan;
        plan.flat = m.result.flatbuf;
        const FetchShader* fetch = r.fetch.present ? &r.fetch : nullptr;
        DecodeResources(r, m.start, fetch, m.user_data, 0, VertexSource{nullptr, m.result.vertex_sharps}, plan);
        BuildSpecializationKey(m.runtime, m.start, r, fetch, plan, m.key);
        m.keyed = true;
    }
}

StageStatus ShaderFrontend::PrepareStage(StageView& s, HwStage hw, SwStage sw, u64 addr,
                                         std::span<const u32, 16> user_data, const BindingStart& start,
                                         GuestReader& memory, bool may_queue) {
    const size_t si = static_cast<size_t>(hw);
    // P4.7: the stage's last ready preparation again (its StageView holds it) when nothing it was built from changed
    StageMemo& memo = memo_[si];
    u64 verify = 0; // SSPC_DIAG_STAGE_MEMO=verify: a hit prepared in full anyway - the fingerprint of what it would give
    if (memo_on_ && memo.valid) {
        const bool same_inputs = memo.addr == addr && memo.start == start && memo.runtime_ver == runtime_ver_[si] &&
                                 memo.program_epoch == programs_.Epoch() && s.status == StageStatus::Ready &&
                                 s.module != nullptr;
        const bool same_ud = std::memcmp(memo.user_data.data(), user_data.data(), sizeof(memo.user_data)) == 0;
        if (same_inputs && same_ud && memo.memory_gen == memory_gen_ &&
            s.module->state.load(std::memory_order_acquire) == ModuleState::Ready) {
            if (!memo_verify_) {
                ++stats_.memo_hits;
                ++s.module->uses;
                return StageStatus::Ready;
            }
            verify = StageFingerprint(s);
            ++stats_.memo_verified;
            ++stats_.memo_mismatches; // (taken back below when the full preparation is ready and the same)
        } else {
            ++(!same_inputs ? stats_.memo_miss_other
                            : !same_ud ? stats_.memo_miss_user_data : stats_.memo_miss_memory);
        }
        if (verify == 0 && memo_diag_) { // SSPC_DIAG_MEMO_STATS=1: why the stages miss (gpu_native.txt)
            if (same_inputs && !same_ud) {
                u32 mask = 0;
                for (u32 i = 0; i < 16; ++i) {
                    mask |= memo.user_data[i] != user_data[i] ? 1u << i : 0u;
                }
                ++memo_ud_masks_[(u64{si} << 16) | mask];
            } else if (!same_inputs) {
                ++memo_other_by_[memo.addr != addr                          ? 0
                                 : !(memo.start == start)                   ? 1
                                 : memo.runtime_ver != runtime_ver_[si]     ? 2
                                 : memo.program_epoch != programs_.Epoch() ? 3
                                                                            : 4];
            }
            // would a memo of the stage's last 8 preparations have had it (a draw alternating between programs)?
            u64 h = addr ^ (u64{start.unified} << 48) ^ (u64{start.buffer} << 40) ^ (runtime_ver_[si] << 20);
            for (const u32 w : user_data) {
                h = (h ^ w) * 0x100000001B3ull;
            }
            const u64 g = memory_gen_.load(std::memory_order_relaxed);
            auto& hist = memo_hist_[si];
            if (std::ranges::any_of(hist, [&](const auto& e) { return e.first == h && e.second == g; })) {
                ++memo_would_hit_[si];
            }
            hist[memo_hist_next_[si]++ % hist.size()] = {h, g};
        }
    }
    memo.valid = false;
    s.start = start;
    s.end = start;
    s.module = nullptr;
    // the program
    const u64 tp = FineTicks();
    const bool keep = options_.validation == Validation::PerFrame && s.program != nullptr && s.address == addr &&
                      stage_epoch_[si] == programs_.Epoch();
    if (keep) {
        ++stats_.program_reuse;
    } else {
        const auto l = programs_.Get(memory, addr, hw);
        s.address = addr;
        s.program = l.program;
        s.footer_hash = l.footer_hash;
        s.program_error = l.error;
        stage_epoch_[si] = programs_.Epoch();
    }
    if (g_fine_timing || g_fine_now) {
        times_.program.Add(TicksToNs(Ticks() - tp));
    }
    if (s.program == nullptr) {
        stage_epoch_[si] = 0;
        return StageStatus::NoProgram;
    }
    Program& program = *s.program;
    (void)sw;

    // the layout: the first module for these runtime parameters
    Layout* layout = program.FindLayout(s.runtime);
    if (layout == nullptr) {
        program.layouts.push_back({s.runtime, NewModule(program, s.runtime, start, user_data)});
        layout = &program.layouts.back();
        ++stats_.layouts;
    }
    if (layout->module->state.load(std::memory_order_acquire) == ModuleState::Failed &&
        layout->retries < options_.layout_retries &&
        (layout->module->result.error == TranslationError::Srt ||
         layout->module->result.error == TranslationError::FetchShader)) {
        // a read of guest memory the game was rewriting: a new first translation from this draw's state
        ++layout->retries;
        ++stats_.layout_retries;
        layout->module = NewModule(program, s.runtime, start, user_data);
    }
    Module& first = *layout->module;
    if (!first.queued && may_queue && first.state.load(std::memory_order_acquire) == ModuleState::Pending) {
        Enqueue(Job{&program, &first, addr, &memory, {}, {}, {}});
    }
    const ModuleState fs = first.state.load(std::memory_order_acquire);
    if (fs == ModuleState::Pending) {
        return StageStatus::Pending;
    }
    if (fs == ModuleState::Failed) {
        s.module = &first;
        return StageStatus::Failed;
    }
    if (!first.prepared) {
        Prepare(first);
    }
    const ShaderReflection& r = first.result.reflection;

    // the draw's fetch shader
    const FetchShader* fetch = nullptr;
    const FetchCache::Entry* fe = nullptr;
    if (r.fetch.present) {
        const u32 b = r.fetch.sgpr_base;
        const u64 faddr = b + 1 < 16 ? ((user_data[b] | (u64{user_data[b + 1]} << 32)) & 0xFFFFFFFFFFFFull) : 0;
        fe = &fetches_.Get(memory, faddr, b, programs_.Epoch(), options_.validation);
        if (!fe->ok) {
            return StageStatus::FetchError;
        }
        fetch = &fe->fetch;
    }

    // resources and the key
    const u64 tr = FineTicks();
    if (!ResolveResources(r, first.srt, start, fetch, user_data, addr, memory, s.plan)) {
        ++stats_.srt_walk_failures;
    }
    if (!first.srt_error.empty()) {
        ++stats_.srt_walk_failures; // a walker SecondSonPC cannot interpret: nothing past the registers resolved
        s.plan.flat.resize(16);
        DecodeResources(r, start, fetch, user_data, addr, VertexSource{&memory, {}}, s.plan);
    }
    stats_.srt_reads += s.plan.srt.reads;
    stats_.srt_faults += s.plan.srt.faults;
    for (size_t i = 0; i < s.plan.status_counts.size(); ++i) {
        stats_.resources[i] += s.plan.status_counts[i];
    }
    const u64 tk = FineTicks();
    if (g_fine_timing || g_fine_now) {
        times_.resources.Add(TicksToNs(tk - tr));
    }
    BuildSpecializationKey(s.runtime, start, r, fetch, s.plan, s.key);
    if (g_fine_timing || g_fine_now) {
        times_.key.Add(TicksToNs(Ticks() - tk));
    }
    s.end = EndOf(start, s.plan);

    // the module
    Module* m = program.FindModule(s.key, stats_.module_collisions);
    if (m == nullptr) {
        ++stats_.module_misses;
        m = NewModule(program, s.runtime, start, user_data);
        m->key = s.key;
        m->keyed = true;
    } else if (m->state.load(std::memory_order_acquire) == ModuleState::Ready) {
        ++stats_.module_hits;
    }
    s.module = m;
    if (!m->queued && may_queue && m->state.load(std::memory_order_acquire) == ModuleState::Pending) {
        Job job{&program, m, addr, &memory, s.plan.flat, fe != nullptr ? fe->code : std::vector<u32>{}, {}};
        for (const VertexBinding& v : s.plan.vertices) {
            job.vertex_sharps.push_back(v.read);
        }
        Enqueue(std::move(job));
    }
    const ModuleState ms = m->state.load(std::memory_order_acquire);
    if (ms == ModuleState::Pending) {
        return StageStatus::Pending;
    }
    if (ms == ModuleState::Failed) {
        return StageStatus::Failed;
    }
    if (!m->prepared) {
        Prepare(*m);
        times_.wait.Add(TicksToNs(Ticks() - m->queued_tick));
    }
    const ShaderReflection& mr = m->result.reflection;
    if (mr.buffers.size() != r.buffers.size() || mr.images.size() != r.images.size() ||
        mr.samplers.size() != r.samplers.size() || mr.flatbuf_dwords != r.flatbuf_dwords) {
        // the specialization changed the resources (an FMask image, ...): the module's own reflection decides the
        // plan and the next stage's bindings
        ++stats_.layout_mismatch;
        ResolveResources(mr, m->srt, start, fetch, user_data, addr, memory, s.plan);
        s.end = EndOf(start, s.plan);
    }
    ++m->uses;
    if (verify != 0 && StageFingerprint(s) == verify) {
        --stats_.memo_mismatches;
    }
    // (the generation after the reads: a read that waited for a GPU write saw it)
    memo.valid = true;
    memo.memory_gen = memory_gen_;
    memo.program_epoch = programs_.Epoch();
    memo.runtime_ver = runtime_ver_[si];
    memo.addr = addr;
    memo.start = start;
    std::memcpy(memo.user_data.data(), user_data.data(), sizeof(memo.user_data));
    return StageStatus::Ready;
}

const DrawPreparation& ShaderFrontend::PrepareDraw(const Cp::DrawDescriptor& d, const Cp::RegisterFile& regs,
                                                   GuestReader& memory) {
    const u64 t0 = FineTicks();
    ++stats_.draws;
    prep_ = {};
    if (DecodeStages(regs.Get(Reg::VgtShaderStagesEn)) != StageConfig::Vs) {
        ++stats_.unsupported;
        prep_.unsupported = true;
        if (g_fine_timing || g_fine_now) {
            times_.draw.Add(TicksToNs(Ticks() - t0));
        }
        return prep_;
    }
    BindingStart start{};
    bool ready = true;
    static constexpr std::pair<HwStage, SwStage> kStages[] = {{HwStage::Ps, SwStage::Fragment},
                                                              {HwStage::Vs, SwStage::Vertex}};
    for (const auto& [hw, sw] : kStages) {
        const size_t si = static_cast<size_t>(hw);
        StageView& s = stages_[si];
        // the runtime parameters: rebuilt when their register groups changed (and for indirect draws, whose SH
        // offsets come with the packet)
        if (!runtime_valid_[si] || (d.changed & kRuntimeGroups) != 0 || d.indirect) {
            const u32 vtx = d.indirect ? d.base_vtx_loc : 0, inst = d.indirect ? d.start_inst_loc : 0;
            runtime_valid_[si] = BuildGraphicsRuntime(regs, hw, sw, vtx, inst, s.runtime);
            ++runtime_ver_[si];
            ++stats_.runtime_builds;
        } else {
            ++stats_.runtime_reuse;
        }
        const auto cpst = static_cast<size_t>(hw); // Cp::Stage has the graphics stages in HwStage's order
        const std::span<const u32, 16> ud(d.user_data[cpst]);
        s.status = runtime_valid_[si] ? PrepareStage(s, hw, sw, d.shaders[cpst].address, ud, start, memory, ready)
                                      : StageStatus::Unsupported;
        ++stats_.stage_status[static_cast<size_t>(s.status)];
        ready &= s.status == StageStatus::Ready;
        start = s.end;
    }
    if (ready) {
        const u64 tp = FineTicks();
        const StageView& ps = stages_[static_cast<size_t>(HwStage::Ps)];
        const StageView& vs = stages_[static_cast<size_t>(HwStage::Vs)];
        const u32 ps_id = ps.module->id, vs_id = vs.module->id;
        if (last_pipeline_ != 0 && ps_id == last_ps_ && vs_id == last_vs_ &&
            (d.changed & PipelineStateGroups()) == 0) {
            ++stats_.pipeline_reuse;
            prep_.pipeline = last_pipeline_;
            prep_.pipeline_reused = true;
        } else {
            GraphicsPipelineState state;
            BuildGraphicsPipelineState(regs, vs_id, ps_id, ps.module->result.reflection.mrt_mask, state);
            bool created = false;
            prep_.pipeline = pipelines_.GetGraphics(state, created);
            last_pipeline_ = prep_.pipeline;
            last_ps_ = ps_id;
            last_vs_ = vs_id;
        }
        if (g_fine_timing || g_fine_now) {
            times_.pipeline.Add(TicksToNs(Ticks() - tp));
        }
        prep_.ready = true;
        ++stats_.ready;
    } else {
        ++stats_.waiting;
        last_pipeline_ = 0;
    }
    if (g_fine_timing || g_fine_now) {
        times_.draw.Add(TicksToNs(Ticks() - t0));
    }
    return prep_;
}

const DrawPreparation& ShaderFrontend::PrepareDispatch(const Cp::DispatchDescriptor& d,
                                                       std::span<const u32, Cp::kComputeCount> cs,
                                                       GuestReader& memory) {
    const u64 t0 = FineTicks();
    ++stats_.dispatches;
    prep_ = {};
    StageView& s = stages_[static_cast<size_t>(HwStage::Cs)];
    BuildComputeRuntime(cs, s.runtime);
    ++runtime_ver_[static_cast<size_t>(HwStage::Cs)];
    ++stats_.runtime_builds;
    const std::span<const u32, 16> ud(d.user_data);
    s.status = PrepareStage(s, HwStage::Cs, SwStage::Compute, d.shader.address, ud, BindingStart{}, memory, true);
    ++stats_.stage_status[static_cast<size_t>(s.status)];
    if (s.status == StageStatus::Ready) {
        bool created = false;
        prep_.pipeline = pipelines_.GetCompute(s.module->id, created);
        prep_.ready = true;
        ++stats_.ready;
    } else {
        ++stats_.waiting;
    }
    if (g_fine_timing || g_fine_now) {
        times_.dispatch.Add(TicksToNs(Ticks() - t0));
    }
    return prep_;
}

u64 ShaderFrontend::MemoryBytes() const {
    u64 bytes = programs_.CodeBytes() + pipelines_.MemoryBytes();
    for (const Program* p : programs_.All()) {
        bytes += sizeof(Program);
        for (const auto& m : p->modules) {
            if (m->state.load(std::memory_order_acquire) == ModuleState::Pending) {
                bytes += sizeof(Module) + m->key.words.capacity() * 4; // the worker may be filling the result
                continue;
            }
            bytes += sizeof(Module) + m->key.words.capacity() * 4 + m->result.spirv.capacity() * 4 +
                     m->result.flatbuf.capacity() * 4 + m->result.reflection.srt_walker.capacity() +
                     (m->result.reflection.buffers.capacity() + m->result.reflection.images.capacity() +
                      m->result.reflection.samplers.capacity()) * 48;
        }
    }
    return bytes;
}

std::string ShaderFrontend::Report() const {
    const FrontendStats& st = stats_;
    const ProgramCacheStats& pc = programs_.Stats();
    std::string t;
    t += fmt::format("draws {}, dispatches {}: ready {}, waiting {}, unsupported stage configuration {}\n", st.draws,
                     st.dispatches, st.ready, st.waiting, st.unsupported);
    t += "stage results:";
    for (size_t i = 0; i < st.stage_status.size(); ++i) {
        t += fmt::format(" {}={}", StageStatusName(static_cast<StageStatus>(i)), st.stage_status[i]);
    }
    t += "\n";
    t += fmt::format("programs: {} distinct ({} addresses, {} KiB of code); lookups {}, kept by the stage {}, trusted "
                     "in the epoch {}, read {} (same {}, changed {}, new {}), {} KiB read, invalidated {}; errors:",
                     programs_.Programs(), programs_.Addresses(), programs_.CodeBytes() / 1024, pc.lookups,
                     st.program_reuse, pc.trusted, pc.reads, pc.same, pc.changed, pc.new_programs,
                     pc.bytes_read / 1024, pc.invalidations);
    for (size_t i = 1; i < pc.errors.size(); ++i) {
        t += fmt::format(" {}={}", ProgramErrorName(static_cast<ProgramError>(i)), pc.errors[i]);
    }
    t += fmt::format("\nfetch shaders: {} (reads {}, changed {})\n", fetches_.Size(), fetches_.reads,
                     fetches_.changed);
    t += fmt::format("runtime parameters: built {}, kept {}\n", st.runtime_builds, st.runtime_reuse);
    t += fmt::format("modules: {} (layouts {}); key hits {}, misses {}, hash collisions {}, layout changed by the "
                     "specialization {}\n",
                     next_module_ - 1, st.layouts, st.module_hits, st.module_misses, st.module_collisions,
                     st.layout_mismatch);
    t += fmt::format("translations: queued {}, queue full {}, waiting for the bridge's translation {}, done {}, "
                     "failed {}, first translations retried {}; translate {} ms, emit {} ms, SPIR-V {} KiB\n",
                     st.queued, st.queue_full, st.gated, st.translations, st.translation_failures, st.layout_retries,
                     st.translate_us / 1000, st.emit_us / 1000, st.spirv_bytes / 1024);
    t += fmt::format("SRT walks: guest reads {}, refused reads (0) {}, walkers not interpreted {}, walks failed {}\n",
                     st.srt_reads, st.srt_faults, st.srt_compile_failures, st.srt_walk_failures);
    t += "resources:";
    for (size_t i = 0; i < st.resources.size(); ++i) {
        t += fmt::format(" {}={}", ResourceStatusName(static_cast<ResourceStatus>(i)), st.resources[i]);
    }
    const PipelineCacheStats& ps = pipelines_.Stats();
    t += fmt::format("\npipelines: {} (graphics {}, compute {}); lookups {}, hits {}, misses {}, hash collisions {}, "
                     "kept without a lookup {}\n",
                     pipelines_.Size(), ps.graphics, ps.compute, ps.lookups, ps.hits, ps.misses, ps.collisions,
                     st.pipeline_reuse);
    t += fmt::format("memory held: {} KiB\n", MemoryBytes() / 1024);
    t += fmt::format("time per draw (whole preparation): {}\n", times_.draw.Text());
    t += fmt::format("  program lookup: {}\n", times_.program.Text());
    t += fmt::format("  resources (SRT walk + descriptors): {}\n", times_.resources.Text());
    t += fmt::format("  specialization key: {}\n", times_.key.Text());
    t += fmt::format("  pipeline (state + lookup, when looked up): {}\n", times_.pipeline.Text());
    t += fmt::format("time per dispatch: {}\n", times_.dispatch.Text());
    t += fmt::format("translation (worker, request to result): {}\n", times_.translate.Text());
    t += fmt::format("wait from queueing to first use: {}\n", times_.wait.Text());
    return t;
}

} // namespace SecondSon::Gpu::Shader
