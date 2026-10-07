// SecondSonPC — guest/host ABI basics.
#pragma once

#include <cstdint>

// PS4 guest code calls host functions with the System V AMD64 calling convention.
// MSVC cannot emit it; clang-cl can (this is why SecondSonPC requires clang-cl).
#define SSPC_SYSV_ABI __attribute__((sysv_abi))

namespace SecondSon {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

} // namespace SecondSon
