// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/state_format.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/execution_types.hpp"
#include "runtime/guest_entry.hpp"
#include "uml_darwin_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::runtime::aarch32 {

namespace a32 = ::uml::cpu::arm64::aarch32;
namespace arm64 = ::uml::cpu::arm64;

/* How an AArch32 task enters an engine and leaves it again: the state the
 * guest kernel hands over (kernel/common/abi/darwin_uml_abi.h, "An AArch32
 * task"), the form CpuState holds it in for the whole entry, and the bridge
 * event each exit becomes. static_guest.cpp composes these around
 * CpuEngine::run_aarch32(); they are inline so that the entry costs no call,
 * and a header so that the tests can check every conversion without an
 * engine that runs AArch32. */

/* BKPT is the AArch32 breakpoint exception: EC 0x38, with the immediate in
 * the syndrome. */
inline constexpr std::int32_t breakpoint_exception_class = 0x38;
/* The software-step exception class the kernel turns into TRAP_TRACE; the
 * same value as for AArch64. */
inline constexpr std::int32_t software_step_class = 0x32;
/* The highest address an AArch32 task has. */
inline constexpr std::uint64_t a32_address_limit = 0xffff'ffffU;

/* Whether the kernel may hand `registers` over as an AArch32 entry. Every
 * condition is a kernel invariant: the mode is
 * User, E is clear (SETEND is undefined, so no AArch32 EL0 code runs
 * big-endian), IT bits come with T, and the task is not in streaming mode. An entry that
 * breaks one is fatal rather than run. */
[[nodiscard]] inline bool entry_is_valid(
	const darwin_uml_guest_registers &registers) noexcept
{
	const auto psr = registers.pstate;
	if ((psr & a32::psr_mode_mask) != a32::psr_mode_user ||
	    (psr & a32::psr_e) != 0U)
		return false;
	if ((psr & a32::psr_it) != 0U && (psr & a32::psr_t) == 0U)
		return false;
	return registers.svcr == 0;
}

/* Loads the state of a valid entry (entry_is_valid()) into `cpu`:
 * - R0-R14 into registers[0..14], zero-extended; the kernel's x15-x28, fp,
 *   lr and sp are no part of an AArch32 task and stay in the bridge;
 * - (pc, T) as the key of the whole entry;
 * - N, Z, C and V in nzcv, Q, the GE flags as their byte-lane mask, and
 *   ITSTATE;
 * - FPSCR across the status half in fpsr (with FPSCR.N, Z, C and V in
 *   a32_fpscr_nzcv) and the control half in fpcr, masked as the profile
 *   has it, with `fpcr_host_mask` the control bits the host takes;
 * - TPIDRURW from `tls` and TPIDRURO, and CP15BEN from the dispatch flags;
 * - no exclusive monitor and no top-byte ignore, which AArch32 lacks;
 * - no streaming mode.
 * The vector registers load lazily, as for AArch64. */
inline void load_a32_cpu(arm64::CpuState &cpu,
	const darwin_uml_guest_registers &registers,
	const darwin_uml_guest_fp_registers &fp, std::uint64_t tls,
	std::uint32_t fpcr_host_mask) noexcept
{
	const std::uint32_t psr = registers.pstate;
	cpu.a32 = 1;
	cpu.tbi_enabled = false;
	for (unsigned index = 0; index < 15; ++index)
		cpu.registers[index] = static_cast<std::uint32_t>(registers.x[index]);
	const auto pc = static_cast<std::uint32_t>(registers.pc);
	cpu.pc = (psr & a32::psr_t) != 0U ? a32::t32_key(pc) : a32::a32_key(pc);
	cpu.nzcv = psr & a32::psr_nzcv;
	cpu.a32_q = (psr & a32::psr_q) != 0U ? 1 : 0;
	cpu.a32_ge_mask = a32::psr_ge_mask(psr);
	cpu.a32_itstate = a32::psr_itstate(psr);
	cpu.a32_cp15ben =
		(registers.dispatch_flags & DARWIN_UML_DISPATCH_A32_CP15BEN) != 0 ?
			1 : 0;
	cpu.a32_tpidruro = static_cast<std::uint32_t>(registers.tpidrro);
	cpu.tls = static_cast<std::uint32_t>(tls);
	cpu.pstate_dit = false;
	cpu.pstate_daif = 0;
	::uml::runtime::clear_entry_monitor(cpu);
	cpu.svcr = 0;
	cpu.sme_loaded = 0;
	cpu.sme_dirty = 0;
	cpu.sme_task = nullptr;
	cpu.a32_fpscr_nzcv = fp.fpsr & a32::fpscr_nzcv;
	cpu.defer_fp_load(fp.vectors, fp.fpcr, fp.fpsr,
		fpcr_host_mask & a32::fpscr_control_mask);
}

