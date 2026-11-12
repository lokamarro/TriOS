// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_shape.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace uml::cpu::arm64::detail {

enum class FloatingRoundOperation : unsigned {
	nearest,
	plus,
	minus,
	zero,
	ties_away,
	exact_current,
	current,
	int32_exact,
	int32_zero,
	int64_exact,
	int64_zero,
};

struct FloatingRound final {
	bool recognized{};
	bool valid{};
	bool scalar{};
	unsigned width{};
	unsigned vector_bytes{};
	FloatingRoundOperation operation{};
};

[[nodiscard]] constexpr FloatingRoundOperation standard_round_operation(
	unsigned opcode) noexcept
{
	switch (opcode) {
	case 8: return FloatingRoundOperation::nearest;
	case 9: return FloatingRoundOperation::plus;
	case 10: return FloatingRoundOperation::minus;
	case 11: return FloatingRoundOperation::zero;
	case 12: return FloatingRoundOperation::ties_away;
	case 14: return FloatingRoundOperation::exact_current;
	default: return FloatingRoundOperation::current;
	}
}

[[nodiscard]] constexpr FloatingRound decode_floating_round(
	std::uint32_t instruction) noexcept
{
	/* Scalar FRINTN/P/M/Z/A/X/I. */
	if ((instruction & 0xff20'7c00U) == 0x1e20'4000U) {
		const unsigned opcode = (instruction >> 15) & 0x3fU;
		if ((opcode >= 8U && opcode <= 12U) || opcode == 14U || opcode == 15U) {
			const unsigned type = (instruction >> 22) & 3U;
			return {true, type != 2U, true,
				floating_width_from_type(type), 16U,
				standard_round_operation(opcode)};
		}
	}

	/* Scalar FEAT_FRINTTS forms; both high ftype values are reserved. */
	const auto sized_scalar = instruction & 0xff3f'fc00U;
	FloatingRoundOperation sized_operation;
	bool sized = true;
	if (sized_scalar == 0x1e28'c000U)
		sized_operation = FloatingRoundOperation::int32_exact;
	else if (sized_scalar == 0x1e28'4000U)
		sized_operation = FloatingRoundOperation::int32_zero;
	else if (sized_scalar == 0x1e29'c000U)
		sized_operation = FloatingRoundOperation::int64_exact;
	else if (sized_scalar == 0x1e29'4000U)
		sized_operation = FloatingRoundOperation::int64_zero;
	else
		sized = false;
	if (sized) {
		const unsigned type = (instruction >> 22) & 3U;
		return {true, type <= 1U, true, type == 0U ? 32U : 64U, 16U,
			sized_operation};
	}

	const bool q = (instruction & 0x4000'0000U) != 0U;
	/* Standard FP16 vector forms. */
	constexpr struct {
		std::uint32_t encoding;
		FloatingRoundOperation operation;
	} half_forms[]{
		{0x0e79'8800U, FloatingRoundOperation::nearest},
		{0x0ef9'8800U, FloatingRoundOperation::plus},
		{0x0e79'9800U, FloatingRoundOperation::minus},
		{0x0ef9'9800U, FloatingRoundOperation::zero},
		{0x2e79'8800U, FloatingRoundOperation::ties_away},
		{0x2e79'9800U, FloatingRoundOperation::exact_current},
		{0x2ef9'9800U, FloatingRoundOperation::current},
	};
	for (const auto &form : half_forms)
		if ((instruction & 0xbfff'fc00U) == form.encoding)
			return {true, true, false, 16U, q ? 16U : 8U,
				form.operation};

	/* Standard FP32/FP64 vector forms. */
	constexpr struct {
		std::uint32_t encoding;
		FloatingRoundOperation operation;
	} ordinary_forms[]{
		{0x0e21'8800U, FloatingRoundOperation::nearest},
		{0x0ea1'8800U, FloatingRoundOperation::plus},
		{0x0e21'9800U, FloatingRoundOperation::minus},
		{0x0ea1'9800U, FloatingRoundOperation::zero},
		{0x2e21'8800U, FloatingRoundOperation::ties_away},
		{0x2e21'9800U, FloatingRoundOperation::exact_current},
		{0x2ea1'9800U, FloatingRoundOperation::current},
	};
	for (const auto &form : ordinary_forms)
		if ((instruction & 0xbfbf'fc00U) == form.encoding) {
			const bool is_double = (instruction & 0x0040'0000U) != 0U;
			return {true, !is_double || q, false,
				is_double ? 64U : 32U, q ? 16U : 8U,
				form.operation};
		}

	/* FEAT_FRINTTS vector forms. */
	const auto sized_vector = instruction & 0xbfbf'fc00U;
	sized = true;
	if (sized_vector == 0x2e21'e800U)
		sized_operation = FloatingRoundOperation::int32_exact;
	else if (sized_vector == 0x0e21'e800U)
		sized_operation = FloatingRoundOperation::int32_zero;
	else if (sized_vector == 0x2e21'f800U)
		sized_operation = FloatingRoundOperation::int64_exact;
	else if (sized_vector == 0x0e21'f800U)
		sized_operation = FloatingRoundOperation::int64_zero;
	else
		sized = false;
	if (sized) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, false,
			is_double ? 64U : 32U, q ? 16U : 8U, sized_operation};
	}
	return {};
}

[[nodiscard]] constexpr bool is_sized_round(FloatingRoundOperation operation)
	noexcept
{
	return operation >= FloatingRoundOperation::int32_exact;
}

struct SizedFloatingRoundResult final {
	std::uint64_t bits;
	std::uint32_t exceptions;
};

template<typename Floating>
[[nodiscard]] SizedFloatingRoundResult round_floating_sized(
	std::uint64_t raw, unsigned integer_bits, bool current_rounding) noexcept
{
	using Integer = std::conditional_t<sizeof(Floating) == 2,
		std::uint16_t, std::conditional_t<sizeof(Floating) == 4,
			std::uint32_t, std::uint64_t>>;
	const auto value = std::bit_cast<Floating>(static_cast<Integer>(raw));
	const auto invalid_result = -std::ldexp(
		static_cast<Floating>(1), static_cast<int>(integer_bits - 1U));
	if (!std::isfinite(value))
		return {std::bit_cast<Integer>(invalid_result), 1U};
	const auto rounded = current_rounding ? std::nearbyint(value) :
		std::trunc(value);
	const auto upper = std::ldexp(static_cast<Floating>(1),
		static_cast<int>(integer_bits - 1U));
	if (rounded < invalid_result || rounded >= upper)
		return {std::bit_cast<Integer>(invalid_result), 1U};
	return {std::bit_cast<Integer>(rounded),
		rounded != value ? 1U << 4 : 0U};
}

[[nodiscard]] constexpr std::uintptr_t floating_round_operand(
	std::uint32_t instruction) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8);
}

[[nodiscard]] constexpr std::uintptr_t sized_floating_round_operand(
	std::uint32_t instruction, const FloatingRound &decoded) noexcept
{
	const unsigned integer_bits =
		decoded.operation == FloatingRoundOperation::int32_exact ||
		decoded.operation == FloatingRoundOperation::int32_zero ? 32U : 64U;
	const bool current =
		decoded.operation == FloatingRoundOperation::int32_exact ||
		decoded.operation == FloatingRoundOperation::int64_exact;
	return floating_round_operand(instruction) |
		(static_cast<std::uintptr_t>(decoded.width == 64U) << 16) |
		(static_cast<std::uintptr_t>(integer_bits == 64U) << 17) |
		(static_cast<std::uintptr_t>(current) << 18) |
		(static_cast<std::uintptr_t>(decoded.scalar) << 19) |
		(static_cast<std::uintptr_t>(decoded.vector_bytes == 16U) << 20);
}

} // namespace uml::cpu::arm64::detail
