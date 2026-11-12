// SPDX-License-Identifier: GPL-2.0-or-later
/* The process's gadget-cache budget bounds what the address spaces' caches
 * keep, whatever their slots would hold. A workload of large blocks, each
 * translated once, runs under a budget a fraction of what it needs: the
 * largest address space is asked to shed, every executor holding it lets go
 * of it at its next entry and translates afresh in a new one, the open spaces
 * stay within the budget, the guest computes what it would have, and the
 * address spaces that go refund what they held. Under the default budget the
 * same workload is never asked to shed. */
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "static_mmu_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace cache = uml::cpu::gadget_cache;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

/* A block is the whole fetch window: 63 additions and a branch. */
constexpr std::size_t width = 64;
constexpr std::size_t kib = std::size_t{1} << 10;
constexpr std::size_t mib = std::size_t{1} << 20;
/* What one block's store and links may add past the budget before the next
 * store's check: a fresh arena chunk with its maps, and a record table's
 * growth. */
constexpr std::size_t one_store = 128 * kib;
/* A quarter of what the workload keeps under the default budget, which the
 * control measures first. */
std::size_t small_budget = mib;

[[nodiscard]] gadgets::BuildOptions options(bool sharing)
{
	return sharing ? sharing_options() : gadgets::BuildOptions{};
}

/* `add x2, x2, #1`. */
constexpr std::uint32_t add_one = 0x9100'0442U;

/* `blocks` blocks of `width` instructions, each adding its 63 to x2 and
 * branching to the next, then a system call. */
struct Chain final {
	explicit Chain(unsigned long generation, std::size_t blocks)
		: blocks{blocks}, backing{pages_for((blocks * width + 1) * 4)},
		  mapping{generation, code_base, backing,
			  pages_for((blocks * width + 1) * 4)}
	{
		for (std::size_t block = 0; block < blocks; ++block) {
			const auto first = block * width * 4;
			for (std::size_t add = 0; add + 1 < width; ++add)
				backing.put32(first + add * 4, add_one);
			backing.put32(first + (width - 1) * 4,
				branch(code_base + first + (width - 1) * 4,
					code_base + first + width * 4));
		}
		backing.put32(blocks * width * 4, system_call);
	}

	std::size_t blocks;
	JitBacking backing;
	JitMapping mapping;
};

/* What the open spaces hold, counted exactly. */
[[nodiscard]] std::size_t open_bytes()
{
	const auto state = cache::ledger_state();
	return state.total - state.shedding;
}

/* Run the chain once, a block or more per entry, and return the most the open
 * spaces held after any entry. */
std::size_t run_chain(Vcpu &vcpu, unsigned long generation,
	const Chain &chain)
{
	auto &cpu = vcpu.cpu;
	cpu = {};
	cpu.pc = code_base;
	std::size_t peak = 0;
	for (std::size_t entry = 0; entry < chain.blocks * 2 + 8; ++entry) {
		require(vcpu.executor.begin_execution(generation, cpu.pc));
		std::size_t executed = 0;
		const auto event = vcpu.executor.run_block(cpu, vcpu.memory,
			generation, executed);
		vcpu.executor.end_execution();
		peak = std::max(peak, open_bytes());
		if (event == ::uml::cpu::EngineEvent::system_call) {
			require(cpu.registers[2] == chain.blocks * (width - 1));
			return peak;
		}
		require(event == ::uml::cpu::EngineEvent::continued);
	}
	require(false);
	return peak;
}

/* One vCPU, one address space, a budget a quarter of what the address space
 * would keep. */
void the_budget_bounds_one_address_space(unsigned long generation,
	bool sharing)
{
	cache::set_budget(small_budget);
	const auto before = cache::ledger_state();
	{
		const Chain chain{generation, 4000};
		const auto vcpu = std::make_unique<Vcpu>(options(sharing));
		std::size_t peak = 0;
		for (int pass = 0; pass < 3; ++pass)
			peak = std::max(peak, run_chain(*vcpu, generation, chain));
		const auto after = cache::ledger_state();
		std::printf("gadget budget: %s, the open spaces held at most "
			    "%zu KiB in %llu reliefs\n",
			sharing ? "shared" : "private", peak / kib,
			static_cast<unsigned long long>(
				after.reliefs - before.reliefs));
		require(after.reliefs > before.reliefs);
		require(peak <= small_budget + one_store);
		/* Every space asked to shed was let go of: what is left is the
		 * one the last pass ran in. */
		require(after.shedding == 0 && after.total <= small_budget +
			one_store);
		/* A shed space is translated afresh. */
		require(vcpu->executor.cache_diagnostics().translations >
			chain.blocks + chain.blocks / 2);
	}
	const auto gone = cache::ledger_state();
	require(gone.total == before.total && gone.accounts == before.accounts);
	cache::set_budget(cache::fallback_budget);
}

