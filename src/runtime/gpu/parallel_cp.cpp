// SecondSonPC — the parallel command processor (P4.10). See parallel_cp.h.
#include <windows.h>

#include "runtime/gpu/parallel_cp.h"

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <exception>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_executor.h"
#include "runtime/gpu/vulkan/vk_resources.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Native {

namespace {

enum QueuedKind : size_t { kQCache, kQFence, kQDma, kQWaitMet, kQAcquired, kQCopy };

constexpr u64 kLookup = ~0ull; ///< a GPU write's value thread 2 publishes once its record ran
constexpr u64 kPage = 4096;    ///< BufferCache's validity pages: its pending GPU writes are per page

/// Every register of the file, through the groups' hashes it keeps up to date (Options::verify).
u64 RegistersHash(const Cp::RegisterFile& regs) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 g = 0; g < static_cast<u32>(Cp::Group::Count); ++g) {
        h = (h ^ regs.GroupHash(static_cast<Cp::Group>(g))) * 0x100000001B3ull;
    }
    return h;
}

/// `lo`/`hi`: the ranges' bounds (P4.11: the processor's reads and writes outside them are answered at once).
bool Overlaps(const std::vector<std::pair<u64, u64>>& ranges, u64 lo, u64 hi, u64 addr, u64 bytes) {
    if (ranges.empty() || addr >= hi || addr + bytes <= lo) {
        return false;
    }
    return std::ranges::any_of(ranges, [&](const auto& r) { return addr < r.second && r.first < addr + bytes; });
}

/// What the executor reads of a stage the frontend prepared (not its specialization key).
void CopyStage(GS::StageView& d, const GS::StageView& s) {
    d.status = s.status;
    d.address = s.address;
    d.program = s.program;
    d.footer_hash = s.footer_hash;
    d.module = s.module;
    d.runtime = s.runtime;
    d.start = s.start;
    d.end = s.end;
    d.program_error = s.program_error;
    d.plan.stage = s.plan.stage;
    d.plan.flat.assign(s.plan.flat.begin(), s.plan.flat.end());
    d.plan.buffers.assign(s.plan.buffers.begin(), s.plan.buffers.end());
    d.plan.images.assign(s.plan.images.begin(), s.plan.images.end());
    d.plan.samplers.assign(s.plan.samplers.begin(), s.plan.samplers.end());
    d.plan.vertices.assign(s.plan.vertices.begin(), s.plan.vertices.end());
    d.plan.status_counts = s.plan.status_counts;
    d.plan.srt = s.plan.srt;
    if (!s.plan.error.empty() || !d.plan.error.empty()) {
        d.plan.error = s.plan.error;
    }
}

/// FB-6 verify: the copies of one view (vectors of trivially copyable bindings: their bytes).
template <typename T>
bool SameBytes(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
bool SameView(const GS::StageView& a, const GS::StageView& b) {
    return a.status == b.status && a.address == b.address && a.program == b.program &&
           a.footer_hash == b.footer_hash && a.module == b.module &&
           std::memcmp(&a.runtime, &b.runtime, sizeof(a.runtime)) == 0 && a.start == b.start && a.end == b.end &&
           a.program_error == b.program_error && a.plan.stage == b.plan.stage && SameBytes(a.plan.flat, b.plan.flat) &&
           SameBytes(a.plan.buffers, b.plan.buffers) && SameBytes(a.plan.images, b.plan.images) &&
           SameBytes(a.plan.samplers, b.plan.samplers) && SameBytes(a.plan.vertices, b.plan.vertices) &&
           a.plan.status_counts == b.plan.status_counts && a.plan.error == b.plan.error;
}

} // namespace

const GS::StageView& ParallelCp::ReuseView(GS::StageView& rec, bool shared, bool same, GS::StageView& last) {
    if (shared || stage_reuse_ == 0) {
        return rec;
    }
    const auto bump = [](std::atomic<u64>& c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); };
    if (same) {
        bump(reuse_same_);
        if (stage_reuse_ == 2 && !SameView(rec, last)) {
            bump(reuse_mismatches_);
        }
        return last;
    }
    bump(reuse_copied_);
    std::swap(rec, last); // (the record keeps the old buffers: thread 1 copies into their capacity later)
    return last;
}

std::string_view DrainReasonName(DrainReason r) {
    static constexpr std::string_view kNames[] = {
        "round end",     "cache action", "fence",       "dma report",  "flip",         "timeline write",
        "timeline copy", "timeline gds", "memory wait", "wait met",    "synchronize",  "memory read",
        "memory write",  "frontend read", "stop"};
    static_assert(std::size(kNames) == static_cast<size_t>(DrainReason::Count));
    const auto i = static_cast<size_t>(r);
    return i < std::size(kNames) ? kNames[i] : "?";
}

u32 ForcedCopyFallbackEvery() {
    static const u32 every = [] {
        char v[16] = {};
        const u32 n = GetEnvironmentVariableA("SSPC_P410_DIAG_COPY_FALLBACK", v, sizeof(v)) > 0
                          ? static_cast<u32>(std::strtoul(v, nullptr, 10))
                          : 0u;
        if (n != 0) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_P410_DIAG_COPY_FALLBACK={} - every {}-th queued timeline copy is "
                                   "copied by the CPU on the recording thread (its refusal path)",
                     n, n);
        }
        return n;
    }();
    return every;
}

ParallelCp::ParallelCp(Vk::Executor& executor, Vk::BufferCache* buffers, const Options& options)
    : exec_(executor), buffers_(buffers), spin_ticks_(Kernel::Clock::TscFrequency() / 1'000'000 * options.spin_us),
      async_(options.async), async_copies_(options.async && options.async_copies), verify_(options.verify),
      async_writes_(options.async && options.async_copies && options.async_writes),
      async_gds_only_(options.async_gds_only),
      frontend_(options.timeline != nullptr ? options.frontend : nullptr), map_gen_(options.map_gen),
      timeline_(options.timeline), slots_(std::make_unique<Record[]>(kSlots)),
      journal_(std::make_unique<u32[]>(kJournalWords)) {
    if (frontend_ != nullptr) {
        gw_bits_ = std::make_unique<u64[]>(kGwWords);
        rec_value_ = std::make_unique<std::atomic<u64>[]>(kRecRing);
    }
    fthread_mode_ = options.frontend_thread && frontend_ != nullptr;
    if (char v[16] = {}; frontend_ != nullptr && GetEnvironmentVariableA("SSPC_CITY_BATCH", v, sizeof(v)) > 0) {
        const std::string_view sv(v);
        batch_on_ = sv == "1" || sv.starts_with("ab:");
        batch_ab_ = sv.starts_with("ab:") ? std::max<u32>(1, static_cast<u32>(std::strtoul(v + 3, nullptr, 10))) : 0;
        if (batch_on_) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_BATCH={}: draws differing only in their geometry are recorded as one draw", sv);
        }
    }
    if (char v[16] = {}; frontend_ != nullptr && !fthread_mode_ &&
                         GetEnvironmentVariableA("SSPC_CITY_GEOFAST", v, sizeof(v)) > 0) {
        const std::string_view sv(v);
        geofast_ = sv == "1" ? 1 : sv == "verify" ? 2 : 0;
        if (geofast_ == 2) {
            geofast_check_ = std::make_unique<Record>();
        }
        if (geofast_ != 0) {
            SSPC_LOG(GPU, Info, "SSPC_CITY_GEOFAST={}: a draw differing from the one before it only in its geometry takes "
                                "that one's preparation with its own vertex bindings", sv);
        }
    }
    async_acquire_ = options.async_acquire && async_ && frontend_ != nullptr && !fthread_mode_;
    journal_changes_only_ = options.journal_changes_only;
    async_flip_ = options.async_flip && frontend_ != nullptr && !fthread_mode_;
    if (char v[16] = {}; frontend_ != nullptr && !fthread_mode_ &&
                         GetEnvironmentVariableA("SSPC_FB_STAGE_REUSE", v, sizeof(v)) > 0) {
        stage_reuse_ = std::string_view(v) == "verify" ? 2 : v[0] == '1' ? 1 : 0;
        if (stage_reuse_ != 0) {
            SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_FB_STAGE_REUSE={} - unchanged draw stages not copied", v);
        }
    }
}

ParallelCp::~ParallelCp() {
    Stop();
}

void ParallelCp::Attach(Cp::CommandProcessor& cp) {
    cp_ = &cp;
    replica_ = cp.Registers(); // (the same registers, versions and hashes from here on, the journal keeps them so)
    replica_.SetSink(nullptr);
    replica_.SetMirror(nullptr);
    if (fthread_mode_) {
        freplica_ = cp.Registers(); // (the frontend thread's own copy, kept by the same journal)
        freplica_.SetSink(nullptr);
        freplica_.SetMirror(nullptr);
    }
    cp.MutableRegisters().SetSink(this);
    cp.MutableRegisters().SetSinkChangesOnly(journal_changes_only_);
    exec_.SetRegisters(&replica_);
}

void ParallelCp::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    if (StallTrace::Deep()) {
        push_tsc_ = std::make_unique<u64[]>(kRecRing);
    }
    if (fthread_mode_) {
        fthread_ = std::thread([this] { FrontLoop(); });
    }
    thread_ = std::thread([this] { Loop(); });
}

