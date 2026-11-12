// SPDX-License-Identifier: GPL-2.0-or-later
#include "allocation_failure.hpp"
#include "cpu/engine.hpp"
#include "guest_register_tail.hpp"
#include "memory/domain_contract.hpp"
#include "platform/host_environment.hpp"
#include "platform/run_routing.hpp"
#include "require.hpp"
#include "runtime/context.hpp"
#include "runtime/execution_engine.hpp"
#include "runtime/execution_state.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_cpu.h"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <poll.h>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using EngineKind = uml::cpu::EngineKind;

using uml::testing::require;

/* An engine that never runs a guest; a run is a fatal exit. The engines below
 * differ in what their destruction does. */
class FakeEngine : public uml::cpu::CpuEngine {
public:
	[[nodiscard]] uml::cpu::EngineKind kind() const noexcept override
	{
		return kind_;
	}

	[[nodiscard]] uml::cpu::ExecutionResult run(
		uml::cpu::ExecutionRequest &) noexcept override
	{
		return {.reason = uml::cpu::ExitReason::fatal};
	}

protected:
	explicit FakeEngine(EngineKind kind = EngineKind::gadgets) noexcept
		: kind_{kind}
	{
	}

private:
	EngineKind kind_;
};

class DestructionCounterEngine final : public FakeEngine {
public:
	explicit DestructionCounterEngine(std::atomic_size_t &counter,
		EngineKind kind = EngineKind::gadgets) noexcept
		: FakeEngine{kind}, counter_{counter}
	{
	}

	~DestructionCounterEngine() override
	{
		counter_.fetch_add(1, std::memory_order_release);
	}

private:
	std::atomic_size_t &counter_;
};

class DomainRetirementObserverEngine final : public FakeEngine {
public:
	explicit DomainRetirementObserverEngine(
		std::atomic_uint64_t &observed) noexcept
		: observed_{observed}
	{
	}

	~DomainRetirementObserverEngine() override
	{
		observed_.store(uml::memory::domain_retirement_sequence(),
			std::memory_order_release);
	}

private:
	std::atomic_uint64_t &observed_;
};

[[nodiscard]] bool read_guest_byte(
	uml::runtime::ThreadExecutionState &state, unsigned long generation,
	std::uint64_t changes, std::uint64_t address,
	std::uint8_t expected)
{
	std::array<std::byte, 1> value{};
	state.memory.prepare(generation, changes);
	return state.memory.read(address, std::span{value}) &&
		value.front() == static_cast<std::byte>(expected);
}

[[nodiscard]] bool execution_state_is_fresh(
	const uml::runtime::ThreadExecutionState &state,
	std::uint64_t marker) noexcept
{
	return state.cpu.registers[0] != marker &&
		state.engines.empty() &&
		state.interrupt.sequence == nullptr && state.interrupt.snapshot == 0;
}

struct RetirementInterleave final {
	std::mutex mutex;
	std::condition_variable changed;
	bool destructor_entered{};
	bool release_destructor{};
	bool contender_started{};
	bool contender_admitted{};
	bool contender_state_acquired{};
	bool release_contender{};
	bool fresh_state{};
	bool destructor_timed_out{};
};

class BlockingRetirementEngine final : public FakeEngine {
public:
	explicit BlockingRetirementEngine(RetirementInterleave &interleave) noexcept
		: interleave_{interleave}
	{
	}

	~BlockingRetirementEngine() override
	{
		std::unique_lock lock{interleave_.mutex};
		interleave_.destructor_entered = true;
		interleave_.changed.notify_all();
		if (!interleave_.changed.wait_for(lock, 5s, [&] {
			return interleave_.release_destructor;
		}))
			interleave_.destructor_timed_out = true;
	}

private:
	RetirementInterleave &interleave_;
};

void test_pre_run_memory_domain_is_retired_before_run()
{
	uml::runtime::Context context;
	const auto pre_run_domain = context.memory_domain();
	require(pre_run_domain != 0,
		"new Context did not own a pre-run compatibility domain");
	{
		uml::routing::ActiveRouteScope active{&context, nullptr};
		require(darwin_uml_static_vm_activate(71) == 0,
			"pre-run Context could not materialize memory-domain state");
	}
	const auto retirement_before =
		uml::memory::domain_retirement_sequence();
	require(context.begin_run(), "Context run was not admitted");
	require(context.memory_domain() != 0 &&
		context.memory_domain() != pre_run_domain,
		"Context run reused its pre-run memory domain");
	require(uml::memory::domain_retirement_sequence() ==
		retirement_before + 1,
		"Context run orphaned its pre-run memory domain");
	context.end_run();
	require(context.memory_domain() == 0,
		"completed Context run retained a memory domain");
	require(uml::memory::domain_retirement_sequence() ==
		retirement_before + 2,
		"completed Context run did not retire its active domain");
}

