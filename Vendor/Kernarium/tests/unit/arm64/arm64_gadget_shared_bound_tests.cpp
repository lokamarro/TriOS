// SPDX-License-Identifier: GPL-2.0-or-later
/* A shared address space bounds the freed arena it accumulates. Once the arena
 * holds at least as many bytes no body uses as its bodies do, the space is
 * closed: it leaves the registry, every executor attached lets go of it at the
 * start of its next entry into translated code, and the next to want the
 * address space gets a new one, so the old one dies with its last executor and
 * takes its arena, its link records and its freed bodies with it.
 *
 * These tests run against the unit-test variant of the gadget executor
 * (uml_arm64_gadget_sweep_stress_executor), which renews on a schedule and lets
 * a test lower the floor and read what a space holds. */
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "static_mmu_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace stress = gadgets::stress;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

/* `blocks` blocks of `width` instructions, each a run of moves ending in a
 * branch to the next, then a system call. */
void put_chain(JitBacking &backing, std::size_t blocks,
	std::size_t width = 1)
{
	for (std::size_t block = 0; block < blocks; ++block) {
		const auto first = block * width * 4;
		for (std::size_t move = 0; move + 1 < width; ++move)
			backing.put32(first + move * 4, move_immediate(2, 7));
		backing.put32(first + (width - 1) * 4,
			branch(code_base + first + (width - 1) * 4,
				code_base + first + width * 4));
	}
	backing.put32(blocks * width * 4, system_call);
}

/* The unit-test variant of the executor holds 1024 translations, 128 sets of
 * eight, so a few thousand blocks of another address space evict a space's
 * slots.
 *
 * The address space counts the bytes of its arena and of the bodies some slot
 * holds, which is what decides when it is renewed. */
