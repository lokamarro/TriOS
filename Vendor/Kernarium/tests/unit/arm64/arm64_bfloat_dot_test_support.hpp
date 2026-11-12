// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_bfloat_arithmetic_test_support.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_dot_test {

namespace shared = arm64_bfloat_arithmetic_test;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

struct Form final {
	bool by_element;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array forms{
	Form{false, false, 0U}, Form{false, true, 0U},
	Form{true, false, 0U}, Form{true, false, 1U},
	Form{true, false, 2U}, Form{true, false, 3U},
	Form{true, true, 0U}, Form{true, true, 1U},
	Form{true, true, 2U}, Form{true, true, 3U},
};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	const std::uint32_t base =
		form.by_element ? 0x0f40'f000U : 0x2e40'fc00U;
	const std::uint32_t index = form.by_element ?
		((form.index & 1U) << 21U) |
			((form.index & 2U) << 10U) : 0U;
	return base |
		(static_cast<std::uint32_t>(form.wide) << 30U) |
		index | ((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

} // namespace arm64_bfloat_dot_test
