// SecondSonPC R8 - the verification sites of R8 (docs/NATIVE_ENGINE_R8_VERIFICATION.md §2, R8_CORE.md §5.5).
//
// A site is one piece of R8 that may replace work of the game's path (a fast path of the command processor, a native
// command of the render layer, ...). In verify mode the site still runs the full path, runs its own, and compares the
// two: the full path's result is `expected` (it is what the draw uses), the site's own is `actual`.
//
//   static const R8::Verify::SiteId kSite = R8::Verify::Register("cp_stage_reuse");
//   if (R8::Verify::Enabled(kSite) && R8::Verify::Sample(kSite)) {
//       R8::Verify::Diff d;
//       R8::Verify::CompareStageView(full, reused, d);          // or d.Field / d.Bytes / d.Fields / d.Sequence
//       R8::Verify::Check(kSite, {frame, draw, key}, d);         // counts the check; a difference is a mismatch
//   }
//
// SSPC_R8_VERIFY=<site>[:<period>],...|all[:<period>] (read once, at the first Register): which sites verify, every
// <period>-th Sample() per thread (1 when not given). Off (the default): Enabled() is one load of a constant byte.
//
// What a mismatch keeps: the first kRecordsPerSite of every site (each site its own slots: a site with many
// mismatches never takes another's), counters per site, the first kLogPerSite in the log and then one line at every
// power of two - a long session's log stays bounded. Report() is the "R8 verify" section of gpu_native.txt.
// Lock-free: any thread may check; a record is written once (claimed by an atomic index) and read by the report.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "base/abi.h"

namespace SecondSon::Gpu::Shader {
struct StageView;
struct ResourceBindingPlan;
} // namespace SecondSon::Gpu::Shader
namespace SecondSon::Engine::Render {
struct DrawOp;
} // namespace SecondSon::Engine::Render

namespace SecondSon::Gpu::R8::Verify {

using SiteId = u32; ///< 0: no site (registration failed: too many sites); Enabled(0) is false
inline constexpr u32 kMaxSites = 64;
inline constexpr u32 kRecordsPerSite = 16;
inline constexpr u32 kLogPerSite = 4;
inline constexpr u32 kFieldChars = 96;

/// Registers a site (by its name, at most 31 characters; the same name again returns the same id). Thread-safe; call
/// once (a function-local static). Whether it verifies is decided here, from SSPC_R8_VERIFY.
SiteId Register(std::string_view name);

namespace Detail {
extern std::atomic<u8> g_enabled[kMaxSites];
extern u32 g_period[kMaxSites];
} // namespace Detail

/// The site verifies (SSPC_R8_VERIFY named it or "all").
inline bool Enabled(SiteId site) {
    return site < kMaxSites && Detail::g_enabled[site].load(std::memory_order_relaxed) != 0;
}
/// Every <period>-th call per thread is true (period 1: every call). Only for an enabled site.
bool Sample(SiteId site);

/// Where a comparison was: the frame (flip), the draw (the caller's index: in the frame, or global), and the site's
/// key (what its reuse was keyed on, a hash or an address; 0 when none).
struct Where {
    u64 frame = 0;
    u64 draw = 0;
    u64 key = 0;
};

/// The first difference of a comparison: the field's path ("plan.buffers[3].address"), expected and actual (the
/// field's value, or for bytes the differing byte's 8-byte window). Further fields are counted, not kept.
class Diff {
public:
    bool Same() const { return differences_ == 0; }
    u32 Differences() const { return differences_; }
    const char* FirstField() const { return field_; }
    u64 Expected() const { return expected_; }
    u64 Actual() const { return actual_; }

    /// A scalar (integral, enum, bool, pointer; a float compares its bits). true: equal.
    template <typename T>
    bool Field(std::string_view name, const T& expected, const T& actual) {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 8, "Field: a scalar; Bytes for larger values");
        u64 e = 0, a = 0;
        std::memcpy(&e, &expected, sizeof(T));
        std::memcpy(&a, &actual, sizeof(T));
        if (e == a) {
            return true;
        }
        Note(name, {}, e, a);
        return false;
    }
    /// Raw bytes (memory a site writes, a register block): the first differing byte names "<name>+0x<offset>",
    /// expected/actual its aligned 8-byte window. true: equal. Not for structs with padding (Fields instead).
    bool Bytes(std::string_view name, const void* expected, const void* actual, size_t bytes);
    /// Two lengths (a vector's size): "<name>.size".
    bool Size(std::string_view name, size_t expected, size_t actual);
    /// A sequence (command order, barriers, the draws of a run): the first different element "<name>[i]", or a
    /// different length "<name>.size".
    bool Sequence(std::string_view name, std::span<const u64> expected, std::span<const u64> actual);

