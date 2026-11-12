// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/static_guest_memory.hpp"
#include "cpu/engine_factory.hpp"
#if DARWIN_UML_X86_64_GUEST
#include "cpu/x86_64/segments.hpp"
#include "runtime/x86_gadget_runtime.hpp"
#include "runtime/x86_runtime_support.hpp"
#endif
#if DARWIN_UML_X86_64_JIT
#include "runtime/x86_jit_runtime.hpp"
#endif
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/engine.hpp"
#include "cpu/floating_point_contract.hpp"
#include "runtime/aarch32_guest.hpp"
#include "runtime/context.hpp"
#include "runtime/execution_state.hpp"
#include "runtime/guest_entry.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::emulator {

namespace arm64 = ::uml::cpu::arm64;
#if DARWIN_UML_X86_64_GUEST
namespace x86_64 = ::uml::cpu::x86_64;
#endif
using ::uml::memory::StaticGuestMemory;
namespace {

#if DARWIN_UML_X86_64_GUEST && !DARWIN_UML_X86_64_USERSPACE_ONLY
constexpr std::int32_t x86_64_guest_isa = 2;
#endif
#if DARWIN_UML_X86_64_GUEST
constexpr std::uint32_t x86_64_fp_marker = 0x5838'3634U;
constexpr std::size_t x86_64_register_count = 16;
constexpr std::size_t x86_64_rflags_slot = 16;
constexpr std::size_t x86_64_fs_base_slot = 17;
constexpr std::size_t x86_64_gs_base_slot = 18;
constexpr std::size_t x86_64_x87_significand_slot = 19;
constexpr std::size_t x86_64_x87_exponent_slot = 27;
constexpr std::size_t x86_32_tls_entry_count = DARWIN_UML_COMPAT_TLS_COUNT;
constexpr std::uint64_t x86_64_compatibility_syscall = 1ULL << 32U;
/* The kernel's own mark of an i386 task, which it keeps in this bit and the
 * host hands back untouched: the execution mode follows CS. */
constexpr std::uint64_t x86_64_i386_task = 1ULL << 33U;
/* The x86 slice's PR_SET_TSC(PR_TSC_SIGSEGV) task state. */
constexpr std::uint64_t x86_64_timestamp_faults = 1ULL << 34U;
#endif

constexpr std::size_t dispatch_budget = 64 * 1024;
constexpr std::uint32_t pstate_nzcv = 0xf000'0000U;
constexpr std::uint32_t pstate_dit = 0x0100'0000U;
constexpr std::uint32_t pstate_daif = 0x0000'03c0U;
/* PSTATE.SS: the kernel asks for one instruction, as for a stepped eret. */
constexpr std::uint32_t pstate_ss = 0x0020'0000U;
/* PSTATE.nRW, set in the SPSR layout of an exception taken from AArch32. */
constexpr std::uint32_t pstate_mode32 = 0x0000'0010U;
/* Trap numbers the kernel turns into SIGTRAP/TRAP_TRACE: the x86 #DB vector
 * and the arm64 software-step exception class. */
#if DARWIN_UML_X86_64_GUEST
constexpr std::int32_t x86_64_debug_trap = 1;
constexpr std::uint32_t x86_64_bound_range = 5;
constexpr std::uint32_t x86_64_segment_not_present = 11;
constexpr std::int32_t x86_64_alignment_check = 17;
/* DR6.BS: the #DB is a single step. Reported as its error code, so Linux
 * tells a step from ICEBP, which leaves DR6 clear, as x86 Linux reads DR6. */
constexpr std::int32_t x86_64_debug_single_step = 1 << 14;
#endif
constexpr std::int32_t arm64_software_step = 0x32;
constexpr std::uint32_t public_floating_point_flags =
	DARWIN_UML_FLOATING_POINT_INVALID |
	DARWIN_UML_FLOATING_POINT_DIVIDE_BY_ZERO |
	DARWIN_UML_FLOATING_POINT_OVERFLOW |
	DARWIN_UML_FLOATING_POINT_UNDERFLOW |
	DARWIN_UML_FLOATING_POINT_INEXACT |
	DARWIN_UML_FLOATING_POINT_INPUT_DENORMAL;
static_assert(static_cast<int>(
		      ::uml::cpu::EngineEvent::floating_point_trap) ==
	      DARWIN_UML_EVENT_FLOATING_POINT_TRAP);
static_assert(static_cast<int>(::uml::cpu::EngineEvent::pc_alignment) ==
	      DARWIN_UML_EVENT_PC_ALIGNMENT);
static_assert(::uml::cpu::floating_point_status_exception_mask ==
	      public_floating_point_flags);
static_assert(::uml::cpu::floating_point_syndrome(0) ==
	      DARWIN_UML_FLOATING_POINT_SYNDROME_BASE);
/* The kernel's SME storage and a vCPU's register file share one layout. */
static_assert(DARWIN_UML_GUEST_SME_P_OFFSET(64, 0) == arm64::sme_p_offset(64, 0));
static_assert(DARWIN_UML_GUEST_SME_ZA_OFFSET(64, 0) ==
	      arm64::sme_za_offset(64, 0));
static_assert(DARWIN_UML_GUEST_SME_ZT0_SIZE == arm64::sme_zt0_bytes);
static_assert(DARWIN_UML_GUEST_SME_STATE_SIZE(16) == arm64::sme_storage_size(16));
static_assert(DARWIN_UML_GUEST_SME_STATE_SIZE(64) == arm64::sme_storage_size(64));
static_assert(DARWIN_UML_GUEST_SME_STATE_SIZE(256) ==
	      arm64::sme_storage_size(256));

/* The FPCR fields a restored context may set: those of an MSR FPCR on this
 * host, decided once from the host the engines translate for. */
[[nodiscard]] std::uint32_t restored_fpcr_mask() noexcept
{
	static const std::uint32_t mask = arm64::fpcr_writable_mask_for(
		::uml::platform::apple::probed_host_capabilities());
	return mask;
}

void load_cpu(arm64::CpuState &cpu,
	      const darwin_uml_guest_registers &registers,
	      const darwin_uml_guest_fp_registers &fp, std::uint64_t tls,
	      bool top_byte_ignore)
{
	cpu.tbi_enabled = top_byte_ignore;
	std::copy(std::begin(registers.x), std::end(registers.x),
		  cpu.registers.begin());
	cpu.registers[29] = registers.fp;
	cpu.registers[30] = registers.lr;
	cpu.sp = registers.sp;
	cpu.pc = registers.pc;
	cpu.nzcv = registers.pstate & pstate_nzcv;
	cpu.pstate_dit = (registers.pstate & pstate_dit) != 0;
	cpu.pstate_daif = static_cast<std::uint8_t>(
		(registers.pstate & pstate_daif) >> 6);
	cpu.tls = tls;
	::uml::runtime::clear_entry_monitor(cpu);
	cpu.defer_fp_load(fp.vectors, fp.fpcr, fp.fpsr, restored_fpcr_mask());
}

/* An arm64 task's SME tail, read only when the run implements SME, which is
 * when the vCPU has a register file. SVCR may name SM or ZA only for a task
 * with storage, and storage is sized for this run's streaming vector length:
 * both are kernel invariants, so a tail that breaks them is fatal. */
[[nodiscard]] bool load_sme(arm64::CpuState &cpu,
	const darwin_uml_guest_registers &registers,
	const darwin_uml_guest_sme_registers &sme) noexcept
{
	if ((registers.svcr & ~std::uint64_t{arm64::svcr_mask}) != 0 ||
		(sme.state == 0 ? registers.svcr != 0 : sme.svl != cpu.sme_svl))
		return false;
	cpu.defer_sme_load(reinterpret_cast<std::byte *>(sme.state),
		static_cast<std::uint8_t>(registers.svcr), sme.tpidr2);
	return true;
}

/* Inline: outlined, it adds a call to every return. */
[[gnu::always_inline]] inline void save_cpu(arm64::CpuState &cpu,
	      darwin_uml_guest_registers &registers,
	      darwin_uml_guest_fp_registers &fp,
	      darwin_uml_guest_sme_registers &sme, unsigned long &tls)
{
	::uml::cpu::materialize_architectural_state(cpu);
	std::copy_n(cpu.registers.begin(), 29, std::begin(registers.x));
	registers.fp = cpu.registers[29];
	registers.lr = cpu.registers[30];
	registers.sp = cpu.sp;
	registers.pc = cpu.pc;
	/* The virtual CPU implements no FEAT_SSBS, so the kernel's
	 * PSTATE.SSBS passes through untouched. */
	registers.pstate =
		(registers.pstate &
		 ~(pstate_nzcv | pstate_dit | pstate_daif)) |
		(cpu.nzcv & pstate_nzcv) |
		(cpu.pstate_dit ? pstate_dit : 0U) |
		(static_cast<std::uint32_t>(cpu.pstate_daif & 15U) << 6);
	registers.fpmr = 0;
	/* Without SME SVCR is zero and the tail is the kernel's alone. With
	 * it, the task gets back its SVCR, TPIDR2 and, ahead of the V copy
	 * below, whatever of its storage the engine exposed. */
	if (cpu.sme_registers != nullptr) [[unlikely]] {
		cpu.write_back_sme_state();
		registers.svcr = cpu.svcr;
		sme.tpidr2 = cpu.tpidr2;
	} else {
		registers.svcr = 0;
	}
	tls = static_cast<unsigned long>(cpu.tls);
	/* Like a context switch on hardware, report the FPCR and FPSR the task
	 * ran with: load_cpu masked them, and an untouched FP image still holds
	 * the values the kernel restored. */
	fp.fpcr = cpu.effective_fpcr();
	fp.fpsr = cpu.effective_fpsr();
	if (cpu.fp_dirty)
		std::memcpy(fp.vectors, cpu.vectors.data(), sizeof(fp.vectors));
}

#if DARWIN_UML_X86_64_GUEST
/* Loads the state the kernel hands over, and returns the #GP, #NP or #SS an
 * unusable CS or SS raises before any instruction runs. */
x86_64::SegmentFault load_x86_cpu(x86_64::CpuState &cpu,
	const darwin_uml_guest_registers &registers,
	const darwin_uml_guest_fp_registers &fp) noexcept
{
	std::copy_n(std::begin(registers.x), x86_64_register_count,
		cpu.registers.begin());
	cpu.timestamp_faults = (registers.svcr & x86_64_timestamp_faults) != 0;
	/* Linux runs userspace with IF set, and no user instruction can clear
	 * it (CLI and STI raise #GP; POPF and IRET keep it). The kernel sets IF
	 * and the reserved bit at execve and keeps both through sigreturn and
	 * tracer writes, which may change only the flags Linux lets them change.
	 * Setting both here too keeps that guarantee for any image the kernel
	 * hands over, and they reach the kernel on the way out. */
	cpu.rflags = registers.x[x86_64_rflags_slot] |
		x86_64::rflags_reserved | x86_64::rflags_interrupt;
	/* Bridge ABI 20: the TLS descriptors of every task, one struct
	 * user_desc each, slot i at GDT entry 12 + i; and in fp the address of
	 * the task's LDT, or zero. */
	static_assert(x86_32_tls_entry_count == std::tuple_size_v<
		decltype(cpu.tls_descriptors)>);
	for (std::size_t index = 0; index < x86_32_tls_entry_count; ++index)
		cpu.tls_descriptors[index] = x86_64::tls_descriptor(
			registers.compat_tls[index][0], registers.compat_tls[index][1]);
	cpu.ldt = reinterpret_cast<const std::uint64_t *>(registers.fp);
	cpu.fs_base = registers.x[x86_64_fs_base_slot];
	cpu.gs_base = registers.x[x86_64_gs_base_slot];
	cpu.deferred_flags = {};
	for (std::size_t index = 0; index < 4; ++index)
		cpu.segment_selectors[index] = static_cast<std::uint16_t>(
			registers.fpmr >> (index * 16U));
	for (std::size_t index = 4; index < 6; ++index)
		cpu.segment_selectors[index] = static_cast<std::uint16_t>(
			registers.svcr >> ((index - 4U) * 16U));
	cpu.system_call_32bit =
		(registers.svcr & x86_64_compatibility_syscall) != 0;
	if (fp.reserved[1] == x86_64_fp_marker) {
		static_assert(sizeof(cpu.vectors) == sizeof(fp.vectors));
		std::memcpy(cpu.vectors.data(), fp.vectors, sizeof(fp.vectors));
		cpu.mxcsr = fp.fpsr;
		cpu.x87_control = static_cast<std::uint16_t>(fp.fpcr);
		cpu.x87_status = static_cast<std::uint16_t>(fp.fpcr >> 16U);
		cpu.x87_tag = static_cast<std::uint16_t>(fp.reserved[0]);
		cpu.x87_top = static_cast<std::uint8_t>(fp.reserved[0] >> 16U);
		for (std::size_t index = 0; index < cpu.x87.size(); ++index) {
			cpu.x87[index].significand =
				registers.x[x86_64_x87_significand_slot + index];
			const auto packed = registers.x[
				x86_64_x87_exponent_slot + index / 4U];
			cpu.x87[index].sign_exponent = static_cast<std::uint16_t>(
				packed >> ((index % 4U) * 16U));
			cpu.mmx[index] = cpu.x87[index].significand;
		}
	} else {
		cpu.vectors = {};
		cpu.mmx = {};
		cpu.x87 = {};
		cpu.mxcsr = 0x1f80;
		cpu.x87_control = 0x037f;
		cpu.x87_status = 0;
		cpu.x87_tag = 0xffff;
		cpu.x87_top = 0;
	}
	/* The descriptor caches, and the mode, which follows CS. */
	const auto fault = x86_64::enter_segments(cpu);
	cpu.rip = x86_64::normalize_instruction_pointer(
		cpu.execution_mode, registers.pc);
	return fault;
}

void save_x86_cpu(x86_64::CpuState &cpu,
	darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp) noexcept
{
	x86_64::materialize_flags(cpu);
	std::copy(cpu.registers.begin(), cpu.registers.end(),
		std::begin(registers.x));
	registers.pc = x86_64::normalize_instruction_pointer(
		cpu.execution_mode, cpu.rip);
	registers.x[x86_64_rflags_slot] = cpu.rflags;
	registers.x[x86_64_fs_base_slot] = cpu.fs_base;
	registers.x[x86_64_gs_base_slot] = cpu.gs_base;
	for (std::size_t index = 0; index < cpu.x87.size(); ++index)
		registers.x[x86_64_x87_significand_slot + index] =
			cpu.x87[index].significand;
	for (std::size_t group = 0; group < 2; ++group) {
		std::uint64_t packed{};
		for (std::size_t index = 0; index < 4; ++index)
			packed |= static_cast<std::uint64_t>(
				cpu.x87[group * 4U + index].sign_exponent) <<
				(index * 16U);
		registers.x[x86_64_x87_exponent_slot + group] = packed;
	}
	registers.fpmr = 0;
	for (std::size_t index = 0; index < 4; ++index)
		registers.fpmr |= static_cast<std::uint64_t>(
			cpu.segment_selectors[index]) << (index * 16U);
	registers.svcr = static_cast<std::uint64_t>(cpu.segment_selectors[4]) |
		(static_cast<std::uint64_t>(cpu.segment_selectors[5]) << 16U) |
		(cpu.system_call_32bit ? x86_64_compatibility_syscall : 0U) |
		(registers.svcr & x86_64_i386_task) |
		(cpu.timestamp_faults ? x86_64_timestamp_faults : 0U);
	static_assert(sizeof(cpu.vectors) == sizeof(fp.vectors));
	std::memcpy(fp.vectors, cpu.vectors.data(), sizeof(fp.vectors));
	fp.fpsr = cpu.mxcsr;
	fp.fpcr = static_cast<std::uint32_t>(cpu.x87_control) |
		(static_cast<std::uint32_t>(cpu.x87_status) << 16U);
	fp.reserved[0] = static_cast<std::uint32_t>(cpu.x87_tag) |
		(static_cast<std::uint32_t>(cpu.x87_top) << 16U);
	fp.reserved[1] = x86_64_fp_marker;
}

/* A fault-class exception saves RFLAGS with RF set, so that the faulting
 * instruction does not raise its instruction breakpoint again when it is
 * resumed; a trap saves the flags as they are, and RF is clear by then.
 * Native Linux shows that image in the signal frame and to a tracer. */
void save_x86_fault_flags(darwin_uml_guest_registers &registers) noexcept
{
	registers.x[x86_64_rflags_slot] |= x86_64::rflags_resume;
}

/* The event a dispatch of x86 code leaves for the kernel. Always inlined,
 * so the gadget entry keeps its single body. */
[[gnu::always_inline]] inline int finish_x86_run(
	darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp, ::uml::runtime::Context &context,
	x86_64::CpuState &cpu, bool stepping,
	::uml::runtime::X86RunResult &run) noexcept
{
	if (stepping &&
		run.execution.reason == uml::cpu::ExitReason::instruction_budget)
		run.execution.reason = uml::cpu::ExitReason::single_step;
	const auto &result = run.execution;
	save_x86_cpu(cpu, registers, fp);
	int event = DARWIN_UML_EVENT_FATAL;
	switch (result.reason) {
	case uml::cpu::ExitReason::system_call:
		event = DARWIN_UML_EVENT_SYSCALL;
		break;
	case uml::cpu::ExitReason::memory_fault:
		if (result.fault.alignment) {
			/* #AC, a fault with an error code of zero, which x86 Linux
			 * delivers with no address. */
			registers.fault.error_code = 0;
			registers.fault.address = 0;
			registers.fault.trap_number = x86_64_alignment_check;
			save_x86_fault_flags(registers);
			event = DARWIN_UML_EVENT_DATA_ALIGNMENT;
			break;
		}
		registers.fault.error_code = result.fault.access ==
			uml::memory::MemoryAccess::write ? DARWIN_UML_FAULT_WRITE :
			result.fault.access == uml::memory::MemoryAccess::execute ?
				DARWIN_UML_FAULT_EXECUTE : 0;
		registers.fault.address = result.fault.address;
		registers.fault.trap_number = DARWIN_UML_FAULT_PAGE;
		save_x86_fault_flags(registers);
		event = DARWIN_UML_EVENT_FAULT;
		break;
	case uml::cpu::ExitReason::interrupt:
	case uml::cpu::ExitReason::instruction_budget:
		event = context.stop_requested() ?
			DARWIN_UML_EVENT_STOP : DARWIN_UML_EVENT_INTERRUPT;
		break;
	case uml::cpu::ExitReason::illegal_instruction:
		save_x86_fault_flags(registers);
		event = DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION;
		break;
	case uml::cpu::ExitReason::breakpoint:
		/* #DB from ICEBP, #BP and #OF are traps; #BR, #NP, #SS and #GP
		 * are faults. */
		registers.fault.error_code = run.error_code;
		registers.fault.address = cpu.rip;
		registers.fault.trap_number = static_cast<std::int32_t>(
			result.syndrome);
		if (result.syndrome == x86_64_bound_range ||
			result.syndrome == x86_64_segment_not_present ||
			result.syndrome == ::uml::runtime::x86_stack_segment_fault ||
			result.syndrome == ::uml::runtime::x86_general_protection)
			save_x86_fault_flags(registers);
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::single_step:
		registers.fault.error_code = x86_64_debug_single_step;
		registers.fault.address = cpu.rip;
		registers.fault.trap_number = x86_64_debug_trap;
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::floating_point_trap:
		/* #DE, #MF or #XM, each a fault. The kernel derives the code of
		 * #MF and #XM from the error code, then clears it. */
		registers.fault.error_code = run.error_code;
		registers.fault.address = cpu.rip;
		registers.fault.trap_number = static_cast<std::int32_t>(
			result.syndrome);
		save_x86_fault_flags(registers);
		event = DARWIN_UML_EVENT_FLOATING_POINT_TRAP;
		break;
	case uml::cpu::ExitReason::pc_alignment:
	case uml::cpu::ExitReason::sme_access:
	case uml::cpu::ExitReason::fatal:
		break;
	}
	return event;
}

#if DARWIN_UML_X86_64_JIT
/* The tier state of `generation` for this vCPU, or null when the run has no
 * tier threshold. The vCPU keeps the one it last used, which it asks the
 * table for again only when it moves to another address space. */
[[nodiscard]] ::uml::runtime::X86TierState *x86_tier_state(
	::uml::runtime::Context &context,
	::uml::runtime::ThreadExecutionState &runtime,
	unsigned long generation) noexcept
{
	auto *table = context.x86_tier_table(&::uml::runtime::x86_generation_live);
	if (table == nullptr)
		return nullptr;
	if (runtime.x86_tier == nullptr ||
	    runtime.x86_tier_generation != generation) {
		runtime.x86_tier = table->acquire(generation);
		runtime.x86_tier_generation = generation;
	}
	return runtime.x86_tier.get();
}
#endif

template<uml::cpu::EngineKind Kind>
int run_x86_guest(darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp,
	unsigned long generation, ::uml::runtime::Context &context,
	::uml::runtime::ThreadExecutionState &runtime) noexcept
{
	auto &cpu = runtime.x86_cpu;
	if (const auto fault = load_x86_cpu(cpu, registers, fp)) {
		/* Like IRET to a CS or SS it may not load, the entry faults with
		 * RIP and the selectors as the kernel handed them over. */
		::uml::runtime::X86RunResult run{};
		run.execution.reason = uml::cpu::ExitReason::breakpoint;
		run.execution.syndrome = fault.vector;
		run.error_code = static_cast<std::int32_t>(fault.error);
		return finish_x86_run(registers, fp, context, cpu, false, run);
	}
	runtime.x86_mm_shared =
		(registers.dispatch_flags & DARWIN_UML_DISPATCH_MM_SHARED) != 0;
	if (::uml::runtime::x86_mm_shared_logging()) [[unlikely]]
		::uml::runtime::note_x86_mm_shared(generation,
			runtime.x86_mm_shared, cpu.rip);
	/* RFLAGS.TF raises #DB after each instruction. Held to an exact
	 * budget the executor retires exactly that many, so a budget of one is
	 * a single step. */
	const bool stepping = (cpu.rflags & x86_64::rflags_trap) != 0;
	const auto &registered_interrupt = runtime.interrupt;
#if DARWIN_UML_X86_64_JIT
	/* The x86-64 product runs long64 and compat32 code on the JIT when
	 * its selection names it. The gadget instantiation, the product's
	 * default, compiles none of this. With a tier threshold the JIT takes an
	 * address space over only after it has retired that many instructions on
	 * the gadget engine. */
	::uml::runtime::X86TierState::Entry tier;
	if constexpr (Kind == uml::cpu::EngineKind::jit) {
		if (auto *state = x86_tier_state(context, runtime, generation)) {
			tier = state->enter([&]() noexcept {
				return context.stop_requested();
			});
			if (tier.abandoned()) {
				/* The run is stopping while the address space changes
				 * engine; the kernel sees an interrupt, as it does when
				 * one arrives before a dispatch. */
				::uml::runtime::X86RunResult run{};
				run.execution.reason = uml::cpu::ExitReason::interrupt;
				return finish_x86_run(registers, fp, context, cpu,
					stepping, run);
			}
		}
		if (tier.engine() == ::uml::runtime::X86TierState::Engine::jit) {
			auto run = ::uml::runtime::run_x86_jit(cpu, runtime.memory,
				runtime.x86_jit, generation,
				stepping ? 1 : dispatch_budget, {
				.sequence = registered_interrupt.sequence,
				.snapshot = registered_interrupt.snapshot,
				.stop = context.stop_token(),
			}, runtime.x86_mm_shared);
			return finish_x86_run(registers, fp, context, cpu, stepping,
				run);
		}
	}
#else
	(void)Kind;
#endif
	/* Everything else runs on the gadget engine: the x86-64 product's
	 * default and the arm64 product's x86 test guest. */
	auto run = ::uml::runtime::run_x86_gadgets(
		cpu, runtime.memory, runtime.x86_executor,
		runtime.x86_checked_executors, generation,
		stepping ? 1 : dispatch_budget, stepping, {
		.sequence = registered_interrupt.sequence,
		.snapshot = registered_interrupt.snapshot,
		.stop = context.stop_token(),
	});
#if DARWIN_UML_X86_64_JIT
	tier.leave(run.execution.retired_instructions);
#endif
	return finish_x86_run(registers, fp, context, cpu, stepping, run);
}
#endif

void save_floating_point_trap(
	darwin_uml_guest_registers &registers, std::uint64_t address,
	std::uint32_t syndrome) noexcept
{
	registers.fault.error_code = static_cast<std::int32_t>(syndrome);
	registers.fault.address = address;
	registers.fault.trap_number = 0;
}

[[nodiscard]] bool prepare_signal_return(arm64::CpuState &cpu) noexcept
{
	if (cpu.pc != DARWIN_UML_SIGRETURN_TRAMPOLINE)
		return false;
	cpu.registers[8] = DARWIN_UML_NR_RT_SIGRETURN;
	cpu.pc += sizeof(std::uint32_t);
	return true;
}

/* A thread's engine is created by its first entry; every later entry finds
 * it. Kept out of line and cold so that the entry's frame and hot path do not
 * carry the creation. */
template<uml::cpu::EngineKind Kind>
[[gnu::cold, gnu::noinline]] void create_common_engine(
	std::unique_ptr<uml::cpu::CpuEngine> &slot)
{
	slot = uml::cpu::make_cpu_engine(Kind,
		uml::cpu::EngineConfiguration{
			.host_capabilities =
				::uml::runtime::guest_host_capabilities(
					Kind == uml::cpu::EngineKind::jit ?
						::uml::runtime::ExecutionEngine::jit :
						::uml::runtime::ExecutionEngine::gadgets),
		});
}

template<uml::cpu::EngineKind Kind>
uml::cpu::CpuEngine *common_engine(
	::uml::runtime::ThreadExecutionState &runtime)
{
	auto &slot = runtime.engines.slot<Kind>();
	if (!slot) [[unlikely]]
		create_common_engine<Kind>(slot);
	return slot.get();
}

bool signal_return_boundary(arm64::CpuState &cpu, void *) noexcept
{
	return prepare_signal_return(cpu);
}

/* Debug builds: the host is never in streaming mode, nor has ZA on, where
 * the runtime enters an engine or an engine returns to it. */
void debug_check_host_streaming_state() noexcept
{
#ifndef NDEBUG
	arm64::debug_check_host_streaming_state(
		::uml::platform::apple::probed_host_capabilities());
#endif
}

/* An AArch32 task's entry (runtime/aarch32_guest.hpp): the engine's
 * run_aarch32() between load_a32_cpu() and save_a32_cpu(). A task is entered
 * only in a state the kernel may hand over; anything else is a kernel
 * invariant violation and fatal.
 *
 * Kept out of line and cold: an AArch64 entry reaches it only through the
 * one test of PSTATE.nRW in run_common_guest(). */
template<uml::cpu::EngineKind Kind>
[[gnu::cold, gnu::noinline]] int run_aarch32_guest(
	darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp,
	unsigned long generation, unsigned long *tls,
	::uml::runtime::Context &context,
	::uml::runtime::ThreadExecutionState &runtime)
{
	namespace a32 = ::uml::runtime::aarch32;
	if (!a32::entry_is_valid(registers))
		return DARWIN_UML_EVENT_FATAL;
	auto *engine = common_engine<Kind>(runtime);
	if (engine == nullptr)
		return DARWIN_UML_EVENT_FATAL;
	auto &cpu = runtime.cpu;
	a32::load_a32_cpu(cpu, registers, fp, *tls, restored_fpcr_mask());
	if constexpr (Kind != uml::cpu::EngineKind::gadgets)
		cpu.access_fp_state();
	const auto &interrupt = runtime.interrupt;
	/* No signal-return trampoline: a 32-bit task returns through its
	 * sigpage or its restorer, which are ordinary code. */
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = runtime.memory,
		.translated_memory = &runtime.memory,
		.generation = generation,
		.instruction_budget = dispatch_budget,
		.interrupt = {
			.sequence = interrupt.sequence,
			.snapshot = interrupt.snapshot,
			.stop = context.stop_token(),
		},
		.single_step = (registers.pstate & pstate_ss) != 0,
	};
	debug_check_host_streaming_state();
	const auto result = engine->run_aarch32(request);
	debug_check_host_streaming_state();
	::uml::cpu::materialize_architectural_state(cpu);
	a32::save_a32_cpu(cpu, registers, fp, *tls);
	return a32::exit_event(registers, cpu, result,
		result.reason == uml::cpu::ExitReason::interrupt &&
			context.stop_requested());
}