void test_pre_run_execution_states_do_not_cross_run_boundary()
{
	constexpr std::size_t thread_count = 8;
	constexpr unsigned long generation = 72;
	constexpr std::uint64_t guest_address =
		32ULL * DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint8_t pre_run_value = 0x35;
	constexpr std::uint8_t run_value = 0xca;
	constexpr std::uint64_t main_marker = 0xcafe'feed'1234'5678ULL;

	struct WorkerResult final {
		bool prepared{};
		bool fresh{};
		bool read_run_value{};
	};

	uml::runtime::Context context;
	uml::routing::ActiveRouteScope active{&context, nullptr};
	uml::testing::ScratchBacking pre_run_backing{
		"uml-runtime-context-XXXXXX", DARWIN_UML_GUEST_PAGE_SIZE};
	uml::testing::ScratchBacking run_backing{
		"uml-runtime-context-XXXXXX", DARWIN_UML_GUEST_PAGE_SIZE};
	pre_run_backing.put_byte(0, std::byte{pre_run_value});
	run_backing.put_byte(0, std::byte{run_value});

	unsigned int main_interrupt = 41;
	uml::testing::fail_next_allocation();
	const bool allocation_failure_reported =
		darwin_uml_static_guest_interrupt_sequence(
			&main_interrupt, main_interrupt) == -1;
	const bool allocation_failure_consumed =
		!uml::testing::cancel_pending_allocation_failure();
	const bool allocation_retry_succeeded =
		darwin_uml_static_guest_interrupt_sequence(
			&main_interrupt, main_interrupt) == 0;

	const bool pre_run_mapped = darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(guest_address),
		pre_run_backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ, 0, 0) == 0;
	const auto pre_run_sequences =
		darwin_uml_static_vm_sequences(generation);

	std::atomic_size_t destroyed{};
	bool main_pre_run_read = false;
	{
		auto main_state = context.thread_execution_state();
		main_pre_run_read = pre_run_mapped && read_guest_byte(
			*main_state, generation, pre_run_sequences.mapping_changes,
			guest_address, pre_run_value);
		main_state->cpu.registers[0] = main_marker;
		main_state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DestructionCounterEngine>(destroyed);
	}

	std::array<unsigned int, thread_count> interrupts{};
	std::array<WorkerResult, thread_count> results{};
	std::array<std::thread, thread_count> workers;
	std::mutex mutex;
	std::condition_variable changed;
	std::size_t ready{};
	bool release{};
	bool inspect_run{};
	std::uint64_t run_mapping_changes{};

	for (std::size_t index = 0; index < thread_count; ++index) {
		interrupts[index] = static_cast<unsigned int>(100 + index);
		workers[index] = std::thread{[&, index] {
			const auto marker = main_marker + index + 1;
			const bool interrupt_registered =
				darwin_uml_static_guest_interrupt_sequence(
					&interrupts[index], interrupts[index]) == 0;
			{
				auto state = context.thread_execution_state();
				results[index].prepared = interrupt_registered &&
					pre_run_mapped && read_guest_byte(
						*state, generation,
						pre_run_sequences.mapping_changes,
						guest_address, pre_run_value);
				state->cpu.registers[0] = marker;
				state->engines.slot<EngineKind::gadgets>() =
					std::make_unique<DestructionCounterEngine>(destroyed);
			}
			{
				std::lock_guard lock{mutex};
				++ready;
			}
			changed.notify_all();

			bool should_inspect = false;
			std::uint64_t changes = 0;
			{
				std::unique_lock lock{mutex};
				(void)changed.wait_for(lock, 5s, [&] { return release; });
				should_inspect = inspect_run;
				changes = run_mapping_changes;
			}
			if (!should_inspect)
				return;

			auto run_state = context.thread_execution_state();
			results[index].fresh =
				execution_state_is_fresh(*run_state, marker);
			if (results[index].fresh)
				results[index].read_run_value = read_guest_byte(
					*run_state, generation, changes,
					guest_address, run_value);
		}};
	}

	bool workers_ready = false;
	{
		std::unique_lock lock{mutex};
		workers_ready = changed.wait_for(lock, 5s, [&] {
			return ready == thread_count;
		});
	}

	uml::testing::fail_next_allocation();
	const bool admitted = workers_ready && context.begin_run();
	const bool begin_was_allocation_free =
		uml::testing::cancel_pending_allocation_failure();
	const auto destroyed_at_boundary =
		destroyed.load(std::memory_order_acquire);

	bool run_mapped = false;
	bool counters_reused = false;
	bool main_state_fresh = false;
	bool main_read_run_value = false;
	if (admitted) {
		run_mapped = darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(guest_address),
			run_backing.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ, 0, 0) == 0;
		const auto run_sequences =
			darwin_uml_static_vm_sequences(generation);
		run_mapping_changes = run_sequences.mapping_changes;
		counters_reused = run_sequences.mapping_changes ==
			pre_run_sequences.mapping_changes;
		auto run_state = context.thread_execution_state();
		main_state_fresh =
			execution_state_is_fresh(*run_state, main_marker);
		if (main_state_fresh && run_mapped)
			main_read_run_value = read_guest_byte(
				*run_state, generation, run_mapping_changes,
				guest_address, run_value);
	}
	{
		std::lock_guard lock{mutex};
		inspect_run = admitted && run_mapped;
		release = true;
	}
	changed.notify_all();
	for (auto &worker : workers)
		worker.join();
	if (admitted)
		context.end_run();

	require(allocation_failure_reported && allocation_failure_consumed,
		"pre-run public state allocation failure was not contained");
	require(allocation_retry_succeeded,
		"pre-run public state allocation could not be retried");
	require(pre_run_mapped && main_pre_run_read,
		"pre-run translated cache could not be populated");
	require(workers_ready && std::ranges::all_of(results,
		[](const WorkerResult &result) { return result.prepared; }),
		"concurrent pre-run execution states were not materialized");
	require(admitted && begin_was_allocation_free,
		"pre-run retirement did not admit without allocation");
	require(destroyed_at_boundary == thread_count + 1,
		"pre-run execution objects survived the run boundary");
	require(run_mapped && counters_reused,
		"fresh address-space state did not reproduce the cache key");
	require(main_state_fresh && main_read_run_value,
		"caller execution state or translated cache crossed the run boundary");
	require(std::ranges::all_of(results,
		[](const WorkerResult &result) {
			return result.fresh && result.read_run_value;
		}), "worker execution state or translated cache crossed the run boundary");
}

