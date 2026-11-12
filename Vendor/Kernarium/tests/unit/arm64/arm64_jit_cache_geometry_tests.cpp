// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_arena.hpp"
#include "cpu/arm64/jit/jit_linking.hpp"
#include "require.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

namespace arm64 = ::uml::cpu::arm64;

constexpr std::size_t mib = std::size_t{1024} * 1024U;
constexpr std::size_t gib = mib * 1024U;

using uml::testing::require;

void geometry_respects_every_supported_executor_count()
{
	const auto per_executor_minimum =
		arm64::minimum_jit_cache_budget_per_executor();
	const auto minimum = arm64::minimum_jit_cache_budget(1);
	require(per_executor_minimum != 0 && minimum > per_executor_minimum,
		"minimum geometry omitted the shared-cache base");
	require(!arm64::derive_jit_cache_geometry(minimum - 1U, 1).valid(),
		"a budget below the true one-executor minimum was accepted");
	const auto exact = arm64::derive_jit_cache_geometry(minimum, 1);
	require(exact.valid() &&
		exact.total_bytes() +
			arm64::derive_shared_translation_cache_budget(minimum, 1) <=
			minimum,
		"the exact one-executor minimum did not fit");

	const auto minimum_for_64 = arm64::minimum_jit_cache_budget(
		arm64::maximum_jit_cache_executors);
	const auto exact_64 = arm64::derive_jit_cache_geometry(
		minimum_for_64, arm64::maximum_jit_cache_executors);
	const auto shared_64 = arm64::derive_shared_translation_cache_budget(
		minimum_for_64, arm64::maximum_jit_cache_executors);
	require(exact_64.valid() &&
		exact_64.total_bytes() * arm64::maximum_jit_cache_executors +
			shared_64 <= minimum_for_64,
		"the exact 64-executor minimum did not fit each executor");
	require(!arm64::derive_jit_cache_geometry(minimum_for_64, 0).valid(),
		"zero executors were accepted");
	require(!arm64::derive_jit_cache_geometry(
		minimum_for_64, arm64::maximum_jit_cache_executors + 1U).valid(),
		"an executor count above CONFIG_NR_CPUS was accepted");

	/* The default 256 MiB guest yields a 128 MiB whole-process JIT budget.
	 * It must remain usable even when all 64 configured CPUs are effective.
	 */
	const auto default_64 = arm64::derive_jit_cache_geometry(
		128U * mib, arm64::maximum_jit_cache_executors);
	const auto default_shared =
		arm64::derive_shared_translation_cache_budget(
			128U * mib, arm64::maximum_jit_cache_executors);
	require(default_64.valid() &&
		default_64.total_bytes() * arm64::maximum_jit_cache_executors +
			default_shared <= 128U * mib,
		"default guest memory cannot fund 64 JIT executors");

	const auto extreme = arm64::derive_jit_cache_geometry(
		std::numeric_limits<std::size_t>::max(),
		arm64::maximum_jit_cache_executors);
	const auto extreme_shared =
		arm64::derive_shared_translation_cache_budget(
			std::numeric_limits<std::size_t>::max(),
			arm64::maximum_jit_cache_executors);
	require(extreme.valid() &&
		extreme.total_bytes() <=
			(std::numeric_limits<std::size_t>::max() - extreme_shared) /
			arm64::maximum_jit_cache_executors,
		"extreme budget overflowed its per-executor share");
}