template<uml::cpu::EngineKind Kind>
int run_common_guest(
	void *register_pointer, void *fp_pointer,
	unsigned long fp_size, unsigned long generation, unsigned long *tls)
{
	if (register_pointer == nullptr || fp_pointer == nullptr || tls == nullptr ||
		fp_size < sizeof(darwin_uml_guest_fp_registers))
		return DARWIN_UML_EVENT_FATAL;
	auto &mutable_registers =
		*static_cast<darwin_uml_guest_registers *>(register_pointer);
	auto &fp = *static_cast<darwin_uml_guest_fp_registers *>(fp_pointer);
	auto *context = ::uml::runtime::current_context();
	if (context == nullptr)
		return DARWIN_UML_EVENT_FATAL;
	auto runtime = context->thread_execution_state();
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	return run_x86_guest<Kind>(mutable_registers, fp, generation,
		*context, *runtime);
#elif DARWIN_UML_X86_64_GUEST
	if (mutable_registers.padding == x86_64_guest_isa) {
		return run_x86_guest<Kind>(mutable_registers, fp, generation,
			*context, *runtime);
	}
#endif
	if constexpr (Kind == uml::cpu::EngineKind::jit) {
		if (!uml::cpu::cpu_engine_available(uml::cpu::EngineKind::jit))
			return DARWIN_UML_EVENT_FATAL;
	}
	/* An arm64 task's tail is its FP registers and then its SME
	 * registers, whether or not the run implements SME. */
	if (fp_size != DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE)
		return DARWIN_UML_EVENT_FATAL;
	/* A 32-bit task: PSTATE.nRW in the SPSR layout the kernel hands over. */
	if (mutable_registers.pstate & pstate_mode32) [[unlikely]]
		return run_aarch32_guest<Kind>(mutable_registers, fp,
			generation, tls, *context, *runtime);
	auto &sme = *reinterpret_cast<darwin_uml_guest_sme_registers *>(
		static_cast<std::byte *>(fp_pointer) +
		DARWIN_UML_ABI_GUEST_SME_REGISTERS_OFFSET);
	const auto &registers = *static_cast<const darwin_uml_guest_registers *>(
		register_pointer);
	if (registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
		auto &mutable_registers =
			*static_cast<darwin_uml_guest_registers *>(register_pointer);
		mutable_registers.x[8] = DARWIN_UML_NR_RT_SIGRETURN;
		mutable_registers.pc += sizeof(std::uint32_t);
		return DARWIN_UML_EVENT_SYSCALL;
	}
	auto &cpu = runtime->cpu;
	auto &memory = runtime->memory;
	load_cpu(cpu, mutable_registers, fp, *tls, context->top_byte_ignore());
	if (cpu.sme_registers != nullptr && !load_sme(cpu, mutable_registers, sme))
		[[unlikely]]
		return DARWIN_UML_EVENT_FATAL;
	if constexpr (Kind != uml::cpu::EngineKind::gadgets)
		cpu.access_fp_state();
	auto *engine = common_engine<Kind>(*runtime);
	if (engine == nullptr)
		return DARWIN_UML_EVENT_FATAL;

	const auto &interrupt = runtime->interrupt;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = dispatch_budget,
		.interrupt = {
			.sequence = interrupt.sequence,
			.snapshot = interrupt.snapshot,
			.stop = context->stop_token(),
		},
		.boundary = signal_return_boundary,
		.single_step = (mutable_registers.pstate & pstate_ss) != 0,
	};
	int event = DARWIN_UML_EVENT_FATAL;
	debug_check_host_streaming_state();
	const auto result = engine->run(request);
	debug_check_host_streaming_state();
	save_cpu(cpu, mutable_registers, fp, sme, *tls);

	switch (result.reason) {
	case uml::cpu::ExitReason::system_call:
		event = DARWIN_UML_EVENT_SYSCALL;
		break;
	case uml::cpu::ExitReason::memory_fault:
		event = ::uml::runtime::report_memory_fault(
			mutable_registers, result.fault);
		break;
	case uml::cpu::ExitReason::interrupt:
		if (context->stop_requested()) {
			event = DARWIN_UML_EVENT_STOP;
			break;
		}
		[[fallthrough]];
	case uml::cpu::ExitReason::instruction_budget:
		event = DARWIN_UML_EVENT_INTERRUPT;
		break;
	case uml::cpu::ExitReason::illegal_instruction:
		event = DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION;
		break;
	case uml::cpu::ExitReason::breakpoint:
		mutable_registers.fault.error_code = 0;
		mutable_registers.fault.address = cpu.pc;
		mutable_registers.fault.trap_number = 0;
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::single_step:
		mutable_registers.fault.error_code = 0;
		mutable_registers.fault.address = cpu.pc;
		mutable_registers.fault.trap_number = arm64_software_step;
		event = DARWIN_UML_EVENT_BREAKPOINT;
		break;
	case uml::cpu::ExitReason::floating_point_trap:
		save_floating_point_trap(
			mutable_registers, cpu.pc, result.syndrome);
		event = DARWIN_UML_EVENT_FLOATING_POINT_TRAP;
		break;
	case uml::cpu::ExitReason::pc_alignment:
		mutable_registers.fault.error_code = 0;
		mutable_registers.fault.address = result.fault.address;
		mutable_registers.fault.trap_number = 0;
		event = DARWIN_UML_EVENT_PC_ALIGNMENT;
		break;
	case uml::cpu::ExitReason::sme_access:
		/* Nothing retired: the kernel allocates the task's storage and
		 * enters again at the same PC. */
		event = DARWIN_UML_EVENT_SME_ACCESS;
		break;
	case uml::cpu::ExitReason::fatal:
		event = DARWIN_UML_EVENT_FATAL;
		break;
	}
	return event;
}

} // namespace
} // namespace uml::emulator

