// SPDX-License-Identifier: GPL-2.0-or-later
/* Executors running one address space share its translations: a miss on code
 * another executor holds adopts that body after validating it, links are the
 * address space's, a code change reaches every executor's links, an executor
 * that retires the address space leaves the others' bodies alone, and bodies
 * are freed only once no executor can be running them. Sharing is off by
 * default, so these tests turn it on. */
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

void a_second_executor_adopts_instead_of_translating()
{
	constexpr unsigned long generation = 940;
	JitBacking backing{1};
	put_program(backing, 3, 4);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory first_memory;
	StaticGuestMemory second_memory;
	gadgets::GadgetExecutor first{sharing_options()};
	gadgets::GadgetExecutor second{sharing_options()};
	arm64::CpuState cpu{};

	/* The first executor translates both blocks and links them. */
	require(run_to_system_call(first, first_memory, generation, cpu,
		target) == 2);
	require(run_to_system_call(first, first_memory, generation, cpu,
		code_base) == 4);
	require(first.cache_diagnostics().translations == 2);

	/* The second takes the entry block, and follows the address space's
	 * link into the block it never looked up, in one run. */
	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(second, second_memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 4);
	require(cpu.registers[0] == 3 && cpu.registers[1] == 4);
	require(second.cache_diagnostics().translations == 0);
	require(second.cache_diagnostics().adoptions == 1);
}

void a_code_change_reaches_every_executor()
{
	constexpr unsigned long generation = 941;
	JitBacking backing{1};
	put_program(backing, 5, 6);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory first_memory;
	StaticGuestMemory second_memory;
	gadgets::GadgetExecutor first{sharing_options()};
	gadgets::GadgetExecutor second{sharing_options()};
	arm64::CpuState cpu{};
	require(run_to_system_call(first, first_memory, generation, cpu,
		target) == 2);
	require(run_to_system_call(first, first_memory, generation, cpu,
		code_base) == 4);
	require(run_to_system_call(second, second_memory, generation, cpu,
		code_base) == 4);

	/* Change the successor. The first executor's link to it must be gone,
	 * and the new successor's translation must reach the second executor
	 * through the address space's link, without it translating. */
	backing.put32(target - code_base, move_immediate(1, 7));
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, target, 4) == 0);
	require(run_to_system_call(first, first_memory, generation, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 5 && cpu.registers[1] == 7);
	require(first.cache_diagnostics().translations == 3);

	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(second, second_memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 4);
	require(cpu.registers[0] == 5 && cpu.registers[1] == 7);
	require(second.cache_diagnostics().translations == 0);
}

/* An executor that retires the address space lets go of its slots; the bodies
 * the other executor holds stay, and coming back adopts them. */
void retiring_in_one_executor_keeps_the_other()
{
	constexpr unsigned long generation = 942;
	JitBacking backing{1};
	put_program(backing, 8, 9);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory first_memory;
	StaticGuestMemory second_memory;
	gadgets::GadgetExecutor first{sharing_options()};
	gadgets::GadgetExecutor second{sharing_options()};
	arm64::CpuState cpu{};
	require(run_to_system_call(first, first_memory, generation, cpu,
		code_base) == 4);
	require(run_to_system_call(second, second_memory, generation, cpu,
		code_base) == 4);
	require(second.cache_diagnostics().adoptions == 1);

	/* Nine other address spaces retire this one from the first executor,
	 * which held the successor alone: its link goes with it. */
	std::vector<std::unique_ptr<JitBacking>> others;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (unsigned long other = 0;
	     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other) {
		others.push_back(std::make_unique<JitBacking>(1));
		put_program(*others.back(), 1, 2);
		mappings.push_back(std::make_unique<JitMapping>(
			950 + other, code_base, *others.back(), 1));
		require(run_to_system_call(first, first_memory, 950 + other,
			cpu, code_base) == 4);
	}
	require(darwin_uml_static_vm_activate(generation) == 0);

	const auto translated = second.cache_diagnostics().translations;
	require(run_to_system_call(second, second_memory, generation, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 8 && cpu.registers[1] == 9);
	require(second.cache_diagnostics().translations == translated + 1);

	const auto before = first.cache_diagnostics();
	require(run_to_system_call(first, first_memory, generation, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 8 && cpu.registers[1] == 9);
	require(first.cache_diagnostics().translations == before.translations);
	require(first.cache_diagnostics().adoptions == before.adoptions + 1);
}

/* Executors built with other options make other streams, so they never
 * share; nor does an executor told not to. One left at the default does. */
void different_options_do_not_share()
{
	constexpr unsigned long generation = 943;
	JitBacking backing{1};
	put_program(backing, 1, 2);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory first_memory;
	StaticGuestMemory second_memory;
	StaticGuestMemory third_memory;
	StaticGuestMemory fourth_memory;
	gadgets::GadgetExecutor first{sharing_options()};
	auto other = sharing_options();
	other.scalar_forwarding = false;
	gadgets::GadgetExecutor second{other};
	arm64::CpuState cpu{};
	require(run_to_system_call(first, first_memory, generation, cpu,
		code_base) == 4);
	require(run_to_system_call(second, second_memory, generation, cpu,
		code_base) == 4);
	require(second.cache_diagnostics().translations == 2);
	require(second.cache_diagnostics().adoptions == 0);

	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1) == 0);
	gadgets::GadgetExecutor third{sharing_options()};
	require(unsetenv("DARWIN_UML_GADGET_SHARED_BODIES") == 0);
	gadgets::GadgetExecutor fourth{sharing_options()};
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	require(run_to_system_call(third, third_memory, generation, cpu,
		code_base) == 4);
	require(third.cache_diagnostics().translations == 2);
	require(third.cache_diagnostics().adoptions == 0);
	require(run_to_system_call(fourth, fourth_memory, generation, cpu,
		code_base) == 4);
	require(fourth.cache_diagnostics().translations == 0);
	require(fourth.cache_diagnostics().adoptions == 1);
}

