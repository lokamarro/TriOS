// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_conversion.hpp"
#include "floating_shape.hpp"

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class FloatingIntegerRounding : std::uint8_t {
	tie_even,
	plus_infinity,
	minus_infinity,
	zero,
	tie_away,
};

struct FloatingIntegerConversion final {
	std::uint64_t value;
	std::uint32_t exceptions;
};

[[nodiscard]] constexpr FloatingPrecisionConversion
convert_integer_to_floating_bits(std::uint64_t input, unsigned integer_width,
	unsigned destination_width, unsigned fractional_bits,
	bool unsigned_source, std::uint32_t fpcr) noexcept
{
	using namespace floating_conversion_detail;
	const auto integer_mask = low_mask(integer_width);
	input &= integer_mask;
	const bool sign = !unsigned_source &&
		(input >> (integer_width - 1U)) != 0U;
	const std::uint64_t magnitude = sign ?
		(~input + 1U) & integer_mask : input;
	if (magnitude == 0U)
		return {0U, 0U};

	/* FixedToFP delegates to FPRound, which clears AHP but retains the
	 * destination-width flush control (FZ or FZ16). */
	return round_to_format(magnitude, -static_cast<int>(fractional_bits), sign,
		destination_width, (fpcr >> 22) & 3U,
		(fpcr & (destination_width == 16U ? 1U << 19 : 1U << 24)) != 0U,
		false, 0U);
}

struct FloatingJavaScriptConversion final {
	std::uint32_t value;
	std::uint32_t exceptions;
	bool sets_z;
};

[[nodiscard]] constexpr FloatingJavaScriptConversion
convert_floating_to_javascript_integer(std::uint64_t input,
	std::uint32_t fpcr) noexcept
{
	using namespace floating_conversion_detail;
	std::uint32_t exceptions{};
	const auto source = unpack(input, 64U, fpcr, exceptions);
	if (source.type == Class::quiet_nan ||
	    source.type == Class::signaling_nan ||
	    source.type == Class::infinity) {
		exceptions |= 1U;
		return {0U, exceptions, false};
	}
	if (source.type == Class::zero)
		return {0U, exceptions,
			!source.sign && (input & low_mask(52U)) == 0U};

	std::uint64_t magnitude{};
	bool inexact{};
	bool outside_signed_range{};
	if (source.exponent >= 0) {
		const unsigned shift = static_cast<unsigned>(source.exponent);
		magnitude = shift >= 32U ? 0U :
			(source.significand << shift) & 0xffff'ffffU;
		const unsigned bits = std::bit_width(source.significand) + shift;
		outside_signed_range = source.sign ? bits > 32U :
			bits >= 32U;
		if (source.sign && bits == 32U && shift < 64U)
			outside_signed_range =
				(source.significand << shift) > 0x8000'0000ULL;
	} else {
		const unsigned shift = static_cast<unsigned>(-source.exponent);
		magnitude = shift < 64U ? source.significand >> shift : 0U;
		const auto remainder = shift < 64U ?
			source.significand & low_mask(shift) : source.significand;
		inexact = remainder != 0U;
		outside_signed_range = source.sign ?
			magnitude > 0x8000'0000ULL : magnitude > 0x7fff'ffffULL;
	}
	const auto value = static_cast<std::uint32_t>(source.sign ?
		~magnitude + 1U : magnitude);
	if (outside_signed_range)
		exceptions |= 1U;
	else if (inexact)
		exceptions |= 1U << 4;
	return {value, exceptions, !outside_signed_range && !inexact};
}

