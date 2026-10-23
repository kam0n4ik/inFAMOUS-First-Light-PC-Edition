// SecondSonPC — the tests of R8-A2 Native CP (the CP work removed with the native engine's state), owned by that branch
// (docs/NATIVE_ENGINE_R8_CORE.md §3). main.cpp calls R8CpTests with the eboot mapped (before the CUE setters' jumps
// go into the image); it returns the number of failures and adds its checks to the total through `checks`.
#include <array>
#include <cstdio>
#include <string>

#include "base/abi.h"
#include "runtime/gpu/r8/cp_native_table.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

int g_fail = 0;
int g_checks = 0;

#define R8CP_CHECK(cond, ...)                                                                                          \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            std::printf("FAILED r8 cp: " __VA_ARGS__);                                                                 \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

namespace R8 = ::SecondSon::Gpu::R8;

R8::SnapshotInfo Snap(u64 base, u8 stage, u64 version, std::initializer_list<std::pair<u8, u16>> refs) {
    R8::SnapshotInfo s;
    s.base = base;
    s.cue = 0x7000'0000ull;
    s.version = version;
    s.dwords = 64;
    s.stage = stage;
    for (const auto& [sg, off] : refs) {
        s.ref_sgpr[s.refs] = sg;
        s.ref_offset[s.refs] = off;
        ++s.refs;
    }
    return s;
}

void Point(std::array<u32, 16>& ud, u32 sgpr, u64 address) {
    ud[sgpr] = static_cast<u32>(address);
    ud[sgpr + 1] = static_cast<u32>(address >> 32);
}

void TableTests() {
    R8::EnableForTests();
    R8CP_CHECK(R8::TableOn(), "the table is on for the tests");

    // publish / lookup: every field comes back
    const u64 a = 0x2'0010'0000ull;
    R8::Publish(Snap(a, 2, 0x1234'5678'9abcull, {{2, 0}, {6, 16}}));
    R8::SnapshotInfo got;
    R8CP_CHECK(R8::Lookup(a, got), "lookup of a published snapshot");
    R8CP_CHECK(got.base == a && got.stage == 2 && got.version == 0x1234'5678'9abcull && got.dwords == 64 &&
                   got.refs == 2 && got.ref_sgpr[0] == 2 && got.ref_offset[0] == 0 && got.ref_sgpr[1] == 6 &&
                   got.ref_offset[1] == 16 && got.cue == 0x7000'0000ull,
               "the entry's fields");
    R8CP_CHECK(!R8::Lookup(a + 0x100, got), "no entry for an address never published");
    R8CP_CHECK(!R8::Lookup(0, got), "no entry for 0");

    // find: both pointers in the user data -> the entry and the mask of their registers
    std::array<u32, 16> ud{};
    ud[0] = 0xdead;
    Point(ud, 2, a);
    Point(ud, 6, a + 16 * 4);
    u32 mask = 0;
    R8CP_CHECK(R8::Find(ud, 2, got, mask) && got.base == a && mask == ((3u << 2) | (3u << 6)),
               "find by the user data (mask %x)", mask);
    R8CP_CHECK(!R8::Find(ud, 1, got, mask) && mask == 0, "another stage finds nothing");

    // one of the commit's pointers differs: not this snapshot
    std::array<u32, 16> ud2 = ud;
    Point(ud2, 6, a + 17 * 4);
    R8CP_CHECK(!R8::Find(ud2, 2, got, mask), "a pointer the commit did not write: not found");

    // the base is in the user data at another pair than the commit's: Find still needs the commit's registers
    std::array<u32, 16> ud3{};
    Point(ud3, 10, a);
    R8CP_CHECK(!R8::Find(ud3, 2, got, mask), "the base in other registers: not found");

    // republish at the same address (a new commit of the ring block): the new entry replaces the old one
    R8::Publish(Snap(a, 2, 77, {{2, 0}}));
    R8CP_CHECK(R8::Lookup(a, got) && got.version == 77 && got.refs == 1, "the newer entry wins");
    R8CP_CHECK(R8::Find(ud, 2, got, mask) && mask == (3u << 2), "find with the newer entry (mask %x)", mask);

    // seven pointers (the most an entry holds) survive the packing
    const u64 b = 0x2'0020'0000ull;
    R8::Publish(Snap(b, 1, 5, {{0, 0}, {2, 4}, {4, 8}, {6, 12}, {8, 0x3ff}, {10, 0xfff0}, {12, 9}}));
    R8CP_CHECK(R8::Lookup(b, got) && got.refs == 7 && got.ref_sgpr[6] == 12 && got.ref_offset[5] == 0xfff0 &&
                   got.ref_offset[4] == 0x3ff && got.ref_offset[6] == 9,
               "seven pointers");

    // a colliding address overwrites the slot; the old one is gone (a hint table, never a wrong entry)
    u64 c = 0;
    for (u64 x = b + 4; x < b + (64ull << 20); x += 4) {
        R8::SnapshotInfo probe;
        R8::Publish(Snap(x, 1, 6, {{0, 0}}));
        if (!R8::Lookup(b, probe)) {
            c = x;
            break;
        }
    }
    R8CP_CHECK(c != 0, "a colliding address found");
    R8CP_CHECK(c == 0 || (R8::Lookup(c, got) && got.base == c), "the collider's entry");
    R8CP_CHECK(!R8::Lookup(b, got), "the overwritten entry is not reported");

    const R8::TableStats st = R8::Stats();
    R8CP_CHECK(st.published > 3 && st.found >= 2 && st.torn == 0, "stats p %llu f %llu t %llu",
               static_cast<unsigned long long>(st.published), static_cast<unsigned long long>(st.found),
               static_cast<unsigned long long>(st.torn));
}

} // namespace

int R8CpTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
              int& checks) {
    (void)image_base, (void)image_size, (void)recomp_module, (void)recomp_dll, (void)cases;
    g_fail = 0;
    g_checks = 0;
    TableTests();
    std::printf("r8 cp: %d checks, %d failures\n", g_checks, g_fail);
    checks += g_checks;
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
