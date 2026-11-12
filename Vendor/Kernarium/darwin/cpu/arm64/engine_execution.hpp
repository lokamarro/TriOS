/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/engine.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::detail {

/* What an entry loop takes from the ISA of the task it runs.  An Entry
 * supplies the PC an entry starts at and a fault reports, the checks of a PC
 * at a block boundary, the executor call that runs a block, and the syndrome
 * of a BKPT exit; the loop itself is shared (run_translated_entry).
 *
 * - A64: cpu.pc is the guest PC.
 * - AArch32: cpu.pc is a key (aarch32/guest_key.hpp) for the whole entry, and
 *   a fault address is the architectural 32-bit address the executor reports,
 *   never a key. */
struct EntryDefaults {
	/* Whether a floating-point trap or an SME access is an exit of its own
	 * (an AArch32 task's trap enables are RAZ/WI, and it has no streaming
	 * mode). */
	static constexpr bool trap_exits = true;

	/* The guest PC that cpu.pc names. */
	[[nodiscard]] static constexpr std::uint64_t guest_pc(
		std::uint64_t pc) noexcept
	{
		return pc;
	}

	/* Whether `pc` can be the PC of an entry of this ISA at all. */
	[[nodiscard]] static constexpr bool native_pc(std::uint64_t) noexcept
	{
		return true;
	}

	/* Whether `pc` is on an instruction boundary: AArch64 checks PC
	 * alignment at the next instruction boundary. */
	[[nodiscard]] static constexpr bool aligned_pc(std::uint64_t pc) noexcept
	{
		return (pc & (alignof(std::uint32_t) - 1U)) == 0U;
	}

	/* Runs once, after the execution scope is open and before the first
	 * block. */
	template<typename Executor>
	static void prepare(Executor &, CpuState &) noexcept
	{
	}
};

struct A64Entry : EntryDefaults {
	template<typename Executor>
	[[nodiscard]] static EngineEvent run_block(
		Executor &executor, CpuState &cpu,
		::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &instructions)
	{
		return executor.run_block(cpu, memory, generation, instructions);
	}

	template<typename Executor>
	[[nodiscard]] static std::uint32_t breakpoint_syndrome(
		Executor &, const ExecutionRequest &,
		::uml::memory::StaticGuestMemory &) noexcept
	{
		return 0;
	}
};

/* The AArch32 entry of an engine whose executor runs blocks of A32 and T32
 * code (the gadget engine's run_a32_block).  The only alignment fault left
 * is an A32 target with bit 1 set, whose address is the PC the key names. */
struct GadgetA32Entry : EntryDefaults {
	static constexpr bool trap_exits = false;

	[[nodiscard]] static constexpr std::uint64_t guest_pc(
		std::uint64_t key) noexcept
	{
		return aarch32::key_pc(key);
	}

	/* The executor's own state of an AArch32 entry: the CpuState slots its
	 * streams borrow beside R0-R14 (GadgetExecutor::enter_aarch32). */
	template<typename Executor>
	static void prepare(Executor &executor, CpuState &cpu) noexcept
	{
		executor.enter_aarch32(cpu);
	}

	template<typename Executor>
	[[nodiscard]] static EngineEvent run_block(
		Executor &executor, CpuState &cpu,
		::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &instructions)
	{
		return executor.run_a32_block(
			cpu, memory, generation, instructions);
	}

	template<typename Executor>
	[[nodiscard]] static std::uint32_t breakpoint_syndrome(
		Executor &executor, const ExecutionRequest &,
		::uml::memory::StaticGuestMemory &) noexcept
	{
		return executor.breakpoint_immediate();
	}
};

/* The translated engines intentionally share only this boundary adapter.  It
 * owns no translation state and does not select an engine, so each engine can
 * retain its own cache, compiler, and execution implementation. */
template<typename Entry, bool PollCallback, typename Executor>
[[nodiscard]] ExecutionResult run_translated_loop(
	Executor &executor, ExecutionRequest &request,
	::uml::memory::StaticGuestMemory &memory)
{
	if (!executor.begin_execution(
		    request.generation, Entry::guest_pc(request.cpu.pc)))
		return {.reason = ExitReason::fatal};
	struct ExecutionScope final {
		Executor &executor;

		~ExecutionScope() noexcept
		{
			executor.end_execution();
		}
	};
	[[maybe_unused]] const ExecutionScope execution_scope{executor};
	executor.set_single_step(request.single_step);
	Entry::prepare(executor, request.cpu);
	const auto interrupt_pending = [&]() noexcept {
		if (request.interrupt.state_pending())
			return true;
		if constexpr (PollCallback)
			return request.interrupt.callback_pending();
		return false;
	};

	const auto finish = [&](ExitReason reason, std::size_t retired,
					std::uint64_t address = 0,
					::uml::memory::MemoryAccess access =
						::uml::memory::MemoryAccess::read,
					std::uint32_t syndrome = 0,
					bool alignment = false) {
		/* Return a materialized architectural FP state. The application
		 * boundary also normalizes it before exporting a task frame, but the
		 * common engine contract must be stable for host callers too. */
		arm64::restore_native_fp_environment(request.cpu);
		return ExecutionResult{
			.reason = reason,
			.retired_instructions = retired,
			.fault = {.address = address, .access = access,
				.alignment = alignment},
			.syndrome = syndrome,
		};
	};

	std::size_t retired = 0;
	try {
		for (;;) {
			if (interrupt_pending())
				return finish(ExitReason::interrupt, retired);
			/* Keep the PC checks outside generated blocks: valid programs
			 * pay one cold block-boundary branch, while a misaligned
			 * indirect target retains the already-retired branch prefix and
			 * never reaches host code. */
			const std::uint64_t pc = request.cpu.pc;
			if (!Entry::native_pc(pc))
				return finish(ExitReason::fatal, retired);
			if (!Entry::aligned_pc(pc))
				return finish(ExitReason::pc_alignment, retired,
					Entry::guest_pc(pc),
					::uml::memory::MemoryAccess::execute);
			std::size_t block_instructions = 0;
			EngineEvent event;
			try {
				event = Entry::run_block(
					executor, request.cpu, memory, request.generation,
					block_instructions);
			} catch (...) {
				/* Executors publish only the committed prefix through the
				 * out-parameter. A cold cache/link postamble can fail after
				 * native work has advanced CpuState, so include that prefix
				 * before materializing the abnormal boundary. */
				retired += block_instructions;
				return finish(ExitReason::fatal, retired);
			}
			retired += block_instructions;
			if (executor.preempted())
				return finish(ExitReason::interrupt, retired);
			if (request.boundary != nullptr &&
				request.boundary(
					request.cpu, request.boundary_context))
				return finish(ExitReason::system_call, retired);
			/* A stepping executor runs one instruction per block. Its
			 * cold-mode boundary is not a guest-visible interrupt. */
			if (request.single_step && block_instructions != 0 &&
				(event == EngineEvent::continued ||
				 event == EngineEvent::interrupt))
				return finish(ExitReason::single_step, retired);
			if (event == EngineEvent::continued) {
				if (retired < request.instruction_budget)
					continue;
				return finish(ExitReason::instruction_budget, retired);
			}

			switch (event) {
			case EngineEvent::system_call:
				return finish(ExitReason::system_call, retired);
			case EngineEvent::memory_fault:
				return finish(ExitReason::memory_fault, retired,
					executor.fault_address(),
					::uml::memory::guest_access(
						executor.fault_access()), 0,
					executor.fault_alignment());
			case EngineEvent::interrupt:
				/* Translated blocks use the interrupt event as their ordinary
				 * cold-boundary/fairness exit. It becomes guest-visible only when
				 * an actual interrupt token or engine preemption is pending.
				 */
				if (interrupt_pending())
					return finish(ExitReason::interrupt, retired);
				if (retired < request.instruction_budget)
					continue;
				return finish(ExitReason::instruction_budget, retired);
			case EngineEvent::illegal_instruction:
				return finish(ExitReason::illegal_instruction, retired);
			case EngineEvent::breakpoint:
				return finish(ExitReason::breakpoint, retired, 0,
					::uml::memory::MemoryAccess::read,
					Entry::breakpoint_syndrome(
						executor, request, memory));
			case EngineEvent::floating_point_trap:
				if constexpr (Entry::trap_exits)
					return finish(ExitReason::floating_point_trap,
						retired, 0, ::uml::memory::MemoryAccess::read,
						executor.floating_point_syndrome());
				else
					return finish(ExitReason::fatal, retired);
			case EngineEvent::pc_alignment:
				return finish(ExitReason::pc_alignment, retired,
					Entry::guest_pc(request.cpu.pc),
					::uml::memory::MemoryAccess::execute);
			case EngineEvent::sme_access:
				if constexpr (Entry::trap_exits)
					return finish(ExitReason::sme_access, retired);
				else
					return finish(ExitReason::fatal, retired);
			case EngineEvent::overflow:
			case EngineEvent::general_protection:
			case EngineEvent::trap_flag:
			case EngineEvent::debug_trap:
			case EngineEvent::bound_range:
				/* x86 INTO, #GP, TF, ICEBP and BOUND are not arm64 engine
				 * events. */
				return finish(ExitReason::fatal, retired);
			case EngineEvent::fatal:
				return finish(ExitReason::fatal, retired);
			case EngineEvent::continued:
				break;
			}
		}
	} catch (...) {
		/* A later block may fail after earlier translated work left native
		 * architectural state live. Convert the exception here, while both the
		 * exact retired prefix and the request CpuState are still available.
		 * ExecutionScope subsequently balances the MMU execution lifetime. */
		return finish(ExitReason::fatal, retired);
	}
}

/* The entry of a task of the ISA `Entry` names, run to an exit the runtime
 * handles. */
template<typename Entry, typename Executor>
[[nodiscard]] ExecutionResult run_translated_entry(
	Executor &executor, ExecutionRequest &request)
{
	if (request.translated_memory == nullptr ||
		request.instruction_budget == 0)
		return {.reason = request.translated_memory == nullptr ?
			ExitReason::fatal : ExitReason::instruction_budget};
	/* Callback presence is stable for the whole guest entry. Production
	 * gadget/JIT requests use only sequence/atomic state, so their block loop
	 * contains no callback branch or indirect call. */
	if (request.interrupt.poll != nullptr)
		return run_translated_loop<Entry, true>(
			executor, request, *request.translated_memory);
	return run_translated_loop<Entry, false>(
		executor, request, *request.translated_memory);
}

template<typename Executor>
[[nodiscard]] ExecutionResult run_translated(
	Executor &executor, ExecutionRequest &request)
{
	return run_translated_entry<A64Entry>(executor, request);
}

/* The entry loop of an AArch32 task for an engine whose executor runs blocks
 * of A32 and T32 code (the gadget engine's run_a32_block). */
template<typename Executor>
[[nodiscard]] ExecutionResult run_translated_aarch32_blocks(
	Executor &executor, ExecutionRequest &request)
{
	return run_translated_entry<GadgetA32Entry>(executor, request);
}

} // namespace uml::cpu::arm64::detail
