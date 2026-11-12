// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_shape.hpp"

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class BasicFloatingOperation : unsigned {
	multiply,
	divide,
	add,
	subtract,
	negative_multiply,
	square_root,
};

struct BasicFloatingArithmetic final {
	bool recognized{};
	bool valid{};
	bool scalar{};
	bool unary{};
	unsigned width{};
	unsigned vector_bytes{};
	BasicFloatingOperation operation{};
};

[[nodiscard]] constexpr BasicFloatingArithmetic
decode_basic_floating_arithmetic(std::uint32_t instruction) noexcept
{
	/* Scalar FMUL/FDIV/FADD/FSUB/FNMUL. Other op values in the same
	 * two-source allocation belong to the min/max family. */
	if ((instruction & 0xff20'0c00U) == 0x1e20'0800U) {
		const unsigned opcode = (instruction >> 12) & 15U;
		BasicFloatingOperation operation;
		switch (opcode) {
		case 0: operation = BasicFloatingOperation::multiply; break;
		case 1: operation = BasicFloatingOperation::divide; break;
		case 2: operation = BasicFloatingOperation::add; break;
		case 3: operation = BasicFloatingOperation::subtract; break;
		case 8: operation = BasicFloatingOperation::negative_multiply; break;
		default: return {};
		}
		const unsigned type = (instruction >> 22) & 3U;
		return {true, type != 2U, true, false,
			floating_width_from_type(type), 16U, operation};
	}

	/* Scalar FSQRT is operation 3 of the one-source allocation. */
	if ((instruction & 0xff20'7c00U) == 0x1e20'4000U &&
	    ((instruction >> 15) & 0x3fU) == 3U) {
		const unsigned type = (instruction >> 22) & 3U;
		return {true, type != 2U, true, true,
			floating_width_from_type(type), 16U,
			BasicFloatingOperation::square_root};
	}

	const bool q = (instruction & 0x4000'0000U) != 0U;
	/* The FP16 three-same encodings use a distinct class from FP32/FP64. */
	if ((instruction & 0xbf60'fc00U) == 0x0e40'1400U)
		return {true, true, false, false, 16U, q ? 16U : 8U,
			(instruction & 0x0080'0000U) != 0U ?
				BasicFloatingOperation::subtract :
				BasicFloatingOperation::add};
	if ((instruction & 0xbfe0'fc00U) == 0x2e40'1c00U)
		return {true, true, false, false, 16U, q ? 16U : 8U,
			BasicFloatingOperation::multiply};
	if ((instruction & 0xbfe0'fc00U) == 0x2e40'3c00U)
		return {true, true, false, false, 16U, q ? 16U : 8U,
			BasicFloatingOperation::divide};
	if ((instruction & 0xbfff'fc00U) == 0x2ef9'f800U)
		return {true, true, false, true, 16U, q ? 16U : 8U,
			BasicFloatingOperation::square_root};

	BasicFloatingOperation operation;
	bool recognized = true;
	if ((instruction & 0xbf20'fc00U) == 0x0e20'd400U)
		operation = (instruction & 0x0080'0000U) != 0U ?
			BasicFloatingOperation::subtract : BasicFloatingOperation::add;
	else if ((instruction & 0xbf20'fc00U) == 0x2e20'dc00U)
		operation = BasicFloatingOperation::multiply;
	else if ((instruction & 0xbf20'fc00U) == 0x2e20'fc00U)
		operation = BasicFloatingOperation::divide;
	else if ((instruction & 0xbfbf'fc00U) == 0x2ea1'f800U)
		operation = BasicFloatingOperation::square_root;
	else
		recognized = false;
	if (!recognized)
		return {};
	const bool is_double = (instruction & 0x0040'0000U) != 0U;
	return {true, !is_double || q, false,
		operation == BasicFloatingOperation::square_root,
		is_double ? 64U : 32U, q ? 16U : 8U, operation};
}

[[nodiscard]] constexpr std::uintptr_t basic_floating_operand(
	std::uint32_t instruction) noexcept
{
	return (instruction & 31U) |
		(static_cast<std::uintptr_t>((instruction >> 5) & 31U) << 8) |
		(static_cast<std::uintptr_t>((instruction >> 16) & 31U) << 16);
}

} // namespace uml::cpu::arm64::detail
