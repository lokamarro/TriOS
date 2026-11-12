// SPDX-License-Identifier: GPL-2.0-or-later
#include "domain_contract.hpp"
#include "domain_registry_testing.hpp"
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "require.hpp"
#include "uml_darwin_host.h"
#include "uml_darwin_static_mmu.h"

#include "native_address_space.hpp"

#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unistd.h>

namespace {

std::atomic_uint64_t active_domain{};
std::atomic_bool capture_next_resolution{};
std::atomic_bool resolution_captured{};
std::atomic_bool release_resolution{};
std::atomic_bool capture_next_software_acquisition{};
std::atomic_bool software_acquisition_captured{};
std::atomic_bool release_software_acquisition{};

using uml::testing::require;

bool provide_test_domain(std::uint64_t &domain) noexcept
{
	const auto captured = active_domain.load(std::memory_order_acquire);
	if (capture_next_resolution.exchange(false, std::memory_order_acq_rel)) {
		resolution_captured.store(true, std::memory_order_release);
		resolution_captured.notify_all();
		release_resolution.wait(false, std::memory_order_acquire);
	}
	domain = captured;
	return domain != 0;
}

void capture_software_acquisition() noexcept
{
	if (!capture_next_software_acquisition.exchange(
		    false, std::memory_order_acq_rel))
		return;
	software_acquisition_captured.store(true, std::memory_order_release);
	software_acquisition_captured.notify_all();
	release_software_acquisition.wait(false, std::memory_order_acquire);
}

void begin_delayed_resolution(std::uint64_t domain)
{
	active_domain.store(domain, std::memory_order_release);
	release_resolution.store(false, std::memory_order_release);
	resolution_captured.store(false, std::memory_order_release);
	capture_next_resolution.store(true, std::memory_order_release);
}

void retire_captured_domain(std::uint64_t domain)
{
	resolution_captured.wait(false, std::memory_order_acquire);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	release_resolution.store(true, std::memory_order_release);
	release_resolution.notify_all();
}

void release_after_unrelated_retirement(std::uint64_t domain)
{
	resolution_captured.wait(false, std::memory_order_acquire);
	uml::memory::retire_domain(domain);
	release_resolution.store(true, std::memory_order_release);
	release_resolution.notify_all();
}

void test_software_retirement_does_not_initialize_registry()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::allocation_attempt_count;
	using uml::memory::testing::fail_next_allocation;
	using uml::memory::testing::reset_allocation_attempt_counts;
	constexpr std::uint64_t untouched_domain = 9'001;
	constexpr std::uint64_t activation_domain = 9'002;
	constexpr unsigned long generation = 900;
	int descriptors[2];
	require(pipe(descriptors) == 0);

	active_domain.store(0, std::memory_order_release);
	reset_allocation_attempt_counts();
	fail_next_allocation(AllocationSite::software_domain_registry);
	require(darwin_uml_close(descriptors[0]) == 0);
	require(close(descriptors[1]) == 0);
	uml::memory::retire_domain(untouched_domain);
	require(allocation_attempt_count(
		AllocationSite::software_domain_registry) == 0);

