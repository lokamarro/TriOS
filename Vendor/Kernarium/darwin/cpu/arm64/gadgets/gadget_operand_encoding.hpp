// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_selection_dependencies.hpp"

namespace uml::cpu::arm64::gadgets::selection {

/* CpuState register operands are byte offsets, not host pointers. Every
 * offset used by the gadget bank is bounded to 16 bits, so up to four of them
 * share one naturally aligned stream cell while gadget and link pointers stay
 * full width. */
[[nodiscard]] constexpr std::uintptr_t packed_u16_operands(
	std::uintptr_t first, std::uintptr_t second = 0,
	std::uintptr_t third = 0, std::uintptr_t fourth = 0) noexcept
{
	return first | (second << 16) | (third << 32) | (fourth << 48);
}

/* The composers' own copy of detail::sign_extend: this shift form folds to
 * SBFX, while detail's xor/subtract form costs a few more instructions at
 * every decode. Both agree for the masked fields the composers pass. */
[[nodiscard]] constexpr std::int64_t sign_extend(std::uint64_t value,
						 unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

/* The base register in bits 4:0, the transfer register in 9:5 and the signed
 * nine-bit byte offset in 18:10. */
[[nodiscard]] constexpr std::uintptr_t rcpc_memory_operand(
	std::uint32_t instruction, std::int64_t offset) noexcept
{
	return ((instruction >> 5) & 31U) |
		(static_cast<std::uintptr_t>(instruction & 31U) << 5) |
		((static_cast<std::uintptr_t>(static_cast<std::uint64_t>(offset)) &
		  0x1ffU) << 10);
}

/* The CpuState offset of a general register operand. Register 31 reads the
 * immutable zero slot here and is SP as a memory base. As a transfer or offset
 * register it selects a gadget entry of its own, which ignores the offset. */
[[nodiscard]] constexpr std::uintptr_t arithmetic_register_offset(
	unsigned index) noexcept
{
	static_assert(offsetof(CpuState, registers) == 0);
	static_assert(offsetof(CpuState, gadget_zero) <= 0xffffU);
	return index == 31U ? offsetof(CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t base_register_offset(
	unsigned index) noexcept
{
	return index == 31U ? offsetof(CpuState, sp) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t transfer_register_offset(
	unsigned index) noexcept
{
	return index == 31U ? 0U : index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t arithmetic_binary_operand(
	std::uint32_t instruction) noexcept
{
	return packed_u16_operands(
		arithmetic_register_offset(instruction & 31U),
		arithmetic_register_offset((instruction >> 5) & 31U),
		arithmetic_register_offset((instruction >> 16) & 31U));
}

[[nodiscard]] constexpr std::uintptr_t mops_operand(
	std::uint32_t instruction) noexcept
{
	/* MOPS never treats register 31 as SP. The only valid use is SET
	 * source XZR, which translation maps to the immutable zero slot. */
	return arithmetic_register_offset(instruction & 31U) |
		(arithmetic_register_offset((instruction >> 5) & 31U) << 16) |
		(arithmetic_register_offset((instruction >> 16) & 31U) << 32);
}

} // namespace uml::cpu::arm64::gadgets::selection
