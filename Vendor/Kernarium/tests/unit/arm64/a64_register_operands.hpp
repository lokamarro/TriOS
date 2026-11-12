// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The general-purpose registers an A64 word names, for the tests that prove
 * emitted host code never uses x18, which Darwin clears when it interrupts a
 * thread. The Base and FP&SIMD classes follow the architecture's encoding
 * index here, independent of the code generators under test. SVE and SME
 * words take their operands from the table generated from the
 * architecture's XML (darwin/cpu/arm64/detail/sme_encodings.hpp). */

#include "cpu/arm64/detail/sme_decode.hpp"

#include <cstdint>
#include <optional>

namespace uml::testing::a64 {

namespace register_operands_detail {

[[nodiscard]] constexpr std::uint32_t named(std::uint32_t word,
	unsigned low) noexcept
{
	return std::uint32_t{1} << ((word >> low) & 31U);
}

[[nodiscard]] constexpr std::uint32_t rd(std::uint32_t word) noexcept
{
	return named(word, 0);
}

[[nodiscard]] constexpr std::uint32_t rn(std::uint32_t word) noexcept
{
	return named(word, 5);
}

[[nodiscard]] constexpr std::uint32_t ra(std::uint32_t word) noexcept
{
	return named(word, 10);
}

[[nodiscard]] constexpr std::uint32_t rm(std::uint32_t word) noexcept
{
	return named(word, 16);
}

[[nodiscard]] constexpr std::optional<std::uint32_t> sve_sme(
	std::uint32_t word) noexcept
{
	namespace table = ::uml::cpu::arm64::detail;
	const auto *row = table::find_sme_encoding(word);
	/* The table records operands only for the rows the virtual CPU
	 * implements. */
	if (row == nullptr || row->requirement == 0U)
		return std::nullopt;
	std::uint32_t mask = 0;
	for (unsigned index = 0; index < row->operand_count; ++index) {
		const auto &operand =
			table::sme_encoding_operands[row->operand_begin + index];
		if (operand.kind != table::SmeOperandKind::gpr &&
		    operand.kind != table::SmeOperandKind::gpr_or_sp)
			continue;
		const auto first = table::sme_operand_number(operand, word);
		for (unsigned member = 0; member < operand.count; ++member)
			mask |= std::uint32_t{1}
				<< ((first + member * operand.stride) & 31U);
	}
	return mask;
}

[[nodiscard]] constexpr std::optional<std::uint32_t> data_immediate(
	std::uint32_t word) noexcept
{
	switch ((word >> 23) & 7U) {
	case 0: // PC-relative addressing
	case 1:
	case 5: // move wide
		return rd(word);
	case 7: // extract
		return rd(word) | rn(word) | rm(word);
	default: // add/subtract, min/max, logical, bitfield
		return rd(word) | rn(word);
	}
}

[[nodiscard]] constexpr std::optional<std::uint32_t> branch_system(
	std::uint32_t word) noexcept
{
	if ((word & 0x7c00'0000U) == 0x1400'0000U || // B, BL
	    (word & 0xff00'0000U) == 0x5400'0000U || // B.cond, BC.cond
	    (word & 0xff00'0000U) == 0xd400'0000U)   // exception generation
		return 0;
	if ((word & 0x7c00'0000U) == 0x3400'0000U) // CBZ, CBNZ, TBZ, TBNZ
		return rd(word);
	if ((word & 0xffc0'0000U) == 0xd500'0000U) // system; hints name x31
		return rd(word);
	if ((word & 0xfe00'0000U) == 0xd600'0000U) // branch (register)
		return rn(word) | rd(word);
	return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::uint32_t> load_store(
	std::uint32_t word) noexcept
{
	const bool vector = (word & (1U << 26)) != 0U;
	const auto group = (word >> 24) & 63U; // bits 29:24, V included
	const bool prefetch_form = (word >> 30) == 3U && !vector &&
		((word >> 22) & 3U) == 2U;
	const std::uint32_t transfer = vector ? 0U : rd(word);
	switch (group) {
	case 0b001000: { // exclusive, ordered, compare and swap
		/* Rt2 belongs to the exclusive pairs, Rs to the store-exclusive
		 * status and the compare and swaps; elsewhere both hold 31. */
		const bool o2 = (word & (1U << 23)) != 0U;
		const bool load = (word & (1U << 22)) != 0U;
		const bool o1 = (word & (1U << 21)) != 0U;
		const bool pair = (word >> 31) != 0U && o1 && !o2;
		const bool swap = o1 && (o2 || (word >> 31) == 0U);
		/* CASP names Rs and Rt with their odd partners. */
		const std::uint32_t partners = swap && !o2 ?
			(rm(word) | rd(word)) << 1 : 0U;
		return rn(word) | rd(word) | partners | (pair ? ra(word) : 0U) |
			((!o2 && !load) || swap ? rm(word) : 0U);
	}
	case 0b001100: // AdvSIMD multiple structures
	case 0b001101: // AdvSIMD single structure
		return rn(word) | ((word & (1U << 23)) != 0U ? rm(word) : 0U);
	case 0b011000: // load literal
	case 0b011100:
		return (word >> 30) == 3U && !vector ? 0U : transfer;
	case 0b011001: // RCpc unscaled, memory copy/set, memory tags
	case 0b011101: { // memory copy/set, bit 26 being o0
		const auto form = (word >> 10) & 3U;
		const bool bit21 = (word & (1U << 21)) != 0U;
		if (!bit21 && form == 1U)
			return rd(word) | rm(word) | rn(word);
		if (group == 0b011101U)
			return std::nullopt;
		if (!bit21 && form == 0U)
			return rn(word) | rd(word);
		if ((word >> 24) == 0xd9U && bit21)
			return rn(word) | rd(word);
		return std::nullopt;
	}
	default:
		break;
	}
	switch ((group >> 3) & 7U) { // bits 29:27
	case 0b101: // register pairs
		return rn(word) | (vector ? 0U : rd(word) | ra(word));
	case 0b111:
		break;
	default:
		return std::nullopt;
	}
	if ((word & (1U << 24)) != 0U) // unsigned immediate
		return rn(word) | (prefetch_form ? 0U : transfer);
	const auto form = (word >> 10) & 3U;
	if ((word & (1U << 21)) == 0U) // unscaled, indexed, unprivileged
		return rn(word) | (form == 0U && prefetch_form ? 0U : transfer);
	if (form == 0U) // atomic memory operations
		return vector ? std::nullopt :
			std::optional<std::uint32_t>{rm(word) | rn(word) | rd(word)};
	if (form == 2U) // register offset
		return rm(word) | rn(word) | (prefetch_form ? 0U : transfer);
	if (!vector && (word >> 30) == 3U) // pointer-authenticated load
		return rn(word) | rd(word);
	return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::uint32_t> data_register(
	std::uint32_t word) noexcept
{
	const auto op2 = (word >> 21) & 15U;
	const auto op3 = (word >> 10) & 63U;
	if ((word & (1U << 28)) == 0U) // logical and add/subtract, shifted
		return rd(word) | rn(word) | rm(word); // or extended
	if ((op2 & 8U) != 0U) // three sources
		return rd(word) | rn(word) | rm(word) | ra(word);
	switch (op2) {
	case 0b0110: // two sources, or one with an opcode in Rm
		return rd(word) | rn(word) |
			((word & (1U << 30)) != 0U ? 0U : rm(word));
	case 0b0000:
		if (op3 == 0U) // add/subtract with carry
			return rd(word) | rn(word) | rm(word);
		if ((op3 & 31U) == 1U || (op3 & 15U) == 2U) // RMIF, SETF
			return rn(word);
		return std::nullopt;
	case 0b0010: // conditional compare, register or immediate
		return rn(word) | ((op3 & 2U) != 0U ? 0U : rm(word));
	case 0b0100: // conditional select
		return rd(word) | rn(word) | rm(word);
	default:
		return std::nullopt;
	}
}

[[nodiscard]] constexpr std::optional<std::uint32_t> data_vector(
	std::uint32_t word) noexcept
{
	const auto opcode = (word >> 16) & 7U;
	if ((word & 0x5f20'0000U) == 0x1e00'0000U) { // FP and fixed point
		if (opcode <= 1U)
			return rd(word);
		if (opcode <= 3U)
			return rn(word);
		return std::nullopt;
	}
	if ((word & 0x5f20'fc00U) == 0x1e20'0000U) { // FP and integer
		if (opcode == 2U || opcode == 3U || opcode == 7U)
			return rn(word);
		return rd(word);
	}
	if ((word & 0x9fe0'8400U) == 0x0e00'0400U && // AdvSIMD copy
	    (word & (1U << 29)) == 0U) {
		switch ((word >> 11) & 15U) {
		case 0b0001: // DUP (general)
		case 0b0011: // INS (general)
			return rn(word);
		case 0b0101: // SMOV
		case 0b0111: // UMOV
			return rd(word);
		default:
			return 0;
		}
	}
	return 0;
}

} // namespace register_operands_detail

/* Bit n is set when the word names Xn or Wn (bit 31: the field holds 31,
 * SP or ZR). nullopt when no class the oracle knows holds the word. */
[[nodiscard]] constexpr std::optional<std::uint32_t> gpr_operands(
	std::uint32_t word) noexcept
{
	namespace detail = register_operands_detail;
	if (::uml::cpu::arm64::detail::sme_encoding_space(word))
		return detail::sve_sme(word);
	const auto op0 = (word >> 25) & 15U;
	if (op0 == 0U)
		return (word >> 16) == 0U ? std::optional<std::uint32_t>{0} :
			std::nullopt; // UDF; the rest is unallocated
	if ((op0 & 14U) == 8U)
		return detail::data_immediate(word);
	if ((op0 & 14U) == 10U)
		return detail::branch_system(word);
	if ((op0 & 5U) == 4U)
		return detail::load_store(word);
	if ((op0 & 7U) == 5U)
		return detail::data_register(word);
	if ((op0 & 7U) == 7U)
		return detail::data_vector(word);
	return std::nullopt;
}

inline constexpr unsigned darwin_platform_register = 18;

/* Whether the word names x18 or w18; nullopt for a word gpr_operands()
 * cannot classify. */
[[nodiscard]] constexpr std::optional<bool> names_platform_register(
	std::uint32_t word) noexcept
{
	const auto operands = gpr_operands(word);
	if (!operands)
		return std::nullopt;
	return (*operands & (std::uint32_t{1} << darwin_platform_register)) !=
		0U;
}

} // namespace uml::testing::a64
