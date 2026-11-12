// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class FloatingComplexOperation : unsigned {
	add,
	multiply_accumulate,
};

struct FloatingComplex final {
	bool recognized{};
	bool valid{};
	FloatingComplexOperation operation{};
	bool by_element{};
	unsigned width{};
	unsigned vector_bytes{};
	unsigned rotation{};
	unsigned index{};
	unsigned second_register{};
};

[[nodiscard]] constexpr FloatingComplex decode_floating_complex(
	std::uint32_t instruction) noexcept
{
	const bool q = (instruction & 0x4000'0000U) != 0U;
	const unsigned size = (instruction >> 22) & 3U;
	const unsigned width = 8U << size;
	const bool common_valid = size != 0U && (size != 3U || q);
	if ((instruction & 0xbf20'ec00U) == 0x2e00'e400U)
		return {true, common_valid, FloatingComplexOperation::add, false,
			width, q ? 16U : 8U,
			(instruction & 0x1000U) != 0U ? 3U : 1U, 0U,
			(instruction >> 16) & 31U};
	if ((instruction & 0xbf20'e400U) == 0x2e00'c400U)
		return {true, common_valid,
			FloatingComplexOperation::multiply_accumulate, false,
			width, q ? 16U : 8U, (instruction >> 11) & 3U, 0U,
			(instruction >> 16) & 31U};
	if ((instruction & 0xbf00'9400U) == 0x2f00'1000U) {
		const bool half = size == 1U;
		const bool single = size == 2U;
		const bool l = (instruction & 0x0020'0000U) != 0U;
		const bool h = (instruction & 0x0000'0800U) != 0U;
		const bool valid = (half || single) &&
			(!single || (q && !l)) && (!half || q || !h);
		return {true, valid,
			FloatingComplexOperation::multiply_accumulate, true,
			half ? 16U : 32U, q ? 16U : 8U,
			(instruction >> 13) & 3U,
			half ? (static_cast<unsigned>(h) << 1) |
				static_cast<unsigned>(l) : static_cast<unsigned>(h),
			((instruction >> 16) & 15U) |
				((instruction >> 20) & 1U) << 4};
	}
	return {};
}

} // namespace uml::cpu::arm64::detail