void test_rejected_begin_preserves_active_execution_state()
{
	uml::runtime::Context context;
	std::atomic_size_t destroyed{};
	require(context.begin_run(), "first admission was rejected");
	const auto domain = context.memory_domain();
	uml::runtime::ThreadExecutionState *state_address = nullptr;
	{
		auto state = context.thread_execution_state();
		state_address = state.operator->();
		state->cpu.registers[0] = 0x1234;
		state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DestructionCounterEngine>(destroyed);
	}
	context.request_stop();

	require(!context.begin_run(), "overlapping admission was accepted");
	{
		auto preserved = context.thread_execution_state();
		require(context.memory_domain() == domain && context.stop_requested(),
			"rejected admission changed active run control state");
		require(preserved.operator->() == state_address &&
			preserved->cpu.registers[0] == 0x1234 &&
			preserved->engines.slot<EngineKind::gadgets>() != nullptr &&
			destroyed.load(std::memory_order_acquire) == 0,
			"rejected admission changed active execution state");
	}

	context.end_run();
	require(destroyed.load(std::memory_order_acquire) == 1,
		"active execution state was not retired exactly once");
	require(context.begin_run(), "admission could not be retried");
	{
		auto retried = context.thread_execution_state();
		require(retried->cpu.registers[0] != 0x1234 &&
			retried->engines.empty(),
			"retried admission inherited prior execution state");
	}
	context.end_run();
}

void test_context_destruction_retires_state_before_domain()
{
	std::atomic_uint64_t observed_retirement{UINT64_MAX};
	const auto retirement_before =
		uml::memory::domain_retirement_sequence();
	{
		uml::runtime::Context context;
		auto state = context.thread_execution_state();
		state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DomainRetirementObserverEngine>(
				observed_retirement);
	}
	require(observed_retirement.load(std::memory_order_acquire) ==
		retirement_before,
		"Context retired its domain before its execution objects");
	require(uml::memory::domain_retirement_sequence() ==
		retirement_before + 1,
		"Context destruction did not retire its pre-run domain");
}

/* The resolution cache is one thread-local entry, so a thread that alternates
 * between two live contexts must be answered from the right one. Without the
 * context comparison the second resolution returns the first context's object,
 * and the two would share engine slots and guest memory across a boundary that
 * exists precisely to keep them apart. */
