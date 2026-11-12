// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_ordinary_memory_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = engine::engine_test_data_base;

struct SingleVariant final {
	unsigned size_code;
	unsigned operation;
	std::size_t size;
	bool load;
	std::string_view name;
};

inline constexpr std::array single_variants{
	SingleVariant{0, 0, 1, false, "store-b"},
	SingleVariant{0, 1, 1, true, "load-b"},
	SingleVariant{1, 0, 2, false, "store-h"},
	SingleVariant{1, 1, 2, true, "load-h"},
	SingleVariant{2, 0, 4, false, "store-s"},
	SingleVariant{2, 1, 4, true, "load-s"},
	SingleVariant{3, 0, 8, false, "store-d"},
	SingleVariant{3, 1, 8, true, "load-d"},
	SingleVariant{0, 2, 16, false, "store-q"},
	SingleVariant{0, 3, 16, true, "load-q"},
};

enum class ImmediateMode : unsigned {
	unscaled = 0,
	post_index = 1,
	pre_index = 3,
};

enum class PairMode : unsigned {
	non_temporal = 0,
	post_index = 1,
	signed_offset = 2,
	pre_index = 3,
};

[[nodiscard]] inline engine::NativeRequirements single_native()
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::floating_point);
}

[[nodiscard]] constexpr std::uint32_t unsigned_instruction(
	unsigned size_code, unsigned operation, unsigned imm12,
	unsigned rn, unsigned rt)
{
	return 0x3d00'0000U | ((size_code & 3U) << 30) |
		((operation & 3U) << 22) | ((imm12 & 0xfffU) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t immediate_instruction(
	unsigned size_code, unsigned operation, unsigned imm9,
	ImmediateMode mode, unsigned rn, unsigned rt)
{
	return 0x3c00'0000U | ((size_code & 3U) << 30) |
		((operation & 3U) << 22) | ((imm9 & 0x1ffU) << 12) |
		((static_cast<unsigned>(mode) & 3U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t register_instruction(
	unsigned size_code, unsigned operation, unsigned rm, unsigned option,
	bool scaled, unsigned rn, unsigned rt)
{
	return 0x3c20'0800U | ((size_code & 3U) << 30) |
		((operation & 3U) << 22) | ((rm & 31U) << 16) |
		((option & 7U) << 13) | (static_cast<unsigned>(scaled) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t pair_instruction(
	bool load, PairMode mode, unsigned imm7, unsigned rn,
	unsigned rt, unsigned rt2)
{
	return 0xec00'0000U | (static_cast<unsigned>(load) << 22) |
		((static_cast<unsigned>(mode) & 3U) << 23) |
		((imm7 & 0x7fU) << 15) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

static_assert(unsigned_instruction(0, 1, 0, 0, 0) == 0x3d40'0000U);
static_assert(unsigned_instruction(0, 3, 0, 0, 0) == 0x3dc0'0000U);
static_assert(immediate_instruction(
	0, 0, 0, ImmediateMode::unscaled, 0, 0) == 0x3c00'0000U);
static_assert(immediate_instruction(
	0, 1, 0, ImmediateMode::post_index, 0, 0) == 0x3c40'0400U);
static_assert(register_instruction(0, 0, 0, 3, false, 0, 0) ==
	0x3c20'6800U);
static_assert(pair_instruction(
	false, PairMode::non_temporal, 0, 0, 0, 0) == 0xec00'0000U);
static_assert(pair_instruction(
	true, PairMode::pre_index, 0, 0, 0, 0) == 0xedc0'0000U);

[[nodiscard]] constexpr std::int64_t signed_imm7(unsigned imm7)
{
	return static_cast<std::int64_t>(
		static_cast<std::uint64_t>(imm7 & 0x7fU) << 57) >> 57;
}

[[nodiscard]] constexpr std::uint64_t sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto shift = 64U - bits;
	return static_cast<std::uint64_t>(
		static_cast<std::int64_t>(value << shift) >> shift);
}

[[nodiscard]] constexpr std::uint64_t extend_offset(
	std::uint64_t value, unsigned option, unsigned shift)
{
	switch (option) {
	case 2:
		value &= 0xffff'ffffU;
		break;
	case 3:
		break;
	case 6:
		value = sign_extend(value & 0xffff'ffffU, 32);
		break;
	case 7:
		break;
	default:
		return 0;
	}
	return value << shift;
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			seed ^ (0x8100'0000'2000'0000ULL |
				(static_cast<std::uint64_t>(index) << 13) |
				(index * 0x109U));
	cpu.sp = (seed ^ 0x1234'5678'9abc'def0ULL) & ~std::uint64_t{15};
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 47U + byte * 23U + 11U);
	return cpu;
}

inline void fill_memory(std::span<std::byte> memory, std::uint64_t seed)
{
	for (std::size_t offset = 0; offset < memory.size(); ++offset)
		memory[offset] = static_cast<std::byte>(
			0x80U |
			((offset * 61U + seed * 31U + (offset >> 5)) & 0x7fU));
}

inline void write_base(
	arm64::CpuState &cpu, unsigned rn, std::uint64_t value)
{
	if (rn == 31U)
		cpu.sp = value;
	else
		cpu.registers[rn] = value;
}

inline void apply_single_oracle(
	const arm64::CpuState &initial, arm64::CpuState &expected,
	std::span<const std::byte> initial_memory,
	std::span<std::byte> expected_memory, std::size_t offset,
	const SingleVariant &variant, unsigned rt)
{
	if (variant.load) {
		auto &destination = expected.vectors[rt].bytes;
		destination.fill(std::byte{});
		std::copy_n(initial_memory.begin() +
				static_cast<std::ptrdiff_t>(offset),
			variant.size, destination.begin());
	} else {
		std::copy_n(initial.vectors[rt].bytes.begin(), variant.size,
			expected_memory.begin() + static_cast<std::ptrdiff_t>(offset));
	}
}

} // namespace arm64_simd_ordinary_memory_test