[[nodiscard]] constexpr FloatingIntegerConversion convert_floating_to_integer_bits(
	std::uint64_t input, unsigned source_width, unsigned integer_width,
	unsigned fractional_bits, bool unsigned_result,
	FloatingIntegerRounding rounding, std::uint32_t fpcr) noexcept
{
	using namespace floating_conversion_detail;
	std::uint32_t exceptions{};
	/* FPToFixed uses FPUnpack, which clears AHP and observes FZ16 for
	 * half-precision inputs.  FPUnpackCV has the opposite controls and is used
	 * only by floating-point precision conversions. */
	const auto conversion_fpcr = fpcr & ~(1U << 26);
	const auto half_fraction = input & 0x03ffU;
	const auto half_exponent = input & 0x7c00U;
	const bool flush_half = source_width == 16U && half_exponent == 0U &&
		half_fraction != 0U && (conversion_fpcr & (1U << 19)) != 0U;
	const auto processed_input = flush_half ? input & 0x8000U : input;
	const auto source =
		unpack(processed_input, source_width, conversion_fpcr, exceptions);
	const auto invalid = [&]() constexpr {
		exceptions |= 1U;
		if (unsigned_result)
			return FloatingIntegerConversion{
				source.sign ? 0U : low_mask(integer_width), exceptions};
		return FloatingIntegerConversion{
			source.sign ? std::uint64_t{1} << (integer_width - 1U) :
				low_mask(integer_width - 1U), exceptions};
	};
	if (source.type == Class::quiet_nan ||
	    source.type == Class::signaling_nan) {
		exceptions |= 1U;
		return {0U, exceptions};
	}
	if (source.type == Class::infinity)
		return invalid();
	if (source.type == Class::zero)
		return {0U, exceptions};

	const int exponent = source.exponent + static_cast<int>(fractional_bits);
	std::uint64_t magnitude{};
	bool overflow{};
	bool inexact{};
	if (exponent >= 0) {
		const unsigned shift = static_cast<unsigned>(exponent);
		overflow = shift >= 64U ||
			std::bit_width(source.significand) + shift > 64U;
		if (!overflow)
			magnitude = source.significand << shift;
	} else {
		const unsigned shift = static_cast<unsigned>(-exponent);
		const std::uint64_t truncated = shift < 64U ?
			source.significand >> shift : 0U;
		const std::uint64_t remainder = shift < 64U ?
			source.significand & low_mask(shift) : source.significand;
		magnitude = truncated;
		inexact = remainder != 0U;
		if (inexact) {
			bool increment{};
			if (rounding == FloatingIntegerRounding::plus_infinity)
				increment = !source.sign;
			else if (rounding == FloatingIntegerRounding::minus_infinity)
				increment = source.sign;
			else if ((rounding == FloatingIntegerRounding::tie_even ||
			          rounding == FloatingIntegerRounding::tie_away) &&
			         shift < 64U) {
				const auto halfway = std::uint64_t{1} << (shift - 1U);
				increment = remainder > halfway ||
					(remainder == halfway &&
					 (rounding == FloatingIntegerRounding::tie_away ||
					  (truncated & 1U) != 0U));
			}
			magnitude += static_cast<unsigned>(increment);
		}
	}

	const auto positive_limit = unsigned_result ? low_mask(integer_width) :
		low_mask(integer_width - 1U);
	const auto negative_limit = unsigned_result ? 0U :
		std::uint64_t{1} << (integer_width - 1U);
	if (overflow || (source.sign ? magnitude > negative_limit :
		magnitude > positive_limit) ||
	    (unsigned_result && source.sign && magnitude != 0U))
		return invalid();
	if (inexact)
		exceptions |= 1U << 4;
	const auto value = source.sign && magnitude != 0U ?
		(~magnitude + 1U) & low_mask(integer_width) : magnitude;
	return {value, exceptions};
}

struct FloatingToIntegerDecode final {
	bool recognized{};
	bool valid{};
	bool destination_gpr{};
	bool unsigned_result{};
	bool scalar{};
	FloatingIntegerRounding rounding{};
	unsigned source_width{};
	unsigned integer_width{};
	unsigned vector_bytes{};
	unsigned lanes{};
	unsigned shape{};
	bool fixed_point{};
	unsigned fractional_bits{};
};

struct IntegerToFloatingDecode final {
	bool recognized{};
	bool valid{};
	bool source_gpr{};
	bool unsigned_source{};
	bool scalar{};
	bool fixed_point{};
	unsigned integer_width{};
	unsigned destination_width{};
	unsigned vector_bytes{};
	unsigned lanes{};
	unsigned shape{};
	unsigned fractional_bits{};
};