void test_execution_state_is_not_shared_between_contexts()
{
	uml::runtime::Context first;
	uml::runtime::Context second;

	auto *first_state = &*first.thread_execution_state();
	auto *second_state = &*second.thread_execution_state();
	require(first_state != second_state,
		"two contexts resolved to one execution object");

	/* Re-resolve in the opposite order: a cache keyed only on liveness would
	 * now be warm for the second context and answer the first with it. */
	require(&*second.thread_execution_state() == second_state,
		"second context did not resolve to its own object");
	require(&*first.thread_execution_state() == first_state,
		"first context did not resolve to its own object");
}

void test_named_engine_slots_share_execution_state_lifetime()
{
	uml::runtime::Context context;
	std::atomic_size_t destroyed{};
	require(context.begin_run(), "engine-slot run was not admitted");
	{
		auto state = context.thread_execution_state();
		state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DestructionCounterEngine>(
				destroyed, EngineKind::gadgets);
		state->engines.slot<EngineKind::jit>() =
			std::make_unique<DestructionCounterEngine>(
				destroyed, EngineKind::jit);
		require(state->engines.active_count() == 2 &&
			state->engines.slot<EngineKind::gadgets>()->kind() ==
				EngineKind::gadgets &&
			state->engines.slot<EngineKind::jit>()->kind() ==
				EngineKind::jit,
			"engine kinds were not stored in their named runtime slots");
	}
	context.end_run();
	require(destroyed.load(std::memory_order_acquire) == 2,
		"run retirement did not destroy all named engine slots");

	require(context.begin_run(), "engine-slot retry was not admitted");
	{
		auto state = context.thread_execution_state();
		require(state->engines.empty(),
			"fresh execution state inherited a named engine slot");
	}
	context.end_run();
}

void test_run_retirement_waits_for_execution_state_access()
{
	uml::runtime::Context context;
	std::atomic_size_t destroyed{};
	std::mutex mutex;
	std::condition_variable changed;
	bool access_acquired{};
	bool release_access{};
	bool retirement_started{};
	bool retirement_finished{};
	bool state_remained_live{};

	require(context.begin_run(), "access-overlap run was not admitted");
	const auto retirement_before =
		uml::memory::domain_retirement_sequence();
	std::thread holder{[&] {
		auto state = context.thread_execution_state();
		state->cpu.registers[0] = 0x9876;
		state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DestructionCounterEngine>(destroyed);
		{
			std::lock_guard lock{mutex};
			access_acquired = true;
		}
		changed.notify_all();
		{
			std::unique_lock lock{mutex};
			(void)changed.wait_for(lock, 5s, [&] { return release_access; });
			state_remained_live = state->cpu.registers[0] == 0x9876 &&
				state->engines.slot<EngineKind::gadgets>() != nullptr;
		}
	}};
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, 5s, [&] { return access_acquired; }),
			"execution-state access was not acquired");
	}

	std::thread finisher{[&] {
		{
			std::lock_guard lock{mutex};
			retirement_started = true;
		}
		changed.notify_all();
		context.end_run();
		{
			std::lock_guard lock{mutex};
			retirement_finished = true;
		}
		changed.notify_all();
	}};
	bool finished_while_accessed = false;
	bool domain_retired_while_accessed = false;
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, 5s, [&] { return retirement_started; }),
			"run retirement did not start");
		finished_while_accessed = changed.wait_for(
			lock, 100ms, [&] { return retirement_finished; });
		domain_retired_while_accessed =
			uml::memory::domain_retirement_sequence() != retirement_before;
		release_access = true;
	}
	changed.notify_all();
	holder.join();
	finisher.join();

	require(!finished_while_accessed && !domain_retired_while_accessed,
		"run retirement crossed a live execution-state access");
	require(state_remained_live &&
		destroyed.load(std::memory_order_acquire) == 1,
		"execution-state access did not retain its object through retirement");
	require(uml::memory::domain_retirement_sequence() ==
		retirement_before + 1,
		"run retirement did not retire the domain after access release");
}

