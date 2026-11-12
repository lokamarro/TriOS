// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace uml::emulator::tests::base_control {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t encode_pc_relative(
	bool page, std::int64_t immediate, unsigned destination) noexcept
{
	const auto encoded =
		static_cast<std::uint64_t>(immediate) & 0x1f'ffffU;
	return (page ? 0x9000'0000U : 0x1000'0000U) |
		((encoded & 3U) << 29) | ((encoded >> 2) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_direct_branch(
	bool link, std::int64_t displacement) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x03ff'ffffU;
	return (link ? 0x9400'0000U : 0x1400'0000U) | immediate;
}

[[nodiscard]] constexpr std::uint32_t encode_conditional_branch(
	bool consistent, unsigned condition, std::int64_t displacement) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x7ffffU;
	return 0x5400'0000U | (immediate << 5) |
		(consistent ? 0x10U : 0U) | (condition & 15U);
}

[[nodiscard]] constexpr std::uint32_t encode_compare_branch(
	bool wide, bool nonzero, std::int64_t displacement,
	unsigned source) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x7ffffU;
	return 0x3400'0000U | (wide ? 0x8000'0000U : 0U) |
		(nonzero ? 0x0100'0000U : 0U) | (immediate << 5) |
		(source & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_test_branch(
	bool nonzero, unsigned bit, std::int64_t displacement,
	unsigned source) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x3fffU;
	return 0x3600'0000U | ((bit >> 5) << 31) |
		(nonzero ? 0x0100'0000U : 0U) | ((bit & 31U) << 19) |
		(immediate << 5) | (source & 31U);
}

enum class RegisterBranch {
	branch,
	branch_link,
	return_,
};

[[nodiscard]] constexpr std::uint32_t encode_register_branch(
	RegisterBranch operation, unsigned source) noexcept
{
	const auto base = operation == RegisterBranch::branch ? 0xd61f'0000U :
		operation == RegisterBranch::branch_link ? 0xd63f'0000U :
		0xd65f'0000U;
	return base | ((source & 31U) << 5);
}

} // namespace uml::emulator::tests::base_control