	/* Teardown for an untouched descriptor or domain leaves the injected failure
	 * for the first activating caller. That caller reports it, and a second
	 * activation retries initialization instead of caching a null permanently. */
	active_domain.store(activation_domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(generation) == -1);
	require(allocation_attempt_count(
		AllocationSite::software_domain_registry) == 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(allocation_attempt_count(
		AllocationSite::software_domain_registry) == 2);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(activation_domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

void test_software_delayed_resolver_cannot_recreate_retired_domain()
{
	constexpr std::uint64_t retired_domain = 10'001;
	constexpr unsigned long generation = 901;
	begin_delayed_resolution(retired_domain);
	std::atomic_int result{};
	std::thread resolver{[&] {
		result.store(darwin_uml_static_vm_activate(generation),
			std::memory_order_release);
	}};
	retire_captured_domain(retired_domain);
	resolver.join();
	require(result.load(std::memory_order_acquire) == -1);
	require(uml::memory::testing::software_domain_count() == 0);

	constexpr std::uint64_t retry_domain = retired_domain + 1;
	active_domain.store(retry_domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(uml::memory::testing::software_domain_count() == 1);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(retry_domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

void test_software_resolver_retries_after_unrelated_retirement()
{
	constexpr std::uint64_t live_domain = 11'001;
	constexpr std::uint64_t unrelated_domain = 11'002;
	constexpr unsigned long generation = 903;
	begin_delayed_resolution(live_domain);
	std::atomic_int result{-1};
	std::thread resolver{[&] {
		result.store(darwin_uml_static_vm_activate(generation),
			std::memory_order_release);
	}};
	release_after_unrelated_retirement(unrelated_domain);
	resolver.join();
	require(result.load(std::memory_order_acquire) == 0);
	require(uml::memory::testing::software_domain_count() == 1);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(live_domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

void test_software_acquired_space_refuses_post_retirement_state_creation()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::allocation_attempt_count;
	using uml::memory::testing::reset_allocation_attempt_counts;
	constexpr std::uint64_t domain = 12'001;
	constexpr unsigned long initial_generation = 904;
	constexpr unsigned long detached_generation = 905;

	active_domain.store(domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(initial_generation) == 0);
	require(uml::memory::testing::software_domain_count() == 1);
	reset_allocation_attempt_counts();
	release_software_acquisition.store(false, std::memory_order_release);
	software_acquisition_captured.store(false, std::memory_order_release);
	capture_next_software_acquisition.store(true, std::memory_order_release);
	uml::memory::testing::set_software_address_space_acquired_hook(
		&capture_software_acquisition);

	std::atomic_int result{};
	std::atomic_int observed_errno{};
	std::thread caller{[&] {
		errno = 0;
		result.store(darwin_uml_static_vm_activate(detached_generation),
			std::memory_order_release);
		observed_errno.store(errno, std::memory_order_release);
	}};
	software_acquisition_captured.wait(false, std::memory_order_acquire);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	require(uml::memory::testing::software_domain_count() == 0);
	release_software_acquisition.store(true, std::memory_order_release);
	release_software_acquisition.notify_all();
	caller.join();
	uml::memory::testing::set_software_address_space_acquired_hook(nullptr);

	require(result.load(std::memory_order_acquire) == -1);
	require(observed_errno.load(std::memory_order_acquire) == ESHUTDOWN);
	require(allocation_attempt_count(AllocationSite::generation) == 0);
}

void test_software_thread_state_recovers_after_domain_retirement()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::allocation_attempt_count;
	using uml::memory::testing::reset_allocation_attempt_counts;
	constexpr std::uint64_t retired_domain = 13'001;
	constexpr std::uint64_t replacement_domain = 13'002;
	constexpr unsigned long retired_generation = 906;
	constexpr unsigned long replacement_generation = 907;

	active_domain.store(retired_domain, std::memory_order_release);
	std::atomic_bool prepared{};
	std::atomic_bool resume{};
	std::atomic_bool initial_state_ready{};
	std::atomic_bool stale_operations_failed{};
	std::atomic_bool replacement_state_ready{};
	std::thread worker{[&] {
		const bool activated =
			darwin_uml_static_vm_activate(retired_generation) == 0;
		const bool batch_started =
			darwin_uml_static_vm_batch_begin(retired_generation) == 0;
		const bool walker_locked =
			darwin_uml_static_vm_walker_lock(retired_generation) == 0;
		/* An engine's execution context is its own, not the thread's: it
		 * keeps its generation alive across the retirement and is left
		 * inside the execution gate here, as a vCPU stopped mid-block
		 * would be. */
		void *execution =
			darwin_uml_static_vm_execution_context_create(
				retired_generation);
		const bool execution_started = execution != nullptr &&
			darwin_uml_static_vm_execution_context_begin(
				execution, 0) == 0;
		initial_state_ready.store(activated && batch_started && walker_locked &&
			execution_started, std::memory_order_release);
		prepared.store(true, std::memory_order_release);
		prepared.notify_all();
		resume.wait(false, std::memory_order_acquire);

		const bool old_batch_closed =
			darwin_uml_static_vm_batch_end() == -1;
		const bool old_walker_closed =
			darwin_uml_static_vm_walker_unlock(retired_generation) == -1;
		/* The retired context still leaves the gate it entered, but
		 * refuses to enter again. */
		const bool old_execution_closed = execution != nullptr &&
			darwin_uml_static_vm_execution_context_end(execution) == 0 &&
			darwin_uml_static_vm_execution_context_begin(
				execution, 0) == -1;
		darwin_uml_static_vm_execution_context_destroy(execution);
		stale_operations_failed.store(old_batch_closed && old_walker_closed &&
			old_execution_closed, std::memory_order_release);

		const bool replacement_activated =
			darwin_uml_static_vm_activate(replacement_generation) == 0;
		const bool replacement_batch =
			darwin_uml_static_vm_batch_begin(replacement_generation) == 0 &&
			darwin_uml_static_vm_batch_end() == 0;
		const bool replacement_walker =
			darwin_uml_static_vm_walker_lock(replacement_generation) == 0 &&
			darwin_uml_static_vm_walker_unlock(replacement_generation) == 0;
		void *replacement_context =
			darwin_uml_static_vm_execution_context_create(
				replacement_generation);
		const bool replacement_execution = replacement_context != nullptr &&
			darwin_uml_static_vm_execution_context_begin(
				replacement_context, 0) == 0 &&
			darwin_uml_static_vm_execution_context_end(
				replacement_context) == 0;
		darwin_uml_static_vm_execution_context_destroy(replacement_context);
		replacement_state_ready.store(replacement_activated &&
			replacement_batch && replacement_walker && replacement_execution,
			std::memory_order_release);
	}};
	prepared.wait(false, std::memory_order_acquire);
	require(initial_state_ready.load(std::memory_order_acquire));
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(retired_domain);
	require(uml::memory::testing::software_domain_count() == 0);
	reset_allocation_attempt_counts();
	active_domain.store(replacement_domain, std::memory_order_release);
	resume.store(true, std::memory_order_release);
	resume.notify_all();
	worker.join();

	require(stale_operations_failed.load(std::memory_order_acquire));
	require(replacement_state_ready.load(std::memory_order_acquire));
	require(allocation_attempt_count(AllocationSite::generation) == 1);
	require(uml::memory::testing::software_domain_count() == 1);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(replacement_domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

/* The resolved address space is cached per thread, so the thread that last used
 * a domain is the one that would serve a stale entry after it is retired.
 * Reusing the same domain id isolates the retirement-sequence check from the
 * domain comparison: a cache validated on the domain alone hands back the
 * retired space here, which then reports ESHUTDOWN and creates no replacement.
 */
void test_software_resolution_cache_drops_a_retired_domain()
{
	constexpr std::uint64_t domain = 15'001;
	constexpr unsigned long generation = 909;

	active_domain.store(domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(uml::memory::testing::software_domain_count() == 1);

	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	require(uml::memory::testing::software_domain_count() == 0);

	active_domain.store(domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(uml::memory::testing::software_domain_count() == 1);

	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

/* The destroyed-generation count moves when a generation is destroyed and
 * when its domain retires, and generation_live() answers 0 for a destroyed
 * generation once the count has moved, while its neighbour stays live. */
void test_software_destroyed_generations_move_the_count()
{
	constexpr std::uint64_t domain = 17'001;
	constexpr unsigned long kept_generation = 912;
	constexpr unsigned long destroyed_generation = 913;

	active_domain.store(domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(kept_generation) == 0);
	require(darwin_uml_static_vm_activate(destroyed_generation) == 0);
	require(darwin_uml_static_vm_generation_live(destroyed_generation) == 1);
	const auto before = darwin_uml_static_vm_generation_retirements();
	require(darwin_uml_static_vm_destroy(destroyed_generation) == 0);
	const auto destroyed = darwin_uml_static_vm_generation_retirements();
	require(destroyed != before);
	require(darwin_uml_static_vm_generation_live(destroyed_generation) == 0);
	require(darwin_uml_static_vm_generation_live(kept_generation) == 1);
	require(darwin_uml_static_vm_generation_retirements() == destroyed);

	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	require(darwin_uml_static_vm_generation_retirements() != destroyed);
}

std::atomic_bool nest_next_software_acquisition{};
std::atomic_int nested_activation_result{-2};
constexpr std::uint64_t borrowed_domain = 16'001;
constexpr std::uint64_t nested_domain = 16'002;
constexpr unsigned long borrowed_generation = 910;
constexpr unsigned long nested_generation = 911;

void retire_and_resolve_while_acquired() noexcept
{
	if (!nest_next_software_acquisition.exchange(
		    false, std::memory_order_acq_rel))
		return;
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(borrowed_domain);
	active_domain.store(nested_domain, std::memory_order_release);
	nested_activation_result.store(
		darwin_uml_static_vm_activate(nested_generation),
		std::memory_order_release);
}

/* An access may borrow its thread's cached reference to the address space, so
 * that cache entry must stay put from resolution until the access is gone.
 * Here the same thread retires the borrowed domain and resolves another one in
 * between; replacing the entry would free the space the outer access is about
 * to check. It must instead see the retirement and report ESHUTDOWN. */
void test_software_nested_resolution_keeps_a_borrowed_space()
{
	active_domain.store(borrowed_domain, std::memory_order_release);
	require(darwin_uml_static_vm_activate(borrowed_generation) == 0);
	nest_next_software_acquisition.store(true, std::memory_order_release);
	uml::memory::testing::set_software_address_space_acquired_hook(
		&retire_and_resolve_while_acquired);
	errno = 0;
	const int outer = darwin_uml_static_vm_activate(borrowed_generation);
	const int outer_errno = errno;
	uml::memory::testing::set_software_address_space_acquired_hook(nullptr);

	require(outer == -1 && outer_errno == ESHUTDOWN);
	require(nested_activation_result.load(std::memory_order_acquire) == 0);
	require(uml::memory::testing::software_domain_count() == 1);
	require(darwin_uml_static_vm_activate(nested_generation) == 0);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(nested_domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

void test_software_thread_exit_closes_walker()
{
	constexpr std::uint64_t domain = 14'001;
	constexpr unsigned long generation = 908;
	active_domain.store(domain, std::memory_order_release);
	auto *context = darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	std::atomic_bool prepared{};
	std::thread worker{[&] {
		const bool activated = darwin_uml_static_vm_activate(generation) == 0;
		const bool walker_locked =
			darwin_uml_static_vm_walker_lock(generation) == 0;
		prepared.store(activated && walker_locked,
			std::memory_order_release);
	}};
	worker.join();
	require(prepared.load(std::memory_order_acquire));

	auto &state = *static_cast<uml::memory::ExecutionContext *>(context)->state;
	require(darwin_uml_static_vm_walker_lock(generation) == 0);
	require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	require(state.execution_gate.load(std::memory_order_acquire) == 0);
	require(darwin_uml_static_vm_execution_context_begin(context, 0) == 0);
	require(darwin_uml_static_vm_execution_context_end(context) == 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
	darwin_uml_static_vm_execution_context_destroy(context);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	require(uml::memory::testing::software_domain_count() == 0);
}

void test_native_registry_initialization_retries_once()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::allocation_attempt_count;
	using uml::memory::testing::fail_next_allocation;
	using uml::memory::testing::reset_allocation_attempt_counts;
	constexpr std::uint64_t domain = 20'000;
	constexpr std::size_t thread_count = 8;

	active_domain.store(domain, std::memory_order_release);
	reset_allocation_attempt_counts();
	fail_next_allocation(AllocationSite::native_domain_registry);
	errno = 0;
	require(uml::memory::native_address_space() == nullptr && errno == ENOMEM);
	require(allocation_attempt_count(
		AllocationSite::native_domain_registry) == 1);

	std::array<std::shared_ptr<uml::memory::NativeAddressSpace>,
		thread_count> spaces;
	std::atomic_bool start{};
	std::array<std::thread, thread_count> threads;
	for (std::size_t index = 0; index < thread_count; ++index) {
		threads[index] = std::thread{[&, index] {
			start.wait(false, std::memory_order_acquire);
			spaces[index] = uml::memory::native_address_space();
		}};
	}
	start.store(true, std::memory_order_release);
	start.notify_all();
	for (auto &thread : threads)
		thread.join();
	for (const auto &space : spaces)
		require(space != nullptr && space == spaces.front());
	require(allocation_attempt_count(
		AllocationSite::native_domain_registry) == 2);
	require(uml::memory::testing::native_domain_count() == 1);

	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	for (auto &space : spaces)
		space.reset();
	require(uml::memory::testing::native_domain_count() == 0);
}

void initialize_native_retirement_observer()
{
	constexpr std::uint64_t domain = 20'001;
	active_domain.store(domain, std::memory_order_release);
	auto space = uml::memory::native_address_space();
	require(space != nullptr);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	space.reset();
	require(uml::memory::testing::native_domain_count() == 0);
}

void test_native_delayed_resolver_cannot_recreate_retired_domain()
{
	constexpr std::uint64_t retired_domain = 20'002;
	initialize_native_retirement_observer();
	begin_delayed_resolution(retired_domain);
	std::shared_ptr<uml::memory::NativeAddressSpace> result;
	std::thread resolver{[&] {
		result = uml::memory::native_address_space();
	}};
	retire_captured_domain(retired_domain);
	resolver.join();
	require(result == nullptr);
	require(uml::memory::testing::native_domain_count() == 0);

	constexpr std::uint64_t retry_domain = retired_domain + 1;
	active_domain.store(retry_domain, std::memory_order_release);
	result = uml::memory::native_address_space();
	require(result != nullptr);
	require(uml::memory::testing::native_domain_count() == 1);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(retry_domain);
	result.reset();
	require(uml::memory::testing::native_domain_count() == 0);
}

void test_native_resolver_retries_after_unrelated_retirement()
{
	constexpr std::uint64_t live_domain = 21'001;
	constexpr std::uint64_t unrelated_domain = 21'002;
	begin_delayed_resolution(live_domain);
	std::shared_ptr<uml::memory::NativeAddressSpace> result;
	std::thread resolver{[&] {
		result = uml::memory::native_address_space();
	}};
	release_after_unrelated_retirement(unrelated_domain);
	resolver.join();
	require(result != nullptr);
	require(uml::memory::testing::native_domain_count() == 1);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(live_domain);
	result.reset();
	require(uml::memory::testing::native_domain_count() == 0);
}

void test_native_acquired_space_refuses_post_retirement_work()
{
	constexpr std::uint64_t domain = 22'001;
	active_domain.store(domain, std::memory_order_release);
	auto space = uml::memory::native_address_space();
	require(space != nullptr);
	active_domain.store(0, std::memory_order_release);
	uml::memory::retire_domain(domain);
	require(uml::memory::testing::native_domain_count() == 0);
	errno = 0;
	require(space->retry_direct(0, false, false) == -1);
	require(errno == ESHUTDOWN);
	space.reset();
}

void test_retirement_metadata_stays_bounded_under_run_churn()
{
	constexpr std::uint64_t first_domain = 30'000;
	constexpr std::uint64_t run_count = 100'000;
	constexpr unsigned long generation = 902;
	for (std::uint64_t iteration = 0; iteration < run_count; ++iteration) {
		const auto domain = first_domain + iteration;
		active_domain.store(domain, std::memory_order_release);
		if ((iteration % 4096) == 0) {
			require(darwin_uml_static_vm_activate(generation) == 0);
			auto native = uml::memory::native_address_space();
			require(native != nullptr);
		}
		active_domain.store(0, std::memory_order_release);
		uml::memory::retire_domain(domain);
	}
	require(uml::memory::testing::software_domain_count() == 0);
	require(uml::memory::testing::native_domain_count() == 0);
}

} // namespace

int main()
{
	uml::memory::set_domain_provider(&provide_test_domain);
	test_software_retirement_does_not_initialize_registry();
	test_software_delayed_resolver_cannot_recreate_retired_domain();
	test_software_resolver_retries_after_unrelated_retirement();
	test_software_acquired_space_refuses_post_retirement_state_creation();
	test_software_thread_state_recovers_after_domain_retirement();
	test_software_resolution_cache_drops_a_retired_domain();
	test_software_destroyed_generations_move_the_count();
	test_software_nested_resolution_keeps_a_borrowed_space();
	test_software_thread_exit_closes_walker();
	test_native_registry_initialization_retries_once();
	test_native_delayed_resolver_cannot_recreate_retired_domain();
	test_native_resolver_retries_after_unrelated_retirement();
	test_native_acquired_space_refuses_post_retirement_work();
	test_retirement_metadata_stays_bounded_under_run_churn();
	uml::memory::set_domain_provider(nullptr);
	std::puts("memory domain lifetime tests passed");
	return EXIT_SUCCESS;
}
