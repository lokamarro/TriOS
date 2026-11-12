// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_bits.hpp"

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct FloatingPrecisionConversion final {
	std::uint64_t bits;
	std::uint32_t exceptions;
};

struct SimdFloatingPrecisionVectorDecode final {
	bool recognized{};
	bool valid{};
	bool narrow{};
	bool upper{};
	bool scalar{};
	bool round_odd{};
	unsigned source_width{};
	unsigned destination_width{};
	unsigned lanes{};
	unsigned shape{};
};

[[nodiscard]] constexpr SimdFloatingPrecisionVectorDecode
decode_simd_floating_precision_vector(std::uint32_t instruction) noexcept
{
	const auto operation = instruction & 0xbfbf'fc00U;
	const bool narrow = operation == 0x0e21'6800U;
	const bool long_conversion = operation == 0x0e21'7800U;
	const bool odd_scalar =
		(instruction & 0xffff'fc00U) == 0x7e61'6800U;
	const bool odd_vector =
		(instruction & 0xbfff'fc00U) == 0x2e61'6800U;
	if (!narrow && !long_conversion && !odd_scalar && !odd_vector)
		return {};
	if (odd_scalar || odd_vector)
		return {true, true, true,
			odd_vector && (instruction & 0x4000'0000U) != 0U,
			odd_scalar, true, 64U, 32U, odd_scalar ? 1U : 2U, 4U};
	const bool wide_source = (instruction & 0x0040'0000U) != 0U;
	const bool upper = (instruction & 0x4000'0000U) != 0U;
	const unsigned source_width = narrow ? (wide_source ? 64U : 32U) :
		(wide_source ? 32U : 16U);
	const unsigned destination_width = narrow ? source_width / 2U :
		source_width * 2U;
	const unsigned shape = narrow ? (wide_source ? 3U : 2U) :
		(wide_source ? 1U : 0U);
	return {true, true, narrow, upper, false, false,
		source_width, destination_width,
		(narrow ? 128U : 64U) / source_width, shape};
}

[[nodiscard]] constexpr std::uintptr_t
simd_floating_precision_vector_operand(std::uint32_t instruction,
	const SimdFloatingPrecisionVectorDecode &decoded) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>(decoded.shape) << 16) |
		(static_cast<std::uintptr_t>(decoded.upper) << 20) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 21);
}

namespace floating_conversion_detail {

enum class Class : std::uint8_t {
	zero,
	denormal,
	finite,
	infinity,
	quiet_nan,
	signaling_nan,
};

using floating_bits::Format;
using floating_bits::format;
using floating_bits::low_mask;

struct Unpacked final {
	std::uint64_t bits;
	std::uint64_t significand;
	int exponent;
	Class type;
	bool sign;
};

[[nodiscard]] constexpr Unpacked unpack(std::uint64_t input, unsigned width,
	std::uint32_t fpcr, std::uint32_t &exceptions) noexcept
{
	const auto source = format(width);
	input &= low_mask(width);
	const bool sign = (input >> (width - 1U)) != 0U;
	const auto fraction_mask = low_mask(source.fraction_bits);
	const auto exponent_field =
		(input >> source.fraction_bits) & low_mask(source.exponent_bits);
	const auto fraction = input & fraction_mask;
	if (exponent_field == 0U) {
		if (fraction == 0U)
			return {input, 0U, 0, Class::zero, sign};
		if (width != 16U && (fpcr & (1U << 24)) != 0U) {
			exceptions |= 1U << 7;
			return {static_cast<std::uint64_t>(sign) << (width - 1U),
				0U, 0, Class::zero, sign};
		}
		return {input, fraction, 1 - source.bias -
			static_cast<int>(source.fraction_bits), Class::denormal, sign};
	}
	const auto exponent_ones = low_mask(source.exponent_bits);
	const bool alternative_half = width == 16U && (fpcr & (1U << 26)) != 0U;
	if (exponent_field == exponent_ones && !alternative_half) {
		if (fraction == 0U)
			return {input, 0U, 0, Class::infinity, sign};
		const auto quiet = std::uint64_t{1} << (source.fraction_bits - 1U);
		return {input, 0U, 0, (fraction & quiet) != 0U ?
			Class::quiet_nan : Class::signaling_nan, sign};
	}
	return {input, (std::uint64_t{1} << source.fraction_bits) | fraction,
		static_cast<int>(exponent_field) - source.bias -
			static_cast<int>(source.fraction_bits),
		Class::finite, sign};
}

struct Rounded final {
	std::uint64_t value;
	bool inexact;
};

[[nodiscard]] constexpr Rounded round_shift_right(std::uint64_t value,
	int shift, unsigned rounding, bool sign) noexcept
{
	if (shift <= 0)
		return {value << static_cast<unsigned>(-shift), false};
	std::uint64_t truncated{};
	std::uint64_t remainder{};
	if (shift < 64) {
		truncated = value >> static_cast<unsigned>(shift);
		remainder = value & low_mask(static_cast<unsigned>(shift));
	} else {
		remainder = value;
	}
	if (remainder == 0U)
		return {truncated, false};
	/* FPRounding_ODD truncates an inexact result and forces the retained least
	 * significant bit to one. */
	if (rounding == 4U)
		return {truncated | 1U, true};
	bool increment{};
	switch (rounding) {
	case 0: /* nearest, ties to even */
		if (shift < 64) {
			const auto halfway = std::uint64_t{1} <<
				(static_cast<unsigned>(shift) - 1U);
			increment = remainder > halfway ||
				(remainder == halfway && (truncated & 1U) != 0U);
		}
		break;
	case 1: increment = !sign; break; /* +infinity */
	case 2: increment = sign; break;  /* -infinity */
	default: break;                    /* zero */
	}
	return {truncated + static_cast<unsigned>(increment), true};
}

[[nodiscard]] constexpr std::uint64_t infinity(unsigned width,
	bool sign) noexcept
{
	const auto destination = format(width);
	return (static_cast<std::uint64_t>(sign) << (width - 1U)) |
		(low_mask(destination.exponent_bits) << destination.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t max_normal(unsigned width,
	bool sign) noexcept
{
	const auto destination = format(width);
	return (static_cast<std::uint64_t>(sign) << (width - 1U)) |
		((low_mask(destination.exponent_bits) - 1U) <<
			destination.fraction_bits) |
		low_mask(destination.fraction_bits);
}

[[nodiscard]] constexpr std::uint64_t convert_nan(const Unpacked &source,
	unsigned source_width, unsigned destination_width,
	std::uint32_t fpcr) noexcept
{
	const auto source_format = format(source_width);
	const auto destination = format(destination_width);
	if ((fpcr & (1U << 25)) != 0U)
		return (low_mask(destination.exponent_bits) <<
				destination.fraction_bits) |
			(std::uint64_t{1} << (destination.fraction_bits - 1U));
	const auto source_payload = source.bits &
		low_mask(source_format.fraction_bits - 1U);
	const unsigned source_payload_bits = source_format.fraction_bits - 1U;
	const unsigned destination_payload_bits = destination.fraction_bits - 1U;
	const auto payload = source_payload_bits > destination_payload_bits ?
		source_payload >> (source_payload_bits - destination_payload_bits) :
		source_payload << (destination_payload_bits - source_payload_bits);
	return (static_cast<std::uint64_t>(source.sign) <<
		(destination_width - 1U)) |
		(low_mask(destination.exponent_bits) << destination.fraction_bits) |
		(std::uint64_t{1} << (destination.fraction_bits - 1U)) | payload;
}

/* Rounds significand * 2^exponent to the destination format: the tail of
 * FPRound (the integer conversions) and of FPRoundCV (the precision
 * conversions). The caller has handled NaN, infinity and zero and decides
 * the two controls that differ: whether a subnormal result flushes to zero,
 * and whether the destination is half precision with FPCR.AHP. Both callers
 * pass most of them as constants, so it is always inlined and folds there. */
[[nodiscard, gnu::always_inline]] constexpr FloatingPrecisionConversion
round_to_format(std::uint64_t significand, int exponent, bool sign,
	unsigned destination_width, unsigned rounding, bool flush,
	bool alternative_half, std::uint32_t exceptions) noexcept
{
	const auto destination = format(destination_width);
	const auto sign_bit = static_cast<std::uint64_t>(sign) <<
		(destination_width - 1U);
	const int highest = static_cast<int>(std::bit_width(significand)) - 1;
	const int value_exponent = highest + exponent;
	const int minimum_exponent = 1 - destination.bias;
	const auto exponent_ones = low_mask(destination.exponent_bits);
	std::uint64_t result{};
	if (value_exponent >= minimum_exponent) {
		auto rounded = round_shift_right(significand,
			highest - static_cast<int>(destination.fraction_bits),
			rounding, sign);
		int biased_exponent = value_exponent + destination.bias;
		if (rounded.value ==
		    (std::uint64_t{1} << (destination.fraction_bits + 1U))) {
			rounded.value >>= 1U;
			++biased_exponent;
		}
		const auto overflow_limit = alternative_half ? exponent_ones + 1U :
			exponent_ones;
		if (biased_exponent >= static_cast<int>(overflow_limit)) {
			if (alternative_half) {
				exceptions |= 1U;
				result = sign_bit | low_mask(destination_width - 1U);
			} else {
				const bool to_infinity = rounding == 0U ||
					(rounding == 1U && !sign) ||
					(rounding == 2U && sign);
				result = to_infinity ?
					infinity(destination_width, sign) :
					max_normal(destination_width, sign);
				exceptions |= (1U << 2) | (1U << 4);
			}
		} else {
			result = sign_bit |
				(static_cast<std::uint64_t>(biased_exponent) <<
					destination.fraction_bits) |
				(rounded.value & low_mask(destination.fraction_bits));
			if (rounded.inexact)
				exceptions |= 1U << 4;
		}
	} else if (flush) {
		exceptions |= 1U << 3;
		result = sign_bit;
	} else {
		const int unit_exponent = minimum_exponent -
			static_cast<int>(destination.fraction_bits);
		const auto rounded = round_shift_right(significand,
			unit_exponent - exponent, rounding, sign);
		const bool became_normal = rounded.value >=
			(std::uint64_t{1} << destination.fraction_bits);
		result = sign_bit | (became_normal ?
			(std::uint64_t{1} << destination.fraction_bits) :
			rounded.value);
		/* Tininess is detected before rounding, so an inexact result
		 * underflows even when it rounds to the minimum normal number. */
		if (rounded.inexact)
			exceptions |= (1U << 3) | (1U << 4);
	}
	return {result, exceptions};
}

} // namespace floating_conversion_detail

[[nodiscard]] constexpr FloatingPrecisionConversion convert_floating_precision(
	std::uint64_t input, unsigned source_width, unsigned destination_width,
	std::uint32_t fpcr, unsigned rounding_override = ~0U) noexcept
{
	using namespace floating_conversion_detail;
	std::uint32_t exceptions{};
	const auto source = unpack(input, source_width, fpcr, exceptions);
	const auto sign_bit = static_cast<std::uint64_t>(source.sign) <<
		(destination_width - 1U);
	const bool alternative_half = destination_width == 16U &&
		(fpcr & (1U << 26)) != 0U;
	if (source.type == Class::quiet_nan || source.type == Class::signaling_nan) {
		if (alternative_half) {
			exceptions |= 1U;
			return {sign_bit, exceptions};
		}
		if (source.type == Class::signaling_nan)
			exceptions |= 1U;
		return {convert_nan(source, source_width, destination_width, fpcr),
			exceptions};
	}
	if (source.type == Class::infinity) {
		if (alternative_half) {
			exceptions |= 1U;
			return {sign_bit | low_mask(destination_width - 1U), exceptions};
		}
		return {infinity(destination_width, source.sign), exceptions};
	}
	if (source.type == Class::zero)
		return {sign_bit, exceptions};

	/* FPRoundCV clears FZ16 but retains FZ for single/double output. */
	return round_to_format(source.significand, source.exponent, source.sign,
		destination_width,
		rounding_override == ~0U ? (fpcr >> 22) & 3U : rounding_override,
		destination_width != 16U && (fpcr & (1U << 24)) != 0U,
		alternative_half, exceptions);
}

} // namespace uml::cpu::arm64::detail