    /// A struct by its listed fields (SSPC_R8_FIELD): only the fields compare, not the padding.
    struct FieldDesc {
        const char* name;
        u32 offset;
        u32 size;
    };
    bool Fields(std::string_view name, std::span<const FieldDesc> fields, const void* expected, const void* actual);

    /// A path prefix for the fields compared while it lives ("plan", "buffers[3]"): nested, at most 4 deep.
    class Scope {
    public:
        Scope(Diff& d, std::string_view name, s64 index = -1);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Diff& d_;
        u32 len_;
    };

    /// A difference found by the caller's own comparison (the expected/actual it knows).
    void Note(std::string_view name, std::string_view suffix, u64 expected, u64 actual);
    void Reset();

private:
    char prefix_[kFieldChars]{};
    u32 prefix_len_ = 0;
    char field_[kFieldChars]{};
    u64 expected_ = 0, actual_ = 0;
    u32 differences_ = 0;
};

#define SSPC_R8_FIELD(Type, member)                                                                                    \
    ::SecondSon::Gpu::R8::Verify::Diff::FieldDesc {                                                                    \
        #member, static_cast<u32>(offsetof(Type, member)), static_cast<u32>(sizeof(Type::member))                     \
    }

/// The executor's view of a prepared stage (what parallel_cp's CopyStage copies: status, program, module, runtime
/// parameters, bindings, the resource plan - not the specialization key), field by field. true: equal.
bool CompareStageView(const Gpu::Shader::StageView& expected, const Gpu::Shader::StageView& actual, Diff& d);
/// A stage's resource plan: the flattened user data, every buffer/image/sampler/vertex binding field, the status
/// counts, the SRT walk's error (not its read statistics: a reused plan did not walk). true: equal.
bool ComparePlan(const Gpu::Shader::ResourceBindingPlan& expected, const Gpu::Shader::ResourceBindingPlan& actual, Diff& d);
/// A draw command as the native stream carries it (engine/render/native_render_ops.h DrawOp: pass, index count and
/// base, the VS/PS registers, the stage tables' shape and hashes), field by field. true: equal.
bool CompareDrawOp(const Engine::Render::DrawOp& expected, const Engine::Render::DrawOp& actual, Diff& d);

/// One comparison of the site: counted; a difference is a mismatch (kept, logged as above). true: equal.
bool Check(SiteId site, const Where& where, const Diff& d);
/// A comparison made without a Diff (counted): `same` false is a mismatch of `field`.
bool Check(SiteId site, const Where& where, bool same, std::string_view field, u64 expected, u64 actual);
/// Something the site could not compare and so did not take (its fallback: the full path's result is used). Counted
/// apart from mismatches, by the site's reason (0..7).
void Unverifiable(SiteId site, u32 reason = 0);

struct SiteStats {
    u64 checks = 0, mismatches = 0, unverifiable = 0;
    u32 records = 0; ///< kept (at most kRecordsPerSite)
};
SiteStats Stats(SiteId site);
/// The site's registered name ("" for none).
std::string_view Name(SiteId site);
/// The mismatch records of a site (in order of claim), as the report lines them.
std::string Records(SiteId site);

/// The "R8 verify" section of gpu_native.txt: every registered site with its counters and kept records, the requested
/// names no site registered (a typo in SSPC_R8_VERIFY). Empty when SSPC_R8_VERIFY is not set.
std::string Report();

/// Tests only: re-reads `spec` as if SSPC_R8_VERIFY were it (sites registered before keep their ids, their enable is
/// decided again), counters and records cleared.
void ResetForTests(std::string_view spec);

} // namespace SecondSon::Gpu::R8::Verify
