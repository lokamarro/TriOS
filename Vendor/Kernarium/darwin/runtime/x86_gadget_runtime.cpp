// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/x86_gadget_runtime.hpp"

#if DARWIN_UML_X86_64_GUEST

#include "cpu/x86_64/gadgets/exception_checks.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "runtime/x86_runtime_support.hpp"
#include "platform/apple/host_features.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace uml::runtime {
namespace {

namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::StaticGuestMemory;

/* The most instructions the builder puts in one block. */
constexpr std::size_t block_instructions = 64;

bool execution_interrupt_boundary(void *context) noexcept
{
	const auto *execution =
		static_cast<const X86StaticExecutionScope *>(context);
	return execution != nullptr &&
		execution->sequences().execution_interrupt != 0;
}

[[nodiscard]] int generation_live(unsigned long generation) noexcept
{
	return darwin_uml_static_vm_generation_live(generation);
}

[[nodiscard]] x86_64::gadgets::ProgramCacheSequences cache_sequences(
	void *context) noexcept
{
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = sequences.code_changes,
		.interrupted = sequences.execution_interrupt != 0,
	};
}

/* The address space keeps a short history of the ranges its code changes
 * touched, which is what lets the cache retire the translations covering them
 * instead of every link it holds. */
[[nodiscard]] int cache_code_changes(void *context, std::uint64_t sequence,
	x86_64::gadgets::ProgramCodeChange *changes, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, changes, capacity, current_sequence);
}

/* The same history for mappings: a reader that is only a few changes behind
 * retires the software-TLB entries those ranges cover instead of every entry
 * it holds. */
[[nodiscard]] int cache_mapping_changes(void *, std::uint64_t generation,
	std::uint64_t sequence, x86_64::gadgets::ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_mapping_changes(generation, sequence,
		changes, capacity, current_sequence);
}

