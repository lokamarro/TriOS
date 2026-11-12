// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/gadget_cache_geometry.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <sys/mman.h>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::cpu::arm64::gadgets::GadgetExecutor;
using uml::memory::StaticGuestMemory;
using namespace uml::emulator::tests;

namespace {

constexpr std::uint64_t code_base = 0x82'0000;

struct Outcome final {
	::uml::cpu::EngineEvent event{::uml::cpu::EngineEvent::fatal};
	arm64::CpuState cpu{};
	std::size_t executed{};
	std::uint64_t fault_address{};
	::uml::memory::Access fault_access{::uml::memory::Access::read};
};

using Configure = void (*)(arm64::CpuState &);

[[nodiscard]] Outcome execute_block(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation,
	arm64::CpuState cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event =
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	gadgets::restore_native_fp_environment(cpu);
	return {
		.event = event,
		.cpu = cpu,
		.executed = executed,
		.fault_address = executor.fault_address(),
		.fault_access = executor.fault_access(),
	};
}

[[nodiscard]] Outcome run(std::span<const std::uint32_t> instructions,
	Configure configure = nullptr, gadgets::BuildOptions options = {})
{
	static unsigned long next_generation = 740;
	const auto generation = next_generation++;

	JitBacking backing{1};
	for (std::size_t index = 0; index < instructions.size(); ++index)
		backing.put32(index * sizeof(std::uint32_t), instructions[index]);
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor{options};
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	if (configure != nullptr)
		configure(cpu);
	return execute_block(executor, memory, generation, cpu);
}

[[nodiscard]] std::uint32_t direct_branch(
	std::uint64_t source, std::uint64_t target)
{
	const auto displacement =
		static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source);
	require((displacement & 3) == 0);
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement >> 2) & 0x03ff'ffffU);
}

/* Mirrors ProgramCache::locate(): the address space is folded into the
 * index, so blocks collide only within one address space's mapping. */
[[nodiscard]] constexpr std::size_t program_cache_set(
	unsigned long generation, std::uint64_t pc) noexcept
{
	constexpr auto sets = ::uml::cpu::gadget_cache::slots /
		::uml::cpu::gadget_cache::ways;
	const auto block = (pc >> 2) ^
		(static_cast<std::uint64_t>(generation) *
			0x9e37'79b9'7f4a'7c15ULL);
	const auto mixed = block ^ (block >> 12) ^ (block >> 24);
	return static_cast<std::size_t>(mixed) & (sets - 1);
}

void illegal_at_block_start_does_not_retire()
{
	constexpr std::array instructions{
		0x0000'0000U,
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions);
	require(outcome.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(outcome.cpu.pc == code_base);
	require(outcome.executed == 0);
}

void valid_prefix_before_illegal_is_counted()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0x0000'0000U,
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions);
	require(outcome.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
}

void memory_fault_excludes_fault_and_later_decoded_words()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0xf940'0041U, // ldr x1, [x2]
		0xd280'00e3U, // movz x3, #7
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions, [](arm64::CpuState &cpu) {
		cpu.registers[2] = code_base + DARWIN_UML_GUEST_PAGE_SIZE;
	});
	require(outcome.event == ::uml::cpu::EngineEvent::memory_fault);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.registers[1] == 0);
	require(outcome.cpu.registers[3] == 0);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
	require(outcome.fault_address ==
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(outcome.fault_access == ::uml::memory::Access::read);
}

void enabled_trap_reserved_allocation_does_not_retire()
{
	constexpr std::array instructions{
		0x2e00'fc00U,
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions, [](arm64::CpuState &cpu) {
		cpu.fpcr = arm64::detail::fpcr_exception_enable_mask;
	});
	require(outcome.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(outcome.cpu.pc == code_base);
	require(outcome.executed == 0);
}

void valid_prefix_before_implementation_gap_is_counted()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0xd500'0000U, // unsupported system allocation
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions);
	require(outcome.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
	require(outcome.cpu.gadget_indirect_miss == nullptr);
}