void ParallelCp::Stop() {
    if (!started_) {
        return;
    }
    Drain(DrainReason::Stop);
    BeginRecord(Kind::Stop);
    PushRecord();
    if (fthread_.joinable()) {
        fthread_.join(); // (it passes the stop record on to thread 2)
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    started_ = false;
    exec_.SetMemoBumps(true);
    if (cp_ != nullptr) {
        cp_->MutableRegisters().SetSink(nullptr);
        cp_->MutableRegisters().SetSinkChangesOnly(false);
        exec_.SetRegisters(&cp_->Registers());
    }
}

// ---- thread 1

ParallelCp::Record& ParallelCp::BeginRecord(Kind kind) {
    if (next_ - head_.load(std::memory_order_acquire) >= kSlots) {
        WaitHead(next_ - kSlots + 1, true, kWhyFull); // backpressure: the oldest slot executed
    }
    Record& r = slots_[next_ & (kSlots - 1)];
    r.kind = kind;
    r.front_memo = pending_front_memo_; // (frontend thread: told before it prepares this record)
    pending_front_memo_ = false;
    r.memory_changed = pending_memory_changed_;
    if (pending_memory_changed_) {
        pending_memory_changed_ = false;
        ++t1_.memo_changes;
    }
    return r;
}

void ParallelCp::PushRecord() {
    Record& r = slots_[next_ & (kSlots - 1)];
    r.journal_end = jtail_; // every register write of the stream before the record
    const u64 depth = next_ - head_.load(std::memory_order_relaxed);
    t1_.depth_sum += depth;
    t1_.depth_max = std::max(t1_.depth_max, depth);
    ++t1_.records;
    if (push_tsc_ != nullptr) {
        push_tsc_[next_ & (kRecRing - 1)] = __rdtsc(); // (published with tail_ below)
    }
    ++next_;
    tail_.store(next_, std::memory_order_seq_cst);
    // (tail_'s waiter: thread 2, or with the frontend thread that thread - thread 2 then waits on fhead_)
    if ((fthread_mode_ ? f_waiting_ : t2_waiting_).load(std::memory_order_seq_cst) != 0) {
        tail_.notify_one();
    }
    if (frontend_ != nullptr && !fthread_mode_ && (next_ & 63) == 0) {
        RetireGpuWrites(); // (the GPU writes completed so far leave the list; the bits follow at the next hit)
    }
}

void ParallelCp::WaitHead(u64 target, bool full, u16 why) {
    u64 h = head_.load(std::memory_order_acquire);
    if (h >= target) {
        return;
    }
    const u64 h0 = h;
    const u64 t0 = __rdtsc();
    while (h < target && __rdtsc() - t0 < spin_ticks_) {
        _mm_pause();
        h = head_.load(std::memory_order_acquire);
    }
    while (h < target) {
        t1_wait_target_.store(target, std::memory_order_seq_cst);
        h = head_.load(std::memory_order_seq_cst);
        if (h < target) {
            ++t1_.t1_sleeps;
            head_.wait(h, std::memory_order_seq_cst);
        }
        t1_wait_target_.store(0, std::memory_order_relaxed);
        h = head_.load(std::memory_order_acquire);
    }
    const u64 t1 = __rdtsc();
    const u64 ticks = t1 - t0;
    if (full) {
        t1_.t1_full_ticks += ticks;
        ++t1_.t1_full_waits;
    } else {
        t1_.t1_drain_ticks += ticks;
        if (why < t1_.drain_ticks_by.size()) {
            t1_.drain_ticks_by[why] += ticks;
        }
    }
    // P4.12: what thread 1 waited for - thread 2 executing records up to `target` (a: the target; b: records then
    // queued; c: the drain's reason)
    StallTrace::Span(!full ? StallTrace::Ev::Drain
                     : why == kWhyJournal ? StallTrace::Ev::JournalFull
                                          : StallTrace::Ev::QueueFull,
                     t0, t1, target, static_cast<u32>(target - std::min(target, h0)), why);
}

void ParallelCp::ApplyPending() {
    ApplyJournal(jtail_);
    if (has_deferred_.load(std::memory_order_acquire)) {
        ApplyDeferred(~0ull);
    }
    if (pending_memory_changed_) {
        pending_memory_changed_ = false;
        ++t1_.memo_changes;
        exec_.GuestMemoryChanged();
    }
    if (frontend_ != nullptr) {
        exec_.SetMemoBumps(true); // (calls made here tell the frontend themselves, in stream order)
        if (fthread_mode_) {
            // the frontend thread is parked (every record prepared and executed): what it was not told yet
            ApplyFrontEvents(~0ull);
            if (pending_front_memo_) {
                pending_front_memo_ = false;
                frontend_->GuestMemoryChanged();
            }
        } else {
            // GPU writes whose completion value fell out of the ring: the executor decides now (rare)
            while (!gw_.empty() && gw_.front().value == kLookup && gw_.front().record + kRecRing <= next_ &&
                   !exec_.FrontendReadPending(gw_.front().begin, gw_.front().end - gw_.front().begin)) {
                gw_.pop_front();
                gw_stale_ = true;
            }
        }
    }
    queued_tw_.clear(); // (every queued timeline copy executed: they are the executor's own now)
    qtw_lo_ = ~0ull;
    qtw_hi_ = 0;
    SnapshotTimeline();
}

void ParallelCp::SnapshotTimeline() {
    const u64 v = exec_.TimelineWritesVersion();
    if (v != tw_version_seen_) {
        exec_.TimelineWriteRanges(exec_tw_);
        tw_version_seen_ = v;
        etw_lo_ = ~0ull;
        etw_hi_ = 0;
        for (const auto& [b, e] : exec_tw_) {
            etw_lo_ = std::min(etw_lo_, b);
            etw_hi_ = std::max(etw_hi_, e);
        }
    }
}

void ParallelCp::Drain(DrainReason why) {
    if (!started_) {
        return;
    }
    if (Idle()) {
        ++t1_.drains_empty;
    } else {
        ++t1_.drains;
        ++t1_.drains_by[static_cast<size_t>(why)];
        WaitHead(next_, false, static_cast<u16>(why));
    }
    // thread 2 is parked (it waits for the next record): what is not handed on yet is applied here
    ApplyPending();
    if (frontend_ != nullptr) {
        RetireGpuWrites(); // (frontend thread: parked too - the queue is empty, so it prepared everything)
    }
}

bool ParallelCp::RoundEndQueued() {
    if (!started_ || !async_ || Idle()) {
        return false;
    }
    BeginRecord(Kind::RoundEnd);
    PushRecord();
    if (frontend_ != nullptr && !fthread_mode_) {
        // (only records already executed: safe with thread 2 running; with the frontend thread the list is that
        // thread's - it retires them itself, FrontStep - and a second writer here corrupted it: CITY-OPT)
        RetireGpuWrites();
    }
    return true;
}

bool ParallelCp::TimelinePending(u64 addr, u64 bytes) {
    if (!started_ || Idle()) {
        if (started_) {
            ApplyPending();
        }
        return exec_.TimelineWritePending(addr, bytes); // (exact: nothing executes on thread 2 now)
    }
    if (Overlaps(queued_tw_, qtw_lo_, qtw_hi_, addr, bytes) || Overlaps(exec_tw_, etw_lo_, etw_hi_, addr, bytes)) {
        ++t1_.timeline_read_drains;
        return true; // the caller drains and asks the executor
    }
    return false;
}

bool ParallelCp::CopyQueuedOver(u64 addr, u64 bytes) const {
    return started_ && !Idle() && Overlaps(queued_tw_, qtw_lo_, qtw_hi_, addr, bytes);
}

void ParallelCp::MemoryChanged() {
    if (fthread_mode_) {
        FrontMemoChanged(); // (with the next record, or at the next idle point)
        return;
    }
    if (frontend_ != nullptr) {
        frontend_->GuestMemoryChanged(); // (the frontend is here: at once, where the write is in the stream)
        return;
    }
    pending_memory_changed_ = true;
}

void ParallelCp::DeferInvalidateIfCached(u64 addr, u64 bytes) {
    std::scoped_lock lock{deferred_mutex_};
    // (a compute ring's read pointer is written after each packet: the same range again is not queued twice)
    if (deferred_.empty() || deferred_.back().before != next_ || deferred_.back().addr != addr ||
        deferred_.back().bytes != bytes) {
        deferred_.push_back({next_, addr, bytes});
        ++t1_.deferred_invalidations;
    }
    has_deferred_.store(true, std::memory_order_release);
}

void ParallelCp::JournalReserve(u32 words) {
    const u64 idx = jtail_ & (kJournalWords - 1);
    const bool wrap = idx + words > kJournalWords;
    const u64 need = words + (wrap ? kJournalWords - idx : 0);
    // (the frontend thread reads the journal too: its words are free once both replicas passed them)
    const u64 read = fthread_mode_ ? std::min(jhead_.load(std::memory_order_acquire), fjhead_.load(std::memory_order_acquire))
                                   : jhead_.load(std::memory_order_acquire);
    if (jtail_ + need - read > kJournalWords) {
        // thread 2 replays the journal with records only: one that carries it so far, then wait until it ran
        BeginRecord(Kind::Journal);
        PushRecord();
        ++t1_.journal_records;
        WaitHead(next_, true, kWhyJournal);
    }
    if (wrap) {
        journal_[idx] = u32{kOpPad} << 28; // the entry starts at the ring's start
        jtail_ += kJournalWords - idx;
    }
}

void ParallelCp::OnWrite(Cp::Space space, u32 offset, const u32* values, u32 count) {
    const u32 words = 2 + count;
    JournalReserve(words);
    u32* w = JournalAt(jtail_);
    w[0] = (u32{kOpWrite} << 28) | (static_cast<u32>(space) << 24) | count;
    w[1] = offset;
    std::memcpy(w + 2, values, size_t{count} * sizeof(u32));
    jtail_ += words;
    t1_.journal_words += words;
}

void ParallelCp::OnReset() {
    JournalReserve(1);
    *JournalAt(jtail_) = u32{kOpReset} << 28;
    jtail_ += 1;
    t1_.journal_words += 1;
}

void ParallelCp::OnLoad(std::span<const u32> values) {
    const u32 words = 1 + static_cast<u32>(values.size());
    JournalReserve(words);
    u32* w = JournalAt(jtail_);
    w[0] = (u32{kOpLoad} << 28) | static_cast<u32>(values.size());
    std::memcpy(w + 1, values.data(), values.size_bytes());
    jtail_ += words;
    t1_.journal_words += words;
}

// ---- stage 2: the frontend on thread 1

void ParallelCp::AddGpuWrite(u64 begin, u64 end, u64 record, u64 value) {
    if (end <= begin) {
        return;
    }
    gw_.push_back({begin, end, record, value});
    Bump(fc_.gpu_writes);
    if (frontend_ != nullptr) {
        frontend_->GpuWrote(begin, end - begin); // (P5.2: trusted shader code under a GPU write is read again)
    }
    if (gw_.size() > fc_.gw_max.load(std::memory_order_relaxed)) {
        fc_.gw_max.store(gw_.size(), std::memory_order_relaxed);
    }
    const u64 b0 = begin >> kGwBlockBits, b1 = std::min((end - 1) >> kGwBlockBits, kGwWords * 64 - 1);
    for (u64 b = b0; b <= b1; ++b) {
        u64& word = gw_bits_[b >> 6];
        if (word == 0) {
            gw_dirty_.push_back(static_cast<u32>(b >> 6));
        }
        word |= u64{1} << (b & 63);
    }
}

void ParallelCp::RetireGpuWrites() {
    if (gw_.empty()) {
        return;
    }
    const u64 head = head_.load(std::memory_order_acquire);
    u64 completed = 0;
    bool queried = false;
    while (!gw_.empty()) {
        const GpuWrite& w = gw_.front();
        u64 v = w.value;
        if (v == kLookup) {
            if (w.record >= head || w.record + kRecRing <= head) {
                break; // not executed yet / its value is gone (the executor decides at an idle point)
            }
            v = rec_value_[w.record & (kRecRing - 1)].load(std::memory_order_relaxed);
        }
        if (!queried) {
            completed = timeline_->QueryCompleted();
            queried = true;
        }
        if (v > completed) {
            break;
        }
        gw_.pop_front();
        gw_stale_ = true;
    }
}

void ParallelCp::RebuildGpuWriteBits() {
    for (const u32 w : gw_dirty_) {
        gw_bits_[w] = 0;
    }
    gw_dirty_.clear();
    for (const GpuWrite& w : gw_) {
        const u64 b0 = w.begin >> kGwBlockBits, b1 = std::min((w.end - 1) >> kGwBlockBits, kGwWords * 64 - 1);
        for (u64 b = b0; b <= b1; ++b) {
            u64& word = gw_bits_[b >> 6];
            if (word == 0) {
                gw_dirty_.push_back(static_cast<u32>(b >> 6));
            }
            word |= u64{1} << (b & 63);
        }
    }
    gw_stale_ = false;
    Bump(fc_.gw_rebuilds);
}

bool ParallelCp::MayBeGpuWritten(u64 addr, u64 bytes) {
    if (gw_.empty() || bytes == 0 || addr >= (1ull << 40)) {
        return false;
    }
    const u64 b0 = addr >> kGwBlockBits, b1 = std::min((addr + bytes - 1) >> kGwBlockBits, kGwWords * 64 - 1);
    const auto any_bit = [&] {
        for (u64 b = b0; b <= b1; ++b) {
            if ((gw_bits_[b >> 6] >> (b & 63)) & 1) {
                return true;
            }
        }
        return false;
    };
    if (!any_bit()) {
        return false;
    }
    Bump(fc_.gw_block_hits);
    RetireGpuWrites();
    if (gw_stale_) {
        RebuildGpuWriteBits();
        if (!any_bit()) {
            return false;
        }
    }
    return std::ranges::any_of(gw_, [&](const GpuWrite& w) { return addr < w.end && w.begin < addr + bytes; });
}

void ParallelCp::NoteStageWrites(const GS::StageView& s, u64 record, bool& writes) {
    for (const GS::BufferBinding& b : s.plan.buffers) {
        if (b.kind == GS::BufferKind::Guest) {
            if (b.written && b.status == GS::ResourceStatus::Ok && b.size != 0 && b.address != 0) {
                // (the buffer cache keeps its GPU writes per 4 KiB page: so does this, its superset)
                AddGpuWrite(b.address & ~(kPage - 1), (b.address + b.size + kPage - 1) & ~(kPage - 1), record, kLookup);
                writes = true;
            }
        } else if (b.written || (b.kind != GS::BufferKind::Flatbuf && b.kind != GS::BufferKind::ClipPlanes)) {
            writes = true; // GDS, the emulated LDS, BDA: what the executor counts as a write (of unknown extent)
        }
    }
    for (const GS::ImageBinding& im : s.plan.images) {
        writes = writes || im.storage;
    }
}

bool ParallelCp::PrepareGeometryOnly(const Record& r, u64 index, Record& out) {
    if (index == 0 || geofast_prev_ != index - 1 || r.memory_changed ||
        frontend_->MemoryGeneration() != geofast_gen_) {
        return false;
    }
    const Record& p = slots_[(index - 1) & (kSlots - 1)];
    if (p.kind != Kind::Draw || !p.prepared || !p.prep.ready || p.state == nullptr || p.stage_shared != nullptr ||
        p.ps_shared != nullptr) {
        return false;
    }
    const Cp::DrawDescriptor& a = p.draw;
    const Cp::DrawDescriptor& b = r.draw;
    using G = Cp::Group;
    constexpr Cp::GroupMask kGeometryGroups = (Cp::GroupMask{1} << static_cast<u32>(G::VsUserData)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::IndexState)) |
                                              (Cp::GroupMask{1} << static_cast<u32>(G::DrawParams));
    constexpr u8 kVsPs = (1u << static_cast<u32>(Cp::Stage::Vs)) | (1u << static_cast<u32>(Cp::Stage::Ps));
    const size_t vs = static_cast<size_t>(Cp::Stage::Vs), ps = static_cast<size_t>(Cp::Stage::Ps);
    if ((b.changed & ~kGeometryGroups) != 0 || a.active_stages != kVsPs || b.active_stages != kVsPs || b.indirect ||
        a.indirect || a.user_data[ps] != b.user_data[ps] || a.shaders[vs].address != b.shaders[vs].address ||
        a.shaders[vs].rsrc1 != b.shaders[vs].rsrc1 || a.shaders[vs].rsrc2 != b.shaders[vs].rsrc2 ||
        a.shaders[ps].address != b.shaders[ps].address || a.shaders[ps].rsrc1 != b.shaders[ps].rsrc1 ||
        a.shaders[ps].rsrc2 != b.shaders[ps].rsrc2) {
        return false;
    }
    const GS::StageView& pv = p.stage;
    if (pv.status != GS::StageStatus::Ready || pv.module == nullptr || pv.plan.flat.size() != 16 ||
        pv.plan.vertices.empty()) {
        return false; // (copies of an SRT walk: they may follow a pointer that changed)
    }
    const GS::FetchShader& fetch = pv.module->result.reflection.fetch;
    if (!fetch.present || fetch.sgpr_base + 1 >= 16) {
        return false;
    }
    u32 fetch_only = 3u << fetch.sgpr_base;
    for (const GS::FetchAttribute& at : fetch.attributes) {
        fetch_only |= at.sgpr_base + 1 < 16 ? 3u << at.sgpr_base : 0u;
    }
    fetch_only &= ~(3u << fetch.sgpr_base); // (the fetch shader's own address must be the same)
    for (u32 i = 0; i < 16; ++i) {
        if (a.user_data[vs][i] != b.user_data[vs][i] && ((fetch_only >> i) & 1) == 0) {
            return false;
        }
    }
    // nothing the previous draw prepared writes memory (its notes would be due again)
    for (const GS::StageView* s : {&pv, &p.ps}) {
        for (const GS::BufferBinding& bb : s->plan.buffers) {
            if (bb.written || (bb.kind != GS::BufferKind::Guest && bb.kind != GS::BufferKind::Flatbuf &&
                               bb.kind != GS::BufferKind::ClipPlanes)) {
                return false;
            }
        }
        for (const GS::ImageBinding& im : s->plan.images) {
            if (im.storage) {
                return false;
            }
        }
    }
    // the vertex bindings from this draw's table (shader_resources.cpp DecodeResources: the same reads and results)
    std::vector<GS::VertexBinding>& nv = geofast_vertices_;
    nv.assign(pv.plan.vertices.begin(), pv.plan.vertices.end());
    for (GS::VertexBinding& v : nv) {
        const GS::FetchAttribute& at = v.fetch;
        if (v.status != GS::ResourceStatus::Ok || at.sgpr_base + 1 >= 16) {
            return false;
        }
        const u64 table = (b.user_data[vs][at.sgpr_base] | (u64{b.user_data[vs][at.sgpr_base + 1]} << 32)) &
                          0xFFFFFFFFFFFFull;
        const u64 where = table + u64{at.dword_offset} * 4;
        GS::BufferDesc d;
        if (where >= (1ull << 40) || !reader_.Read(where, d.raw.data(), 16)) {
            return false;
        }
        v.read = d.raw;
        if (at.data_format != 0) {
            d.raw[3] = (d.raw[3] & ~((0xFu << 15) | (7u << 12))) | (at.data_format << 15) | (at.num_format << 12);
        }
        const u64 addr = d.Base() + at.inst_offset, size = d.Size();
        if (!d.IsBuffer() || d.NumRecords() == 0 || addr >= (1ull << 40) || size > (1ull << 40) - addr ||
            (d.raw[3] & 0x7FFFFu) != (v.desc.raw[3] & 0x7FFFFu)) {
            return false; // (not Ok, or another specialization of the module)
        }
        v.desc = d;
        v.address = addr;
        v.size = size;
    }
    out.prep = p.prep;
    out.state = p.state;
    CopyStage(out.stage, pv);
    out.stage.plan.vertices.assign(nv.begin(), nv.end());
    std::copy(b.user_data[vs].begin(), b.user_data[vs].end(), out.stage.plan.flat.begin());
    CopyStage(out.ps, p.ps);
    return true;
}