void representative_geometry_remains_stable_and_bounded()
{
	constexpr std::array guest_sizes{4U * gib, 8U * gib, 16U * gib};
	for (const auto executors : std::array{4U, 10U}) {
		std::optional<arm64::JitCacheGeometry> expected_geometry;
		std::size_t preceding_shared = 0;
		for (const auto guest_bytes : guest_sizes) {
			const auto total = guest_bytes / 2U;
			const auto geometry =
				arm64::derive_jit_cache_geometry(total, executors);
			const auto shared =
				arm64::derive_shared_translation_cache_budget(total, executors);
			require(geometry.valid(), "representative geometry was rejected");
			require(geometry.total_bytes() <=
					arm64::default_jit_cache_budget_per_executor,
				"private executor geometry exceeded its fixed cap");
			if (total >= executors *
					arm64::default_jit_cache_budget_per_executor +
					arm64::minimum_shared_translation_cache_bytes) {
				if (expected_geometry)
					require(geometry == *expected_geometry,
						"fully funded private geometry grew with guest memory");
				else
					expected_geometry = geometry;
			}
			require(shared > preceding_shared,
				"shared cache did not grow with guest memory");
			require(geometry.total_bytes() * executors + shared <= total,
				"representative geometry exceeded its executor budget");
			preceding_shared = shared;
		}
	}

	std::optional<arm64::JitCacheGeometry> preceding_geometry;
	for (const auto cap :
	     std::array{16U * mib, 32U * mib, 48U * mib, 64U * mib,
		128U * mib, 256U * mib}) {
		const auto geometry = arm64::derive_jit_cache_geometry(
			4U * gib, 10, cap);
		require(geometry.valid() && geometry.total_bytes() <= cap,
			"private tuning cap was not respected");
		if (preceding_geometry) {
			require(geometry.cache_sets >= preceding_geometry->cache_sets &&
				geometry.arena_bytes >= preceding_geometry->arena_bytes,
				"larger private cap reduced private geometry");
		}
		preceding_geometry = geometry;
	}
	require(!arm64::derive_jit_cache_geometry(
		4U * gib, 10,
		arm64::maximum_jit_cache_budget_per_executor + 1U).valid(),
		"private cap above the production ceiling was accepted");
}

void publication_is_an_atomic_first_valid_pair()
{
	const auto minimum = arm64::minimum_jit_cache_budget(1);
	require(!arm64::set_jit_cache_budget(0, 1),
		"zero-byte publication succeeded");
	require(!arm64::set_jit_cache_budget(minimum - 1U, 1),
		"below-minimum publication succeeded");
	require(!arm64::set_jit_cache_budget(minimum, 0),
		"zero-executor publication succeeded");

	struct Configuration final {
		std::size_t total;
		unsigned executors;
	};
	constexpr Configuration first{512U * mib, 1};
	constexpr Configuration second{512U * mib, 64};
	std::atomic<unsigned> ready{};
	std::atomic<bool> start{};
	bool first_accepted = false;
	bool second_accepted = false;
	const auto publish = [&](Configuration configuration, bool &accepted) {
		ready.fetch_add(1, std::memory_order_release);
		while (!start.load(std::memory_order_acquire))
			std::this_thread::yield();
		accepted = arm64::set_jit_cache_budget(
			configuration.total, configuration.executors);
	};
	std::thread first_thread{publish, first, std::ref(first_accepted)};
	std::thread second_thread{publish, second, std::ref(second_accepted)};
	while (ready.load(std::memory_order_acquire) != 2U)
		std::this_thread::yield();
	start.store(true, std::memory_order_release);
	first_thread.join();
	second_thread.join();
	require(first_accepted != second_accepted,
		"concurrent incompatible pairs did not choose exactly one winner");

	const auto winner = first_accepted ? first : second;
	const auto loser = first_accepted ? second : first;
	const auto expected = arm64::derive_jit_cache_geometry(
		winner.total, winner.executors);
	require(arm64::jit_cache_geometry() == expected,
		"published geometry combined fields from different pairs");
	require(arm64::set_jit_cache_budget(winner.total, winner.executors),
		"an identical sequential publication was rejected");
	require(!arm64::set_jit_cache_budget(loser.total, loser.executors),
		"an incompatible sequential publication was silently accepted");
	require(!arm64::set_jit_cache_budget(
		winner.total, winner.executors, 32U * mib),
		"an incompatible private cap was silently accepted");
	require(arm64::jit_cache_geometry() == expected,
		"a rejected pair changed the published geometry");

	/* The winning pair is also the process-wide live-executor ceiling. Hold
	 * every contender until all threads have attempted admission so released
	 * slots cannot be recycled within this observation.
	 */
	const auto contenders = winner.executors + 8U;
	std::atomic<unsigned> lease_ready{};
	std::atomic<unsigned> lease_admitted{};
	std::atomic<bool> release_leases{};
	std::vector<std::thread> lease_threads;
	lease_threads.reserve(contenders);
	for (unsigned index = 0; index < contenders; ++index) {
		lease_threads.emplace_back([&] {
			arm64::JitCacheExecutorLease lease;
			lease_admitted.fetch_add(
				lease.admitted(), std::memory_order_relaxed);
			lease_ready.fetch_add(1, std::memory_order_release);
			while (!release_leases.load(std::memory_order_acquire))
				std::this_thread::yield();
		});
	}
	while (lease_ready.load(std::memory_order_acquire) != contenders)
		std::this_thread::yield();
	require(lease_admitted.load(std::memory_order_relaxed) ==
			winner.executors,
		"concurrent executors exceeded the published count");
	release_leases.store(true, std::memory_order_release);
	for (auto &thread : lease_threads)
		thread.join();
	arm64::JitCacheExecutorLease recycled;
	require(recycled.admitted(),
		"a released executor slot could not be reused");
}

