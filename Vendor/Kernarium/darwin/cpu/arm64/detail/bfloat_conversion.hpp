// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

struct BFloatConversion final {
	bool recognized{};
	bool scalar{};
	bool upper{};
};

struct BFloatConversionResult final {
	std::uint16_t bits{};
	std::uint32_t exceptions{};
};

[[nodiscard]] constexpr BFloatConversion decode_bfloat_conversion(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xffff'fc00U) == 0x1e63'4000U)
		return {true, true, false};
	if ((instruction & 0xbfff'fc00U) == 0x0ea1'6800U)
		return {true, false, (instruction & 0x4000'0000U) != 0U};
	return {};
}

[[nodiscard]] constexpr BFloatConversionResult convert_to_bfloat(
	std::uint32_t input, std::uint32_t fpcr) noexcept
{
	const bool sign = (input >> 31) != 0U;
	const unsigned exponent = (input >> 23) & 0xffU;
	const unsigned fraction = input & 0x7f'ffffU;
	const bool signaling_nan = exponent == 0xffU && fraction != 0U &&
		(input & 0x0040'0000U) == 0U;
	if (exponent == 0xffU) {
		if (fraction == 0U)
			return {static_cast<std::uint16_t>(input >> 16), 0U};
		if ((fpcr & (1U << 25)) != 0U)
			return {0x7fc0U, signaling_nan ? 1U : 0U};
		return {static_cast<std::uint16_t>(
			(input | 0x0040'0000U) >> 16),
			signaling_nan ? 1U : 0U};
	}
	if (exponent == 0U && fraction == 0U)
		return {static_cast<std::uint16_t>(input >> 16), 0U};
	if (exponent == 0U && (fpcr & (1U << 24)) != 0U)
		return {static_cast<std::uint16_t>(sign ? 0x8000U : 0U), 1U << 7};

	const unsigned rounding = (fpcr >> 22) & 3U;
	const std::uint16_t truncated = static_cast<std::uint16_t>(input >> 16);
	const unsigned remainder = input & 0xffffU;
	if (remainder == 0U)
		return {truncated, 0U};
	bool increment{};
	switch (rounding) {
	case 0:
		increment = remainder > 0x8000U ||
			(remainder == 0x8000U && (truncated & 1U) != 0U);
		break;
	case 1: increment = !sign; break;
	case 2: increment = sign; break;
	default: break;
	}
	const auto result = static_cast<std::uint16_t>(
		truncated + static_cast<unsigned>(increment));
	std::uint32_t exceptions = 1U << 4;
	if (exponent == 0U)
		exceptions |= 1U << 3;
	if (exponent != 0U &&
	    (result & 0x7f80U) == 0x7f80U &&
	    (result & 0x007fU) == 0U)
		exceptions |= 1U << 2;
	return {result, exceptions};
}

} // namespace uml::cpu::arm64::detail
