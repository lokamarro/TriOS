// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class FloatingReciprocalOperation : unsigned {
	reciprocal_estimate,
	reciprocal_step,
	reciprocal_exponent,
	reciprocal_sqrt_estimate,
	reciprocal_sqrt_step,
};

struct FloatingReciprocal final {
	bool recognized{};
	bool valid{};
	bool scalar{};
	bool unary{};
	unsigned width{};
	unsigned vector_bytes{};
	FloatingReciprocalOperation operation{};
};

[[nodiscard]] constexpr FloatingReciprocal decode_floating_reciprocal(
	std::uint32_t instruction) noexcept
{
	using Operation = FloatingReciprocalOperation;
	/* Scalar FP16 unary allocations. */
	switch (instruction & 0xffff'fc00U) {
	case 0x5ef9'd800U:
		return {true, true, true, true, 16U, 16U,
			Operation::reciprocal_estimate};
	case 0x5ef9'f800U:
		return {true, true, true, true, 16U, 16U,
			Operation::reciprocal_exponent};
	case 0x7ef9'd800U:
		return {true, true, true, true, 16U, 16U,
			Operation::reciprocal_sqrt_estimate};
	default: break;
	}
	/* Scalar FP32/FP64 unary allocations. */
	FloatingReciprocalOperation operation;
	bool matched = true;
	switch (instruction & 0xffbf'fc00U) {
	case 0x5ea1'd800U: operation = Operation::reciprocal_estimate; break;
	case 0x5ea1'f800U: operation = Operation::reciprocal_exponent; break;
	case 0x7ea1'd800U:
		operation = Operation::reciprocal_sqrt_estimate; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, true, true,
			(instruction & 0x0040'0000U) != 0U ? 64U : 32U,
			16U, operation};

	/* Scalar binary step allocations. */
	matched = true;
	switch (instruction & 0xffe0'fc00U) {
	case 0x5e40'3c00U: operation = Operation::reciprocal_step; break;
	case 0x5ec0'3c00U: operation = Operation::reciprocal_sqrt_step; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, true, false, 16U, 16U, operation};
	matched = true;
	switch (instruction & 0xffa0'fc00U) {
	case 0x5e20'fc00U: operation = Operation::reciprocal_step; break;
	case 0x5ea0'fc00U: operation = Operation::reciprocal_sqrt_step; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, true, false,
			(instruction & 0x0040'0000U) != 0U ? 64U : 32U,
			16U, operation};

	const bool q = (instruction & 0x4000'0000U) != 0U;
	/* Vector FP16 unary and binary allocations. */
	switch (instruction & 0xbfff'fc00U) {
	case 0x0ef9'd800U:
		return {true, true, false, true, 16U, q ? 16U : 8U,
			Operation::reciprocal_estimate};
	case 0x2ef9'd800U:
		return {true, true, false, true, 16U, q ? 16U : 8U,
			Operation::reciprocal_sqrt_estimate};
	default: break;
	}
	matched = true;
	switch (instruction & 0xbfe0'fc00U) {
	case 0x0e40'3c00U: operation = Operation::reciprocal_step; break;
	case 0x0ec0'3c00U: operation = Operation::reciprocal_sqrt_step; break;
	default: matched = false; break;
	}
	if (matched)
		return {true, true, false, false, 16U, q ? 16U : 8U,
			operation};

	/* Vector FP32/FP64 allocations; the .1D shape is reserved. */
	matched = true;
	switch (instruction & 0xbfbf'fc00U) {
	case 0x0ea1'd800U: operation = Operation::reciprocal_estimate; break;
	case 0x2ea1'd800U:
		operation = Operation::reciprocal_sqrt_estimate; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, false, true,
			is_double ? 64U : 32U, q ? 16U : 8U, operation};
	}
	matched = true;
	switch (instruction & 0xbfa0'fc00U) {
	case 0x0e20'fc00U: operation = Operation::reciprocal_step; break;
	case 0x0ea0'fc00U: operation = Operation::reciprocal_sqrt_step; break;
	default: matched = false; break;
	}
	if (matched) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, false, false,
			is_double ? 64U : 32U, q ? 16U : 8U, operation};
	}
	return {};
}

[[nodiscard]] constexpr std::uintptr_t floating_reciprocal_operand(
	std::uint32_t instruction, const FloatingReciprocal &decoded) noexcept
{
	constexpr auto vector_base = offsetof(CpuState, vectors);
	const auto offset = [](unsigned reg) {
		return vector_base + reg * sizeof(VectorRegister);
	};
	static_assert(offset(31U) <= 0xffffU);
	return offset(instruction & 31U) |
		(static_cast<std::uintptr_t>(
			offset((instruction >> 5) & 31U)) << 16) |
		(decoded.unary ? 0U :
			(static_cast<std::uintptr_t>(
				offset((instruction >> 16) & 31U)) << 32));
}

} // namespace uml::cpu::arm64::detail