void ParallelCp::PrepareDraw(Record& r, u64 index, const Cp::RegisterFile& regs) {
    r.prepared = false;
    r.vs_same = r.ps_same = false;
    r.stage_shared.reset();
    r.ps_shared.reset();
    r.state = nullptr;
    if (Vk::Executor::DrawFiltered(regs, r.draw)) {
        return; // no preparation: thread 2 sees the same registers and skips it as filtered
    }
    const u64 t0 = __rdtsc();
    if (geofast_ == 1 && PrepareGeometryOnly(r, index, r)) {
        r.prepared = true;
        geofast_prev_ = index;
        geofast_hits_.store(geofast_hits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        Bump(fc_.ticks, __rdtsc() - t0);
        return;
    }
    const bool check = geofast_ == 2 && PrepareGeometryOnly(r, index, *geofast_check_);
    {
        const GS::FineSample fine(front_fine_counter_);
        const GS::DrawPreparation& prep = frontend_->PrepareDraw(r.draw, regs, reader_);
        r.prep = prep;
        // P5.3: a preparation the native cache shares is kept by reference (immutable), not copied
        r.stage_shared = frontend_->SharedStage(GS::HwStage::Vs);
        r.ps_shared = frontend_->SharedStage(GS::HwStage::Ps);
        // FB-6: a view the frontend did not change since the last copy is not copied again (stage_reuse_)
        const auto copy = [&](GS::HwStage hw, GS::StageView& dst, bool shared, u64& copied, bool& same) {
            same = false;
            if (shared) {
                copied = kNoGen; // (thread 2 does not see this one: the next own view is copied)
                return;
            }
            const u64 gen = frontend_->StageGeneration(hw);
            same = stage_reuse_ != 0 && gen == copied;
            if (!same || stage_reuse_ == 2) {
                CopyStage(dst, frontend_->Stage(hw));
            }
            copied = gen;
        };
        copy(GS::HwStage::Vs, r.stage, r.stage_shared != nullptr, copied_gen_vs_, r.vs_same);
        copy(GS::HwStage::Ps, r.ps, r.ps_shared != nullptr, copied_gen_ps_, r.ps_same);
        r.state = prep.ready ? frontend_->Pipelines().Graphics(prep.pipeline) : nullptr;
    }
    r.prepared = true;
    bool writes = false;
    // (a view not copied is the frontend's current one)
    NoteStageWrites(r.vs_same ? frontend_->Stage(GS::HwStage::Vs) : r.Vs(), index, writes);
    NoteStageWrites(r.ps_same ? frontend_->Stage(GS::HwStage::Ps) : r.Ps(), index, writes);
    if (writes) {
        frontend_->GuestMemoryChanged(); // (Executor::MarkWrites after the draw: no memoized preparation past it)
    }
    if (check) {
        // SSPC_CITY_GEOFAST=verify: the fast preparation against the full one, field by field (the bindings' padding
        // bytes differ between two builds of the same values)
        const auto same_view = [](const GS::StageView& x, const GS::StageView& y) {
            const auto bufs = [](const GS::BufferBinding& a, const GS::BufferBinding& b) {
                return a.binding == b.binding && a.kind == b.kind && a.desc.raw == b.desc.raw && a.address == b.address &&
                       a.size == b.size && a.written == b.written && a.formatted == b.formatted && a.status == b.status;
            };
            const auto imgs = [](const GS::ImageBinding& a, const GS::ImageBinding& b) {
                return a.binding == b.binding && a.count == b.count && a.desc.raw == b.desc.raw &&
                       a.view_type == b.view_type && a.storage == b.storage && a.depth == b.depth && a.array == b.array &&
                       a.atomic == b.atomic && a.fmask == b.fmask && a.status == b.status;
            };
            const auto smps = [](const GS::SamplerBinding& a, const GS::SamplerBinding& b) {
                return a.binding == b.binding && a.desc.raw == b.desc.raw && a.status == b.status;
            };
            const auto vtxs = [](const GS::VertexBinding& a, const GS::VertexBinding& b) {
                return a.attribute == b.attribute && a.fetch == b.fetch && a.read == b.read && a.desc.raw == b.desc.raw &&
                       a.address == b.address && a.size == b.size && a.status == b.status;
            };
            return x.status == y.status && x.address == y.address && x.program == y.program &&
                   x.footer_hash == y.footer_hash && x.module == y.module &&
                   std::memcmp(&x.runtime, &y.runtime, sizeof(x.runtime)) == 0 && x.start == y.start &&
                   x.end == y.end && x.program_error == y.program_error && x.plan.flat == y.plan.flat &&
                   std::ranges::equal(x.plan.buffers, y.plan.buffers, bufs) &&
                   std::ranges::equal(x.plan.images, y.plan.images, imgs) &&
                   std::ranges::equal(x.plan.samplers, y.plan.samplers, smps) &&
                   std::ranges::equal(x.plan.vertices, y.plan.vertices, vtxs) &&
                   x.plan.status_counts == y.plan.status_counts && x.plan.error == y.plan.error;
        };
        const bool same = r.prep.ready && r.prep.pipeline == geofast_check_->prep.pipeline &&
                          r.state == geofast_check_->state && same_view(r.Vs(), geofast_check_->stage) &&
                          same_view(r.Ps(), geofast_check_->ps);
        geofast_verified_.store(geofast_verified_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (!same) {
            const u64 n = geofast_mismatches_.load(std::memory_order_relaxed) + 1;
            geofast_mismatches_.store(n, std::memory_order_relaxed);
            if (n <= 12) {
                const auto what = [](const GS::StageView& x, const GS::StageView& y) {
                    std::string t;
                    t += x.status != y.status ? " status" : "";
                    t += x.address != y.address || x.program != y.program ? " program" : "";
                    t += x.footer_hash != y.footer_hash ? " footer" : "";
                    t += x.module != y.module ? " module" : "";
                    t += std::memcmp(&x.runtime, &y.runtime, sizeof(x.runtime)) != 0 ? " runtime" : "";
                    t += !(x.start == y.start) || !(x.end == y.end) ? " start/end" : "";
                    t += !SameBytes(x.plan.flat, y.plan.flat) ? " flat" : "";
                    if (!SameBytes(x.plan.buffers, y.plan.buffers)) {
                        t += fmt::format(" buffers({}/{}", x.plan.buffers.size(), y.plan.buffers.size());
                        for (size_t i = 0; i < std::min(x.plan.buffers.size(), y.plan.buffers.size()); ++i) {
                            const auto& a = x.plan.buffers[i];
                            const auto& b = y.plan.buffers[i];
                            t += fmt::format(" {}:k{}{}{}{}{}", i, static_cast<int>(a.kind),
                                             a.address != b.address ? fmt::format("a{:x}/{:x}", a.address, b.address) : "",
                                             a.size != b.size ? "s" : "", a.desc.raw != b.desc.raw ? "d" : "",
                                             a.status != b.status ? "t" : "");
                        }
                        t += ")";
                    }
                    t += !SameBytes(x.plan.images, y.plan.images) ? " images" : "";
                    t += !SameBytes(x.plan.samplers, y.plan.samplers) ? " samplers" : "";
                    t += x.plan.status_counts != y.plan.status_counts ? " counts" : "";
                    if (!SameBytes(x.plan.vertices, y.plan.vertices)) {
                        t += fmt::format(" vertices({}/{}", x.plan.vertices.size(), y.plan.vertices.size());
                        for (size_t i = 0; i < std::min(x.plan.vertices.size(), y.plan.vertices.size()); ++i) {
                            const auto& a = x.plan.vertices[i];
                            const auto& b = y.plan.vertices[i];
                            t += fmt::format(" {}:{}{}{}{}{}", i, a.address != b.address ? "a" : "", a.size != b.size ? "s" : "",
                                             a.desc.raw != b.desc.raw ? "d" : "", a.read != b.read ? "r" : "",
                                             a.status != b.status ? "t" : "");
                        }
                        t += ")";
                    }
                    t += x.plan.error != y.plan.error ? " error" : "";
                    return t;
                };
                SSPC_LOG(GPU, Warning, "SSPC_CITY_GEOFAST=verify: record {} (VS {:#x}) differs: pipeline {}/{} state {} | "
                                       "VS:{} | PS:{}", index,
                         r.draw.shaders[static_cast<size_t>(Cp::Stage::Vs)].address, r.prep.pipeline,
                         geofast_check_->prep.pipeline, r.state == geofast_check_->state, what(r.Vs(), geofast_check_->stage),
                         what(r.Ps(), geofast_check_->ps));
            }
        }
    }
    geofast_prev_ = index;
    geofast_gen_ = frontend_->MemoryGeneration();
    Bump(fc_.ticks, __rdtsc() - t0);
}

void ParallelCp::PrepareDispatch(Record& r, u64 index) {
    r.prepared = false;
    r.stage_shared.reset(); // (a compute stage is never shared: Execute takes r.stage)
    r.ps_shared.reset();
    r.state = nullptr;
    if (!r.has_cs) {
        return;
    }
    const u64 t0 = __rdtsc();
    {
        const GS::FineSample fine(front_fine_counter_);
        r.prep = frontend_->PrepareDispatch(r.dispatch, std::span<const u32, Cp::kComputeCount>(r.cs.values), reader_);
        const u64 tc = GS::FineTicks();
        CopyStage(r.stage, frontend_->Stage(GS::HwStage::Cs));
        if (tc != 0) {
            frontend_->NoteDispatchCopy(GS::TicksToNs(__rdtsc() - tc)); // (CC-1: the copy's share)
        }
    }
    r.prepared = true;
    bool writes = false;
    NoteStageWrites(r.stage, index, writes);
    if (writes) {
        frontend_->GuestMemoryChanged(); // (Executor::NoteAccesses after the dispatch)
    }
    Bump(fc_.ticks, __rdtsc() - t0);
}

bool ParallelCp::FrontReader::Read(u64 addr, void* dst, u64 bytes) {
    Bump(p_.fc_.reads);
    if (p_.fthread_mode_) {
        // The frontend thread prepares record k: thread 2 runs only the records before it. With every one of them
        // executed, thread 2 is parked on k and thread 1 does not use the executor (k is queued, the queue not empty):
        // the executor decides exactly, as at thread 1's idle point. A possible pending GPU write otherwise: wait for
        // thread 2 to get there first (not a drain of thread 1's later records).
        const u64 k = p_.f_current_;
        bool ask = p_.head_.load(std::memory_order_acquire) == k;
        if (!ask && (p_.MayBeGpuWritten(addr, bytes) || p_.exec_.KeptBack(addr, bytes))) { // (FB-7: kept back)
            Bump(p_.fc_.read_hits);
            p_.FrontWaitExecuted(k);
            ask = true;
        }
        if (ask) {
            p_.DropUnknownGpuWrites();
            Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0);
        }
    } else if (p_.Idle()) {
        p_.ApplyPending();
        Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0); // (exact: TimelineReader's check)
    } else if (p_.MayBeGpuWritten(addr, bytes) || p_.exec_.KeptBack(addr, bytes)) { // (FB-7: kept back)
        Bump(p_.fc_.read_hits);
        p_.Drain(DrainReason::FrontendRead);
        Bump(p_.fc_.syncs, p_.exec_.SyncForFrontendRead(addr, bytes) ? 1 : 0);
    }
    // (TimelineReader's read: a run of the physical backing read before - its host view, what CopySparse copies)
    if (p_.map_gen_ != nullptr && GuestMemory::IsValidGpuMapping(addr, bytes)) {
        const u64 gen = p_.map_gen_->load(std::memory_order_acquire);
        for (const Run& r : runs_) {
            if (r.gen == gen && r.host != nullptr && addr >= r.begin && addr + bytes <= r.end) {
                std::memcpy(dst, r.host + (addr - r.begin), bytes);
                return true;
            }
        }
        const u64 block = (addr >> 20) + 1;
        const bool known_none =
            std::ranges::any_of(no_run_, [&](const auto& n) { return n.first == block && n.second == gen; });
        if (GuestMemory* gm = GetGuestMemory(); gm != nullptr && !known_none) {
            u8* host = nullptr;
            u64 rb = 0, rs = 0;
            if (gm->BackingRun(addr, &host, &rb, &rs) && host != nullptr && addr >= rb && addr + bytes <= rb + rs) {
                runs_[next_run_++ % runs_.size()] = {rb, rb + rs, host, gen};
                std::memcpy(dst, host + (addr - rb), bytes);
                return true;
            }
            no_run_[next_no_run_++ % no_run_.size()] = {block, gen};
        }
    }
    return GS::GameMemory().Read(addr, dst, bytes);
}