void retiring_terminals_keep_their_instruction_count()
{
	{
		constexpr std::array instructions{
			0xd400'0001U, // svc #0
		};
		const auto outcome = run(instructions);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
		require(outcome.executed == 1);
	}
	{
		constexpr std::array instructions{
			0xd420'0000U, // brk #0
		};
		const auto outcome = run(instructions);
		require(outcome.event == ::uml::cpu::EngineEvent::breakpoint);
		require(outcome.cpu.pc == code_base);
		require(outcome.executed == 1);
	}
}

void add_immediate_at_block_tail_reaches_synthetic_terminal()
{
	/* Builder must append two complete words after the final ADD operand. The
	 * successor-hoist candidate consumes the operand and terminal pointer in
	 * one LDP, then the terminal consumes its fallthrough PC. */
	constexpr std::array instructions{
		0x9100'0694U, // add x20, x20, #1
	};
	const auto built = gadgets::Builder{}.build(code_base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == 1U);
	require(built.program.words().size() == 4U);

	arm64::CpuState cpu{};
	cpu.registers[20] = 41U;
	::uml::memory::Tlb tlb;
	std::size_t executed{};
	const auto event = gadgets::execute(built.program, cpu, tlb, executed);
	require(event == ::uml::cpu::EngineEvent::interrupt);
	require(cpu.pc == code_base + sizeof(std::uint32_t));
	require(cpu.registers[20] == 42U);
	require(executed == 1U);
}

void unallocated_modified_immediate_ends_its_block()
{
	/* With o2 set, the Advanced SIMD modified-immediate allocation holds only
	 * the FP16 FMOV, cmode 0b1111. Any other cmode is unallocated, and like
	 * every other unallocated word it ends the block where it stands, so the
	 * words after it are never decoded. */
	constexpr std::array instructions{
		0x0f05'0cbfU, // op=0, o2=1, cmode=0b0000
		0xd503'201fU, // nop
	};
	const auto built = gadgets::Builder{}.build(code_base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == 1U);
	require(built.stop_address == code_base);
	require(built.stop_instruction == instructions[0]);
	require(built.program.words().size() == 2U);
}

