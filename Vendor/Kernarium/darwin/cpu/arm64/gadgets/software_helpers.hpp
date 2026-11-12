// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::gadgets {

/* A guest vector operand in a gadget stream is the byte offset of its register
 * inside CpuState. Every such offset fits a 16-bit operand field. */
[[nodiscard]] constexpr std::size_t vector_operand_offset(
	unsigned index) noexcept
{
	return offsetof(CpuState, vectors) + index * sizeof(VectorRegister);
}

static_assert(vector_operand_offset(31U) <= 0xffffU,
	"a vector operand offset must fit the 16-bit operand field");

/* The register such an offset names, as a software helper resolves it. */
[[nodiscard]] inline VectorRegister &vector_at(CpuState *cpu,
	std::uintptr_t offset) noexcept
{
	return *reinterpret_cast<VectorRegister *>(
		reinterpret_cast<std::byte *>(cpu) + offset);
}

} // namespace uml::cpu::arm64::gadgets