/* The control: under the default budget the same workload keeps every
 * translation and nothing is asked to shed. What it holds sets the small
 * budget of the tests after it. */
void the_default_budget_never_sheds_it(unsigned long generation)
{
	require(cache::budget() == cache::fallback_budget);
	const auto before = cache::ledger_state();
	{
		const Chain chain{generation, 4000};
		const auto vcpu = std::make_unique<Vcpu>(options(false));
		std::size_t peak = 0;
		for (int pass = 0; pass < 3; ++pass)
			peak = std::max(peak, run_chain(*vcpu, generation, chain));
		const auto after = cache::ledger_state();
		require(after.reliefs == before.reliefs);
		/* Each block once, and the system call's. */
		require(vcpu->executor.cache_diagnostics().translations ==
			chain.blocks + 1);
		/* Every block keeps at least its 63 gadgets. */
		require(peak >= chain.blocks * (width - 1) * sizeof(void *));
		small_budget = std::max(peak / 4, 4 * one_store);
		std::printf("gadget budget: the control held %zu KiB; the "
			    "small budget is %zu KiB\n", peak / kib,
			small_budget / kib);
	}
	require(cache::ledger_state().total == before.total);
}

/* The largest address space sheds first: a small one that fits beside the
 * budget keeps its translations while a large one is shed around it. */
void the_largest_sheds_first(unsigned long small_generation,
	unsigned long large_generation)
{
	cache::set_budget(small_budget);
	{
		const Chain small{small_generation, 50};
		const Chain large{large_generation, 4000};
		const auto vcpu = std::make_unique<Vcpu>(options(false));
		(void)run_chain(*vcpu, small_generation, small);
		const auto translated =
			vcpu->executor.cache_diagnostics().translations;
		const auto reliefs = cache::ledger_state().reliefs;
		(void)run_chain(*vcpu, large_generation, large);
		require(cache::ledger_state().reliefs > reliefs);
		const auto large_done =
			vcpu->executor.cache_diagnostics().translations;
		(void)run_chain(*vcpu, small_generation, small);
		require(vcpu->executor.cache_diagnostics().translations ==
			large_done);
		require(large_done > translated + large.blocks);
	}
	cache::set_budget(cache::fallback_budget);
}

/* vCPUs sharing one address space under the budget, from several threads:
 * whichever store passes it asks the space to shed, each lets go of it at
 * its next entry, and the next to want the address space opens a new one,
 * which the others then adopt from. */
void vcpus_sharing_a_shed_space(unsigned long generation)
{
	cache::set_budget(small_budget);
	const auto before = cache::ledger_state();
	{
		const Chain chain{generation, 3000};
		constexpr std::size_t vcpus = 3;
		std::atomic<std::size_t> peak{};
		std::vector<std::thread> threads;
		for (std::size_t index = 0; index < vcpus; ++index)
			threads.emplace_back([&] {
				const auto vcpu = std::make_unique<Vcpu>(options(true));
				for (int pass = 0; pass < 3; ++pass) {
					const auto seen = run_chain(*vcpu,
						generation, chain);
					auto known = peak.load();
					while (seen > known &&
					       !peak.compare_exchange_weak(
						       known, seen)) {
					}
				}
			});
		for (auto &thread : threads)
			thread.join();
		std::printf("gadget budget: %zu vCPUs sharing, the open spaces "
			    "held at most %zu KiB\n", vcpus, peak.load() / kib);
		require(cache::ledger_state().reliefs > before.reliefs);
		/* Each vCPU may store once past the budget before it checks. */
		require(peak.load() <= small_budget + vcpus * one_store);
	}
	const auto gone = cache::ledger_state();
	require(gone.total == before.total && gone.accounts == before.accounts);
	cache::set_budget(cache::fallback_budget);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	the_default_budget_never_sheds_it(1300);
	the_budget_bounds_one_address_space(1301, false);
	the_budget_bounds_one_address_space(1302, true);
	the_largest_sheds_first(1303, 1304);
	vcpus_sharing_a_shed_space(1305);
	return 0;
}
