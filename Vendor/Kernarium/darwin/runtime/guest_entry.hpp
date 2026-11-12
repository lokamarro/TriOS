// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/execution_types.hpp"
#include "uml_darwin_abi.h"

#include <cstdint>

namespace uml::runtime {

/* What an AArch64 and an AArch32 entry both start from: no exclusive
 * monitor. An exception return may clear the local monitor, and the bridge
 * has no reservation token in uml_pt_regs, so every field is cleared, with
 * the gadget scratch, and reusing the large CpuState cannot make a later
 * STXR succeed spuriously.
 *
 * The stores are spelled out, in place: through
 * CpuState::clear_exclusive_monitor() the optimizer schedules them
 * differently in the entry. */
[[gnu::always_inline]] inline void clear_entry_monitor(
	::uml::cpu::arm64::CpuState &cpu) noexcept
{
	cpu.exclusive_address = ~std::uint64_t{0};
	cpu.exclusive_key = 0;
	cpu.exclusive_value = 0;
	cpu.exclusive_value2 = 0;
	cpu.exclusive_version = 0;
	cpu.exclusive_size = 0;
	cpu.gadget_zero = 0;
	cpu.gadget_indirect_miss = nullptr;
}

/* The fault record of a memory-fault exit, filled as the kernel reads it, and
 * the event the exit becomes: the access is the error code, the trap is the
 * page fault, and an alignment fault is reported as such. The x86 exits
 * write their own record, since this form changes the code of their
 * entries. */
[[nodiscard]] inline int report_memory_fault(
	darwin_uml_guest_registers &registers,
	const ::uml::cpu::FaultInfo &fault) noexcept
{
	using ::uml::memory::MemoryAccess;
	switch (fault.access) {
	case MemoryAccess::read:
		registers.fault.error_code = 0;
		break;
	case MemoryAccess::write:
		registers.fault.error_code = DARWIN_UML_FAULT_WRITE;
		break;
	case MemoryAccess::execute:
		registers.fault.error_code = DARWIN_UML_FAULT_EXECUTE;
		break;
	}
	registers.fault.address = fault.address;
	registers.fault.trap_number = DARWIN_UML_FAULT_PAGE;
	return fault.alignment ? DARWIN_UML_EVENT_DATA_ALIGNMENT :
		DARWIN_UML_EVENT_FAULT;
}

} // namespace uml::runtime