/* Only an address space one executor keeps to itself sweeps its link
 * records: a shared one is named from the slots, memos and return rings of
 * every executor holding it, so it keeps them all. Rewriting one branch
 * thousands of times leaves a record behind for every target it ever had;
 * the private executor sweeps those, and the sharing one must not. */
void a_shared_space_is_never_swept()
{
	constexpr unsigned long generation = 944;
	constexpr std::size_t rounds = 5000;
	constexpr std::uint64_t first_target = code_base + 0x1000;
	constexpr std::size_t pages = (0x1000 + rounds * 8 +
		DARWIN_UML_GUEST_PAGE_SIZE - 1) / DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{pages};
	JitMapping mapping{generation, code_base, backing, pages};
	const auto rewrite = [&](gadgets::GadgetExecutor &executor,
				 StaticGuestMemory &memory) {
		for (std::size_t round = 0; round < rounds; ++round) {
			const auto to = first_target + round * 8;
			backing.put32(0, branch(code_base, to));
			require(darwin_uml_static_vm_invalidate_code_generation(
				generation, code_base, 4) == 0);
			arm64::CpuState cpu{};
			cpu.pc = code_base;
			const auto outcome = run_once(executor, memory, generation,
				cpu);
			require(outcome.event == ::uml::cpu::EngineEvent::continued);
			require(cpu.pc == to);
		}
		return executor.cache_diagnostics();
	};

	StaticGuestMemory shared_memory;
	gadgets::GadgetExecutor shared{sharing_options()};
	const auto sharing = rewrite(shared, shared_memory);
	require(sharing.translations == rounds);
	require(sharing.link_sweeps == 0);

	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1) == 0);
	StaticGuestMemory private_memory;
	gadgets::GadgetExecutor alone{sharing_options()};
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	const auto kept = rewrite(alone, private_memory);
	require(kept.translations == rounds);
	require(kept.link_sweeps != 0);
	require(kept.released_links >= rounds / 2);
}

/* Several vCPUs run, link, retire and revalidate the same address spaces
 * while another rewrites their code, and some executor must have adopted a
 * translation another held. */
void concurrent_executors_follow_code_changes()
{
	std::atomic<std::uint64_t> adoptions{};
	code_change_storm(960, UML_GADGET_RESIDENT_GENERATIONS + 2, 4,
		[&](gadgets::GadgetExecutor &executor) {
			adoptions.fetch_add(
				executor.cache_diagnostics().adoptions);
		});
	require(adoptions.load() != 0);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	a_second_executor_adopts_instead_of_translating();
	a_code_change_reaches_every_executor();
	retiring_in_one_executor_keeps_the_other();
	different_options_do_not_share();
	a_shared_space_is_never_swept();
	concurrent_executors_follow_code_changes();
	return 0;
}
