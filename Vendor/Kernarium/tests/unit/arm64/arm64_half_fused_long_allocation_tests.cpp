// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_arithmetic_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/detail/half_fused_long.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_half_fused_long_test {

namespace arm64 = uml::cpu::arm64;

struct Form final {
	bool by_element;
	bool subtract;
	bool upper;
	bool wide;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array vector_forms{
	Form{false, false, false, false, 0U},
	Form{false, false, false, true, 0U},
	Form{false, false, true, false, 0U},
	Form{false, false, true, true, 0U},
	Form{false, true, false, false, 0U},
	Form{false, true, false, true, 0U},
	Form{false, true, true, false, 0U},
	Form{false, true, true, true, 0U},
};

[[nodiscard]] constexpr Form indexed_form(bool subtract, bool upper,
	bool wide, unsigned index) noexcept
{
	return {true, subtract, upper, wide, index};
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t encoding;
	if (form.by_element) {
		encoding = form.upper ? 0x2f80'8000U : 0x0f80'0000U;
		if (form.subtract)
			encoding |= 0x0000'4000U;
		encoding |= ((form.index & 3U) << 20U) |
			((form.index & 4U) << 9U) |
			((registers.second & 15U) << 16U);
	} else {
		encoding = form.upper ? 0x2e20'cc00U : 0x0e20'ec00U;
		if (form.subtract)
			encoding |= 0x0080'0000U;
		encoding |= (registers.second & 31U) << 16U;
	}
	return encoding |
		(static_cast<std::uint32_t>(form.wide) << 30U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

using uml::testing::require;

} // namespace arm64_half_fused_long_test

namespace test = arm64_half_fused_long_test;

int main()
{
	using test::arm64::detail::decode_half_fused_long;
	test::require(
		decode_half_fused_long(test::instruction(
			test::vector_forms[0])).recognized,
		"vector allocation was not recognized");
	test::require(
		decode_half_fused_long(test::instruction(
			test::indexed_form(false, false, false, 7U))).recognized,
		"indexed allocation was not recognized");
	test::require(
		!decode_half_fused_long(0x0e60'ec00U).recognized,
		"reserved vector size was recognized");
	test::require(
		!decode_half_fused_long(0x0fc0'0000U).recognized,
		"reserved indexed size was recognized");
	test::require(
		!decode_half_fused_long(0x0f80'0400U).recognized,
		"reserved indexed opcode was recognized");
}
