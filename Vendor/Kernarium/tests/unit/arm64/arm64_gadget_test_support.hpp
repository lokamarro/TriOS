// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the gadget-structure tests compare a built program's words against:
 * a gadget's entry address and a vector register's CpuState offset. */

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/gadgets/afp_twin.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::emulator::tests {

/* An extern "C" gadget entry, as the tests declare them. */
using Gadget = void (*)();

[[nodiscard]] inline std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

/* The word a default Builder emits for a selected entry: where the virtual
 * CPU implements FEAT_AFP on the host capabilities it translates for, the
 * entry's FEAT_AFP twin, and the entry itself elsewhere. */
[[nodiscard]] inline std::uintptr_t emitted_entry(std::uintptr_t entry)
{
	return ::uml::cpu::arm64::implements_afp(
		       ::uml::cpu::host_capabilities()) ?
		::uml::cpu::arm64::gadgets::afp_twin(entry) : entry;
}

/* Computed here rather than with the builder's helper, so a packed operand
 * is checked against an independent value. */
[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg) noexcept
{
	return offsetof(::uml::cpu::arm64::CpuState, vectors) +
		reg * sizeof(::uml::cpu::arm64::VectorRegister);
}

} // namespace uml::emulator::tests
