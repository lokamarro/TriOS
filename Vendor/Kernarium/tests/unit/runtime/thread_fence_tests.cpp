// SPDX-License-Identifier: GPL-2.0-or-later
/* The fence against real threads: every thread that can touch guest RAM is
 * held and nothing else is, the process's locks are never left in the hands
 * of a held thread, and no thread of the kernel can be started and missed. */
#include "kernel_threads.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "thread_fence.hpp"
#include "thread_nap.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <source_location>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::ThreadFence;
using uml::testing::nap;
using uml::testing::require;

void sleep_ms(unsigned milliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

/* Holds, or stops the test saying why the fence could not. */
void require_hold(ThreadFence &fence, unsigned budget_ms, const char *what,
	const std::source_location location = std::source_location::current())
{
	if (fence.hold(budget_ms))
		return;
	const ThreadFence::Report &report = fence.report();
	std::fprintf(stderr, "%s: failure %d after %u passes, %u deferred, "
		"%u unsettled\n", what, report.failure, report.passes,
		report.deferred, report.unsettled);
	uml::testing::fail(what, location);
}

/* A thread that does a little work forever and says how much it has done. */
struct Worker final {
	std::atomic<std::uint64_t> ticks{0};
	std::atomic<bool> stop{false};
};

void *tick_until_stopped(void *argument)
{
	auto *worker = static_cast<Worker *>(argument);
	while (!worker->stop.load(std::memory_order_acquire)) {
		worker->ticks.fetch_add(1, std::memory_order_relaxed);
		nap(100);
	}
	return nullptr;
}

void wait_for_ticks(const Worker &worker)
{
	const auto before = worker.ticks.load();
	for (int attempt = 0; attempt < 500; ++attempt) {
		if (worker.ticks.load() > before + 2)
			return;
		sleep_ms(2);
	}
	uml::testing::fail("a worker does not run");
}

/* Whether the worker stood still for a while. */
[[nodiscard]] bool stood_still(const Worker &worker)
{
	const auto first = worker.ticks.load();
	sleep_ms(40);
	return worker.ticks.load() == first;
}

[[nodiscard]] unsigned suspend_count(mach_port_t thread)
{
	thread_basic_info_data_t information{};
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	require(thread_info(thread, THREAD_BASIC_INFO,
		reinterpret_cast<thread_info_t>(&information), &count) ==
		KERN_SUCCESS, "thread_info failed");
	return static_cast<unsigned>(information.suspend_count);
}

class NamedThread final {
public:
	NamedThread(const char *name, Worker &worker)
		: thread_([name, &worker] {
			  (void)pthread_setname_np(name);
			  (void)tick_until_stopped(&worker);
		  })
	{
	}
	~NamedThread() { thread_.join(); }

private:
	std::thread thread_;
};

void a_thread_the_kernel_created_is_held_and_then_released()
{
	Worker kernel_thread;
	pthread_t thread;
	require(uml::runtime::kernel_pthread_create(&thread, nullptr,
		&tick_until_stopped, &kernel_thread) == 0, "creating the thread");
	wait_for_ticks(kernel_thread);
	ThreadFence fence;
	require(fence.hold(2000), "the fence must hold");
	require(fence.report().held == 1 && fence.report().registered == 1 &&
		fence.report().named == 0,
		"a registered thread is held by the registry alone");
	require(stood_still(kernel_thread),
		"a registered kernel thread must not run while it is held");
	require(fence.holding(), "the fence says it holds");
	fence.release();
	require(!fence.holding(), "the fence says it is idle");
	wait_for_ticks(kernel_thread);
	kernel_thread.stop.store(true);
	require(pthread_join(thread, nullptr) == 0, "join");
}

void a_named_service_thread_is_held_and_others_run()
{
	Worker service;
	Worker bystander;
	NamedThread named{"uml-test-service", service};
	NamedThread unrelated{"test-bystander", bystander};
	wait_for_ticks(service);
	wait_for_ticks(bystander);
	ThreadFence fence;
	require(fence.hold(2000), "the fence must hold");
	require(fence.report().held == 1 && fence.report().named == 1 &&
		fence.report().registered == 0,
		"a thread named uml-* is held by its name alone");
	require(stood_still(service), "a service thread must be held");
	require(!stood_still(bystander),
		"a thread that cannot touch guest RAM must keep running");
	fence.release();
	wait_for_ticks(service);
	service.stop.store(true);
	bystander.stop.store(true);
}

void a_kernel_thread_that_is_also_named_is_held_once()
{
	Worker both;
	struct Start final {
		Worker *worker;
	} start{&both};
	pthread_t thread;
	require(uml::runtime::kernel_pthread_create(&thread, nullptr,
		[](void *argument) -> void * {
			(void)pthread_setname_np("uml-vcpu-9-carrier");
			return tick_until_stopped(static_cast<Start *>(argument)->worker);
		}, &start) == 0, "creating the thread");
	wait_for_ticks(both);
	ThreadFence fence;
	require(fence.hold(2000), "the fence must hold");
	require(fence.report().held == 1 && fence.report().named == 1 &&
		fence.report().registered == 1, "one thread, two reasons");
	require(suspend_count(pthread_mach_thread_np(thread)) == 1,
		"held exactly once");
	fence.release();
	require(suspend_count(pthread_mach_thread_np(thread)) == 0,
		"released exactly once");
	both.stop.store(true);
	require(pthread_join(thread, nullptr) == 0, "join");
}

/* The threads the kernel starts for itself name themselves first thing
 * (os_set_thread_name: a carrier per vCPU, uml-irq-notify, uml-rtc-alarm).
 * Each is then both in the registry and named uml-*, and the fence
 * must hold it once, whichever rule finds it first; a kernel thread that
 * never names itself is held by the registry alone. */
void each_named_kernel_thread_is_held_exactly_once()
{
	struct Start final {
		const char *name;
		Worker *worker;
	};
	constexpr const char *names[] = {"uml-vcpu-0-carrier", "uml-irq-notify",
		"uml-rtc-alarm", nullptr};
	constexpr std::size_t count = sizeof names / sizeof names[0];
	constexpr std::size_t named_count = count - 1;
	Worker workers[count];
	Start starts[count];
	pthread_t threads[count];
	for (std::size_t index = 0; index < count; ++index) {
		starts[index] = {names[index], &workers[index]};
		require(uml::runtime::kernel_pthread_create(&threads[index], nullptr,
			[](void *argument) -> void * {
				const auto *start = static_cast<Start *>(argument);
				if (start->name != nullptr)
					(void)pthread_setname_np(start->name);
				return tick_until_stopped(start->worker);
			}, &starts[index]) == 0, "creating a kernel thread");
	}
	for (const Worker &worker : workers)
		wait_for_ticks(worker);
	ThreadFence fence;
	require_hold(fence, 2000, "holding the kernel's threads");
	require(fence.report().held == count &&
		fence.report().registered == count &&
		fence.report().named == named_count,
		"every kernel thread is held once: all registered, those named "
		"uml-* also by name");
	for (std::size_t index = 0; index < count; ++index) {
		require(suspend_count(pthread_mach_thread_np(threads[index])) == 1,
			"a kernel thread is held exactly once");
		require(stood_still(workers[index]),
			"a kernel thread must not run while it is held");
	}
	fence.release();
	for (std::size_t index = 0; index < count; ++index)
		require(suspend_count(pthread_mach_thread_np(threads[index])) == 0,
			"a kernel thread is released exactly once");
	for (Worker &worker : workers) {
		wait_for_ticks(worker);
		worker.stop.store(true);
	}
	for (pthread_t thread : threads)
		require(pthread_join(thread, nullptr) == 0, "join");
}

/* Runs `action` on a thread of its own, one the fence does not hold, and says
 * whether it finished in time. The thread is made before anything is held. */
class Probe final {
public:
	Probe() : thread_([this] { loop(); }) {}
	~Probe()
	{
		quit_.store(true, std::memory_order_release);
		thread_.join();
	}

	/* True when the action ran to the end within `milliseconds`. */
	[[nodiscard]] bool run(void (*action)(), unsigned milliseconds)
	{
		action_ = action;
		done_.store(false, std::memory_order_relaxed);
		go_.store(true, std::memory_order_release);
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds{milliseconds};
		while (!done_.load(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() > deadline)
				return false;
			nap(200);
		}
		return true;
	}

private:
	void loop()
	{
		while (!quit_.load(std::memory_order_acquire)) {
			if (go_.exchange(false, std::memory_order_acq_rel)) {
				action_();
				done_.store(true, std::memory_order_release);
			}
			nap(200);
		}
	}

	std::atomic<bool> go_{false};
	std::atomic<bool> done_{false};
	std::atomic<bool> quit_{false};
	void (*action_)(){};
	std::thread thread_;
};

std::FILE *probe_sink;

void allocate_and_print()
{
	void *block = std::malloc(2000);
	std::memset(block, 2, 2000);
	std::fprintf(probe_sink, "held\n");
	std::free(block);
}

/* Threads that live in the allocator and in a stream. A thread stopped there
 * would keep the lock, so the rest of the process could neither allocate nor
 * print until it was let go. */
void no_thread_is_held_inside_the_allocator_or_a_stream()
{
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> work{0};
	std::atomic<int> named{0};
	probe_sink = std::fopen("/dev/null", "w");
	require(probe_sink != nullptr, "cannot open /dev/null");
	std::vector<std::thread> threads;
	for (int index = 0; index < 8; ++index)
		threads.emplace_back([&, index] {
			const std::string name = "uml-test-heap-" + std::to_string(index);
			(void)pthread_setname_np(name.c_str());
			named.fetch_add(1);
			std::uint64_t state = static_cast<std::uint64_t>(index) + 1;
			while (!stop.load(std::memory_order_acquire)) {
				state = state * 6364136223846793005ULL + 1442695040888963407ULL;
				const std::size_t size = 16 + (state >> 40) % 4096;
				void *block = std::malloc(size);
				std::memset(block, 1, size);
				std::fprintf(probe_sink, "%zu\n", size);
				std::free(block);
				work.fetch_add(1, std::memory_order_relaxed);
			}
		});
	while (named.load() != 8)
		nap(500);
	Probe probe;
	ThreadFence fence;
	unsigned deferred = 0;
	for (int round = 0; round < 20; ++round) {
		require(fence.hold(10000), "the fence must find a moment to hold");
		deferred += fence.report().deferred;
		/* A held thread inside the allocator or the stream keeps its lock, and
		 * the probe, which is not held, waits on it. */
		const bool probed = probe.run(&allocate_and_print, 3000);
		const auto first = work.load();
		nap(1000);
		const bool still = work.load() == first;
		/* Released before a failure aborts: abort allocates too. */
		fence.release();
		require(probed, "a held thread kept the allocator's or a stream's lock");
		require(still, "the held threads must not run");
	}
	require(deferred != 0,
		"the threads were never found inside the allocator, so the rule "
		"that avoids it was not exercised");
	stop.store(true);
	for (auto &thread : threads)
		thread.join();
	std::fclose(probe_sink);
}

/* A thread blocked in write(2) on a full pipe, inside fprintf, holds the
 * stream's lock for as long as the pipe stays full. Its program counter is in
 * the kernel's system call stub; only its frames say that it is inside the C
 * library. */
void a_thread_blocked_inside_a_stream_write_is_not_held()
{
	int pipes[2];
	require(pipe(pipes) == 0, "pipe");
	/* Fill the pipe, so that the next write blocks. */
	require(fcntl(pipes[1], F_SETFL, O_NONBLOCK) == 0, "nonblocking");
	char block[4096] = {};
	while (write(pipes[1], block, sizeof block) > 0) {
	}
	require(fcntl(pipes[1], F_SETFL, 0) == 0, "blocking");
	std::FILE *stream = fdopen(pipes[1], "w");
	require(stream != nullptr, "fdopen");
	std::atomic<bool> started{false};
	std::atomic<bool> finished{false};
	std::thread writer{[&] {
		(void)pthread_setname_np("uml-test-writer");
		started.store(true);
		std::fprintf(stream, "x\n");
		std::fflush(stream);
		finished.store(true);
	}};
	while (!started.load())
		nap(500);
	nap(50000);
	require(!finished.load(), "the write must be blocked");
	ThreadFence fence;
	require(!fence.hold(300), "a thread inside a stream write must not be held");
	require(fence.report().failure == ThreadFence::out_of_time &&
		fence.report().deferred != 0,
		"the fence must have refused the thread, not failed otherwise");
	require(!fence.holding(), "nothing is held");
	/* Once the pipe drains, the write finishes and the thread can be held. */
	char sink[4096];
	/* The writer stops after one line, and it may not have said it is finished
	 * when the last of it has been read, so a blocking read could wait for
	 * ever on a pipe nobody writes to again. */
	require(fcntl(pipes[0], F_SETFL, O_NONBLOCK) == 0, "nonblocking drain");
	while (!finished.load()) {
		if (read(pipes[0], sink, sizeof sink) <= 0)
			nap(200);
	}
	writer.join();
	require(fence.hold(2000), "a thread past its write is held");
	fence.release();
	std::fclose(stream);
	close(pipes[0]);
}

std::atomic<bool> comparator_blocked{true};

int block_in_comparator(const void *, const void *)
{
	while (comparator_blocked.load(std::memory_order_acquire))
		nap(500);
	return 0;
}

/* A thread several frames deep in the C library, on a callback of the test's
 * own, has neither its program counter nor its caller there. The frame chain
 * is what shows it. The sort is heapsort because the sanitizers intercept
 * qsort and call the comparator from their own runtime first, which would put
 * the thread outside the C library. */
void a_thread_called_from_the_c_library_is_not_held()
{
	comparator_blocked.store(true);
	std::atomic<bool> started{false};
	std::thread sorter{[&] {
		(void)pthread_setname_np("uml-test-sorter");
		int values[2] = {2, 1};
		started.store(true);
		(void)heapsort(values, 2, sizeof values[0], &block_in_comparator);
	}};
	while (!started.load())
		nap(500);
	nap(50000);
	ThreadFence fence;
	require(!fence.hold(300), "a thread inside the C library must not be held");
	require(fence.report().failure == ThreadFence::out_of_time &&
		fence.report().deferred != 0, "the fence must have refused the thread");
	comparator_blocked.store(false);
	sorter.join();
	require(fence.hold(2000), "a thread past the library is held");
	fence.release();
}

struct Spawning final {
	std::atomic<int> alive{0};
	std::atomic<std::uint64_t> started{0};
	std::atomic<bool> stop{false};
};

/* Threads that make short-lived threads through the kernel's pthread_create.
 * While the fence holds, no child may be running that it did not hold. */
void a_thread_being_created_is_never_missed()
{
	Spawning spawning;
	std::atomic<int> named{0};
	std::vector<std::thread> spawners;
	for (int index = 0; index < 4; ++index)
		spawners.emplace_back([&spawning, &named, index] {
			const std::string name = "uml-test-spawner-" + std::to_string(index);
			(void)pthread_setname_np(name.c_str());
			named.fetch_add(1);
			while (!spawning.stop.load(std::memory_order_acquire)) {
				pthread_t child;
				if (uml::runtime::kernel_pthread_create(&child, nullptr,
					    [](void *argument) -> void * {
						    auto *state = static_cast<Spawning *>(argument);
						    state->alive.fetch_add(1);
						    state->started.fetch_add(1);
						    nap(30);
						    state->alive.fetch_sub(1);
						    return nullptr;
					    }, &spawning) == 0)
					(void)pthread_join(child, nullptr);
			}
		});
	while (named.load() != 4)
		nap(500);
	ThreadFence fence;
	unsigned stalls = 0;
	for (int round = 0; round < 150; ++round) {
		require_hold(fence, 10000, "the fence must hold");
		stalls += fence.report().creator_stalls;
		uml::runtime::KernelThreadSnapshot snapshot;
		uml::runtime::snapshot_kernel_threads(snapshot);
		require(snapshot.creator_count == 0,
			"no creation may be under way while the fence holds");
		const int alive = spawning.alive.load();
		const auto started = spawning.started.load();
		sleep_ms(1);
		require(spawning.alive.load() == alive &&
			spawning.started.load() == started,
			"a thread the fence did not hold ran meanwhile");
		for (std::size_t index = 0; index < snapshot.thread_count; ++index) {
			thread_basic_info_data_t information{};
			mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
			if (thread_info(snapshot.threads[index], THREAD_BASIC_INFO,
				    reinterpret_cast<thread_info_t>(&information),
				    &count) == KERN_SUCCESS)
				require(information.suspend_count != 0 ||
					information.run_state == TH_STATE_HALTED,
					"a listed, living thread is not held");
		}
		fence.release();
		nap(200);
	}
	spawning.stop.store(true);
	for (auto &spawner : spawners)
		spawner.join();
	/* A creator waiting on a lock that a held thread owns is let go and tried
	 * again; how often that happens depends on the load, so it is reported
	 * and not required. */
	std::printf("    creators let go to finish: %u\n", stalls);
}

struct Settling final {
	Worker *worker;
	std::atomic<int> calls{0};
	int refuse;
	bool ran_while_judged{false};
};

void the_settled_condition_is_judged_with_every_thread_held()
{
	Worker service;
	NamedThread named{"uml-test-settle", service};
	wait_for_ticks(service);
	Settling settling;
	settling.worker = &service;
	settling.refuse = 3;
	ThreadFence fence;
	const bool held = fence.hold(5000, [](void *context) noexcept {
		auto *settling = static_cast<Settling *>(context);
		const int call = settling->calls.fetch_add(1);
		/* Every thread is held at every call, the refused ones included. */
		const auto before = settling->worker->ticks.load();
		nap(20000);
		if (settling->worker->ticks.load() != before)
			settling->ran_while_judged = true;
		return call >= settling->refuse;
	}, &settling);
	require(held, "the fence holds once the condition is met");
	require(!settling.ran_while_judged,
		"the condition must be judged with the threads held");
	require(settling.calls.load() == 4 && fence.report().unsettled == 3,
		"a refused moment is repeated");
	require(stood_still(service), "and the threads are held at the end");
	fence.release();

	/* A condition that never holds ends within the budget, with nothing
	 * held. */
	const auto started = std::chrono::steady_clock::now();
	require(!fence.hold(60, [](void *) noexcept { return false; }),
		"an unmet condition must fail");
	const auto elapsed = std::chrono::steady_clock::now() - started;
	require(fence.report().failure == ThreadFence::out_of_time,
		"the failure must say it ran out of time");
	require(elapsed < std::chrono::seconds{5}, "the budget bounds the wait");
	require(!fence.holding(), "a failed hold holds nothing");
	wait_for_ticks(service);
	service.stop.store(true);
}

void an_overflowed_registry_fails_the_fence_closed()
{
	/* More long-lived kernel threads than the registry has slots. */
	std::atomic<bool> release{false};
	std::vector<pthread_t> threads;
	for (std::size_t index = 0;
	     index < uml::runtime::kernel_thread_capacity + 8; ++index) {
		pthread_t thread;
		require(uml::runtime::kernel_pthread_create(&thread, nullptr,
			[](void *argument) -> void * {
				auto *flag = static_cast<std::atomic<bool> *>(argument);
				while (!flag->load(std::memory_order_acquire))
					nap(1000);
				return nullptr;
			}, &release) == 0, "creating a parked thread");
		threads.push_back(thread);
	}
	ThreadFence fence;
	require(!fence.hold(500), "an incomplete list must not be trusted");
	require(fence.report().failure == ThreadFence::registry_overflowed,
		"the failure must name the registry");
	require(!fence.holding(), "nothing is held");
	release.store(true, std::memory_order_release);
	for (const pthread_t thread : threads)
		require(pthread_join(thread, nullptr) == 0, "join");
}

} // namespace