void one_jit_run_session_is_admitted_at_a_time()
{
	constexpr unsigned contenders = 12;
	std::atomic<unsigned> ready{};
	std::atomic<unsigned> admitted{};
	std::atomic<bool> release{};
	std::array<std::thread, contenders> threads;
	for (auto &thread : threads) {
		thread = std::thread{[&] {
			arm64::JitCacheRunSession session;
			admitted.fetch_add(session.admitted(),
				std::memory_order_relaxed);
			ready.fetch_add(1, std::memory_order_release);
			while (!release.load(std::memory_order_acquire))
				std::this_thread::yield();
		}};
	}
	while (ready.load(std::memory_order_acquire) != contenders)
		std::this_thread::yield();
	require(admitted.load(std::memory_order_relaxed) == 1,
		"multiple concurrent JIT run sessions were admitted");
	release.store(true, std::memory_order_release);
	for (auto &thread : threads)
		thread.join();
	arm64::JitCacheRunSession retry;
	require(retry.admitted(), "a completed JIT session was not reusable");
}

void a_standalone_executor_lease_is_excluded_by_a_managed_session()
{
	{
		arm64::JitCacheRunSession session;
		require(session.admitted(), "the lifecycle test session was rejected");
		arm64::JitCacheExecutorLease standalone;
		require(!standalone.admitted(),
			"a standalone lease entered an active managed session");
	}
	{
		arm64::JitCacheExecutorLease standalone;
		require(standalone.admitted(),
			"standalone admission did not recover after managed teardown");
	}
	arm64::JitCacheRunSession recovered;
	require(recovered.admitted(),
		"managed admission did not recover after standalone teardown");
}

void fixed_link_tables_are_stable_bounded_and_reusable()
{
	arm64::JitBlockLink empty{.guest_pc = 1};
	arm64::JitBlockLinkTable direct{3};
	arm64::JitIndirectLinkTable indirect{3, &empty};
	require(direct.capacity() == 3 && indirect.capacity() == 3,
		"fixed link-table capacity changed");
	require(arm64::JitBlockLinkTable::allocation_bytes(3) >
			3 * sizeof(arm64::JitBlockLink),
		"direct-link records and index were not charged");
	require(arm64::JitIndirectLinkTable::allocation_bytes(3) >
			3 * sizeof(arm64::JitIndirectLink),
		"indirect-link index/allocation bytes were not charged");
	auto *first = direct.find_or_create(0x1000);
	require(first != nullptr && direct.find(0x1000) == first &&
		direct.find_or_create(0x1000) == first,
		"direct-link address was not stable");
	require(direct.find_or_create(0x2000) != nullptr &&
		direct.find_or_create(0x3000) != nullptr &&
		direct.find_or_create(0x4000) == nullptr && direct.size() == 3,
		"direct-link table did not stop at fixed capacity");
	auto *site = indirect.find_or_create(0x5000);
	require(site != nullptr && indirect.find(0x5000) == site &&
		indirect.find_or_create(0x5000) == site,
		"indirect-link address was not stable");
	require(indirect.find_or_create(0x6000) != nullptr &&
		indirect.find_or_create(0x7000) != nullptr &&
		indirect.find_or_create(0x8000) == nullptr && indirect.size() == 3,
		"indirect-link table did not stop at fixed capacity");
	direct.clear();
	indirect.clear();
	require(direct.size() == 0 && indirect.size() == 0 &&
		direct.find_or_create(0x9000) != nullptr &&
		indirect.find_or_create(0xa000) != nullptr,
		"fixed link tables could not reuse storage after clear");
}

