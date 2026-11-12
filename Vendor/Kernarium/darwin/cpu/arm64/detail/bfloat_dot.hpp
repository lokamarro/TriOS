// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct BFloatDot final {
	bool recognized{};
	bool by_element{};
	bool wide{};
	unsigned index{};
};

[[nodiscard]] constexpr BFloatDot decode_bfloat_dot(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xbfe0'fc00U) == 0x2e40'fc00U)
		return {true, false, (instruction & 0x4000'0000U) != 0U, 0U};
	if ((instruction & 0xbfc0'f400U) == 0x0f40'f000U) {
		const unsigned index = ((instruction >> 21) & 1U) |
			((instruction >> 10) & 2U);
		return {true, true, (instruction & 0x4000'0000U) != 0U, index};
	}
	return {};
}

namespace bfloat_dot_detail {

/* FPDefaultNaN for binary32: positive, quiet, with no payload. */
inline constexpr std::uint32_t default_nan = 0x7fc0'0000U;

enum class Class : unsigned { zero, finite, infinity, nan };

struct Value final {
	Class type{};
	bool sign{};
	std::uint64_t significand{};
	int exponent{};
};

[[nodiscard]] constexpr Value unpack_standard_bfloat(
	std::uint16_t bits) noexcept
{
	const bool sign = (bits >> 15) != 0U;
	const unsigned exponent = (bits >> 7) & 0xffU;
	const unsigned fraction = bits & 0x7fU;
	if (exponent == 0U)
		return {Class::zero, sign, 0U, 0};
	if (exponent == 0xffU)
		return {fraction == 0U ? Class::infinity : Class::nan,
			sign, 0U, 0};
	return {Class::finite, sign, 0x80U | fraction,
		static_cast<int>(exponent) - 127 - 7};
}

[[nodiscard]] constexpr Value unpack_standard_single(
	std::uint32_t bits) noexcept
{
	const bool sign = (bits >> 31) != 0U;
	const unsigned exponent = (bits >> 23) & 0xffU;
	const unsigned fraction = bits & 0x7f'ffffU;
	if (exponent == 0U)
		return {Class::zero, sign, 0U, 0};
	if (exponent == 0xffU)
		return {fraction == 0U ? Class::infinity : Class::nan,
			sign, 0U, 0};
	return {Class::finite, sign, 0x80'0000U | fraction,
		static_cast<int>(exponent) - 127 - 23};
}

[[nodiscard]] constexpr Value product(const Value &left,
	const Value &right) noexcept
{
	return {Class::finite, left.sign != right.sign,
		left.significand * right.significand,
		left.exponent + right.exponent};
}

[[nodiscard]] constexpr unsigned bit_width128(unsigned __int128 value) noexcept
{
	const auto high = static_cast<std::uint64_t>(value >> 64);
	return high != 0U ? 64U + std::bit_width(high) :
		std::bit_width(static_cast<std::uint64_t>(value));
}

[[nodiscard]] constexpr unsigned __int128 shift_right_jam(
	unsigned __int128 value, unsigned shift) noexcept
{
	if (shift == 0U)
		return value;
	if (shift >= 128U)
		return value != 0U ? 1U : 0U;
	const auto shifted = value >> shift;
	const auto mask = (static_cast<unsigned __int128>(1U) << shift) - 1U;
	return shifted | static_cast<unsigned>(value & mask ? 1U : 0U);
}

/* FPCR.EBF is RES0, so BFDOT and BFMMLA always take the standard behavior,
 * which ignores FPCR: denormal inputs and results are zero, rounding is to
 * odd, overflow gives infinity, and a NaN result is the default NaN. */
[[nodiscard]] constexpr std::uint32_t round_finite(unsigned __int128 magnitude,
	int unit_exponent, bool sign) noexcept
{
	const unsigned high = bit_width128(magnitude) - 1U;
	const int value_exponent = unit_exponent + static_cast<int>(high);
	if (value_exponent < -126)
		return static_cast<std::uint32_t>(sign) << 31;
	const int signed_shift = static_cast<int>(high) - 23;
	/* Rounding to odd keeps the jammed sticky bit, so it never carries. */
	const auto significand = static_cast<std::uint64_t>(signed_shift < 0 ?
		magnitude << static_cast<unsigned>(-signed_shift) :
		shift_right_jam(magnitude, static_cast<unsigned>(signed_shift)));
	const auto exponent_field = static_cast<unsigned>(value_exponent + 127);
	if (exponent_field >= 0xffU)
		return (static_cast<std::uint32_t>(sign) << 31) | 0x7f80'0000U;
	return (static_cast<std::uint32_t>(sign) << 31) |
		(exponent_field << 23) |
		(static_cast<std::uint32_t>(significand) & 0x7f'ffffU);
}

[[nodiscard]] constexpr std::uint32_t add_finite(Value left,
	Value right) noexcept
{
	if (left.significand == 0U && right.significand == 0U) {
		const bool sign = left.sign == right.sign && left.sign;
		return static_cast<std::uint32_t>(sign) << 31;
	}
	constexpr unsigned target = 120U;
	const int left_top = left.significand == 0U ? -10000 : left.exponent +
		static_cast<int>(std::bit_width(left.significand)) - 1;
	const int right_top = right.significand == 0U ? -10000 : right.exponent +
		static_cast<int>(std::bit_width(right.significand)) - 1;
	const int top = std::max(left_top, right_top);
	const auto align = [top](const Value &value) constexpr {
		if (value.significand == 0U)
			return static_cast<unsigned __int128>(0U);
		const unsigned high = std::bit_width(value.significand) - 1U;
		auto aligned = static_cast<unsigned __int128>(value.significand) <<
			(target - high);
		const unsigned difference = static_cast<unsigned>(top -
			(value.exponent + static_cast<int>(high)));
		return shift_right_jam(aligned, difference);
	};
	const auto left_magnitude = align(left);
	const auto right_magnitude = align(right);
	unsigned __int128 magnitude;
	bool sign;
	if (left.sign == right.sign) {
		magnitude = left_magnitude + right_magnitude;
		sign = left.sign;
	} else if (left_magnitude >= right_magnitude) {
		magnitude = left_magnitude - right_magnitude;
		sign = left.sign;
	} else {
		magnitude = right_magnitude - left_magnitude;
		sign = right.sign;
	}
	if (magnitude == 0U)
		return 0U;
	return round_finite(magnitude, top - static_cast<int>(target), sign);
}

[[nodiscard]] constexpr std::uint32_t standard_multiply(std::uint16_t left_bits,
	std::uint16_t right_bits) noexcept
{
	const auto left = unpack_standard_bfloat(left_bits);
	const auto right = unpack_standard_bfloat(right_bits);
	if (left.type == Class::nan || right.type == Class::nan)
		return default_nan;
	if ((left.type == Class::infinity && right.type == Class::zero) ||
	    (left.type == Class::zero && right.type == Class::infinity))
		return default_nan;
	const bool sign = left.sign != right.sign;
	if (left.type == Class::infinity || right.type == Class::infinity)
		return (static_cast<std::uint32_t>(sign) << 31) | 0x7f80'0000U;
	if (left.type == Class::zero || right.type == Class::zero)
		return static_cast<std::uint32_t>(sign) << 31;
	const auto multiplied = product(left, right);
	return round_finite(multiplied.significand, multiplied.exponent, sign);
}

[[nodiscard]] constexpr std::uint32_t standard_add(std::uint32_t left_bits,
	std::uint32_t right_bits) noexcept
{
	const auto left = unpack_standard_single(left_bits);
	const auto right = unpack_standard_single(right_bits);
	if (left.type == Class::nan || right.type == Class::nan)
		return default_nan;
	if (left.type == Class::infinity && right.type == Class::infinity &&
	    left.sign != right.sign)
		return default_nan;
	if (left.type == Class::infinity)
		return left_bits;
	if (right.type == Class::infinity)
		return right_bits;
	return add_finite(left, right);
}

} // namespace bfloat_dot_detail

[[nodiscard]] constexpr std::uint32_t bfloat_dot_add(std::uint32_t addend,
	std::uint16_t left_a, std::uint16_t left_b, std::uint16_t right_a,
	std::uint16_t right_b) noexcept
{
	using namespace bfloat_dot_detail;
	const auto product_a = standard_multiply(left_a, right_a);
	const auto product_b = standard_multiply(left_b, right_b);
	return standard_add(addend, standard_add(product_a, product_b));
}

} // namespace uml::cpu::arm64::detail