// ---- either thread (never both at once: thread 1 only while the queue is drained)

void ParallelCp::ApplyJournal(u64 end) {
    u64 pos = jhead_.load(std::memory_order_relaxed);
    while (pos < end) {
        const u32* w = JournalAt(pos);
        const u32 op = w[0] >> 28;
        if (op == kOpPad) {
            pos = (pos + kJournalWords) & ~u64{kJournalWords - 1};
            continue;
        }
        if (op == kOpWrite) {
            const u32 count = w[0] & 0xFFFFFF;
            replica_.Write(static_cast<Cp::Space>((w[0] >> 24) & 0xF), w[1], w + 2, count);
            pos += 2 + count;
        } else if (op == kOpReset) {
            replica_.Reset();
            pos += 1;
        } else if (op == kOpLoad && (w[0] & 0xFFFFFF) == Cp::kNumRegs) {
            replica_.Load(std::span<const u32, Cp::kNumRegs>(w + 1, Cp::kNumRegs));
            pos += 1 + Cp::kNumRegs;
        } else {
            // never written: a broken journal would make every later draw read wrong registers - stop executing
            Fail(fmt::format("parallel command processor: corrupt register journal at {} (word {:#x})", pos, w[0]));
            pos = end;
        }
    }
    jhead_.store(pos, std::memory_order_release);
}

