// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "floating_shape.hpp"

#include <cstdint>

namespace uml::cpu::arm64::detail {

struct FusedFloatingArithmetic final {
	bool recognized{};
	bool valid{};
	bool scalar{};
	bool negate{};
	bool subtract{};
	unsigned width{};
	unsigned vector_bytes{};
};

[[nodiscard]] constexpr FusedFloatingArithmetic
decode_fused_floating_arithmetic(std::uint32_t instruction) noexcept
{
	/* FMADD/FMSUB/FNMADD/FNMSUB are the complete scalar 0x1f class. */
	if ((instruction & 0xff00'0000U) == 0x1f00'0000U) {
		const unsigned type = (instruction >> 22) & 3U;
		return {true, type != 2U, true,
			(instruction & 0x0020'0000U) != 0U,
			(instruction & 0x0000'8000U) != 0U,
			floating_width_from_type(type), 16U};
	}
	const bool q = (instruction & 0x4000'0000U) != 0U;
	/* FP16 FMLA/FMLS use bit 23 to select subtract. */
	if ((instruction & 0xbf60'fc00U) == 0x0e40'0c00U)
		return {true, true, false, false,
			(instruction & 0x0080'0000U) != 0U, 16U,
			q ? 16U : 8U};
	/* FP32/FP64 FMLA/FMLS use the same subtract bit and sz::Q shape. */
	if ((instruction & 0xbf20'fc00U) == 0x0e20'cc00U) {
		const bool is_double = (instruction & 0x0040'0000U) != 0U;
		return {true, !is_double || q, false, false,
			(instruction & 0x0080'0000U) != 0U,
			is_double ? 64U : 32U, q ? 16U : 8U};
	}
	return {};
}

} // namespace uml::cpu::arm64::detail