void direct_link_records_are_writable_outside_the_code_arena()
{
	constexpr std::uint64_t pc = 0x1000;
	constexpr std::uint64_t target = 0x2000;
	constexpr std::array<std::uint32_t, 2> words{};
	constexpr std::array relocations{
		arm64::JitRelocation{
			.word = 0,
			.key = target,
			.kind = arm64::JitRelocationKind::direct_link,
		},
	};
	arm64::JitBlockLinkTable links{4};
	arm64::JitBlockLink empty{.guest_pc = 1};
	arm64::jit::detail::ExecutableArena arena{16U * 1024U};
	require(arena.valid(), "the JIT arena could not be allocated");
	const auto stored =
		arena.store_block(pc, words, relocations, {}, links, empty);
	auto *direct_link = links.find(target);
	require(static_cast<bool>(stored) && stored.target_link != nullptr &&
		direct_link != nullptr,
		"code publication did not resolve its private link records");
	const auto code_address = reinterpret_cast<std::uintptr_t>(stored.code);
	const auto link_address = reinterpret_cast<std::uintptr_t>(stored.target_link);
	require(link_address < code_address ||
		link_address >= code_address + 16U * 1024U,
		"mutable link state still occupies executable storage");
	stored.target_link->allowed_selectors = 3;
	require(stored.target_link->allowed_selectors == 3,
		"link state could not be updated after executable publication");
	std::uint64_t patched{};
	std::memcpy(&patched, stored.code, sizeof(patched));
	require(patched == reinterpret_cast<std::uintptr_t>(direct_link),
		"the direct-link literal does not point to its private record");
	arm64::JitBlockLinkActivation activation{
		.instruction_count = 3,
		.internal_entry = 0x1234,
		.allowed_selectors = 1,
		.epoch = 9,
	};
	arena.activate_link(*direct_link, activation);
	require(direct_link->instruction_count == 3 &&
		direct_link->internal_entry == 0x1234 &&
		direct_link->allowed_selectors == 1 &&
		direct_link->activation_epoch == 9,
		"the reverse index did not activate the adjacent target anchor");
	const auto repeated =
		arena.store_block(0x3000, words, relocations, {}, links, empty);
	require(static_cast<bool>(repeated),
		"the repeated-target code block was not stored");
	std::uint64_t repeated_patch{};
	std::memcpy(&repeated_patch, repeated.code, sizeof(repeated_patch));
	require(repeated_patch == reinterpret_cast<std::uintptr_t>(direct_link) &&
		links.find(target) == direct_link && links.size() == 3,
		"a repeated target allocated duplicate mutable activation state");
}

} // namespace

int main()
{
	try {
		geometry_respects_every_supported_executor_count();
		representative_geometry_remains_stable_and_bounded();
		publication_is_an_atomic_first_valid_pair();
		one_jit_run_session_is_admitted_at_a_time();
		a_standalone_executor_lease_is_excluded_by_a_managed_session();
		fixed_link_tables_are_stable_bounded_and_reusable();
		direct_link_records_are_writable_outside_the_code_arena();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "JIT cache geometry test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