void ParallelCp::ApplyDeferred(u64 before) {
    std::scoped_lock lock{deferred_mutex_};
    size_t n = 0;
    while (n < deferred_.size() && deferred_[n].before <= before) {
        if (buffers_ != nullptr) {
            buffers_->InvalidateIfCached(deferred_[n].addr, deferred_[n].bytes);
        }
        ++n;
    }
    deferred_.erase(deferred_.begin(), deferred_.begin() + static_cast<std::ptrdiff_t>(n));
    has_deferred_.store(!deferred_.empty(), std::memory_order_release);
}

// ---- P5.3D: the frontend thread (between thread 1 and thread 2)

void ParallelCp::FrontApplyJournal(u64 end) {
    u64 pos = fjhead_.load(std::memory_order_relaxed);
    while (pos < end) {
        const u32* w = JournalAt(pos);
        const u32 op = w[0] >> 28;
        if (op == kOpPad) {
            pos = (pos + kJournalWords) & ~u64{kJournalWords - 1};
            continue;
        }
        if (op == kOpWrite) {
            const u32 count = w[0] & 0xFFFFFF;
            freplica_.Write(static_cast<Cp::Space>((w[0] >> 24) & 0xF), w[1], w + 2, count);
            pos += 2 + count;
        } else if (op == kOpReset) {
            freplica_.Reset();
            pos += 1;
        } else if (op == kOpLoad && (w[0] & 0xFFFFFF) == Cp::kNumRegs) {
            freplica_.Load(std::span<const u32, Cp::kNumRegs>(w + 1, Cp::kNumRegs));
            pos += 1 + Cp::kNumRegs;
        } else {
            Fail(fmt::format("parallel command processor: corrupt register journal at {} (word {:#x}, frontend thread)",
                             pos, w[0]));
            pos = end;
        }
    }
    fjhead_.store(pos, std::memory_order_release);
}

void ParallelCp::FrontWaitExecuted(u64 index) {
    u64 h = head_.load(std::memory_order_acquire);
    if (h >= index) {
        return;
    }
    const u64 t0 = __rdtsc();
    while (h < index && __rdtsc() - t0 < spin_ticks_) {
        _mm_pause();
        h = head_.load(std::memory_order_acquire);
    }
    while (h < index) {
        f_wait_target_.store(index, std::memory_order_seq_cst);
        h = head_.load(std::memory_order_seq_cst);
        if (h < index) {
            head_.wait(h, std::memory_order_seq_cst);
        }
        f_wait_target_.store(0, std::memory_order_relaxed);
        h = head_.load(std::memory_order_acquire);
    }
    Bump(fc_.waits);
    Bump(fc_.wait_ticks, __rdtsc() - t0);
}

void ParallelCp::DropUnknownGpuWrites() {
    // (thread 2 parked before f_current_: the executor may be asked)
    while (!gw_.empty() && gw_.front().value == kLookup && gw_.front().record + kRecRing <= f_current_ &&
           !exec_.FrontendReadPending(gw_.front().begin, gw_.front().end - gw_.front().begin)) {
        gw_.pop_front();
        gw_stale_ = true;
    }
}

void ParallelCp::FrontEvent(u64 addr, u64 size, bool gpu) {
    if (!fthread_mode_ || !started_ || Idle()) {
        // the frontend thread (if any) parked: at once, after what is still queued for it (stream order)
        ApplyFrontEvents(~0ull);
        if (gpu) {
            frontend_->GpuWrote(addr, size);
        } else {
            frontend_->Invalidate(addr, size);
        }
        return;
    }
    std::scoped_lock lock{fe_mutex_};
    fe_.push_back({next_, addr, size, gpu});
    has_fe_.store(true, std::memory_order_release);
}

void ParallelCp::ApplyFrontEvents(u64 upto) {
    if (!has_fe_.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{fe_mutex_};
    size_t n = 0;
    while (n < fe_.size() && fe_[n].before <= upto) {
        const FrontEventEntry& e = fe_[n];
        if (e.gpu) {
            frontend_->GpuWrote(e.addr, e.size);
        } else {
            frontend_->Invalidate(e.addr, e.size);
        }
        ++n;
    }
    fe_.erase(fe_.begin(), fe_.begin() + static_cast<std::ptrdiff_t>(n));
    has_fe_.store(!fe_.empty(), std::memory_order_release);
}

void ParallelCp::FrontStep(Record& r, u64 index) {
    f_current_ = index;
    ApplyFrontEvents(index); // what thread 1 told the frontend before this record
    if (r.front_memo) {
        frontend_->GuestMemoryChanged(); // (a change thread 1 met before this record: stream order)
    }
    FrontApplyJournal(r.journal_end); // the registers as they were at the record's packet
    switch (r.kind) {
    case Kind::Draw:
        PrepareDraw(r, index, freplica_);
        break;
    case Kind::Dispatch:
        PrepareDispatch(r, index);
        break;
    case Kind::Copy:
        AddGpuWrite(r.dst, r.dst + r.bytes, index, kLookup); // (until thread 2 recorded it: the frontend's reads wait)
        break;
    default:
        break;
    }
    if ((index & 63) == 0) {
        RetireGpuWrites();
    }
    fc_.gw_now.store(gw_.size(), std::memory_order_relaxed);
}

void ParallelCp::FrontLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuFrontend");
    u64 k = fhead_.load(std::memory_order_relaxed);
    for (;;) {
        u64 t = tail_.load(std::memory_order_acquire);
        if (t == k) {
            const u64 w0 = __rdtsc();
            while (t == k && __rdtsc() - w0 < spin_ticks_) {
                _mm_pause();
                t = tail_.load(std::memory_order_acquire);
            }
            while (t == k) {
                f_waiting_.store(1, std::memory_order_seq_cst);
                t = tail_.load(std::memory_order_seq_cst);
                if (t == k) {
                    tail_.wait(k, std::memory_order_seq_cst);
                }
                f_waiting_.store(0, std::memory_order_relaxed);
                t = tail_.load(std::memory_order_acquire);
            }
            Bump(fc_.idle, __rdtsc() - w0);
        }
        const u64 b0 = __rdtsc();
        Record& r = slots_[k & (kSlots - 1)];
        const bool stop = r.kind == Kind::Stop;
        if (!stop && !failed_.load(std::memory_order_relaxed)) {
            try {
                FrontStep(r, k);
            } catch (const std::exception& e) {
                r.prepared = false;
                Fail(fmt::format("parallel command processor: the frontend thread threw: {}", e.what()));
            } catch (...) {
                r.prepared = false;
                Fail("parallel command processor: the frontend thread threw");
            }
        }
        Bump(fc_.busy, __rdtsc() - b0);
        ++k;
        fhead_.store(k, std::memory_order_seq_cst); // (the record's preparation published to thread 2)
        if (t2_waiting_.load(std::memory_order_seq_cst) != 0) {
            fhead_.notify_one();
        }
        if (stop) {
            break;
        }
    }
}

// ---- thread 2

