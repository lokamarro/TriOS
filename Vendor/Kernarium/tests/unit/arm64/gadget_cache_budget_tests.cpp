// SPDX-License-Identifier: GPL-2.0-or-later
/* The process's byte budget for the gadget caches' address spaces, on its own:
 * what it derives from a guest's memory and a host's limit, how the accounts
 * add up, and which of them relieve() asks to shed. The engines' own use of it
 * is tested with each engine. */
#include "cpu/gadget_cache_budget.hpp"
#include "require.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace cache = uml::cpu::gadget_cache;
using uml::testing::require;

namespace {

constexpr std::size_t kib = std::size_t{1} << 10;
constexpr std::size_t mib = std::size_t{1} << 20;
constexpr std::size_t gib = std::size_t{1} << 30;

/* A quarter of guest memory, at least 128 MiB, and at most an eighth of a
 * host's limit when it sets one. */
void derived_budgets()
{
	require(cache::derive_budget(8 * gib, 0) == 2 * gib);
	require(cache::derive_budget(256 * mib, 0) == 128 * mib);
	require(cache::derive_budget(0, 0) == 128 * mib);
	/* The iPad mini 6 lets an application reach 2,867 MiB. */
	require(cache::derive_budget(2 * gib, 2867 * mib) == 2867 * mib / 8);
	require(cache::derive_budget(4 * gib, 2867 * mib) == 2867 * mib / 8);
	require(cache::derive_budget(8 * gib, 64 * gib) == 2 * gib);
	/* A host limit below the minimum still leaves a budget. */
	require(cache::derive_budget(gib, 64 * mib) == 16 * mib);
}

/* What the measured workloads charged at their peak in a census at 4 KiB
 * guest pages (G282), at the geometry each ran with, against the budget
 * derived for it. None comes within half of it. */
void measured_workloads_stay_well_within_their_budget()
{
	struct Workload final {
		std::size_t guest_memory;
		std::size_t host_limit;
		std::size_t peak;
	};
	constexpr std::array workloads{
		/* Four arm64 vCPUs, 8 GiB: Geekbench single- and
		 * multi-core, and WebGL Aquarium. */
		Workload{8 * gib, 0, 68 * mib},
		Workload{8 * gib, 0, 74 * mib},
		Workload{8 * gib, 0, 223 * mib},
		/* Four x86 vCPUs, 8 GiB, whose executors keep their
		 * translations to themselves: multi-core Geekbench. */
		Workload{8 * gib, 0, 296 * mib},
		/* The iPad's geometry, two vCPUs and 2 GiB under its
		 * limit: multi-core Geekbench and Aquarium. */
		Workload{2 * gib, 2867 * mib, 71 * mib},
		Workload{2 * gib, 2867 * mib, 169 * mib},
	};
	for (const auto &workload : workloads)
		require(workload.peak * 2 <= cache::derive_budget(
			workload.guest_memory, workload.host_limit));
}

struct Owner final {
	std::atomic<unsigned> notified{};
};

void notify(void *owner) noexcept
{
	static_cast<Owner *>(owner)->notified.fetch_add(1,
		std::memory_order_relaxed);
}

struct Holder final {
	Owner owner;
	cache::Account account{notify, &owner};
};

/* Whether the open and shedding figures every store reads are what the
 * accounts hold, counted one by one. */
[[nodiscard]] bool figures_agree()
{
	const auto state = cache::ledger_state();
	const auto &book = cache::ledger();
	return book.open.load() == state.total - state.shedding &&
		book.shedding.load() == state.shedding;
}

/* Accounts add up, the largest are asked to shed until the open ones are
 * down to seven eighths of the budget, a shedding account's bytes count
 * apart however it changes, and an account that goes refunds what it held.
 * The figures every store reads stay exact throughout. */
void relief_sheds_the_largest()
{
	cache::set_budget(64 * kib);
	const auto base = cache::ledger_state();
	require(base.total == 0 && base.accounts == 0);
	auto small = std::make_unique<Holder>();
	auto large = std::make_unique<Holder>();
	auto middle = std::make_unique<Holder>();
	small->account.charge(10 * kib);
	large->account.charge(50 * kib);
	middle->account.charge(30 * kib);
	auto state = cache::ledger_state();
	require(state.total == 90 * kib && state.accounts == 3 &&
		state.shedding == 0);
	require(cache::over_budget());

	cache::relieve();
	state = cache::ledger_state();
	require(state.reliefs == base.reliefs + 1 &&
		state.shed_accounts == base.shed_accounts + 1);
	require(large->account.shedding() && !middle->account.shedding() &&
		!small->account.shedding());
	require(large->owner.notified == 1 && middle->owner.notified == 0);
	require(state.shedding == 50 * kib && !cache::over_budget());
	require(state.peak_open >= 90 * kib);

	/* A shedding address space may go on growing until its executors let
	 * go of it; it counts apart, and the open ones are what the budget
	 * bounds. */
	large->account.charge(40 * kib);
	require(!cache::over_budget());
	require(cache::ledger_state().shedding == 90 * kib);
	require(figures_agree());
	cache::relieve();
	require(cache::ledger_state().reliefs == base.reliefs + 1);
	require(large->owner.notified == 1 && !cache::over_budget());

	/* The open ones pass it again: both are asked, the larger first, until
	 * the rest is within seven eighths. */
	middle->account.charge(30 * kib);
	small->account.charge(-5 * static_cast<std::ptrdiff_t>(kib));
	require(cache::over_budget());
	cache::relieve();
	require(middle->account.shedding() && !small->account.shedding());
	require(middle->owner.notified == 1 && small->owner.notified == 0);
	state = cache::ledger_state();
	require(state.total == 155 * kib && state.shedding == 150 * kib);
	require(!cache::over_budget() && figures_agree());

	/* Address spaces that go refund what they held. */
	large.reset();
	middle.reset();
	state = cache::ledger_state();
	require(state.total == 5 * kib && state.shedding == 0 &&
		state.accounts == 1);
	require(!cache::over_budget());
	small.reset();
	require(cache::ledger_state().total == 0);
	cache::set_budget(cache::fallback_budget);
}

/* A shed address space may give back part of what it held before its last
 * executor lets go of it, as each frees what only it held. That leaves the
 * open accounts as they were, so a store finds the budget passed as soon as
 * they pass it, not once they have also grown by what the shed one gave back.
 */
void a_shed_account_that_shrinks_leaves_the_open_ones_exact()
{
	cache::set_budget(64 * kib);
	const auto base = cache::ledger_state();
	auto old_space = std::make_unique<Holder>();
	auto new_space = std::make_unique<Holder>();
	old_space->account.charge(60 * kib);
	new_space->account.charge(10 * kib);
	require(cache::over_budget());
	cache::relieve();
	require(old_space->account.shedding() &&
		!new_space->account.shedding());
	old_space->account.charge(-50 * static_cast<std::ptrdiff_t>(kib));
	new_space->account.charge(50 * kib);
	require(!cache::over_budget() && figures_agree());
	new_space->account.charge(5 * kib);
	require(cache::over_budget());
	cache::relieve();
	require(new_space->account.shedding() && figures_agree());
	require(cache::ledger_state().reliefs == base.reliefs + 2);
	old_space.reset();
	new_space.reset();
	require(cache::ledger_state().total == 0 && figures_agree());
	cache::set_budget(cache::fallback_budget);
}

/* A store past the budget relieves it even while another thread holds the
 * ledger's lock -- an address space opening or going, a census, another
 * relief -- by waiting its turn. One that gave up would go on storing past the
 * budget for as long as the holder kept the lock, which a preempted holder
 * keeps for a scheduling quantum. */
void relief_waits_for_the_lock()
{
	cache::set_budget(64 * kib);
	Holder space;
	std::atomic<bool> held{};
	std::thread holder{[&held] {
		const std::lock_guard guard{cache::ledger().mutex};
		held.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds{200});
	}};
	while (!held.load())
		std::this_thread::yield();
	space.account.charge(65 * kib);
	require(cache::over_budget());
	cache::relieve();
	require(space.account.shedding() && space.owner.notified == 1);
	holder.join();
	require(!cache::over_budget() && figures_agree());
	cache::set_budget(cache::fallback_budget);
}