extern "C" int darwin_uml_static_guest_interrupt_sequence(
	const unsigned int *sequence, unsigned int snapshot)
{
	try {
		auto *context = ::uml::runtime::current_context();
		if (context == nullptr)
			return -1;
		auto runtime = context->thread_execution_state();
		runtime->interrupt.sequence = sequence;
		runtime->interrupt.snapshot = snapshot;
		return 0;
	} catch (...) {
		/* Registration is a checked transaction. Linux must not enter the
		 * guest after this failure, and an explicit retry replaces any prior
		 * token before execution can resume. */
		return -1;
	}
}

extern "C" int darwin_uml_jit_enter_guest(
	void *register_pointer, void *fp_pointer, unsigned long fp_size,
	unsigned long generation, unsigned long *tls)
{
	try {
		return uml::emulator::run_common_guest<
			uml::cpu::EngineKind::jit>(register_pointer, fp_pointer,
			fp_size, generation, tls);
	} catch (...) {
		return DARWIN_UML_EVENT_FATAL;
	}
}

extern "C" int darwin_uml_static_enter_guest(
	void *register_pointer, void *fp_pointer, unsigned long fp_size,
	unsigned long generation, unsigned long *tls)
{
	try {
		return uml::emulator::run_common_guest<
			uml::cpu::EngineKind::gadgets>(register_pointer, fp_pointer,
			fp_size, generation, tls);
	} catch (...) {
		return DARWIN_UML_EVENT_FATAL;
	}
}
