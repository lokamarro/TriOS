// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The helpers the generated operand programs (a32_operand_decode.cpp) call:
 * Arm's own shared pseudocode for the functions Decode sections use, written
 * for the values the decoder holds. */

#include "cpu/arm64/aarch32/a32_encodings.hpp"

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::aarch32::detail {

enum class Constraint : std::uint8_t { UNDEF, NOP };

[[nodiscard]] constexpr std::int64_t sint(std::uint64_t value, unsigned width) noexcept
{
	const std::uint64_t sign = std::uint64_t{1} << (width - 1);
	return static_cast<std::int64_t>((value ^ sign) - sign);
}

/* LowestSetBit(x): the index of the lowest set bit, or the width. */
[[nodiscard]] constexpr std::int64_t lowest_set_bit(std::uint64_t value, unsigned width) noexcept
{
	return value == 0 ? static_cast<std::int64_t>(width)
			  : static_cast<std::int64_t>(std::countr_zero(value));
}

struct ImmShift final {
	SRType type;
	std::int64_t amount;
};

[[nodiscard]] constexpr ImmShift decode_imm_shift(std::uint64_t type, std::uint64_t imm5) noexcept
{
	switch (type) {
	case 0:
		return {SRType::LSL, static_cast<std::int64_t>(imm5)};
	case 1:
		return {SRType::LSR, imm5 == 0 ? 32 : static_cast<std::int64_t>(imm5)};
	case 2:
		return {SRType::ASR, imm5 == 0 ? 32 : static_cast<std::int64_t>(imm5)};
	default:
		if (imm5 == 0)
			return {SRType::RRX, 1};
		return {SRType::ROR, static_cast<std::int64_t>(imm5)};
	}
}

[[nodiscard]] constexpr SRType decode_reg_shift(std::uint64_t type) noexcept
{
	switch (type) {
	case 0:
		return SRType::LSL;
	case 1:
		return SRType::LSR;
	case 2:
		return SRType::ASR;
	default:
		return SRType::ROR;
	}
}

/* An expanded immediate and its carry-out. carry_in means the carry-out is
 * the carry flag itself (a rotation of zero), so carry is not meaningful. */
struct ExpandedImm final {
	std::uint64_t imm32;
	std::uint64_t carry;
	bool carry_in;
};

[[nodiscard]] constexpr ExpandedImm a32_expand_imm_c(std::uint64_t imm12) noexcept
{
	const unsigned rotation = static_cast<unsigned>(imm12 >> 8 & 0xF) * 2;
	const std::uint32_t value = static_cast<std::uint32_t>(imm12 & 0xFF);
	if (rotation == 0)
		return {value, 0, true};
	const std::uint32_t result = std::rotr(value, static_cast<int>(rotation));
	return {result, result >> 31, false};
}

[[nodiscard]] constexpr std::uint64_t a32_expand_imm(std::uint64_t imm12) noexcept
{
	return a32_expand_imm_c(imm12).imm32;
}

[[nodiscard]] constexpr ExpandedImm t32_expand_imm_c(std::uint64_t imm12) noexcept
{
	const std::uint32_t byte = static_cast<std::uint32_t>(imm12 & 0xFF);
	if ((imm12 >> 10 & 3) == 0) {
		switch (imm12 >> 8 & 3) {
		case 0:
			return {byte, 0, true};
		case 1:
			return {byte << 16 | byte, 0, true};
		case 2:
			return {byte << 24 | byte << 8, 0, true};
		default:
			return {byte * UINT32_C(0x01010101), 0, true};
		}
	}
	const std::uint32_t unrotated = UINT32_C(0x80) | (byte & 0x7F);
	const std::uint32_t result = std::rotr(unrotated, static_cast<int>(imm12 >> 7 & 0x1F));
	return {result, result >> 31, false};
}

[[nodiscard]] constexpr std::uint64_t t32_expand_imm(std::uint64_t imm12) noexcept
{
	return t32_expand_imm_c(imm12).imm32;
}

[[nodiscard]] constexpr std::uint64_t replicate32(std::uint64_t value) noexcept
{
	return (value & UINT32_MAX) * UINT64_C(0x100000001);
}

[[nodiscard]] constexpr std::uint64_t advsimd_expand_imm(
	std::uint64_t op, std::uint64_t cmode, std::uint64_t imm8) noexcept
{
	switch (cmode >> 1) {
	case 0:
		return replicate32(imm8);
	case 1:
		return replicate32(imm8 << 8);
	case 2:
		return replicate32(imm8 << 16);
	case 3:
		return replicate32(imm8 << 24);
	case 4:
		return (imm8 & 0xFF) * UINT64_C(0x0001000100010001);
	case 5:
		return (imm8 << 8 & 0xFF00) * UINT64_C(0x0001000100010001);
	case 6:
		if ((cmode & 1) == 0)
			return replicate32(imm8 << 8 | 0xFF);
		return replicate32(imm8 << 16 | 0xFFFF);
	default:
		break;
	}
	if ((cmode & 1) == 0) {
		if (op == 0)
			return (imm8 & 0xFF) * UINT64_C(0x0101010101010101);
		std::uint64_t result = 0;
		for (unsigned bit = 0; bit < 8; ++bit)
			if (imm8 >> bit & 1)
				result |= UINT64_C(0xFF) << (bit * 8);
		return result;
	}
	if (op == 0) {
		const std::uint64_t b6 = imm8 >> 6 & 1;
		const std::uint64_t imm32 = (imm8 >> 7 & 1) << 31 | (b6 ^ 1) << 30
			| (b6 ? UINT64_C(0x1F) << 25 : 0) | (imm8 & 0x3F) << 19;
		return replicate32(imm32);
	}
	/* AArch32 reserves op 1 with cmode 1111; the index never allocates it. */
	return 0;
}

[[nodiscard]] constexpr std::uint64_t vfp_expand_imm(std::uint64_t imm8, unsigned width) noexcept
{
	const unsigned exp_bits = width == 16 ? 5 : (width == 32 ? 8 : 11);
	const unsigned frac_bits = width - exp_bits - 1;
	const std::uint64_t sign = imm8 >> 7 & 1;
	const std::uint64_t b6 = imm8 >> 6 & 1;
	std::uint64_t exp = (b6 ^ 1) << (exp_bits - 1);
	if (b6)
		exp |= ((std::uint64_t{1} << (exp_bits - 3)) - 1) << 2;
	exp |= imm8 >> 4 & 3;
	const std::uint64_t frac = (imm8 & 0xF) << (frac_bits - 4);
	return sign << (width - 1) | exp << frac_bits | frac;
}

[[nodiscard]] constexpr FPRounding fp_decode_rm(std::uint64_t rm) noexcept
{
	switch (rm) {
	case 0:
		return FPRounding::TIEAWAY;
	case 1:
		return FPRounding::TIEEVEN;
	case 2:
		return FPRounding::POSINF;
	default:
		return FPRounding::NEGINF;
	}
}

} // namespace uml::cpu::arm64::aarch32::detail