void test_run_admission_waits_for_pre_run_execution_state_access()
{
	uml::runtime::Context context;
	std::atomic_size_t destroyed{};
	std::mutex mutex;
	std::condition_variable changed;
	bool admission_started{};
	bool admission_finished{};
	bool admitted{};
	bool finished_while_accessed{};
	bool state_remained_live{};
	bool domain_changed_while_accessed{};
	const auto pre_run_domain = context.memory_domain();
	const auto retirement_before =
		uml::memory::domain_retirement_sequence();
	std::thread beginner;
	{
		auto state = context.thread_execution_state();
		state->cpu.registers[0] = 0x2468;
		state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DestructionCounterEngine>(destroyed);
		beginner = std::thread{[&] {
			{
				std::lock_guard lock{mutex};
				admission_started = true;
			}
			changed.notify_all();
			const bool result = context.begin_run();
			{
				std::lock_guard lock{mutex};
				admitted = result;
				admission_finished = true;
			}
			changed.notify_all();
		}};
		{
			std::unique_lock lock{mutex};
			require(changed.wait_for(lock, 5s, [&] {
				return admission_started;
			}), "run admission did not start");
			finished_while_accessed = changed.wait_for(
				lock, 100ms, [&] { return admission_finished; });
		}
		state_remained_live = state->cpu.registers[0] == 0x2468 &&
			state->engines.slot<EngineKind::gadgets>() != nullptr &&
			destroyed.load(std::memory_order_acquire) == 0;
		domain_changed_while_accessed =
			context.memory_domain() != pre_run_domain ||
			uml::memory::domain_retirement_sequence() != retirement_before;
	}
	beginner.join();

	require(!finished_while_accessed && !domain_changed_while_accessed,
		"run admission crossed a live pre-run execution-state access");
	require(state_remained_live && admitted &&
		destroyed.load(std::memory_order_acquire) == 1,
		"pre-run execution object did not remain live through access release");
	require(context.memory_domain() != pre_run_domain &&
		uml::memory::domain_retirement_sequence() == retirement_before + 1,
		"run admission did not rotate the domain after access release");
	context.end_run();
}

void test_context_destruction_waits_for_execution_state_access()
{
	auto context = std::make_unique<uml::runtime::Context>();
	auto *const context_pointer = context.get();
	std::atomic_size_t destroyed{};
	std::mutex mutex;
	std::condition_variable changed;
	bool access_acquired{};
	bool release_access{};
	bool destruction_started{};
	bool destruction_finished{};
	bool state_remained_live{};
	const auto retirement_before =
		uml::memory::domain_retirement_sequence();

	std::thread holder{[&] {
		auto state = context_pointer->thread_execution_state();
		state->cpu.registers[0] = 0x5678;
		state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<DestructionCounterEngine>(destroyed);
		{
			std::lock_guard lock{mutex};
			access_acquired = true;
		}
		changed.notify_all();
		{
			std::unique_lock lock{mutex};
			(void)changed.wait_for(lock, 5s, [&] { return release_access; });
			state_remained_live = state->cpu.registers[0] == 0x5678 &&
				state->engines.slot<EngineKind::gadgets>() != nullptr;
		}
	}};
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, 5s, [&] { return access_acquired; }),
			"destruction access was not acquired");
	}

	std::thread destroyer{
		[&, owned = std::move(context)]() mutable {
			{
				std::lock_guard lock{mutex};
				destruction_started = true;
			}
			changed.notify_all();
			owned.reset();
			{
				std::lock_guard lock{mutex};
				destruction_finished = true;
			}
			changed.notify_all();
		}};
	bool finished_while_accessed = false;
	bool domain_retired_while_accessed = false;
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, 5s, [&] {
			return destruction_started;
		}), "Context destruction did not start");
		finished_while_accessed = changed.wait_for(
			lock, 100ms, [&] { return destruction_finished; });
		domain_retired_while_accessed =
			uml::memory::domain_retirement_sequence() != retirement_before;
		release_access = true;
	}
	changed.notify_all();
	holder.join();
	destroyer.join();

	require(!finished_while_accessed && !domain_retired_while_accessed,
		"Context destruction crossed a live execution-state access");
	require(state_remained_live &&
		destroyed.load(std::memory_order_acquire) == 1,
		"destruction did not retain the accessed execution object");
	require(uml::memory::domain_retirement_sequence() ==
		retirement_before + 1,
		"Context destruction did not retire the domain after access release");
}

void test_steady_execution_state_access_is_allocation_free()
{
	uml::runtime::Context context;
	require(context.begin_run(), "access-cost run was not admitted");
	{
		[[maybe_unused]] auto warm = context.thread_execution_state();
	}
	constexpr std::size_t repetitions = 4096;
	uml::runtime::ThreadExecutionState *observed{};
	bool stable_identity = true;
	uml::testing::begin_allocation_count();
	for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
		auto state = context.thread_execution_state();
		if (observed == nullptr)
			observed = state.operator->();
		else
			stable_identity = stable_identity &&
				observed == state.operator->();
	}
	const auto allocations = uml::testing::end_allocation_count();
	require(allocations == 0,
		"steady execution-state access performed an allocation");
	require(observed != nullptr && stable_identity,
		"steady execution-state access changed object identity");
	context.end_run();
}