/* Within the budget nothing is asked to shed, and an empty account never
 * is, however far over the rest are. */
void within_the_budget_nothing_sheds()
{
	cache::set_budget(64 * kib);
	const auto base = cache::ledger_state();
	Holder empty;
	Holder full;
	full.account.charge(64 * kib);
	require(!cache::over_budget());
	cache::relieve();
	require(cache::ledger_state().reliefs == base.reliefs);
	require(!full.account.shedding() && full.owner.notified == 0);
	full.account.charge(1);
	cache::relieve();
	require(full.account.shedding() && !empty.account.shedding());
	require(empty.owner.notified == 0);
	cache::set_budget(cache::fallback_budget);
}

/* Executors charge their address spaces and relieve the budget from their
 * own threads at once, and an address space comes and goes while another
 * relieves: what the ledger holds stays the sum of its accounts. */
void concurrent_charges_add_up()
{
	cache::set_budget(256 * kib);
	constexpr unsigned threads = 4;
	std::array<std::thread, threads> workers;
	std::atomic<std::size_t> notified{};
	for (unsigned index = 0; index < threads; ++index)
		workers[index] = std::thread{[index, &notified] {
			std::mt19937_64 random{index + 1U};
			for (unsigned round = 0; round < 200; ++round) {
				Holder holder;
				std::size_t held = 0;
				for (unsigned step = 0; step < 50; ++step) {
					const auto bytes = 512 +
						random() % (8 * kib);
					holder.account.charge(
						static_cast<std::ptrdiff_t>(bytes));
					held += bytes;
					if (cache::over_budget())
						cache::relieve();
					if (holder.account.shedding())
						break;
				}
				holder.account.charge(
					-static_cast<std::ptrdiff_t>(held / 2));
				notified.fetch_add(holder.owner.notified,
					std::memory_order_relaxed);
			}
		}};
	for (auto &worker : workers)
		worker.join();
	const auto state = cache::ledger_state();
	require(state.total == 0 && state.shedding == 0 &&
		state.accounts == 0);
	/* Every charge landed in one figure or the other exactly once. */
	require(cache::ledger().open.load() == 0 &&
		cache::ledger().shedding.load() == 0);
	require(notified.load() != 0);
	cache::set_budget(cache::fallback_budget);
}

} // namespace

int main()
{
	require(cache::budget() == cache::fallback_budget);
	derived_budgets();
	measured_workloads_stay_well_within_their_budget();
	relief_sheds_the_largest();
	a_shed_account_that_shrinks_leaves_the_open_ones_exact();
	relief_waits_for_the_lock();
	within_the_budget_nothing_sheds();
	concurrent_charges_add_up();
	return 0;
}
