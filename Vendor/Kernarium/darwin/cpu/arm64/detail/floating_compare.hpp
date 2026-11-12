// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_shape.hpp"

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class SimdFloatingCompareOperation : std::uint8_t {
	equal,
	greater_equal,
	greater,
	less_equal,
	less,
	absolute_greater_equal,
	absolute_greater,
};

struct SimdFloatingCompareDecode final {
	bool recognized{};
	bool valid{};
	bool zero{};
	bool scalar{};
	unsigned width{};
	unsigned vector_bytes{};
	unsigned shape{};
	SimdFloatingCompareOperation operation{};
};

namespace floating_compare_detail {

struct Form final {
	SimdFloatingCompareOperation operation;
	bool zero;
	std::uint32_t half_base;
	std::uint32_t ordinary_base;
};

/* The register forms have an Rm field; the zero forms compare with #0.0. */
inline constexpr Form forms[] = {
	{SimdFloatingCompareOperation::equal, false, 0x0e40'2400U, 0x0e20'e400U},
	{SimdFloatingCompareOperation::greater_equal, false,
		0x2e40'2400U, 0x2e20'e400U},
	{SimdFloatingCompareOperation::greater, false,
		0x2ec0'2400U, 0x2ea0'e400U},
	{SimdFloatingCompareOperation::absolute_greater_equal, false,
		0x2e40'2c00U, 0x2e20'ec00U},
	{SimdFloatingCompareOperation::absolute_greater, false,
		0x2ec0'2c00U, 0x2ea0'ec00U},
	{SimdFloatingCompareOperation::equal, true, 0x0ef8'd800U, 0x0ea0'd800U},
	{SimdFloatingCompareOperation::greater_equal, true,
		0x2ef8'c800U, 0x2ea0'c800U},
	{SimdFloatingCompareOperation::greater, true,
		0x0ef8'c800U, 0x0ea0'c800U},
	{SimdFloatingCompareOperation::less_equal, true,
		0x2ef8'd800U, 0x2ea0'd800U},
	{SimdFloatingCompareOperation::less, true, 0x0ef8'e800U, 0x0ea0'e800U},
};

} // namespace floating_compare_detail

[[nodiscard]] constexpr SimdFloatingCompareDecode decode_simd_floating_compare(
	std::uint32_t instruction) noexcept
{
	for (const auto &form : floating_compare_detail::forms) {
		const auto shape = match_floating_shape(instruction,
			form.half_base, form.ordinary_base, !form.zero);
		if (shape.matched)
			return {true, shape.valid, form.zero, shape.scalar, shape.width,
				shape.vector_bytes, shape.shape, form.operation};
	}
	return {};
}

[[nodiscard]] constexpr std::uintptr_t simd_floating_compare_operand(
	std::uint32_t instruction, const SimdFloatingCompareDecode &decoded) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16) |
		(static_cast<std::uintptr_t>(decoded.shape) << 24) |
		(static_cast<std::uintptr_t>(decoded.operation) << 28) |
		(static_cast<std::uintptr_t>(decoded.zero) << 32) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 33);
}

} // namespace uml::cpu::arm64::detail