void test_stop_for_admitted_run_is_observed()
{
	constexpr std::size_t repetitions = 256;
	for (std::size_t repetition = 0; repetition < repetitions;
	     ++repetition) {
		uml::runtime::Context context;
		std::mutex mutex;
		std::condition_variable changed;
		bool entered = false;
		bool release = false;
		bool observed_stop = false;
		const int stop_fd = context.stop_notification_fd();
		require(stop_fd >= 0, "Context lacks a stop notification descriptor");
		struct pollfd ready = {.fd = stop_fd, .events = POLLIN};
		require(poll(&ready, 1, 0) == 0,
			"new Context exposed a stale stop notification");
		std::thread runner{[&] {
			uml::runtime::RunScope run{context};
			{
				std::lock_guard lock{mutex};
				entered = run.entered();
			}
			changed.notify_all();
			std::unique_lock lock{mutex};
			(void)changed.wait_for(lock, 5s, [&] { return release; });
			observed_stop = context.stop_requested();
		}};
		{
			std::unique_lock lock{mutex};
			require(changed.wait_for(lock, 5s, [&] { return entered; }),
				"run admission timed out");
		}
		context.request_stop();
		ready.revents = 0;
		require(poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN),
			"stop request did not wake a host idle wait");
		{
			std::lock_guard lock{mutex};
			release = true;
		}
		changed.notify_all();
		runner.join();
		require(observed_stop,
			"stop request for an admitted run was lost");
		require(!context.stop_requested(),
			"completed run retained its stop token");
		context.request_stop();
		require(!context.stop_requested(),
			"idle stop request leaked into a later run");
		require(context.begin_run(), "stop notification retry was not admitted");
		ready.revents = 0;
		require(poll(&ready, 1, 0) == 0,
			"next run retained a stale stop notification");
		context.end_run();
	}
}

void test_next_run_cannot_enter_during_state_retirement()
{
	uml::runtime::Context context;
	RetirementInterleave interleave;
	require(context.begin_run(), "first retirement run was not admitted");
	{
		auto first_state = context.thread_execution_state();
		first_state->cpu.registers[0] = 0xfeed;
		first_state->engines.slot<EngineKind::gadgets>() =
			std::make_unique<BlockingRetirementEngine>(interleave);
	}

	std::thread finisher{[&] { context.end_run(); }};
	{
		std::unique_lock lock{interleave.mutex};
		require(interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.destructor_entered;
		}), "run retirement boundary timed out");
	}

	std::thread contender{[&] {
		{
			std::lock_guard lock{interleave.mutex};
			interleave.contender_started = true;
		}
		interleave.changed.notify_all();
		if (!context.begin_run())
			return;
		{
			std::lock_guard lock{interleave.mutex};
			interleave.contender_admitted = true;
		}
		interleave.changed.notify_all();
		{
			auto state = context.thread_execution_state();
			{
				std::unique_lock lock{interleave.mutex};
				interleave.fresh_state =
					state->cpu.registers[0] != 0xfeed;
				interleave.contender_state_acquired = true;
				interleave.changed.notify_all();
				(void)interleave.changed.wait_for(lock, 5s, [&] {
					return interleave.release_contender;
				});
			}
		}
		context.end_run();
	}};

	bool entered_while_retiring = false;
	{
		std::unique_lock lock{interleave.mutex};
		require(interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.contender_started;
		}), "next run did not reach retirement contention");
		entered_while_retiring = interleave.changed.wait_for(
			lock, 100ms, [&] { return interleave.contender_admitted; });
		interleave.release_destructor = true;
	}
	interleave.changed.notify_all();
	finisher.join();

	bool acquired = false;
	bool fresh = false;
	{
		std::unique_lock lock{interleave.mutex};
		acquired = interleave.changed.wait_for(lock, 5s, [&] {
			return interleave.contender_state_acquired;
		});
		fresh = interleave.fresh_state;
		interleave.release_contender = true;
	}
	interleave.changed.notify_all();
	contender.join();

	require(!entered_while_retiring,
		"next run entered while prior state retirement was open");
	require(!interleave.destructor_timed_out && acquired,
		"run retirement interleave timed out");
	require(fresh,
		"next run inherited CPU or translated state from the prior run");
}

/* The gadget engine is always available; the JIT is a host capability. */
[[nodiscard]] std::size_t expected_engine_slots() noexcept
{
	return uml::runtime::execution_engine_available(
		uml::runtime::ExecutionEngine::jit) ? 2U : 1U;
}

std::size_t enter_available_engines(uml::runtime::Context &context)
{
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = 0x1000;
	unsigned long tls = 0;
	(void)darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), 1, &tls);
	if (uml::runtime::execution_engine_available(
			uml::runtime::ExecutionEngine::jit)) {
		registers.pc = 0x1000;
		(void)darwin_uml_jit_enter_guest(
			&registers, &fp, sizeof(fp), 1, &tls);
	}
	auto state = context.thread_execution_state();
	return state->engines.active_count();
}

