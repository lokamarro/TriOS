// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <cstdint>

/*
 * Test-local reference semantics for the instruction families that the gadget
 * dependency tests generate: integer data processing, CRC32, conditional
 * branches, SVC and NOP, plus the vector ORR, ADD, MOVI and UMOV forms that
 * frame vector forwarding cases. It follows the Arm pseudocode and shares no
 * code with an engine. Every other word is reported as unsupported.
 */
namespace arm64_gadget_dependency_reference {

enum class Step {
	continued,
	system_call,
	unsupported,
};

[[nodiscard]] Step execute(uml::cpu::arm64::CpuState &cpu,
	std::uint32_t instruction);

} // namespace arm64_gadget_dependency_reference