/* The reverse of load_a32_cpu(), once the engine has returned and the
 * runtime has materialized the architectural state: R0-R14 zero-extended, the
 * PC and T from the key, the engine-owned fields of pstate repacked and the
 * kernel's own bits kept, FPSCR rejoined, and TPIDRURW. TPIDRURO is not
 * written back, since EL0 cannot write it, and nor is anything the entry did
 * not load. The engine's marker is cleared, so an AArch64 entry that follows
 * on the same vCPU never sees an AArch32 one. */
inline void save_a32_cpu(arm64::CpuState &cpu,
	darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp, unsigned long &tls) noexcept
{
	for (unsigned index = 0; index < 15; ++index)
		registers.x[index] = static_cast<std::uint32_t>(cpu.registers[index]);
	const std::uint64_t key = cpu.pc;
	registers.pc = a32::key_pc(key);
	registers.pstate = (registers.pstate & ~a32::psr_engine_owned) |
		(cpu.nzcv & a32::psr_nzcv) |
		(cpu.a32_q != 0 ? a32::psr_q : 0U) |
		a32::psr_ge_bits(cpu.a32_ge_mask) |
		a32::psr_it_bits(cpu.a32_itstate) |
		(a32::key_is_thumb(key) ? a32::psr_t : 0U);
	tls = static_cast<unsigned long>(
		static_cast<std::uint32_t>(cpu.tls));
	/* Like a context switch on hardware, report the FPSCR the task ran
	 * with; an untouched FP image still holds what the kernel restored. */
	fp.fpcr = cpu.effective_fpcr() & a32::fpscr_control_mask;
	fp.fpsr = (cpu.effective_fpsr() & arm64::fpsr_writable_mask) |
		(cpu.a32_fpscr_nzcv & a32::fpscr_nzcv);
	if (cpu.fp_dirty)
		std::memcpy(fp.vectors, cpu.vectors.data(), sizeof(fp.vectors));
	cpu.a32 = 0;
}

/* The bridge event an AArch32 exit becomes, with the fault record filled as
 * the kernel reads it. `cpu` is already saved, so cpu.pc is the exit's key.
 * - A system call leaves the PC after the SVC, with IT advanced, and is
 *   retired; BKPT is reported with its immediate in the error code and the
 *   BKPT32 exception class, and a completed single step with the
 *   software-step class, at the PC of the next instruction.
 * - A data abort, a PC alignment fault and a data alignment fault report
 *   the architectural 32-bit address the engine gives; an instruction fetch
 *   fault names the address that could not be read. An address wider than
 *   32 bits is no guest address but the engine's error, which the kernel's
 *   compat siginfo would truncate into an ordinary SIGSEGV, so it is the
 *   fatal event.
 * - An UNDEFINED instruction is the illegal-instruction event with the state
 *   of the instruction, which has done nothing.
 * An AArch32 task cannot reach a floating-point trap (the trap enables read
 * as zero) or an SME access, so both are fatal here. */
[[nodiscard]] inline int exit_event(darwin_uml_guest_registers &registers,
	const arm64::CpuState &cpu, const ::uml::cpu::ExecutionResult &result,
	bool stop_requested) noexcept
{
	using ::uml::cpu::ExitReason;
	switch (result.reason) {
	case ExitReason::system_call:
		return DARWIN_UML_EVENT_SYSCALL;
	case ExitReason::memory_fault:
		if (result.fault.address > a32_address_limit)
			break;
		return ::uml::runtime::report_memory_fault(registers, result.fault);
	case ExitReason::interrupt:
		if (stop_requested)
			return DARWIN_UML_EVENT_STOP;
		return DARWIN_UML_EVENT_INTERRUPT;
	case ExitReason::instruction_budget:
		return DARWIN_UML_EVENT_INTERRUPT;
	case ExitReason::illegal_instruction:
		return DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION;
	case ExitReason::breakpoint:
		registers.fault.error_code =
			static_cast<std::int32_t>(result.syndrome & 0xffffU);
		registers.fault.address = a32::key_pc(cpu.pc);
		registers.fault.trap_number = breakpoint_exception_class;
		return DARWIN_UML_EVENT_BREAKPOINT;
	case ExitReason::single_step:
		registers.fault.error_code = 0;
		registers.fault.address = a32::key_pc(cpu.pc);
		registers.fault.trap_number = software_step_class;
		return DARWIN_UML_EVENT_BREAKPOINT;
	case ExitReason::pc_alignment:
		if (result.fault.address > a32_address_limit)
			break;
		registers.fault.error_code = 0;
		registers.fault.address = result.fault.address;
		registers.fault.trap_number = 0;
		return DARWIN_UML_EVENT_PC_ALIGNMENT;
	case ExitReason::floating_point_trap:
	case ExitReason::sme_access:
	case ExitReason::fatal:
		break;
	}
	return DARWIN_UML_EVENT_FATAL;
}

} // namespace uml::runtime::aarch32
