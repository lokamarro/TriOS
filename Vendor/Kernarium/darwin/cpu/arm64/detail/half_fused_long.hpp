// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "bfloat_multiply.hpp"
#include "floating_exception.hpp"

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct HalfFusedLong final {
	bool recognized{};
	bool by_element{};
	bool subtract{};
	bool upper{};
	bool wide{};
	unsigned index{};
};

[[nodiscard]] constexpr HalfFusedLong decode_half_fused_long(
	std::uint32_t instruction) noexcept
{
	const bool wide = (instruction & 0x4000'0000U) != 0U;
	const auto vector = instruction & 0xbfe0'fc00U;
	if (vector == 0x0e20'ec00U || vector == 0x2e20'cc00U ||
	    vector == 0x0ea0'ec00U || vector == 0x2ea0'cc00U)
		return {true, false, (instruction & 0x0080'0000U) != 0U,
			(instruction & 0x2000'0000U) != 0U, wide, 0U};
	const auto indexed = instruction & 0xbfc0'f400U;
	if (indexed == 0x0f80'0000U || indexed == 0x2f80'8000U ||
	    indexed == 0x0f80'4000U || indexed == 0x2f80'c000U) {
		const unsigned index = ((instruction >> 20) & 3U) |
			((instruction >> 9) & 4U);
		return {true, true, (instruction & 0x0000'4000U) != 0U,
			(instruction & 0x2000'0000U) != 0U, wide, index};
	}
	return {};
}

[[nodiscard]] constexpr bool reserved_half_fused_long(
	std::uint32_t instruction) noexcept
{
	const auto vector_without_size =
		instruction & ~0x0040'0000U;
	const auto vector = decode_half_fused_long(vector_without_size);
	if ((instruction & 0x0040'0000U) != 0U &&
	    vector.recognized && !vector.by_element)
		return true;
	const auto indexed_without_opcode =
		instruction & ~0x0000'0400U;
	const auto indexed =
		decode_half_fused_long(indexed_without_opcode);
	return (instruction & 0x0000'0400U) != 0U &&
		indexed.recognized && indexed.by_element;
}

struct HalfFusedLongResult final {
	std::uint32_t bits{};
	std::uint32_t exceptions{};
};

namespace half_fused_long_detail {

[[nodiscard]] constexpr std::uint32_t expand_half(std::uint16_t bits,
	std::uint32_t fpcr) noexcept
{
	const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16;
	const unsigned exponent = (bits >> 10) & 31U;
	const unsigned fraction = bits & 0x03ffU;
	if (exponent == 0U) {
		if (fraction == 0U || (fpcr & (1U << 19)) != 0U)
			return sign;
		const unsigned high = std::bit_width(fraction) - 1U;
		const unsigned float_exponent = static_cast<unsigned>(-24 +
			static_cast<int>(high) + 127);
		const std::uint32_t significand =
			static_cast<std::uint32_t>(fraction) << (23U - high);
		return sign | (float_exponent << 23) |
			(significand & 0x7f'ffffU);
	}
	if (exponent == 31U)
		return sign | 0x7f80'0000U |
			(static_cast<std::uint32_t>(fraction) << 13);
	return sign | ((exponent + 112U) << 23) |
		(static_cast<std::uint32_t>(fraction) << 13);
}

[[nodiscard]] inline HalfFusedLongResult captured_fmadd(
	std::uint32_t addend, std::uint32_t left, std::uint32_t right,
	std::uint32_t fpcr) noexcept
{
	const auto captured = run_with_local_fpcr(host_fpcr(fpcr), [&] {
		return bfloat_multiply_detail::fmadd(addend, left, right);
	});
	return {captured.value, captured.fpsr};
}

} // namespace half_fused_long_detail

/* A half operand of FPMulAddH() as the single precision FMADD takes it:
 * FPUnpack ignores AHP and flushes a denormal under FZ16. */
[[nodiscard]] constexpr std::uint32_t half_fused_operand(
	std::uint16_t bits, std::uint32_t fpcr) noexcept
{
	return half_fused_long_detail::expand_half(bits, fpcr);
}

[[nodiscard]] inline HalfFusedLongResult half_fused_multiply_add(
	std::uint32_t addend, std::uint16_t left_half, std::uint16_t right_half,
	bool subtract, std::uint32_t fpcr) noexcept
{
	using namespace half_fused_long_detail;
	if (subtract)
		left_half ^= 0x8000U;
	return captured_fmadd(addend, expand_half(left_half, fpcr),
		expand_half(right_half, fpcr), fpcr);
}

} // namespace uml::cpu::arm64::detail
