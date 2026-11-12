/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The handover of an x86-64 address space from the gadget engine to the JIT:
 * it opens once the address space has retired its threshold, no dispatch on
 * the gadgets overlaps one on the JIT, and it never closes. */
#include "require.hpp"
#include "runtime/x86_tier.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using uml::runtime::X86TierState;
using uml::runtime::X86TierTable;
using Engine = X86TierState::Engine;
using uml::testing::require;
using uml::testing::waitUntil;

/* An address space starts on the gadgets, counts what each dispatch retires,
 * and hands over when the sum reaches the threshold and the dispatch that
 * got it there has left. */
void test_the_threshold_opens_the_jit()
{
	X86TierState state{1000};
	{
		auto entry = state.enter();
		require(entry.engine() == Engine::gadgets, "first dispatch not on gadgets");
		entry.leave(400);
	}
	require(!state.on_jit() && state.retired() == 400, "opened below the threshold");
	{
		auto entry = state.enter();
		require(entry.engine() == Engine::gadgets, "second dispatch not on gadgets");
		entry.leave(599);
	}
	require(!state.on_jit(), "opened one short of the threshold");
	{
		auto entry = state.enter();
		require(entry.engine() == Engine::gadgets, "third dispatch not on gadgets");
		entry.leave(1);
	}
	require(state.on_jit(), "did not open at the threshold");
	for (int round = 0; round < 3; ++round) {
		auto entry = state.enter();
		require(entry.engine() == Engine::jit && !entry.abandoned(),
			"dispatch after the handover not on the JIT");
	}
}

/* The dispatch that crosses the threshold waits for the others that are
 * running; the JIT opens when the last of them leaves, and a dispatch that
 * starts meanwhile is held until then. */
void test_the_handover_drains_running_dispatches()
{
	X86TierState state{10};
	auto first = state.enter();
	auto second = state.enter();
	require(first.engine() == Engine::gadgets &&
		second.engine() == Engine::gadgets, "dispatches not on gadgets");
	first.leave(50);
	require(!state.on_jit(), "opened while a gadget dispatch was running");

	std::atomic_bool started{};
	std::atomic<Engine> third_engine{Engine::gadgets};
	std::thread third{[&] {
		started.store(true);
		auto entry = state.enter();
		third_engine.store(entry.engine());
	}};
	require(waitUntil([&] { return started.load(); },
		std::chrono::seconds{60}, std::chrono::milliseconds{0}),
		"the third dispatch never started");
	std::this_thread::sleep_for(std::chrono::milliseconds{50});
	require(third_engine.load() == Engine::gadgets && !state.on_jit(),
		"a dispatch started during the handover");
	second.leave(0);
	third.join();
	require(state.on_jit() && third_engine.load() == Engine::jit,
		"the last dispatch leaving did not open the JIT");
}

/* A run being stopped does not wait out a handover. */
void test_a_stopping_run_does_not_wait()
{
	X86TierState state{1};
	auto running = state.enter();
	auto crossing = state.enter();
	crossing.leave(5);
	std::atomic_bool stop{};
	std::thread stopper{[&] {
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		stop.store(true);
	}};
	auto waiting = state.enter([&]() noexcept { return stop.load(); });
	require(waiting.abandoned(), "wait not abandoned");
	stopper.join();
	running.leave(0);
	require(state.on_jit(), "handover did not complete");
}

/* The invariant the handover exists for, under load: from the moment any
 * thread is given the JIT, none is on the gadgets, and none is given the
 * gadgets again. */
void test_no_dispatch_overlaps_across_the_handover()
{
	constexpr unsigned threads = 8;
	X86TierState state{20'000};
	std::atomic<int> on_gadgets{};
	std::atomic_bool jit_seen{};
	std::atomic_bool broken{};
	std::vector<std::thread> pool;
	for (unsigned index = 0; index < threads; ++index)
		pool.emplace_back([&] {
			for (int round = 0; round < 2000; ++round) {
				auto entry = state.enter();
				if (entry.engine() == Engine::gadgets) {
					if (jit_seen.load())
						broken.store(true);
					on_gadgets.fetch_add(1);
					std::this_thread::yield();
					on_gadgets.fetch_sub(1);
					entry.leave(100);
				} else {
					jit_seen.store(true);
					if (on_gadgets.load() != 0)
						broken.store(true);
				}
			}
		});
	for (auto &thread : pool)
		thread.join();
	require(!broken.load(), "a gadget dispatch overlapped the JIT");
	require(jit_seen.load() && state.on_jit(), "the handover never happened");
}

bool is_live(unsigned long generation) noexcept
{
	return generation % 2 == 0;
}

/* A table gives one state to each address space, drops the states of dead
 * ones once it is large, and keeps a state something still holds. */
void test_the_table_prunes_dead_address_spaces()
{
	X86TierTable table{500, &is_live};
	require(table.threshold() == 500, "threshold lost");
	const auto held = table.acquire(1);
	require(held != nullptr && table.acquire(1) == held, "state not shared");
	require(table.acquire(2) != held, "address spaces share a state");
	for (unsigned long generation = 3; generation < 5000; ++generation)
		require(table.acquire(generation) != nullptr, "no state");
	require(table.size() < 5000, "dead address spaces never dropped");
	require(table.acquire(1) != nullptr, "a held state vanished");
	held->enter().leave(600);
	require(held->on_jit(), "a held state does not work");
}

} // namespace

int main()
{
	test_the_threshold_opens_the_jit();
	test_the_handover_drains_running_dispatches();
	test_a_stopping_run_does_not_wait();
	test_no_dispatch_overlaps_across_the_handover();
	test_the_table_prunes_dead_address_spaces();
	std::puts("x86 tier tests passed");
	return 0;
}
