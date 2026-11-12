// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

/* The element width of an FP type field (ftype, bits 23:22). Type 2 is
 * reserved and each caller rejects it, so its width is never used. */
[[nodiscard]] constexpr unsigned floating_width_from_type(unsigned type)
	noexcept
{
	return type == 3U ? 16U : type == 0U ? 32U : 64U;
}

/* The operand shape of the AdvSIMD floating-point forms that exist as a
 * scalar and as a vector. `shape` is the gadget selector: 0 to 2 are scalar
 * H, S and D, 3 and 4 are 4H and 8H, 5 and 6 are 2S and 4S, and 7 is 2D.
 * 1D is reserved, so a vector D without Q is not `valid`. */
struct FloatingShape final {
	bool matched{};
	bool valid{};
	bool scalar{};
	unsigned width{};
	unsigned vector_bytes{};
	unsigned lanes{};
	unsigned shape{};
};

[[nodiscard]] constexpr FloatingShape floating_shape(bool scalar,
	unsigned width, bool q) noexcept
{
	const unsigned bytes = scalar ? width / 8U : q ? 16U : 8U;
	const unsigned half_code = q ? 1U : 0U;
	const unsigned shape = scalar ?
		(width == 16U ? 0U : width == 32U ? 1U : 2U) :
		width == 16U ? 3U + half_code :
		width == 32U ? 5U + half_code : 7U;
	return {true, scalar || width != 64U || q, scalar, width, bytes,
		bytes / (width / 8U), shape};
}

/* Matches one operation in its four allocations: scalar half, scalar
 * single/double, vector half and vector single/double. The bases are the
 * vector encodings with the Q bit, sz and, when `has_rm`, Rm clear, since the
 * match leaves those free; the scalar encodings differ by 0x5000'0000. */
[[nodiscard]] constexpr FloatingShape match_floating_shape(
	std::uint32_t instruction, std::uint32_t half_base,
	std::uint32_t ordinary_base, bool has_rm) noexcept
{
	const std::uint32_t rm = has_rm ? 0U : 0x001f'0000U;
	const bool q = (instruction & 0x4000'0000U) != 0U;
	const unsigned ordinary_width =
		(instruction & 0x0040'0000U) != 0U ? 64U : 32U;
	if ((instruction & (0xffe0'fc00U | rm)) == (half_base | 0x5000'0000U))
		return floating_shape(true, 16U, q);
	if ((instruction & (0xffa0'fc00U | rm)) ==
	    (ordinary_base | 0x5000'0000U))
		return floating_shape(true, ordinary_width, q);
	if ((instruction & (0xbfe0'fc00U | rm)) == half_base)
		return floating_shape(false, 16U, q);
	if ((instruction & (0xbfa0'fc00U | rm)) == ordinary_base)
		return floating_shape(false, ordinary_width, q);
	return {};
}

struct FloatingFixedPointShape final {
	FloatingShape shape{};
	bool unsigned_form{};
	unsigned fractional_bits{};
};

/* Matches the scalar or vector fixed-point conversion whose bits 15:10 are
 * `opcode` (SCVTF and UCVTF 0xe400, FCVTZS and FCVTZU 0xfc00); bit 29 selects
 * the unsigned form. immh:immb gives the element width, and twice the width
 * minus immh:immb is the fractional-bit count. */
[[nodiscard]] constexpr FloatingFixedPointShape match_fixed_point_shape(
	std::uint32_t instruction, std::uint32_t opcode) noexcept
{
	const bool scalar =
		(instruction & 0xdf00'fc00U) == (0x5f00'0000U | opcode);
	if (!scalar && (instruction & 0x9f00'fc00U) != (0x0f00'0000U | opcode))
		return {};
	const unsigned immediate = (instruction >> 16) & 127U;
	/* immh == 0000 is not part of the vector fixed-point class: the same
	 * words belong to AdvSIMD modified-immediate (MOVI/MVNI). Scalar
	 * encodings have no such overlap and remain reserved. */
	if (!scalar && immediate < 8U)
		return {};
	const unsigned width = immediate >= 64U ? 64U :
		immediate >= 32U ? 32U : 16U;
	auto shape = floating_shape(scalar, width,
		(instruction & 0x4000'0000U) != 0U);
	shape.valid = shape.valid && immediate >= 16U;
	return {shape, (instruction & 0x2000'0000U) != 0U, 2U * width - immediate};
}

} // namespace uml::cpu::arm64::detail