void held_bytes_follow_the_slots()
{
	constexpr unsigned long generation = 980;
	constexpr std::size_t blocks = 700;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	const auto vcpu = std::make_unique<Vcpu>();
	auto &memory = vcpu->memory;
	auto &executor = vcpu->executor;
	auto &cpu = vcpu->cpu;

	const auto empty = stress::open_space_bytes(generation);
	require(empty.spaces == 0);
	require(run_to_system_call(executor, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	const auto full = stress::open_space_bytes(generation);
	require(full.spaces == 1);
	/* Every body takes at least its header and a stream word, in sixteen-byte
	 * granules; a few sets overflow and drop theirs. */
	require(full.held >= (blocks - blocks / 4) * 32 && full.held <= full.arena);

	/* Revalidating a block after a code change replaces its body, and the
	 * old one stops being held: the same bytes are held afterwards. */
	backing.put32(0, branch(code_base, code_base + 8));
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, code_base, 4) == 0);
	require(run_to_system_call(executor, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	const auto again = stress::open_space_bytes(generation);
	require(again.spaces == 1 && again.held == full.held);
}

enum class Leave { window, eviction };

/* Executor A fills a space with a long chain of blocks while B holds one of
 * them, then moves on until it has let go of every one of A's: either its
 * window retires the space, or the blocks of another address space take its
 * slots, which stores nothing in the first. What is left is the arena A's
 * bodies took and one body of B's; once that is more than the floor the space
 * is closed, and B, which kept it, lets go of it at its next entry and
 * translates its block again. Under a floor the arena does not reach, B keeps
 * its slot. */
void a_mostly_free_space_is_renewed(unsigned long generation, Leave leave,
	bool renewed)
{
	constexpr std::size_t blocks = 700;
	/* Wide enough that its bodies take more of the arena than the narrow
	 * ones of the blocks that evict them, and odd, so that the blocks spread
	 * over the cache's sets, which are indexed by the address in words. */
	constexpr std::size_t width = 31;
	constexpr std::size_t evicting_blocks = 6000;
	/* The body arena maps its chunks 64 KiB at a time. */
	constexpr std::size_t arena_chunk = std::size_t{64} << 10;
	/* The chain must fill more of them than the floor and B's pins leave. */
	constexpr std::size_t renewal_probe_arena = 4 * arena_chunk;
	/* Fills the space's arena, which is read before the floor is set. */
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks * width + 1) * 4)};
	put_chain(backing, blocks, width);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks * width + 1) * 4)};
	std::vector<std::unique_ptr<JitBacking>> others;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	if (leave == Leave::window) {
		for (unsigned long other = 0;
		     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other) {
			others.push_back(std::make_unique<JitBacking>(1));
			put_program(*others.back(), 1, 2);
			mappings.push_back(std::make_unique<JitMapping>(
				generation + 1 + other, code_base, *others.back(), 1));
		}
	} else {
		others.push_back(std::make_unique<JitBacking>(
			pages_for((evicting_blocks + 1) * 4)));
		put_chain(*others.back(), evicting_blocks);
		mappings.push_back(std::make_unique<JitMapping>(generation + 1,
			code_base, *others.back(), pages_for((evicting_blocks + 1) * 4)));
	}
	const auto a_vcpu = std::make_unique<Vcpu>();
	const auto b_vcpu = std::make_unique<Vcpu>();
	auto &a = a_vcpu->executor;
	auto &b = b_vcpu->executor;
	auto &a_memory = a_vcpu->memory;
	auto &b_memory = b_vcpu->memory;
	auto &cpu = a_vcpu->cpu;

	require(run_to_system_call(a, a_memory, generation, cpu, code_base,
		blocks + 4) != 0);
	/* B takes the entry block and follows A's links through the rest, which
	 * a run does for a few thousand instructions before it stops and the next
	 * adopts the block it stopped at. */
	require(run_to_system_call(b, b_memory, generation, cpu, code_base,
		blocks) != 0);
	require(b.cache_diagnostics().adoptions >= 1 &&
		b.cache_diagnostics().adoptions < blocks / 16 &&
		b.cache_diagnostics().translations == 0);
	/* The arena returns a chunk of 64 KiB when none of its bodies is live, so
	 * what A leaves behind is the chunks that bodies of B still pin. B holds
	 * a body at the entry and three more spread along the chain, each in a
	 * chunk of its own. */
	for (const auto at : {blocks / 4, blocks / 2, blocks * 3 / 4}) {
		cpu = {};
		cpu.pc = code_base + at * width * 4;
		(void)run_once(b, b_memory, generation, cpu);
	}
	const auto full = stress::open_space_bytes(generation);
	require(full.spaces == 1 && full.held <= full.arena &&
		full.arena >= renewal_probe_arena);
	/* Two chunks of idle arena are more than any other space here has to
	 * spare, and less than this one has once A lets go. */
	stress::set_renewal_floor(renewed ? 2 * arena_chunk :
		std::numeric_limits<std::size_t>::max());
	const auto closes = stress::closes();

	if (leave == Leave::window) {
		for (unsigned long other = 0;
		     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other)
			require(run_to_system_call(a, a_memory,
				generation + 1 + other, cpu, code_base) == 4);
	} else {
		require(run_to_system_call(a, a_memory, generation + 1, cpu,
			code_base, evicting_blocks + 4) != 0);
	}
	const auto left = stress::open_space_bytes(generation);
	if (renewed) {
		/* No new executor would be given it. */
		require(left.spaces == 0 && stress::closes() == closes + 1);
	} else {
		require(left.spaces == 1 && stress::closes() == closes);
		require(left.held < full.held / 8);
	}

	const auto translations = b.cache_diagnostics().translations;
	const auto alive = stress::live_spaces();
	cpu = {};
	cpu.pc = code_base;
	const auto outcome = run_once(b, b_memory, generation, cpu);
	if (renewed) {
		/* The old space went with B, its last executor, as the new one came
		 * into being. */
		require(stress::live_spaces() == alive);
		require(b.cache_diagnostics().translations == translations + 1);
		require(outcome.event == ::uml::cpu::EngineEvent::continued &&
			outcome.executed == width);
		const auto fresh = stress::open_space_bytes(generation);
		require(fresh.spaces == 1 && fresh.arena < full.arena / 2 &&
			fresh.held != 0);
	} else {
		require(b.cache_diagnostics().translations == translations);
	}
}

void a_mostly_free_space_is_renewed_and_a_busy_one_is_not()
{
	a_mostly_free_space_is_renewed(1000, Leave::window, true);
	a_mostly_free_space_is_renewed(1020, Leave::window, false);
	/* The space gets no store after A leaves it: its executors let go of
	 * bodies, and the one that did closes it. */
	a_mostly_free_space_is_renewed(1040, Leave::eviction, true);
}

/* A private cache is not renewed, whatever its arena holds: its window
 * retires it. */