[[nodiscard]] bool fatal_trace_enabled() noexcept
{
	const auto *value = std::getenv("UML_X86_FATAL_TRACE");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void trace_fatal(const char *stage, const x86_64::CpuState &cpu,
	unsigned long generation, int error = 0, std::size_t retired = 0,
	StaticGuestMemory *memory = nullptr) noexcept
{
	if (!fatal_trace_enabled())
		return;
	std::fprintf(stderr,
		"Darwin x86 runtime fatal stage=%s rip=%#llx mode=%u "
		"generation=%lu errno=%d retired=%zu",
		stage, static_cast<unsigned long long>(cpu.rip),
		static_cast<unsigned>(cpu.execution_mode), generation, error,
		retired);
	if (memory != nullptr) {
		std::array<std::byte, 15> bytes{};
		if (memory->fetch(cpu.rip, bytes)) {
			std::fputs(" bytes=", stderr);
			for (const auto byte : bytes)
				std::fprintf(stderr, "%02x",
					std::to_integer<unsigned>(byte));
		} else {
			std::fputs(" bytes=<unreadable>", stderr);
		}
	}
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

/* The engine keeps an unaligned locked operand inside one 16-byte granule on
 * its exclusive path, which needs FEAT_LSE2. Every Apple-silicon Mac has it;
 * on a host without it no x86 code runs. Decided, and reported, once. */
[[nodiscard]] bool host_runs_x86_engine() noexcept
{
	static const bool supported = [] {
		const bool lse2 =
			::uml::platform::apple::probed_host_capabilities().has(
				::uml::platform::ArmFeature::lse2);
		if (!lse2)
			std::fputs("Darwin x86 runtime: the host lacks FEAT_LSE2, "
				"which the x86-64 engine requires\n", stderr);
		return lse2;
	}();
	return supported;
}

/* The #GP error code of the instruction at RIP. INT n that user mode may
 * not use names its IDT entry, (n << 3) | 2, as the hardware pushes when
 * the gate refuses it; every other #GP the engine raises has none. */
[[nodiscard]] std::int32_t general_protection_error(
	StaticGuestMemory &memory, const x86_64::CpuState &cpu) noexcept
{
	const auto decoded = x86_64::decode(memory, cpu.rip, cpu.execution_mode);
	if (decoded.status != x86_64::DecodeStatus::decoded)
		return 0;
	const auto &instruction = decoded.instruction;
	if (instruction.operation != x86_64::Operation::privileged ||
		instruction.vector_opcode != 0xcd)
		return 0;
	return static_cast<std::int32_t>((instruction.source.immediate << 3U) | 2U);
}

/* A run of an executor, and of the checked executors it made, brackets the
 * time it may be inside translated code, and handling the exit of one, which
 * can follow a link into a body that only the address space holds, so that
 * executors sharing an address space free no body this one may be inside of.
 * A checked executor made during the run is bracketed when it ends. */
class RunBracket final {
public:
	RunBracket(x86_64::gadgets::Executor &executor,
		X86CheckedExecutors &checked_executors) noexcept
		: executor_{executor}, checked_executors_{checked_executors}
	{
		executor_.begin_run();
		for (auto &checked_executor : checked_executors_)
			if (checked_executor)
				checked_executor->begin_run();
	}
	~RunBracket()
	{
		executor_.end_run();
		for (auto &checked_executor : checked_executors_)
			if (checked_executor)
				checked_executor->end_run();
	}
	RunBracket(const RunBracket &) = delete;
	RunBracket &operator=(const RunBracket &) = delete;

private:
	x86_64::gadgets::Executor &executor_;
	X86CheckedExecutors &checked_executors_;
};

} // namespace

static_assert((x86_64::gadgets::check_x87 | x86_64::gadgets::check_simd |
	x86_64::gadgets::check_alignment) <
	std::tuple_size_v<X86CheckedExecutors>,
	"every set of checked exceptions names its own executor");

X86RunResult run_x86_gadgets(x86_64::CpuState &cpu,
	StaticGuestMemory &memory, x86_64::gadgets::Executor &executor,
	X86CheckedExecutors &checked_executors,
	unsigned long generation, std::size_t instruction_budget,
	bool exact_budget, const ::uml::cpu::InterruptToken &interrupt) noexcept
{
	X86RunResult run{};
	auto &result = run.execution;
	result.reason = ::uml::cpu::ExitReason::fatal;
	if (!host_runs_x86_engine())
		return run;
	/* Before anything of the run: the executors let go of the address spaces
	 * of generations the kernel has destroyed since they last looked. No
	 * translated code of theirs is running, and each prepares again below. */
	const auto retirements = darwin_uml_static_vm_generation_retirements();
	executor.leave_gone_generations(retirements, &generation_live);
	for (auto &checked_executor : checked_executors)
		if (checked_executor)
			checked_executor->leave_gone_generations(
				retirements, &generation_live);
	X86StaticExecutionScope execution{generation, cpu.rip};
	if (!execution.entered()) {
		trace_fatal("execution-scope", cpu, generation, errno);
		return run;
	}
	const RunBracket bracket{executor, checked_executors};
	auto guarded_interrupt = interrupt;
	guarded_interrupt.poll = execution_interrupt_boundary;
	guarded_interrupt.poll_context = &execution;
	/* What each executor was last prepared for: the ordinary one, then the
	 * checked one for each set of checked exceptions. */
	struct Prepared final {
		bool done{};
		std::uint64_t mapping_changes{};
		std::uint64_t code_changes{};
	};
	std::array<Prepared, std::tuple_size_v<X86CheckedExecutors>> prepared{};
	/* Set once POPF or IRET arms RFLAGS.TF: the run ends after one more
	 * instruction and reports the #DB that instruction raises. */
	bool trap_armed = false;
	const auto budget_reason = [&]() noexcept {
		return trap_armed ? ::uml::cpu::ExitReason::single_step :
			::uml::cpu::ExitReason::instruction_budget;
	};
	/* A long-mode access to a non-canonical address never reaches the page
	 * tables: x86 raises #GP or #SS for it, not #PF. A branch to one faults
	 * at its target here rather than at the branch. */
	const auto memory_fault = [&]() noexcept {
		if (cpu.execution_mode == x86_64::ExecutionMode::long64 &&
			!x86_canonical(result.fault.address)) {
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = result.fault.access ==
				::uml::memory::MemoryAccess::execute ?
				x86_general_protection : x86_noncanonical_vector(memory, cpu);
			result.fault = {};
			return;
		}
		result.reason = ::uml::cpu::ExitReason::memory_fault;
	};
	/* What a run of the engine left: true to go on, false to return. */
	const auto handle = [&](const x86_64::gadgets::ExecuteResult &executed,
		bool stepping) noexcept -> bool {
		result.retired_instructions += executed.retired_instructions;
		result.fault = executed.fault;
		if (executed.instruction_budget_exhausted) {
			result.reason = budget_reason();
			return false;
		}
		if (stepping && executed.repeat_yielded) {
			/* An iteration that did not end its instruction is a whole
			 * step: #DB follows it with RIP naming the instruction and,
			 * as the gadget leaves it, RF set, as it follows each
			 * iteration on x86. */
			result.reason = budget_reason();
			return false;
		}
		switch (executed.event) {
		case ::uml::cpu::EngineEvent::continued:
			/* A repeated string instruction retires once however many
			 * elements it moves, and one its repeat budget stopped has
			 * retired nothing yet, so a loop of long ones would never spend
			 * this budget. The kernel services its timer only when a
			 * dispatch returns, so charge the work the repeat budget covered
			 * instead. A step keeps its exact budget. */
			if (executed.repeat_yielded && !exact_budget && !trap_armed)
				instruction_budget -= std::min(
					instruction_budget - result.retired_instructions,
					x86_64::gadgets::string_repeat_budget);
			return true;
		case ::uml::cpu::EngineEvent::trap_flag:
			/* With no budget left for the next instruction, the next entry
			 * sees TF and steps it instead. */
			if (result.retired_instructions < instruction_budget) {
				instruction_budget = result.retired_instructions + 1;
				trap_armed = true;
			}
			return true;
		case ::uml::cpu::EngineEvent::system_call:
			result.reason = ::uml::cpu::ExitReason::system_call;
			return false;
		case ::uml::cpu::EngineEvent::memory_fault:
			memory_fault();
			return false;
		case ::uml::cpu::EngineEvent::interrupt:
			result.reason = ::uml::cpu::ExitReason::interrupt;
			return false;
		case ::uml::cpu::EngineEvent::illegal_instruction:
			result.reason = ::uml::cpu::ExitReason::illegal_instruction;
			trace_fatal("execute-illegal", cpu, generation, 0,
				result.retired_instructions, &memory);
			return false;
		case ::uml::cpu::EngineEvent::breakpoint:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 3;
			return false;
		case ::uml::cpu::EngineEvent::overflow:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 4;
			return false;
		case ::uml::cpu::EngineEvent::general_protection:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			if (executed.vector != 0) {
				/* The segment model's #NP, #SS or #GP. */
				result.syndrome = executed.vector;
				run.error_code = static_cast<std::int32_t>(executed.error);
				return false;
			}
			result.syndrome = x86_general_protection;
			run.error_code = general_protection_error(memory, cpu);
			return false;
		case ::uml::cpu::EngineEvent::debug_trap:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 1;
			return false;
		case ::uml::cpu::EngineEvent::bound_range:
			result.reason = ::uml::cpu::ExitReason::breakpoint;
			result.syndrome = 5;
			return false;
		case ::uml::cpu::EngineEvent::floating_point_trap:
			result.reason = ::uml::cpu::ExitReason::floating_point_trap;
			result.syndrome = executed.vector;
			run.error_code = static_cast<std::int32_t>(executed.error);
			return false;
		case ::uml::cpu::EngineEvent::pc_alignment:
		case ::uml::cpu::EngineEvent::sme_access:
		case ::uml::cpu::EngineEvent::fatal:
			trace_fatal("execute", cpu, generation, 0,
				result.retired_instructions);
			return false;
		}
	return false;
	};
	while (result.retired_instructions < instruction_budget) {
		const auto sequences = execution.sequences();
		if (sequences.execution_interrupt != 0) {
			result.reason = ::uml::cpu::ExitReason::interrupt;
			return run;
		}
		memory.prepare(generation, sequences.mapping_changes);
		/* #AC, #XM and a pending #MF exist only while the guest enables
		 * them, and only then does the run check the instructions that
		 * could raise one, with an executor of its own for each set. Its
		 * blocks were built for exactly that set: one built for more would
		 * run singly what the gadgets check themselves. */
		const auto checked = x86_64::gadgets::checked_exceptions(cpu);
		auto *active = &executor;
		auto *state = &prepared[0];
		if (checked != 0) {
			auto &checked_executor = checked_executors[checked];
			if (!checked_executor) {
				try {
					checked_executor =
						std::make_unique<x86_64::gadgets::Executor>();
				} catch (...) {
					trace_fatal("checked-executor", cpu, generation, 0,
						result.retired_instructions);
					return run;
				}
				checked_executor->check_exceptions(checked);
			}
			active = checked_executor.get();
			state = &prepared[checked];
		}
		if (!state->done ||
			state->mapping_changes != sequences.mapping_changes ||
			state->code_changes != sequences.code_changes) {
			active->prepare(memory, generation, sequences.mapping_changes,
				sequences.code_changes, execution.context(), cache_sequences,
				cache_code_changes, cache_mapping_changes);
			state->mapping_changes = sequences.mapping_changes;
			state->code_changes = sequences.code_changes;
			state->done = true;
		}
		if (cpu.rip == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
			x86_signal_return(cpu, result);
			return run;
		}
		const auto available = instruction_budget - result.retired_instructions;
		/* A task whose ES, SS or DS has a base or a limit runs one
		 * instruction at a time after the exact check of its accesses. */
		if (cpu.execution_mode == x86_64::ExecutionMode::compat32 &&
			!cpu.flat_segments) {
			const bool stepping = exact_budget || trap_armed;
			const auto stepped = x86_64::gadgets::segment_step(cpu, memory,
				*active, guarded_interrupt, checked,
				stepping ? 1U : x86_64::gadgets::string_repeat_budget);
			state->code_changes = active->prepared_code_changes();
			if (!handle(stepped, stepping))
				return run;
			continue;
		}
		/* Only a step has to stop inside a block. Cutting the block that
		 * crosses an ordinary budget would decode it again for a truncated
		 * copy, which the program keeps -- a different block every
		 * dispatch, and on a busy guest more bytes than the streams -- and
		 * would build short blocks that a later full budget throws away. */
		const std::size_t limit = exact_budget || trap_armed ? available :
			std::max<std::size_t>(available, block_instructions);
		const auto selected = active->select_program(
			memory, cpu.rip, cpu.execution_mode,
			std::min(block_instructions, limit));
		switch (selected.status) {
		case x86_64::gadgets::BuildStatus::fetch_fault:
			result.fault = {
				selected.fault_address,
				::uml::memory::MemoryAccess::execute};
			memory_fault();
			return run;
		case x86_64::gadgets::BuildStatus::illegal_instruction:
			result.reason = ::uml::cpu::ExitReason::illegal_instruction;
			trace_fatal("build-illegal", cpu, generation, 0,
				result.retired_instructions, &memory);
			return run;
		case x86_64::gadgets::BuildStatus::allocation_failure:
			trace_fatal("build-allocation", cpu, generation, errno,
				result.retired_instructions);
			return run;
		case x86_64::gadgets::BuildStatus::built:
			break;
		}
		if (selected.program == nullptr) {
			trace_fatal("select-program", cpu, generation, errno,
				result.retired_instructions);
			return run;
		}
		/* A step of a repeated string instruction is one iteration. */
		const bool stepping = (exact_budget || trap_armed) && limit == 1;
		const auto executed = checked == 0 ?
			x86_64::gadgets::execute(*selected.program, cpu, memory,
				executor, limit, guarded_interrupt, stepping) :
			x86_64::gadgets::execute_checked(*selected.program, cpu,
				memory, *active, limit, guarded_interrupt, stepping,
				checked);
		state->code_changes = active->prepared_code_changes();
		if (!handle(executed, stepping))
			return run;
	}
	result.reason = budget_reason();
	return run;
}

} // namespace uml::runtime

#endif