void ParallelCp::Loop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuRecord");
    StallTrace::RegisterThread(StallTrace::Role::Record);
    const bool trace_records = StallTrace::DeepRecords();
    u64 h = head_.load(std::memory_order_relaxed);
    const auto add = [](std::atomic<u64>& a, u64 v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); };
    // (with the frontend thread, the records it prepared: fhead_)
    std::atomic<u64>& source = fthread_mode_ ? fhead_ : tail_;
    // FB-7 (SSPC_FB_LAZY_WB): another thread's request for kept-back blocks is served after a record, before it is
    // published as executed (thread 1 uses the executor only once every record is: never both at once); with the
    // queue empty, thread 1 serves them (Backend::Loop)
    const bool lazy = exec_.Lazy();
    if (lazy) {
        exec_.SetRecordThread();
    }
    for (;;) {
        u64 t = source.load(std::memory_order_acquire);
        if (t == h) {
            const u64 w0 = __rdtsc();
            while (t == h && __rdtsc() - w0 < spin_ticks_) {
                _mm_pause();
                t = source.load(std::memory_order_acquire);
            }
            while (t == h) {
                t2_waiting_.store(1, std::memory_order_seq_cst);
                t = source.load(std::memory_order_seq_cst);
                if (t == h) {
                    add(t2_sleeps_, 1);
                    source.wait(h, std::memory_order_seq_cst);
                }
                t2_waiting_.store(0, std::memory_order_relaxed);
                t = source.load(std::memory_order_acquire);
            }
            const u64 w1 = __rdtsc();
            add(t2_idle_, w1 - w0);
            // P4.12: waited for record h - thread 1 pushed it at push_tsc_[h] (deep), the wake-up is the rest
            StallTrace::Span(StallTrace::Ev::RecordWait, w0, w1, push_tsc_ != nullptr ? push_tsc_[h & (kRecRing - 1)] : 0,
                             static_cast<u32>(h));
        }
        const u64 b0 = __rdtsc();
        Record& r = slots_[h & (kSlots - 1)];
        const bool stop = r.kind == Kind::Stop;
        const u16 kind = static_cast<u16>(r.kind); // (before Execute: a record is not touched by thread 1 until head_ moves)
        const u64 queue = r.kind == Kind::Dispatch ? r.dispatch.queue : 0; // (P5.4: the guest queue in the trace)
        u64 extra = 0;
        if (!stop) {
            extra = Execute(r, h, t);
            if (lazy) {
                exec_.ServeKeptBack();
            }
            if (rec_value_ != nullptr) {
                for (u64 k = 0; k <= extra; ++k) {
                    rec_value_[(h + k) & (kRecRing - 1)].store(exec_.RecordedValue(), std::memory_order_relaxed);
                }
            }
        }
        const u64 b1 = __rdtsc();
        add(t2_busy_, b1 - b0);
        if (trace_records) {
            StallTrace::Span(StallTrace::Ev::Record, b0, b1, queue, static_cast<u32>(h), kind);
        }
        add(t2_records_, 1 + extra);
        h += 1 + extra;
        head_.store(h, std::memory_order_seq_cst);
        const u64 wt = t1_wait_target_.load(std::memory_order_seq_cst);
        const u64 ft = f_wait_target_.load(std::memory_order_seq_cst);
        if ((wt != 0 && h >= wt) || (ft != 0 && h >= ft)) {
            head_.notify_all(); // (thread 1 and the frontend thread may both wait on it)
        }
        if (stop) {
            break;
        }
    }
}