void a_private_cache_is_never_renewed()
{
	constexpr unsigned long generation = 1060;
	constexpr std::size_t blocks = 700;
	stress::set_renewal_period(1);
	stress::set_renewal_floor(0);
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1) == 0);
	const auto vcpu = std::make_unique<Vcpu>();
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	auto &memory = vcpu->memory;
	auto &alone = vcpu->executor;
	auto &cpu = vcpu->cpu;
	const auto closes = stress::closes();
	require(run_to_system_call(alone, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	require(alone.cache_diagnostics().translations == blocks + 1);
	require(stress::closes() == closes);
	stress::set_renewal_period(0);
}

/* A closes the space while it is in a bracket, so it goes on in the old one;
 * B goes to another address space and back, and so enters the successor first
 * and builds only the entry block; A's next bracket lets go of the old space,
 * which dies with it, takes the entry block from the successor and builds the
 * rest there, where it is linked, so that after that one run reaches the
 * system call. A cache that did not prepare the successor would bind to it at
 * the code-change sequence zero, which the gate B set does not accept:
 * nothing A built would be linked, and every run would stop at the second
 * block. (B has to prepare the successor itself for the gate to be set, as an
 * executor does when it comes back from elsewhere.) */
void renewal_reaches_every_executor()
{
	constexpr unsigned long generation = 1080;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{1};
	put_program(backing, 3, 4);
	JitMapping mapping{generation, code_base, backing, 1};
	JitBacking elsewhere{1};
	put_program(elsewhere, 1, 2);
	JitMapping elsewhere_mapping{generation + 1, code_base, elsewhere, 1};
	const auto spaces = stress::live_spaces();
	{
		const auto a_vcpu = std::make_unique<Vcpu>();
		const auto b_vcpu = std::make_unique<Vcpu>();
		auto &a = a_vcpu->executor;
		auto &b = b_vcpu->executor;
		auto &a_memory = a_vcpu->memory;
		auto &b_memory = b_vcpu->memory;
		auto &cpu = a_vcpu->cpu;
		require(run_to_system_call(a, a_memory, generation, cpu,
			code_base) == 4);
		/* A code change, so the sequence is not zero. */
		backing.put32(target - code_base, move_immediate(1, 5));
		require(darwin_uml_static_vm_invalidate_code_generation(
			generation, target, 4) == 0);
		require(darwin_uml_static_vm_code_changes(generation) != 0);
		require(run_to_system_call(a, a_memory, generation, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		require(run_to_system_call(b, b_memory, generation, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		require(b.cache_diagnostics().adoptions == 1);
		require(stress::live_spaces() == spaces + 1);

		/* The space is closed while A is inside it. */
		cpu = {};
		cpu.pc = code_base;
		require(a.begin_execution(generation, cpu.pc));
		require(stress::close_spaces(generation) == 1);
		require(stress::open_space_bytes(generation).spaces == 0);
		std::size_t executed = 0;
		const auto event = a.run_block(cpu, a_memory, generation, executed);
		a.end_execution();
		require(event == ::uml::cpu::EngineEvent::system_call &&
			executed == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		require(stress::live_spaces() == spaces + 1);

		/* B leaves the old space as it goes elsewhere, and on coming back
		 * builds the entry block of the new one. */
		require(run_to_system_call(b, b_memory, generation + 1, cpu,
			code_base) == 4);
		require(stress::live_spaces() == spaces + 2);
		cpu = {};
		cpu.pc = code_base;
		const auto b_translations = b.cache_diagnostics().translations;
		auto outcome = run_once(b, b_memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued &&
			outcome.executed == 2 && cpu.pc == target);
		require(b.cache_diagnostics().translations == b_translations + 1);
		require(stress::open_space_bytes(generation).spaces == 1);
		require(stress::live_spaces() == spaces + 3);

		/* A's next bracket lets go of the old space, which is destroyed with
		 * its last executor, and adopts what B built. */
		cpu = {};
		cpu.pc = code_base;
		const auto a_translations = a.cache_diagnostics().translations;
		const auto a_adoptions = a.cache_diagnostics().adoptions;
		outcome = run_once(a, a_memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued &&
			outcome.executed == 2 && cpu.pc == target);
		require(a.cache_diagnostics().translations == a_translations);
		require(a.cache_diagnostics().adoptions == a_adoptions + 1);
		require(stress::live_spaces() == spaces + 2);

		/* A builds the second block in the new space, which links it. */
		outcome = run_once(a, a_memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call &&
			outcome.executed == 2);
		require(a.cache_diagnostics().translations == a_translations + 1);
		require(cpu.registers[1] == 5);

		/* From here one run of either executor reaches the system call
		 * through the links. */
		for (auto *executor : {&a, &b}) {
			cpu = {};
			cpu.pc = code_base;
			outcome = run_once(*executor,
				executor == &a ? a_memory : b_memory, generation, cpu);
			require(outcome.event ==
					::uml::cpu::EngineEvent::system_call &&
				outcome.executed == 4);
			require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		}
		require(stress::open_space_bytes(generation).spaces == 1);
	}
	require(stress::live_spaces() == spaces);
}

/* Several vCPUs run, link, retire, revalidate and renew the same address
 * spaces while another rewrites their code. Every run must see one of the
 * values the code has held, and once the writer stops, the last ones; and when
 * the executors are gone so is every space, the closed ones included. */
void concurrent_executors_follow_renewals()
{
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	stress::set_renewal_period(8);
	const auto alive = stress::live_spaces();
	const auto closed = stress::closes();
	code_change_storm(1100, UML_GADGET_RESIDENT_GENERATIONS + 2, 6);
	/* The count varies with how the threads interleave (about 120 to 190 on
	 * an idle host); renewals that never happen leave it at zero. */
	require(stress::closes() > closed + 20);
	require(stress::live_spaces() == alive);
	stress::set_renewal_period(0);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	held_bytes_follow_the_slots();
	a_mostly_free_space_is_renewed_and_a_busy_one_is_not();
	a_private_cache_is_never_renewed();
	renewal_reaches_every_executor();
	concurrent_executors_follow_renewals();
	return 0;
}