[[nodiscard]] constexpr IntegerToFloatingDecode decode_integer_to_floating(
	std::uint32_t instruction) noexcept
{
	const unsigned rmode = (instruction >> 19) & 3U;
	const unsigned opcode = (instruction >> 16) & 7U;
	const bool fixed = (instruction & 0x0020'0000U) == 0U;
	if ((instruction & 0x7f00'0000U) == 0x1e00'0000U &&
	    rmode == 0U && (opcode == 2U || opcode == 3U) &&
	    (fixed || (instruction & 0x0000'fc00U) == 0U)) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned destination_width = floating_width_from_type(type);
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		const unsigned fractional_bits = fixed ?
			64U - ((instruction >> 10) & 63U) : 0U;
		const bool fixed_fields_valid =
			!fixed || fractional_bits <= integer_width;
		return {true, type != 2U && fixed_fields_valid, true,
			opcode == 3U, true, fixed, integer_width, destination_width,
			destination_width / 8U, 1U,
			destination_width == 16U ? 0U :
				destination_width == 32U ? 1U : 2U,
			fractional_bits};
	}

	/* The FEAT_FPRCVT SCVTF and UCVTF from a SIMD&FP register of another
	 * width: the virtual CPU does not implement FEAT_FPRCVT. */
	if ((instruction & 0x7f20'fc00U) == 0x1e20'0000U &&
	    rmode == 3U && (opcode == 4U || opcode == 5U))
		return {true, false};

	if (const auto fixed = match_fixed_point_shape(instruction, 0xe400U);
	    fixed.shape.matched) {
		const auto &shape = fixed.shape;
		return {true, shape.valid, false, fixed.unsigned_form, shape.scalar,
			true, shape.width, shape.width, shape.vector_bytes, shape.lanes,
			shape.shape, fixed.fractional_bits};
	}

	const auto shape = match_floating_shape(instruction & ~0x2000'0000U,
		0x0e79'd800U, 0x0e21'd800U, false);
	if (!shape.matched)
		return {};
	return {true, shape.valid, false, (instruction & 0x2000'0000U) != 0U,
		shape.scalar, false, shape.width, shape.width, shape.vector_bytes,
		shape.lanes, shape.shape, 0U};
}

[[nodiscard]] constexpr std::uintptr_t integer_to_floating_operand(
	std::uint32_t instruction, const IntegerToFloatingDecode &decoded) noexcept
{
	const unsigned destination_code = decoded.destination_width == 16U ? 0U :
		decoded.destination_width == 32U ? 1U : 2U;
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>(decoded.shape) << 16) |
		(static_cast<std::uintptr_t>(decoded.unsigned_source) << 20) |
		(static_cast<std::uintptr_t>(decoded.source_gpr) << 21) |
		(static_cast<std::uintptr_t>(decoded.integer_width == 64U) << 22) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 23) |
		(static_cast<std::uintptr_t>(destination_code) << 24) |
		(static_cast<std::uintptr_t>(decoded.fractional_bits) << 26) |
		(static_cast<std::uintptr_t>(decoded.fixed_point) << 33);
}

namespace floating_to_integer_detail {

struct Form final {
	FloatingIntegerRounding rounding;
	std::uint32_t half_base;
	std::uint32_t ordinary_base;
};

/* The signed forms; bit 29 selects the unsigned ones. */
inline constexpr Form forms[] = {
	{FloatingIntegerRounding::tie_even, 0x0e79'a800U, 0x0e21'a800U},
	{FloatingIntegerRounding::plus_infinity, 0x0ef9'a800U, 0x0ea1'a800U},
	{FloatingIntegerRounding::minus_infinity, 0x0e79'b800U, 0x0e21'b800U},
	{FloatingIntegerRounding::zero, 0x0ef9'b800U, 0x0ea1'b800U},
	{FloatingIntegerRounding::tie_away, 0x0e79'c800U, 0x0e21'c800U},
};

} // namespace floating_to_integer_detail

[[nodiscard]] constexpr FloatingToIntegerDecode decode_floating_to_integer(
	std::uint32_t instruction) noexcept
{
	/* Scalar fixed-point FCVTZS/FCVTZU to a general-purpose register. */
	if ((instruction & 0x7f20'0000U) == 0x1e00'0000U &&
	    ((instruction >> 19) & 3U) == 3U &&
	    ((instruction >> 16) & 7U) <= 1U) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned width = floating_width_from_type(type);
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		const unsigned fractional_bits =
			64U - ((instruction >> 10) & 63U);
		return {true, type != 2U && fractional_bits <= integer_width,
			true, (instruction & 0x0001'0000U) != 0U, true,
			FloatingIntegerRounding::zero, width, integer_width,
			width / 8U, 1U,
			width == 16U ? 0U : width == 32U ? 1U : 2U,
			true, fractional_bits};
	}

	/* Scalar FP-to-GPR conversions share this encoding class with the
	 * FEAT_FPRCVT conversions into a SIMD&FP register, which the virtual CPU
	 * does not implement. Classify the exact opcode/rmode pairs here so those
	 * and the malformed encodings are rejected before any architectural state
	 * read. */
	if ((instruction & 0x7f20'fc00U) == 0x1e20'0000U) {
		const unsigned type = (instruction >> 22) & 3U;
		const unsigned rmode = (instruction >> 19) & 3U;
		const unsigned opcode = (instruction >> 16) & 7U;
		const auto fixed_encoding = instruction & 0xffff'fc00U;
		const bool general_move =
			fixed_encoding == 0x1ee6'0000U ||
			fixed_encoding == 0x9ee6'0000U ||
			fixed_encoding == 0x1ee7'0000U ||
			fixed_encoding == 0x9ee7'0000U ||
			fixed_encoding == 0x9e66'0000U ||
			fixed_encoding == 0x9e67'0000U ||
			fixed_encoding == 0x1e26'0000U ||
			fixed_encoding == 0x1e27'0000U ||
			fixed_encoding == 0x9eae'0000U ||
			fixed_encoding == 0x9eaf'0000U;
		bool recognized{};
		bool opcode_valid{true};
		FloatingIntegerRounding rounding{};
		if (opcode <= 1U) {
			recognized = true;
			rounding = rmode == 0U ? FloatingIntegerRounding::tie_even :
				rmode == 1U ? FloatingIntegerRounding::plus_infinity :
				rmode == 2U ? FloatingIntegerRounding::minus_infinity :
					FloatingIntegerRounding::zero;
		} else if ((opcode == 4U || opcode == 5U) && rmode == 0U) {
			recognized = true;
			rounding = FloatingIntegerRounding::tie_away;
		} else if (((opcode == 2U || opcode == 3U) && rmode != 0U) ||
			   (opcode >= 4U && !general_move)) {
			/* The FEAT_FPRCVT conversions into a SIMD&FP register and the
			 * remaining reserved opcode 100..111 combinations of the
			 * FP-to-integer encoding space, not integer-to-FP. */
			recognized = true;
			opcode_valid = false;
		}
		if (!recognized)
			return {};
		const unsigned width = floating_width_from_type(type);
		const unsigned integer_width =
			(instruction & 0x8000'0000U) != 0U ? 64U : 32U;
		return {true, opcode_valid && type != 2U, true,
			(instruction & 0x0001'0000U) != 0U, true, rounding, width,
			integer_width, integer_width / 8U, 1U,
			width == 16U ? 0U : width == 32U ? 1U : 2U};
	}

	if (const auto fixed = match_fixed_point_shape(instruction, 0xfc00U);
	    fixed.shape.matched) {
		const auto &shape = fixed.shape;
		return {true, shape.valid, false, fixed.unsigned_form, shape.scalar,
			FloatingIntegerRounding::zero, shape.width, shape.width,
			shape.vector_bytes, shape.lanes, shape.shape, true,
			fixed.fractional_bits};
	}

	for (const auto &form : floating_to_integer_detail::forms) {
		const auto shape = match_floating_shape(instruction & ~0x2000'0000U,
			form.half_base, form.ordinary_base, false);
		if (shape.matched)
			return {true, shape.valid, false,
				(instruction & 0x2000'0000U) != 0U, shape.scalar,
				form.rounding, shape.width, shape.width, shape.vector_bytes,
				shape.lanes, shape.shape};
	}
	return {};
}

[[nodiscard]] constexpr std::uintptr_t floating_to_integer_operand(
	std::uint32_t instruction, const FloatingToIntegerDecode &decoded) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>(decoded.shape) << 16) |
		(static_cast<std::uintptr_t>(decoded.unsigned_result) << 20) |
		(static_cast<std::uintptr_t>(decoded.destination_gpr) << 21) |
		(static_cast<std::uintptr_t>(decoded.integer_width == 64U) << 22) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 23) |
		(static_cast<std::uintptr_t>(decoded.rounding) << 24) |
		(static_cast<std::uintptr_t>(decoded.fractional_bits) << 28) |
		(static_cast<std::uintptr_t>(decoded.fixed_point) << 35);
}

} // namespace uml::cpu::arm64::detail