void test_thread_churn_retires_engine_state()
{
	uml::runtime::Context context;
	uml::runtime::RunScope run{context};
	require(run.entered(), "thread-churn run was not admitted");
	uml::routing::ActiveRouteScope active{&context, nullptr};
	constexpr std::size_t thread_count = 32;
	std::atomic_size_t destroyed{};
	for (std::size_t index = 0; index < thread_count; ++index) {
		std::size_t engines = 0;
		std::thread worker{[&] {
			engines = enter_available_engines(context);
			auto state = context.thread_execution_state();
			state->engines.slot<EngineKind::gadgets>() =
				std::make_unique<DestructionCounterEngine>(destroyed);
		}};
		worker.join();
		require(engines == expected_engine_slots(),
			"thread did not enter every available engine");
		require(destroyed.load(std::memory_order_acquire) == index + 1,
			"exited host thread retained CPU, memory, or engine state");
	}
}

void test_reused_thread_starts_fresh()
{
	uml::runtime::Context context;
	constexpr std::uint64_t stale_value = 0xfeed'face'cafe'beefULL;
	{
		uml::runtime::RunScope run{context};
		require(run.entered(), "first reuse run was not admitted");
		uml::routing::ActiveRouteScope active{&context, nullptr};
		require(enter_available_engines(context) == expected_engine_slots(),
			"first reuse run did not create translated engines");
		auto state = context.thread_execution_state();
		state->cpu.registers[0] = stale_value;
	}
	{
		uml::runtime::RunScope run{context};
		require(run.entered(), "second reuse run was not admitted");
		uml::routing::ActiveRouteScope active{&context, nullptr};
		auto state = context.thread_execution_state();
		require(state->cpu.registers[0] != stale_value,
			"reused host thread inherited stale CPU state");
		require(state->engines.empty(),
			"reused host thread inherited stale translated engines");
	}
}

void test_same_thread_registration_churn_is_bounded()
{
	constexpr std::size_t cycles = 3000;
	std::size_t steady_allocations = 0;
	for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
		uml::runtime::Context context;
		uml::runtime::RunScope run{context};
		require(run.entered(), "registration-churn run was not admitted");
		uml::testing::begin_allocation_count();
		(void)context.thread_execution_state();
		const auto allocations = uml::testing::end_allocation_count();
		if (cycle == 1)
			steady_allocations = allocations;
		else if (cycle > 1)
			require(allocations == steady_allocations,
				"expired thread-state registrations grew without bound");
	}
}

void test_interrupt_registration_is_transactional()
{
	uml::runtime::Context context;
	unsigned int stale_sequence = 7;
	{
		uml::runtime::RunScope run{context};
		require(run.entered(), "interrupt baseline run was not admitted");
		uml::routing::ActiveRouteScope active{&context, nullptr};
		require(darwin_uml_static_guest_interrupt_sequence(
			&stale_sequence, stale_sequence) == 0,
			"baseline interrupt registration failed");
		auto state = context.thread_execution_state();
		require(state->interrupt.sequence == &stale_sequence &&
			state->interrupt.snapshot == stale_sequence,
			"baseline interrupt token was not published exactly");
	}

	unsigned int replacement_sequence = 19;
	uml::runtime::RunScope run{context};
	require(run.entered(), "interrupt retry run was not admitted");
	uml::routing::ActiveRouteScope active{&context, nullptr};
	uml::testing::fail_next_allocation();
	require(darwin_uml_static_guest_interrupt_sequence(
		&replacement_sequence, replacement_sequence) == -1,
		"interrupt registration allocation failure was not reported");
	require(darwin_uml_static_guest_interrupt_sequence(
		&replacement_sequence, replacement_sequence) == 0,
		"interrupt registration could not be retried");
	{
		auto state = context.thread_execution_state();
		require(state->interrupt.sequence == &replacement_sequence &&
			state->interrupt.sequence != &stale_sequence &&
			state->interrupt.snapshot == replacement_sequence,
			"interrupt retry exposed a stale or partial token");
	}
	require(darwin_uml_static_guest_interrupt_sequence(nullptr, 0) == 0,
		"interrupt token could not be cleared");
}

void test_invalid_engine_selection_fails_closed()
{
	uml::runtime::Context context;
	const auto selected = context.execution_engine();
	const auto entry = context.guest_entry();
	const auto invalid =
		static_cast<uml::runtime::ExecutionEngine>(0xff);
	require(!uml::runtime::execution_engine_available(invalid),
		"invalid execution engine was reported as available");
	require(context.set_execution_engine(invalid) == -1,
		"invalid execution engine was accepted");
	require(context.execution_engine() == selected &&
		context.guest_entry() == entry,
		"invalid execution engine changed the selected production path");
}