int main(int argc, char **argv)
{
	/* A test that hangs is a failed test, not a stuck suite. */
	const uml::testing::Watchdog watchdog{"the thread fence tests",
		std::chrono::seconds{180}};
	/* The overflow test leaves the registry unusable for the rest of the
	 * process, so it runs alone. */
	const bool overflow = argc > 1 && std::strcmp(argv[1], "overflow") == 0;
	if (overflow) {
		an_overflowed_registry_fails_the_fence_closed();
		std::puts("ok: an overflowed registry fails the fence closed");
		return 0;
	}
	const uml::testing::NamedTest tests[] = {
		{"a thread the kernel created is held and then released",
			a_thread_the_kernel_created_is_held_and_then_released},
		{"a named service thread is held and others run",
			a_named_service_thread_is_held_and_others_run},
		{"a kernel thread that is also named is held once",
			a_kernel_thread_that_is_also_named_is_held_once},
		{"each named kernel thread is held exactly once",
			each_named_kernel_thread_is_held_exactly_once},
		{"no thread is held inside the allocator or a stream",
			no_thread_is_held_inside_the_allocator_or_a_stream},
		{"a thread blocked inside a stream write is not held",
			a_thread_blocked_inside_a_stream_write_is_not_held},
		{"a thread called from the C library is not held",
			a_thread_called_from_the_c_library_is_not_held},
		{"a thread being created is never missed",
			a_thread_being_created_is_never_missed},
		{"the settled condition is judged with every thread held",
			the_settled_condition_is_judged_with_every_thread_held},
	};
	return uml::testing::runNamedTests(tests);
}