void linked_successor_terminal_subtracts_only_its_suffix()
{
	constexpr unsigned long generation = 760;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, direct_branch(code_base, target));
	backing.put32(0x100, 0xd280'0121U); // movz x1, #9
	backing.put32(0x104, 0x0000'0000U);
	backing.put32(0x108, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(warm.executed == 1);
	require(warm.cpu.gadget_indirect_miss == nullptr);

	arm64::CpuState linked_cpu{};
	linked_cpu.pc = code_base;
	const auto linked = execute_block(
		executor, memory, generation, linked_cpu);
	require(linked.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(linked.cpu.pc == target + sizeof(std::uint32_t));
	require(linked.cpu.registers[1] == 9);
	require(linked.executed == 2);
	require(linked.cpu.gadget_indirect_miss == nullptr);
}

/* A pair transfer carries no PC word, so a fault in one inside a linked
 * successor is named by its position in that successor, which only the
 * executor can resolve. */
void linked_successor_pair_fault_reports_its_pc(bool store)
{
	static unsigned long next_generation = 765;
	const auto generation = next_generation++;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, direct_branch(code_base, target));
	backing.put32(0x100, 0xd280'0121U); // movz x1, #9
	backing.put32(0x104, 0xd280'0141U); // movz x1, #10
	backing.put32(0x108, store ?
		0xa900'1043U : // stp x3, x4, [x2]
		0xa940'1043U); // ldp x3, x4, [x2]
	backing.put32(0x10c, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;
	const auto unmapped = code_base + DARWIN_UML_GUEST_PAGE_SIZE;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	warm_cpu.registers[2] = unmapped;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == ::uml::cpu::EngineEvent::memory_fault);
	require(warm.cpu.pc == target + 2 * sizeof(std::uint32_t));
	require(warm.executed == 2);

	arm64::CpuState linked_cpu{};
	linked_cpu.pc = code_base;
	linked_cpu.registers[2] = unmapped;
	const auto linked = execute_block(
		executor, memory, generation, linked_cpu);
	require(linked.event == ::uml::cpu::EngineEvent::memory_fault);
	require(linked.cpu.pc == target + 2 * sizeof(std::uint32_t));
	require(linked.cpu.registers[1] == 10);
	require(linked.executed == 3);
	require(linked.fault_address == unmapped);
	require(linked.fault_access == (store ?
		::uml::memory::Access::write : ::uml::memory::Access::read));
	require(linked.cpu.gadget_indirect_miss == nullptr);
}

void linked_successor_fpcr_exit_subtracts_only_its_suffix()
{
	constexpr unsigned long generation = 761;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, direct_branch(code_base, target));
	backing.put32(0x100, 0xd51b'4409U); // msr fpcr, x9
	backing.put32(0x104, 0xd280'00e0U); // movz x0, #7
	backing.put32(0x108, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == ::uml::cpu::EngineEvent::system_call);
	require(warm.executed == 3);
	require(warm.cpu.gadget_indirect_miss == nullptr);

	arm64::CpuState linked_cpu{};
	linked_cpu.pc = code_base;
	linked_cpu.registers[9] =
		arm64::detail::fpcr_exception_enable_mask;
	const auto linked = execute_block(
		executor, memory, generation, linked_cpu);
	require(linked.event == ::uml::cpu::EngineEvent::continued);
	require(linked.cpu.pc == target + sizeof(std::uint32_t));
	require(linked.cpu.fpcr ==
		arm64::detail::fpcr_exception_enable_mask);
	require(linked.cpu.registers[0] == 0);
	require(linked.executed == 2);
	require(linked.cpu.gadget_indirect_miss == nullptr);
}

/* Replacing a translation's slot frees its body, and the retranslation may be
 * given those very bytes back; a fault linked into it must be attributed to
 * the new translation. */
void a_retranslated_successor_answers_from_its_new_body()
{
	constexpr unsigned long generation = 763;
	constexpr auto source = code_base;
	constexpr auto target = code_base + 0x100;
	constexpr std::size_t cache_ways = ::uml::cpu::gadget_cache::ways;
	std::array<std::uint64_t, cache_ways + 1> colliding{};
	colliding[0] = target;
	std::size_t found = 1;
	for (auto candidate = target + sizeof(std::uint32_t);
	     found < colliding.size(); candidate += sizeof(std::uint32_t)) {
		require(candidate - code_base < 64 * 1024 * 1024);
		if (program_cache_set(generation, candidate) ==
		    program_cache_set(generation, target))
			colliding[found++] = candidate;
	}
	require(program_cache_set(generation, source) !=
		program_cache_set(generation, target));

	const auto mapped_bytes =
		colliding.back() - code_base + sizeof(std::uint32_t);
	const auto pages = static_cast<std::size_t>(
		(mapped_bytes + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE);
	JitBacking backing{pages};
	backing.put32(source - code_base, direct_branch(source, target));
	backing.put32(target - code_base, 0xd280'0121U); // movz x1, #9
	backing.put32(target - code_base + 4, 0x0000'0000U);
	backing.put32(target - code_base + 8, 0xd400'0001U); // svc #0
	for (std::size_t index = 1; index < colliding.size(); ++index)
		backing.put32(
			colliding[index] - code_base, 0xd400'0001U);
	JitMapping mapping{
		generation, code_base, backing, pages, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_target{};
	warm_target.pc = target;
	require(execute_block(
			executor, memory, generation, warm_target).executed == 1);

	arm64::CpuState warm_source{};
	warm_source.pc = source;
	const auto first = execute_block(
		executor, memory, generation, warm_source);
	require(first.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(first.cpu.registers[1] == 9);
	require(first.executed == 2);

	/* A translation per way replaces every way in target's cache set. */
	for (std::size_t index = 1; index < colliding.size(); ++index) {
		arm64::CpuState collision{};
		collision.pc = colliding[index];
		require(execute_block(
				executor, memory, generation, collision).event ==
			::uml::cpu::EngineEvent::system_call);
	}
	require(executor.cache_diagnostics().translations == cache_ways + 2);

	arm64::CpuState rewarm_target{};
	rewarm_target.pc = target;
	require(execute_block(
			executor, memory, generation, rewarm_target).executed == 1);
	require(executor.cache_diagnostics().translations == cache_ways + 3);

	arm64::CpuState linked{};
	linked.pc = source;
	const auto second =
		execute_block(executor, memory, generation, linked);
	require(second.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(second.cpu.pc == target + sizeof(std::uint32_t));
	require(second.cpu.registers[1] == 9);
	require(second.executed == 2);
	require(second.cpu.gadget_indirect_miss == nullptr);
}

/* A terminal exit inside a linked successor is attributed to that successor
 * by asking the body arena which live block holds the exit stream. Enough
 * blocks are translated here to spread their bodies over several arena
 * chunks. With `fault_first` the faulting block is the first translation, so
 * its body opens the first chunk and its exit lies far enough into the stream
 * to sit past the first word of that chunk's start map; otherwise it is the
 * last, so the search has to pick the newest chunk wherever the host placed
 * it. Exact retirement over the whole chain is what proves the right body was
 * found: a failed lookup leaves the faulting block's precharge uncorrected.
 */
void linked_successor_is_found_across_arena_chunks(bool fault_first)
{
	static unsigned long next_generation = 770;
	const auto generation = next_generation++;
	/* One instruction a block keeps the whole linked chain under the
	 * 4,096-instruction limit, whose exit reports the unexecuted target's
	 * precharge and would make the total inexact for a reason of its own. */
	constexpr std::size_t chain = 4000;
	constexpr std::size_t adds = 62;
	constexpr auto fault_block = code_base;
	constexpr auto first_link =
		code_base + (adds + 2) * sizeof(std::uint32_t);
	const auto link_address = [](std::size_t index) {
		return first_link + (index - 1) * sizeof(std::uint32_t);
	};
	const auto mapped_bytes =
		link_address(chain) + sizeof(std::uint32_t) - code_base;
	const auto pages = static_cast<std::size_t>(
		(mapped_bytes + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE);
	const auto unmapped = code_base + pages * DARWIN_UML_GUEST_PAGE_SIZE;

	JitBacking backing{pages};
	/* The faulting block: adds, a load from an unmapped page, a syscall. */
	for (std::size_t index = 0; index < adds; ++index)
		backing.put32(index * sizeof(std::uint32_t),
			0x9100'0421U); // add x1, x1, #1
	backing.put32(adds * sizeof(std::uint32_t), 0xf940'0040U); // ldr x0, [x2]
	backing.put32((adds + 1) * sizeof(std::uint32_t),
		0xd400'0001U); // svc #0
	/* Block i: b block i-1, with block 1 branching to the fault. */
	for (std::size_t index = 1; index <= chain; ++index) {
		const auto address = link_address(index);
		const auto target =
			index == 1 ? fault_block : link_address(index - 1);
		backing.put32(address - code_base,
			direct_branch(address, target));
	}
	JitMapping mapping{
		generation, code_base, backing, pages, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	struct Run final {
		std::size_t retired;
		std::size_t last_round;
	};
	/* From an entry to the fault, over however many round trips the chain
	 * takes; the last round's count says whether the fault was reached
	 * through a link. */
	const auto run_to_fault = [&](std::uint64_t entry) {
		Outcome outcome{};
		outcome.cpu.pc = entry;
		outcome.cpu.registers[2] = unmapped;
		std::size_t retired = 0;
		for (std::size_t round = 0; round <= chain; ++round) {
			outcome = execute_block(
				executor, memory, generation, outcome.cpu);
			retired += outcome.executed;
			require(outcome.cpu.gadget_indirect_miss == nullptr);
			if (outcome.event == ::uml::cpu::EngineEvent::memory_fault)
				break;
			require(outcome.event ==
				::uml::cpu::EngineEvent::continued);
		}
		require(outcome.event == ::uml::cpu::EngineEvent::memory_fault);
		require(outcome.cpu.pc ==
			fault_block + adds * sizeof(std::uint32_t));
		require(outcome.fault_address == unmapped);
		require(outcome.cpu.registers[1] == adds);
		return Run{retired, outcome.executed};
	};

	if (fault_first) {
		const auto warm = run_to_fault(fault_block);
		require(warm.retired == adds);
		require(executor.cache_diagnostics().translations == 1);
	}
	/* Every block is translated as it is reached. */
	const auto first = run_to_fault(link_address(chain));
	require(first.retired == chain + adds);
	require(executor.cache_diagnostics().translations == chain + 1);
	require((first.last_round > adds) == fault_first);
	/* The whole chain now runs linked, and the fault at its end is reached
	 * from a block translated a chunk or more away. */
	const auto linked = run_to_fault(link_address(chain));
	require(linked.retired == chain + adds);
	require(linked.last_round > adds);
	require(executor.cache_diagnostics().translations == chain + 1);
	require(executor.cache_diagnostics().validation_attempts == 0);
}

void indirect_link_learning_still_uses_and_clears_scratch()
{
	constexpr unsigned long generation = 762;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, 0xd61f'00a0U); // br x5
	backing.put32(0x100, 0xd280'0160U); // movz x0, #11
	backing.put32(0x104, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == ::uml::cpu::EngineEvent::system_call);
	require(warm.executed == 2);

	arm64::CpuState miss_cpu{};
	miss_cpu.pc = code_base;
	miss_cpu.registers[5] = target;
	const auto miss = execute_block(
		executor, memory, generation, miss_cpu);
	require(miss.event == ::uml::cpu::EngineEvent::continued);
	require(miss.cpu.pc == target);
	require(miss.executed == 1);
	require(miss.cpu.gadget_indirect_miss == nullptr);

	arm64::CpuState hit_cpu{};
	hit_cpu.pc = code_base;
	hit_cpu.registers[5] = target;
	const auto hit = execute_block(
		executor, memory, generation, hit_cpu);
	require(hit.event == ::uml::cpu::EngineEvent::system_call);
	require(hit.cpu.pc == target + 2 * sizeof(std::uint32_t));
	require(hit.cpu.registers[0] == 11);
	require(hit.executed == 3);
	require(hit.cpu.gadget_indirect_miss == nullptr);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	illegal_at_block_start_does_not_retire();
	valid_prefix_before_illegal_is_counted();
	memory_fault_excludes_fault_and_later_decoded_words();
	enabled_trap_reserved_allocation_does_not_retire();
	valid_prefix_before_implementation_gap_is_counted();
	retiring_terminals_keep_their_instruction_count();
	add_immediate_at_block_tail_reaches_synthetic_terminal();
	unallocated_modified_immediate_ends_its_block();
	linked_successor_terminal_subtracts_only_its_suffix();
	linked_successor_pair_fault_reports_its_pc(false);
	linked_successor_pair_fault_reports_its_pc(true);
	linked_successor_fpcr_exit_subtracts_only_its_suffix();
	a_retranslated_successor_answers_from_its_new_body();
	linked_successor_is_found_across_arena_chunks(true);
	linked_successor_is_found_across_arena_chunks(false);
	indirect_link_learning_still_uses_and_clears_scratch();
	return 0;
}
