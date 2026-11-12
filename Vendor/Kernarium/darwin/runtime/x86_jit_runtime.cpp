// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/x86_jit_runtime.hpp"

#if DARWIN_UML_X86_64_JIT

#include "cpu/x86_64/jit/fex_engine.hpp"
#include "platform/apple/host_features.hpp"
#include "runtime/x86_runtime_support.hpp"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <new>
#include <span>

namespace uml::runtime {
namespace {

namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::StaticGuestMemory;

/* The most changes of either kind the engine is handed as ranges; a reader
 * further behind retires everything instead. A vCPU that was inside the
 * guest kernel while it mapped a large buffer is a few thousand mapping
 * changes behind, none of which touches a translation, so each capacity
 * covers what the address space remembers of its kind. */
constexpr std::size_t mapping_change_capacity = 4096;
constexpr std::size_t code_change_capacity = 1024;

/* The first `count` records of `records` as the engine's ranges, in
 * `ranges`. */
template<typename Record, std::size_t Capacity>
[[gnu::always_inline]] std::span<const x86_64::jit::GuestRange> guest_ranges(
	const std::array<Record, Capacity> &records, std::size_t count,
	std::array<x86_64::jit::GuestRange, Capacity> &ranges) noexcept
{
	for (std::size_t index = 0; index < count; ++index)
		ranges[index] = {records[index].address, records[index].length};
	return {ranges.data(), count};
}

/* Hands the engine what changed in the address space since it last ran in
 * it: the ranges while the address space can still name them, everything
 * once it cannot. */
void sync_engine(x86_64::jit::FexEngine &engine,
	const X86StaticExecutionScope &execution, unsigned long generation,
	x86_64::ExecutionMode mode,
	const darwin_uml_static_sequences &sequences,
	bool orders_stores) noexcept
{
	x86_64::jit::AddressSpaceChanges changes{};
	changes.mapping_sequence = sequences.mapping_changes;
	changes.code_sequence = sequences.code_changes;
	std::uint64_t mapping_seen = 0;
	std::uint64_t code_seen = 0;
	if (!engine.consumed_sequences(generation, mode, mapping_seen,
		code_seen)) {
		engine.apply_changes(generation, mode, changes, orders_stores);
		return;
	}
	if (mapping_seen == sequences.mapping_changes &&
		code_seen == sequences.code_changes)
		return;
	thread_local std::array<darwin_uml_static_mapping_change,
		mapping_change_capacity>
		mapped;
	thread_local std::array<x86_64::jit::GuestRange, mapping_change_capacity>
		mapping_ranges;
	if (mapping_seen != sequences.mapping_changes) {
		std::uint64_t current = sequences.mapping_changes;
		const int count = darwin_uml_static_vm_mapping_changes(generation,
			mapping_seen, mapped.data(), mapped.size(), &current);
		if (count < 0) {
			changes.all_mappings = true;
		} else {
			changes.mapping_ranges = guest_ranges(mapped,
				static_cast<std::size_t>(count), mapping_ranges);
			changes.mapping_sequence = current;
		}
	}
	thread_local std::array<darwin_uml_static_code_change,
		code_change_capacity>
		coded;
	thread_local std::array<x86_64::jit::GuestRange, code_change_capacity>
		code_ranges;
	if (code_seen != sequences.code_changes) {
		std::uint64_t current = sequences.code_changes;
		const int count =
			darwin_uml_static_vm_execution_context_code_change_ranges(
				execution.context(), code_seen, coded.data(),
				coded.size(), &current);
		if (count < 0) {
			changes.all_code = true;
		} else {
			changes.code_ranges = guest_ranges(coded,
				static_cast<std::size_t>(count), code_ranges);
			changes.code_sequence = current;
		}
	}
	engine.apply_changes(generation, mode, changes, orders_stores);
}

} // namespace

void X86JitEngineDeleter::operator()(
	x86_64::jit::FexEngine *engine) const noexcept
{
	delete engine;
}

bool x86_jit_available() noexcept
{
	static const bool available = x86_64::jit::fex_engine_available(
		::uml::platform::apple::probed_host_capabilities());
	return available;
}

X86RunResult run_x86_jit(x86_64::CpuState &cpu, StaticGuestMemory &memory,
	X86JitEngine &engine, unsigned long generation,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt, bool mm_shared) noexcept
{
	X86RunResult run{};
	auto &result = run.execution;
	result.reason = ::uml::cpu::ExitReason::fatal;
	if (!x86_jit_available()) {
		std::fputs("Darwin x86 runtime: this host cannot run the "
			"x86-64 JIT\n", stderr);
		return run;
	}
	if (!engine) {
		engine.reset(new (std::nothrow) x86_64::jit::FexEngine(
			::uml::platform::apple::probed_host_capabilities(),
			{.generation_live = &x86_generation_live}));
		if (!engine)
			return run;
	}
	X86StaticExecutionScope execution{generation, cpu.rip};
	if (!execution.entered())
		return run;
	const auto sequences = execution.sequences();
	if (sequences.execution_interrupt != 0 || interrupt.state_pending()) {
		result.reason = ::uml::cpu::ExitReason::interrupt;
		return run;
	}
	memory.prepare(generation, sequences.mapping_changes);
	if (cpu.rip == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
		x86_signal_return(cpu, result);
		return run;
	}
	/* The kernel's mark alone decides: a space another agent may observe
	 * keeps x86's store order. */
	const bool ordered = mm_shared;
	std::size_t retired = 0;
	for (auto sequences_now = sequences;;) {
		sync_engine(*engine, execution, generation, cpu.execution_mode,
			sequences_now, ordered);
		const auto start = cpu.rip;
		const auto executed = engine->run(cpu, memory, {
			.generation = generation,
			.instruction_budget = instruction_budget - retired,
			.execution_context = execution.context(),
			.interrupt = &interrupt,
			.orders_stores = ordered,
		});
		retired += executed.execution.retired_instructions;
		result = executed.execution;
		result.retired_instructions = retired;
		run.error_code = executed.error_code;
		if (x86_mm_shared_logging()) [[unlikely]]
			note_x86_jit_ordering(generation, mm_shared,
				executed.stores_ordered, start);
		/* CPUID or IRET: the code after it is what other vCPUs stored
		 * before it, so the changes they published are handed over
		 * before it runs, as at any entry. */
		if (!executed.serialized || retired >= instruction_budget)
			break;
		sequences_now = execution.sequences();
		if (sequences_now.execution_interrupt != 0 ||
			interrupt.state_pending())
			break;
		memory.prepare(generation, sequences_now.mapping_changes);
	}
	/* A handler without SA_RESTORER returns to the trampoline, which no
	 * guest page backs on the x86-64 slice, so the engine stops there with
	 * a fetch fault. Reaching it means rt_sigreturn, as at entry. */
	if (result.reason == ::uml::cpu::ExitReason::memory_fault &&
		result.fault.access == ::uml::memory::MemoryAccess::execute &&
		cpu.rip == DARWIN_UML_SIGRETURN_TRAMPOLINE) {
		x86_signal_return(cpu, result);
		result.fault = {};
		return run;
	}
	/* As in the gadget runtime: a long-mode access to a non-canonical
	 * address raises #GP or #SS, not #PF. */
	if (result.reason == ::uml::cpu::ExitReason::memory_fault &&
		cpu.execution_mode == x86_64::ExecutionMode::long64 &&
		!x86_canonical(result.fault.address)) {
		result.reason = ::uml::cpu::ExitReason::breakpoint;
		result.syndrome = result.fault.access ==
			::uml::memory::MemoryAccess::execute ?
			x86_general_protection : x86_noncanonical_vector(memory, cpu);
		result.fault = {};
		run.error_code = 0;
	}
	return run;
}

} // namespace uml::runtime

#endif
