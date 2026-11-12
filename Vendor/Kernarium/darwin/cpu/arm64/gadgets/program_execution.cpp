// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_entry.hpp"
#include "program.hpp"
#include "sme_layout.h"

#include <algorithm>
#include <utility>

namespace uml::cpu::arm64::gadgets {
namespace {

/* A fault in the entered block itself resolves here. One in a linked
 * successor left from a stream this program does not hold, and the executor,
 * which can find that successor, resolves it. */
void resolve_entered_fault(const Program &program, CpuState &cpu,
	::uml::cpu::EngineEvent event) noexcept
{
	if (event == ::uml::cpu::EngineEvent::memory_fault &&
	    program_contains_stream(program, cpu.gadget_indirect_miss))
		resolve_fault_position(program, cpu.pc);
}

/* The executor's C++ ran since the program last left, and its locks and
 * atomics may have cleared the host's exclusive monitor, which is the
 * guest's. A live record the load-exclusive left is marked, so that a
 * store-exclusive whose host store then fails completes from the record
 * (engine/exclusive.inc): only an exception may cost the guest its monitor,
 * and entering a program again is none. The mark lives only while the
 * program runs: unmark_exclusive_reentry() takes off a mark this call set
 * when the program returns, so the record a run leaves is the one its
 * load-exclusive wrote, or the one it was given. */
[[nodiscard]] bool mark_exclusive_reentry(CpuState &cpu) noexcept
{
	if (cpu.exclusive_key == 0U ||
	    (cpu.exclusive_key & exclusive_key_reentered) != 0U)
		return false;
	cpu.exclusive_key |= exclusive_key_reentered;
	return true;
}

void unmark_exclusive_reentry(CpuState &cpu, bool marked) noexcept
{
	if (marked)
		cpu.exclusive_key &= ~exclusive_key_reentered;
}

} // namespace

Program::Program(std::uint64_t guest_address, std::size_t instruction_count,
		 ProgramWords words,
		 bool accesses_fp_state) noexcept
	: guest_address_{guest_address}, instruction_count_{instruction_count},
	  words_{std::move(words)}, accesses_fp_state_{accesses_fp_state}
{}
::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb) noexcept
{
	std::size_t executed = 0;
	return execute(program, cpu, tlb, executed);
}

/* The per-block form below with the host environment handed back; fault
 * resolution touches only the PC and the exit scratch, so doing it first does
 * not matter. */
::uml::cpu::EngineEvent execute(const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	      std::size_t &executed) noexcept
{
	const auto event = execute_preserving_native_fp_environment(
		program, cpu, tlb, executed);
	restore_native_fp_environment(cpu);
	return event;
}

::uml::cpu::EngineEvent execute_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed) noexcept
{
	executed = 0;
	if (program.words().empty())
		return ::uml::cpu::EngineEvent::fatal;
	if (program.accesses_fp_state())
		cpu.access_fp_state();
	const bool marked = mark_exclusive_reentry(cpu);
	const auto result =
		uml_arm64_gadget_enter(program.words().data(), &cpu, &tlb,
			program.instruction_count()
#if UML_GADGET_EXECUTION_HISTOGRAM
			, diagnostics::execution_counters()
#endif
			);
	unmark_exclusive_reentry(cpu, marked);
	executed = result.executed;
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	resolve_entered_fault(program, cpu, event);
	return event;
}

::uml::cpu::EngineEvent execute_streaming_preserving_native_fp_environment(
	const Program &program, CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::size_t &executed, bool zt0) noexcept
{
	executed = 0;
	if (program.words().empty())
		return ::uml::cpu::EngineEvent::fatal;
	cpu.sme_host_mode = zt0 ? UML_GADGET_SME_HOST_ZT0 : 0U;
	const bool marked = mark_exclusive_reentry(cpu);
	const auto result =
		uml_arm64_gadget_enter_streaming(program.words().data(), &cpu,
			&tlb, program.instruction_count()
#if UML_GADGET_EXECUTION_HISTOGRAM
			, diagnostics::execution_counters()
#endif
			);
	cpu.sme_host_mode = 0;
	unmark_exclusive_reentry(cpu, marked);
	executed = result.executed;
	const auto event = static_cast<::uml::cpu::EngineEvent>(result.event);
	resolve_entered_fault(program, cpu, event);
	return event;
}

} // namespace uml::cpu::arm64::gadgets