void ParallelCp::RunWrite(const Record& r) {
    if (exec_.WriteOnTimeline(r.queue, r.seq, r.dst, r.payload.data(), r.bytes)) {
        return;
    }
    // refused when it ran (an import): what the processor does after a refusal, here in stream order
    exec_.Synchronize(r.queue);
    if (write_fallback_ == nullptr || !write_fallback_(write_ctx_, r.dst, r.payload.data(), r.bytes)) {
        SSPC_LOG(GPU, Error, "parallel command processor: a queued timeline write of {} bytes at {:#x} could be neither "
                             "recorded nor written by the CPU (its work completes)", r.bytes, r.dst);
    } else {
        exec_.GuestMemoryChanged();
    }
    write_fallbacks_.store(write_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    exec_.CompleteRefusedWork(r.queue, r.seq);
}

void ParallelCp::RunGds(const Record& r) {
    if (exec_.GdsOnTimeline(r.queue, r.seq, r.gds_kind, r.src, r.gds_offset, static_cast<u32>(r.bytes), r.pattern)) {
        return;
    }
    exec_.Synchronize(r.queue);
    if (gds_fallback_ == nullptr ||
        !gds_fallback_(write_ctx_, r.gds_kind, r.src, r.gds_offset, static_cast<u32>(r.bytes), r.pattern)) {
        SSPC_LOG(GPU, Error, "parallel command processor: a queued GDS DMA ({} bytes, GDS +{:#x}, memory {:#x}) could be "
                             "neither recorded nor done by the CPU (its work completes)", r.bytes, r.gds_offset, r.src);
    } else {
        exec_.GuestMemoryChanged();
    }
    write_fallbacks_.store(write_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    exec_.CompleteRefusedWork(r.queue, r.seq);
}

void ParallelCp::RunCopy(const Record& r) {
    const u32 every = ForcedCopyFallbackEvery();
    const bool forced = every != 0 && ++forced_fallback_n_ % every == 0;
    if (!forced && exec_.CopyOnTimeline(r.queue, r.seq, r.src, r.dst, r.bytes)) {
        return;
    }
    // the executor refused what thread 1 accepted by its shape: the processor's own fallback, here in stream order -
    // the earlier work completes, the CPU copies (caches invalidated), the copy's work number completes with the next
    // batch (its fences wait for it as for a recorded copy)
    const StallTrace::Scope trace_copy(StallTrace::Ev::CopyFallback, r.bytes);
    exec_.Synchronize(r.queue);
    if (copy_fallback_ == nullptr || !copy_fallback_(copy_ctx_, r.src, r.dst, r.bytes)) {
        // P4.11: what the processor does with a DMA it cannot copy (a failed submission: an error, the game goes on) -
        // here the copy is reported and its work number completed (its fences are not held forever); the rest of the
        // stream was processed already. Before: the whole backend stopped (exit code 5), a stricter end than the one
        // thread's.
        const u64 n = copy_failures_.load(std::memory_order_relaxed) + 1;
        copy_failures_.store(n, std::memory_order_relaxed);
        if (n <= 8) {
            SSPC_LOG(GPU, Error, "parallel command processor: a queued timeline copy {:#x} -> {:#x} ({} bytes) could be "
                                 "neither recorded nor copied by the CPU (not copied; its work completes)",
                     r.src, r.dst, r.bytes);
        }
    } else {
        exec_.GuestMemoryChanged();
        copy_fallbacks_.store(copy_fallbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    exec_.CompleteRefusedWork(r.queue, r.seq);
}

u64 ParallelCp::Execute(Record& r, u64 index, u64 avail) {
    u64 extra = 0;
    if (failed_.load(std::memory_order_relaxed)) {
        return 0; // consumed, not executed: thread 1's drains still complete; the backend stops at its next check
    }
    try {
        // (stage 2: thread 1 tells the frontend about guest memory changes in stream order; nothing from here)
        exec_.SetMemoBumps(frontend_ == nullptr);
        ApplyJournal(r.journal_end);
        if (verify_ && (r.kind == Kind::Draw || r.kind == Kind::Dispatch)) {
            verified_.store(verified_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (RegistersHash(replica_) != r.reg_hash) {
                verify_mismatches_.store(verify_mismatches_.load(std::memory_order_relaxed) + 1,
                                         std::memory_order_relaxed);
            }
        }
        if (has_deferred_.load(std::memory_order_acquire)) {
            ApplyDeferred(index); // the processor's writes made before this record was pushed
        }
        if (r.memory_changed) {
            exec_.GuestMemoryChanged();
        }
        switch (r.kind) {
        case Kind::Draw:
            if (frontend_ != nullptr) {
                Vk::PreparedCommand pc;
                if (r.prepared) {
                    pc = {&r.prep, &ReuseView(r.stage, r.stage_shared != nullptr, r.vs_same, last_vs_),
                          &ReuseView(r.ps, r.ps_shared != nullptr, r.ps_same, last_ps_), r.state};
                    if (r.stage_shared != nullptr) {
                        pc.stage = &r.Vs();
                    }
                    if (r.ps_shared != nullptr) {
                        pc.ps = &r.Ps();
                    }
                }
                if (r.prepared && batch_on_ && stage_reuse_ == 0 &&
                    (batch_ab_ == 0 || (VideoOut::NextFlipId() / batch_ab_) % 2 == 1) &&
                    !has_deferred_.load(std::memory_order_acquire)) {
                    // the published draws right after it that differ only in their geometry (each from the one
                    // before it: its changed register groups are since that one)
                    constexpr size_t kMaxBatch = 256;
                    batch_items_.clear();
                    batch_items_.push_back({&r.draw, pc});
                    while (batch_items_.size() < kMaxBatch && index + batch_items_.size() < avail) {
                        Record& f = slots_[(index + batch_items_.size()) & (kSlots - 1)];
                        if (f.kind != Kind::Draw || !f.prepared || f.memory_changed) {
                            break;
                        }
                        const Vk::PreparedCommand fp{&f.prep, &f.Vs(), &f.Ps(), f.state};
                        if (!exec_.Batchable(*batch_items_.back().draw, batch_items_.back().prepared, f.draw, fp)) {
                            break;
                        }
                        batch_items_.push_back({&f.draw, fp});
                    }
                    extra = batch_items_.size() - 1;
                }
                if (extra != 0) {
                    const bool one = exec_.DrawPreparedBatch(batch_items_);
                    for (u64 k = 1; k <= extra; ++k) {
                        // each draw's registers (only theirs changed: VS user data, index, draw parameters); not
                        // recorded as one: each on its own after its registers, as Execute does it
                        ApplyJournal(slots_[(index + k) & (kSlots - 1)].journal_end);
                        if (!one) {
                            exec_.DrawPrepared(*batch_items_[k].draw, batch_items_[k].prepared);
                        }
                    }
                } else {
                    exec_.DrawPrepared(r.draw, pc);
                }
            } else {
                exec_.Draw(r.draw);
            }
            break;
        case Kind::Dispatch:
            if (frontend_ != nullptr) {
                Vk::PreparedCommand pc;
                if (r.prepared) {
                    pc = {&r.prep, &r.stage, nullptr, nullptr};
                }
                exec_.DispatchPrepared(r.dispatch, r.has_cs ? &r.cs : nullptr, pc);
            } else {
                exec_.DispatchWith(r.dispatch, r.has_cs ? &r.cs : nullptr);
            }
            break;
        case Kind::CacheAction:
            exec_.CacheAction(r.cache);
            break;
        case Kind::Fence:
            exec_.Fence(r.fence);
            if (flush_at_fence_ && exec_.WantsFlush()) {
                exec_.FlushFromLoop(); // (CP-9: a held fence's work to the GPU now, its completion polled)
                exec_.Poll();
            }
            break;
        case Kind::Dma:
            exec_.Dma(r.dma);
            break;
        case Kind::WaitMet:
            exec_.WaitMetByHeldFence(r.queue);
            break;
        case Kind::MemoryAcquired:
            if (r.hook_done) {
                exec_.MemoryAcquiredApplied(r.queue, r.blocked, r.dst, r.seq, r.applied);
            } else {
                exec_.MemoryAcquired(r.queue, r.blocked, r.dst);
            }
            break;
        case Kind::Copy:
            RunCopy(r);
            break;
        case Kind::Flip:
            flip_totals_ = &r.flip_totals; // (the flip hook's frame log row: thread 1's totals at the marker)
            exec_.FlipQueued(r.flip);
            flip_totals_ = nullptr;
            break;
        case Kind::Write:
            RunWrite(r);
            break;
        case Kind::Gds:
            RunGds(r);
            break;
        case Kind::RoundEnd:
            // CP-9: what the loop does after a drained round end - a held fence's batch submitted, completed batches
            // collected (their completions kept for thread 1)
            if (exec_.WantsFlush()) {
                exec_.FlushFromLoop();
            }
            exec_.Poll();
            break;
        case Kind::Journal:
        case Kind::Stop:
            break;
        }
    } catch (const std::exception& e) {
        Fail(fmt::format("parallel command processor: the recording thread threw: {}", e.what()));
    } catch (...) {
        Fail("parallel command processor: the recording thread threw");
    }
    return extra;
}

void ParallelCp::Fail(std::string text) {
    if (!failed_.load(std::memory_order_relaxed)) {
        failure_ = std::move(text);
        failed_.store(true, std::memory_order_release);
    }
}

// ---- Cp::GpuWork (thread 1)

void ParallelCp::Draw(const Cp::DrawDescriptor& draw) {
    Record& r = BeginRecord(Kind::Draw);
    r.draw = draw;
    if (verify_) {
        r.reg_hash = RegistersHash(cp_->Registers());
    }
    if (frontend_ != nullptr && !fthread_mode_) {
        PrepareDraw(r, next_, cp_->Registers()); // (as they are at the draw's packet)
    }
    ++t1_.draws;
    PushRecord();
}

void ParallelCp::Dispatch(const Cp::DispatchDescriptor& dispatch) {
    Record& r = BeginRecord(Kind::Dispatch);
    r.dispatch = dispatch;
    const Cp::ComputeRegisters* cs = cp_ != nullptr ? cp_->Compute(dispatch.queue) : nullptr;
    r.has_cs = cs != nullptr;
    if (cs != nullptr) {
        r.cs = *cs; // as they are at the packet: later packets of the queue change them
    }
    if (verify_) {
        r.reg_hash = RegistersHash(cp_->Registers());
    }
    if (frontend_ != nullptr && !fthread_mode_) {
        PrepareDispatch(r, next_);
    }
    ++t1_.dispatches;
    PushRecord();
}

// The processor's calls that need no answer: queued behind the draws before them (P4.10.4), or - the queue empty, thread
// 2 parked - made at once here; without `async_` each drains (the first prototype). Queued with the frontend on thread 1
// (stage 2): it is told about the call's guest memory change here, in stream order (thread 2 does not tell it).
#define SSPC_P410_QUEUED_CALL(KIND, SLOT, REASON, DIRECT, FILL)                                                        \
    do {                                                                                                               \
        if (!async_) {                                                                                                 \
            Drain(REASON);                                                                                             \
            DIRECT;                                                                                                    \
            SnapshotTimeline();                                                                                        \
        } else if (Idle()) {                                                                                           \
            ApplyPending();                                                                                            \
            ++t1_.direct_by[SLOT];                                                                                     \
            DIRECT;                                                                                                    \
            SnapshotTimeline();                                                                                        \
        } else {                                                                                                       \
            Record& r = BeginRecord(KIND);                                                                             \
            FILL;                                                                                                      \
            ++t1_.queued_by[SLOT];                                                                                     \
            PushRecord();                                                                                              \
            FrontMemoChanged();                                                                                        \
        }                                                                                                              \
    } while (false)

void ParallelCp::CacheAction(const Cp::CacheActionOp& op) {
    SSPC_P410_QUEUED_CALL(Kind::CacheAction, kQCache, DrainReason::CacheAction, exec_.CacheAction(op), r.cache = op);
}

void ParallelCp::Fence(const Cp::FenceOp& op) {
    SSPC_P410_QUEUED_CALL(Kind::Fence, kQFence, DrainReason::Fence, exec_.Fence(op), r.fence = op);
}

void ParallelCp::Dma(const Cp::DmaOp& op) {
    SSPC_P410_QUEUED_CALL(Kind::Dma, kQDma, DrainReason::Dma, exec_.Dma(op), r.dma = op);
}

void ParallelCp::WaitMetByHeldFence(u32 queue) {
    SSPC_P410_QUEUED_CALL(Kind::WaitMet, kQWaitMet, DrainReason::WaitMet, exec_.WaitMetByHeldFence(queue),
                          r.queue = queue);
}

void ParallelCp::MemoryAcquired(u32 queue, bool blocked, u64 poll_address) {
    if (frontend_ != nullptr) {
        if (async_acquire_ && !Idle()) {
            // P4.11 (Options::async_acquire): the CPU writes applied here, the rest in stream order on thread 2
            const u64 met = blocked ? __rdtsc() : 0;
            const bool applied = exec_.RunAcquireHook();
            Record& r = BeginRecord(Kind::MemoryAcquired);
            r.queue = queue;
            r.blocked = blocked;
            r.dst = poll_address;
            r.seq = met;
            r.hook_done = true;
            r.applied = applied;
            ++t1_.queued_by[kQAcquired];
            ++t1_.async_acquires;
            PushRecord();
            FrontMemoChanged(); // (GuestSync's memo change)
            return;
        }
        // the CPU writes it applies invalidate the frontend's programs too: on the frontend's thread, the queue drained
        Drain(DrainReason::MemoryAcquired);
        ++t1_.direct_by[kQAcquired];
        exec_.MemoryAcquired(queue, blocked, poll_address);
        SnapshotTimeline();
        return;
    }
    SSPC_P410_QUEUED_CALL(Kind::MemoryAcquired, kQAcquired, DrainReason::MemoryAcquired,
                          exec_.MemoryAcquired(queue, blocked, poll_address),
                          (r.queue = queue, r.blocked = blocked, r.dst = poll_address, r.hook_done = false));
}

bool ParallelCp::CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) {
    if (!async_copies_) {
        Drain(DrainReason::TimelineCopy);
        const bool ok = exec_.CopyOnTimeline(queue, seq, src, dst, bytes);
        SnapshotTimeline();
        if (ok && frontend_ != nullptr) {
            AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
        }
        return ok;
    }
    // the executor's own refusals by shape (Executor::CopyOnTimeline): the processor copies (it drains first)
    if (bytes == 0 || src >= (1ull << 40) || dst >= (1ull << 40) || (src < dst + bytes && dst < src + bytes)) {
        ++t1_.copies_refused;
        return false;
    }
    if (Idle()) {
        ApplyPending();
        ++t1_.direct_by[kQCopy];
        const bool ok = exec_.CopyOnTimeline(queue, seq, src, dst, bytes);
        SnapshotTimeline();
        if (ok && frontend_ != nullptr) {
            AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
        }
        return ok;
    }
    // accepted: the executor records it in stream order (or, refusing, the CPU copies there - RunCopy); until it ran,
    // the processor's reads of the destination drain first (TimelinePending), the frontend's too (stage 2)
    Record& r = BeginRecord(Kind::Copy);
    r.queue = queue;
    r.seq = seq;
    r.src = src;
    r.dst = dst;
    r.bytes = bytes;
    ++t1_.queued_by[kQCopy];
    queued_tw_.emplace_back(dst, dst + bytes);
    qtw_lo_ = std::min(qtw_lo_, dst);
    qtw_hi_ = std::max(qtw_hi_, dst + bytes);
    if (frontend_ != nullptr && !fthread_mode_) {
        AddGpuWrite(dst, dst + bytes, next_, kLookup); // (the frontend thread notes it when it passes the record)
    }
    PushRecord();
    FrontMemoChanged(); // (the copy's NoteAccesses)
    return true;
}

void ParallelCp::Flip(const Cp::FlipMarker& marker) {
    if (async_flip_ && started_ && !exec_.EvictionDue() && !StallTrace::On() && !Inspector::On()) {
        // Native CP: executed by thread 2 behind the frame's draws; the frontend's frame begins here (thread 1's order)
        Record& r = BeginRecord(Kind::Flip);
        r.flip = marker;
        r.flip_totals = {};
        if (flip_snapshot_ != nullptr) {
            flip_snapshot_(flip_snapshot_ctx_, r.flip_totals);
        }
        ++t1_.queued_flips;
        PushRecord();
        frontend_->NewFrame();
        return;
    }
    Drain(DrainReason::Flip);
    const u64 v0 = exec_.TimelineWritesVersion();
    exec_.Flip(marker); // (the frontend's new frame, evictions: here, on the frontend's thread)
    if (frontend_ != nullptr && exec_.TimelineWritesVersion() != v0) {
        // images written back at the flip (EvictGpuImages): the executor's timeline writes now are pending until then
        std::vector<std::pair<u64, u64>> tw;
        exec_.TimelineWriteRanges(tw);
        const u64 value = exec_.RecordedValue();
        for (const auto& [b, e] : tw) {
            AddGpuWrite(b, e, next_, value);
        }
    }
    SnapshotTimeline();
}

void ParallelCp::Interrupt(u32 queue, Cp::FenceKind kind) {
    exec_.Interrupt(queue, kind); // (the guest's interrupt: no executor state)
}

void ParallelCp::Synchronize(u32 queue) {
    Drain(DrainReason::Synchronize);
    exec_.Synchronize(queue);
    SnapshotTimeline();
}

bool ParallelCp::WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) {
    if (async_writes_ && !async_gds_only_ && started_ && !Idle()) {
        // CP-9: accepted by its shape, recorded in stream order (RunWrite: the CPU writes there after a refusal)
        if (!Vk::Executor::TimelineWriteShapeOk(dst, bytes)) {
            return false; // (the processor writes it, draining first - as the executor's refusal)
        }
        Record& r = BeginRecord(Kind::Write);
        r.queue = queue;
        r.seq = seq;
        r.dst = dst;
        r.bytes = bytes;
        r.payload.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + bytes);
        ++writes_queued_;
        queued_tw_.emplace_back(dst, dst + bytes);
        qtw_lo_ = std::min(qtw_lo_, dst);
        qtw_hi_ = std::max(qtw_hi_, dst + bytes);
        if (frontend_ != nullptr && !fthread_mode_) {
            AddGpuWrite(dst, dst + bytes, next_, kLookup);
        }
        PushRecord();
        FrontMemoChanged();
        return true;
    }
    Drain(DrainReason::TimelineWrite);
    const bool ok = exec_.WriteOnTimeline(queue, seq, dst, data, bytes);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr) {
        AddGpuWrite(dst, dst + bytes, next_, exec_.RecordedValue());
    }
    return ok;
}

bool ParallelCp::ZpassOnTimeline(u32 queue, u64 seq, u64 dst, u32 db_mask, u32 stride) {
    // after the draws queued before it (the query's own draw among them), as the executor records it
    Drain(DrainReason::TimelineWrite);
    const bool ok = exec_.ZpassOnTimeline(queue, seq, dst, db_mask, stride);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr) {
        AddGpuWrite(dst, dst + 16ull * stride, next_, exec_.RecordedValue());
    }
    return ok;
}