/* A Mac run that states the limit iOS enforces gets the budgets the device
 * gets: the gadget cache takes at most an eighth of it, and no more than the
 * cap of its vCPU count. */
void test_stated_memory_limit_sizes_the_gadget_cache_budget()
{
	constexpr std::size_t mib = 1024U * 1024U;
	const auto budget = [](std::size_t guest, std::size_t bytes,
				       unsigned executors) {
		return uml::runtime::set_gadget_cache_budget(guest * mib, bytes,
			executors);
	};
	const char *saved = std::getenv(uml::platform::memory_limit_variable);
	const std::string kept = saved != nullptr ? saved : "";
	::unsetenv(uml::platform::memory_limit_variable);
	uml::runtime::set_process_memory_limit(0);
	if (uml::platform::process_memory_limit_bytes() == 0) {
		/* Nothing stated: a quarter of the guest, at least 128 MiB, and no
		 * cap whatever the vCPU count. */
		require(budget(1536, 0, 1) == 384 * mib &&
			budget(2048, 0, 8) == 512 * mib &&
			budget(256, 0, 4) == 128 * mib,
			"the budget changed with no limit stated");
		/* The iPad mini 6's 2,867 MiB: an eighth of it, 358 MiB, is under a
		 * quarter of either guest, and above the cap of up to eight vCPUs
		 * and of up to fifteen. */
		uml::runtime::set_process_memory_limit(2867 * mib);
		constexpr std::size_t eighth = 2867 * mib / 8;
		require(eighth / mib == 358,
			"the limit's eighth is not the iPad's 358 MiB");
		require(budget(1536, 0, 16) == eighth &&
			budget(2048, 0, 16) == eighth,
			"a stated limit did not cap the budget");
		require(budget(1536, 0, 1) == 128 * mib &&
			budget(1536, 0, 4) == 224 * mib &&
			budget(2048, 0, 6) == 240 * mib &&
			budget(2048, 0, 8) == 272 * mib,
			"a stated limit did not cap the budget by the vCPU count");
		/* A budget the launch gives is its own. */
		require(budget(1536, 96 * mib, 4) == 96 * mib &&
			budget(1536, 512 * mib, 1) == 512 * mib,
			"a stated limit overrode an explicit budget");
		/* The environment states it as well, and the option beats it. */
		uml::runtime::set_process_memory_limit(0);
		::setenv(uml::platform::memory_limit_variable, "2867", 1);
		require(budget(1536, 0, 16) == eighth,
			"the environment did not state the limit");
		require(budget(1536, 0, 2) == 160 * mib,
			"the environment's limit did not cap by the vCPU count");
		uml::runtime::set_process_memory_limit(1024 * mib);
		require(budget(1536, 0, 12) == 128 * mib,
			"the option did not beat the environment");
	}
	uml::runtime::set_process_memory_limit(0);
	if (saved != nullptr)
		::setenv(uml::platform::memory_limit_variable, kept.c_str(), 1);
	else
		::unsetenv(uml::platform::memory_limit_variable);
	/* The process keeps the budget a run never chose. */
	(void)uml::runtime::set_gadget_cache_budget(0, std::size_t{1} << 30, 1);
}

} // namespace

int main()
{
	try {
		test_pre_run_memory_domain_is_retired_before_run();
		test_pre_run_execution_states_do_not_cross_run_boundary();
		test_rejected_begin_preserves_active_execution_state();
		test_context_destruction_retires_state_before_domain();
		test_execution_state_is_not_shared_between_contexts();
		test_named_engine_slots_share_execution_state_lifetime();
		test_run_admission_waits_for_pre_run_execution_state_access();
		test_run_retirement_waits_for_execution_state_access();
		test_context_destruction_waits_for_execution_state_access();
		test_steady_execution_state_access_is_allocation_free();
		test_stop_for_admitted_run_is_observed();
		test_next_run_cannot_enter_during_state_retirement();
		test_thread_churn_retires_engine_state();
		test_reused_thread_starts_fresh();
		test_same_thread_registration_churn_is_bounded();
		test_interrupt_registration_is_transactional();
		test_invalid_engine_selection_fails_closed();
		test_stated_memory_limit_sizes_the_gadget_cache_budget();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "runtime context test failed: %s\n", error.what());
		return EXIT_FAILURE;
	} catch (...) {
		std::fprintf(stderr, "runtime context test failed: unknown exception\n");
		return EXIT_FAILURE;
	}
}