bool ParallelCp::GdsOnTimeline(u32 queue, u64 seq, Cp::DmaKind kind, u64 memory, u32 gds_offset, u32 bytes,
                               u32 pattern) {
    if (async_writes_ && started_ && !Idle()) {
        if (!exec_.GdsShapeOk(kind, memory, gds_offset, bytes)) {
            return false;
        }
        Record& r = BeginRecord(Kind::Gds);
        r.queue = queue;
        r.seq = seq;
        r.gds_kind = kind;
        r.src = memory;
        r.gds_offset = gds_offset;
        r.bytes = bytes;
        r.pattern = pattern;
        ++writes_queued_;
        if (kind == Cp::DmaKind::GdsToMemory) {
            queued_tw_.emplace_back(memory, memory + bytes);
            qtw_lo_ = std::min(qtw_lo_, memory);
            qtw_hi_ = std::max(qtw_hi_, memory + bytes);
            if (frontend_ != nullptr && !fthread_mode_) {
                AddGpuWrite(memory, memory + bytes, next_, kLookup);
            }
        }
        PushRecord();
        FrontMemoChanged();
        return true;
    }
    Drain(DrainReason::TimelineGds);
    const bool ok = exec_.GdsOnTimeline(queue, seq, kind, memory, gds_offset, bytes, pattern);
    SnapshotTimeline();
    if (ok && frontend_ != nullptr && kind == Cp::DmaKind::GdsToMemory) {
        AddGpuWrite(memory, memory + bytes, next_, exec_.RecordedValue());
    }
    return ok;
}

// ---- reports

std::string ParallelCp::FailureText() const {
    return failed_.load(std::memory_order_acquire) ? failure_ : std::string{};
}

u64 ParallelCp::Depth() const {
    return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
}

u64 ParallelCp::RecordingThreadCpuNs() const {
    if (!started_ || !thread_.joinable()) {
        return 0;
    }
    FILETIME c{}, e{}, k{}, u{};
    if (!GetThreadTimes(const_cast<std::thread&>(thread_).native_handle(), &c, &e, &k, &u)) {
        return 0;
    }
    const auto ft = [](const FILETIME& f) { return (u64{f.dwHighDateTime} << 32 | f.dwLowDateTime) * 100; };
    return ft(k) + ft(u);
}

ParallelTotals ParallelCp::Totals() const {
    ParallelTotals t = t1_;
    t.t2_busy_ticks = t2_busy_.load(std::memory_order_relaxed);
    t.t2_idle_ticks = t2_idle_.load(std::memory_order_relaxed);
    t.t2_sleeps = t2_sleeps_.load(std::memory_order_relaxed);
    t.t2_records = t2_records_.load(std::memory_order_relaxed);
    t.copy_fallbacks = copy_fallbacks_.load(std::memory_order_relaxed);
    t.copy_failures = copy_failures_.load(std::memory_order_relaxed);
    const auto get = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    t.front_ticks = get(fc_.ticks);
    t.front_reads = get(fc_.reads);
    t.front_read_hits = get(fc_.read_hits);
    t.front_syncs = get(fc_.syncs);
    t.gpu_writes = get(fc_.gpu_writes);
    t.gw_max = get(fc_.gw_max);
    t.gw_block_hits = get(fc_.gw_block_hits);
    t.gw_rebuilds = get(fc_.gw_rebuilds);
    t.gw_now = fthread_mode_ ? get(fc_.gw_now) : gw_.size(); // (thread 1's own list without the frontend thread)
    t.tf_busy_ticks = get(fc_.busy);
    t.tf_idle_ticks = get(fc_.idle);
    t.tf_wait_ticks = get(fc_.wait_ticks);
    t.tf_waits = get(fc_.waits);
    return t;
}

std::string ParallelCp::Report() const {
    const ParallelTotals t = Totals();
    const double freq = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    const auto ms = [&](u64 ticks) { return static_cast<double>(ticks) * 1000.0 / freq; };
    std::string out = fmt::format(
        "parallel command processor (P4.10, SSPC_P410_PARALLEL_CP=1; other calls {}, timeline copies {}, frontend on "
        "{}): records {} (draws {}, dispatches {}, journal {}), executed {}; queue depth at a push: mean {:.1f}, max {}; "
        "register journal {} words\n"
        "  thread 1 waited: drains {:.0f} ms ({} drains that waited, {} of an empty queue), a full queue {:.0f} ms ({} "
        "times); blocking waits {}\n"
        "  thread 2: executing {:.0f} ms, waiting for records {:.0f} ms (blocking waits {}); processor writes' "
        "invalidations handed on {}, memory changes handed on {}\n"
        "  queued / made at once (queue empty): cache actions {}/{}, fences {}/{}, DMA reports {}/{}, waits met {}/{}, "
        "memory acquired {}/{}, timeline copies {}/{} (refused by shape {}, copied by the CPU after a refusal {}); "
        "processor reads drained for a pending timeline copy {}\n",
        async_ ? "queued" : "drained", async_copies_ ? "queued" : "drained",
        fthread_mode_ ? "its own thread (SSPC_P53_FRONTEND_THREAD=1)"
        : frontend_ != nullptr ? "the processor's thread" : "the recording thread",
        t.records, t.draws, t.dispatches,
        t.journal_records, t.t2_records,
        t.records != 0 ? static_cast<double>(t.depth_sum) / static_cast<double>(t.records) : 0.0, t.depth_max,
        t.journal_words, ms(t.t1_drain_ticks), t.drains, t.drains_empty, ms(t.t1_full_ticks), t.t1_full_waits,
        t.t1_sleeps, ms(t.t2_busy_ticks), ms(t.t2_idle_ticks), t.t2_sleeps, t.deferred_invalidations, t.memo_changes,
        t.queued_by[kQCache], t.direct_by[kQCache], t.queued_by[kQFence], t.direct_by[kQFence], t.queued_by[kQDma],
        t.direct_by[kQDma], t.queued_by[kQWaitMet], t.direct_by[kQWaitMet], t.queued_by[kQAcquired],
        t.direct_by[kQAcquired], t.queued_by[kQCopy], t.direct_by[kQCopy], t.copies_refused, t.copy_fallbacks,
        t.timeline_read_drains);
    if (frontend_ != nullptr) {
        out += fmt::format("  frontend on {}: {:.0f} ms; its reads {} (a GPU write possibly pending: {} - {}; the "
                           "executor waited for the GPU {}); GPU write ranges noted {} (at most {} at once, {} now), "
                           "block hits {}, bitmap rebuilds {}\n",
                           fthread_mode_ ? "its own thread" : "the processor's thread", ms(t.front_ticks), t.front_reads,
                           t.front_read_hits, fthread_mode_ ? "waited for thread 2" : "drained", t.front_syncs,
                           t.gpu_writes, t.gw_max, t.gw_now, t.gw_block_hits, t.gw_rebuilds);
    }
    if (fthread_mode_) {
        out += fmt::format("  frontend thread (P5.3D): preparing {:.0f} ms, waiting for records {:.0f} ms, waiting for "
                           "thread 2 {:.0f} ms ({} times)\n",
                           ms(t.tf_busy_ticks), ms(t.tf_idle_ticks), ms(t.tf_wait_ticks), t.tf_waits);
    }
    if (geofast_ != 0) {
        out += fmt::format("  CITY-OPT geometry-only draw preparations (SSPC_CITY_GEOFAST): taken {}, verified {} "
                           "(mismatches {})\n",
                           geofast_hits_.load(std::memory_order_relaxed), geofast_verified_.load(std::memory_order_relaxed),
                           geofast_mismatches_.load(std::memory_order_relaxed));
    }
    if (stage_reuse_ != 0) {
        out += fmt::format("  FB-6 unchanged draw stages not copied (SSPC_FB_STAGE_REUSE={}): taken from the last copy "
                           "{}, copied {}; verify mismatches {}\n",
                           stage_reuse_ == 2 ? "verify" : "1", reuse_same_.load(std::memory_order_relaxed),
                           reuse_copied_.load(std::memory_order_relaxed),
                           reuse_mismatches_.load(std::memory_order_relaxed));
    }
    if (verify_) {
        out += fmt::format("  register replica verification (SSPC_P410_VERIFY=1): draws/dispatches compared {}, "
                           "different {}\n",
                           verified_.load(std::memory_order_relaxed), verify_mismatches_.load(std::memory_order_relaxed));
    }
    if (async_acquire_ || t.copy_failures != 0) {
        out += fmt::format("  P4.11: met memory waits queued with their CPU writes applied on the processor's thread "
                           "(SSPC_P411_ASYNC_ACQUIRE=1) {}; queued copies neither recorded nor copied {}\n",
                           t.async_acquires, t.copy_failures);
    }
    if (async_flip_) {
        out += fmt::format("  Native CP: flip markers queued to the recording thread (SSPC_CP_ASYNC_FLIP=1) {}\n",
                           t.queued_flips);
    }
    out += "  drains that waited, by reason:";
    for (size_t i = 0; i < t.drains_by.size(); ++i) {
        if (t.drains_by[i] != 0) {
            out += fmt::format(" {} {} ({:.0f} ms);", DrainReasonName(static_cast<DrainReason>(i)), t.drains_by[i],
                               ms(t.drain_ticks_by[i]));
        }
    }
    out += "\n";
    if (Failed()) {
        out += "  FAILED: " + failure_ + "\n";
    }
    return out;
}

} // namespace SecondSon::Gpu::Native
