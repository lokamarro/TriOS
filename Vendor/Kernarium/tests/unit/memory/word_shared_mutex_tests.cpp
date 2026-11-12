// SPDX-License-Identifier: GPL-2.0-or-later
#include "wait_until.hpp"
#include "word_shared_mutex.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace {

using uml::memory::WordSharedMutex;

[[nodiscard]] bool expect(bool condition, const char *message) noexcept
{
	if (!condition)
		std::fprintf(stderr, "word shared mutex: %s\n", message);
	return condition;
}

template<std::size_t Count>
[[nodiscard]] bool every_reader_passed(
	const std::array<std::atomic<unsigned long>, Count> &passes)
{
	for (const auto &count : passes)
		if (count.load(std::memory_order_acquire) == 0)
			return false;
	return true;
}

/* Waits until every reader has made a pass, so that what follows overlaps
 * them: a loaded host can schedule a reader only after the test is over, and
 * the test would then pass having read nothing. */
template<std::size_t Count>
[[nodiscard]] bool readers_are_going(
	const std::array<std::atomic<unsigned long>, Count> &passes)
{
	return uml::testing::waitUntil(
		[&] { return every_reader_passed(passes); },
		std::chrono::seconds{10});
}

/* Writers exclude each other and every reader: a writer tears the two halves
 * of the pair apart for the length of its hold, and nobody may see that. */
[[nodiscard]] bool writers_exclude_readers_and_each_other()
{
	constexpr unsigned writers = 3;
	constexpr unsigned readers = 5;
	constexpr unsigned rounds = 20000;
	WordSharedMutex mutex;
	std::uint64_t first = 0;
	std::uint64_t second = 0;
	std::atomic<bool> torn{false};
	std::atomic<bool> done{false};
	std::array<std::atomic<unsigned long>, readers> passes{};
	const auto read_pair = [&] {
		std::shared_lock lock{mutex};
		if (first != second)
			torn.store(true, std::memory_order_relaxed);
	};
	std::vector<std::thread> threads;
	for (unsigned index = 0; index < readers; ++index) {
		threads.emplace_back([&, index] {
			/* The first pass is made whatever `done` says. */
			do {
				read_pair();
				++passes[index];
			} while (!done.load(std::memory_order_relaxed));
		});
	}
	const bool going = readers_are_going(passes);
	const unsigned writer_count = going ? writers : 0;
	std::vector<std::thread> writing;
	for (unsigned index = 0; index < writer_count; ++index) {
		writing.emplace_back([&] {
			for (unsigned round = 0; round < rounds; ++round) {
				std::lock_guard lock{mutex};
				++first;
				if (first == second)
					torn.store(true, std::memory_order_relaxed);
				++second;
			}
		});
	}
	for (auto &thread : writing)
		thread.join();
	done.store(true, std::memory_order_relaxed);
	for (auto &thread : threads)
		thread.join();
	return expect(going, "the readers never started") &&
		expect(every_reader_passed(passes), "a reader made no pass") &&
		expect(!torn.load(), "a holder saw another writer's update") &&
		expect(first == writers * rounds && second == first,
			"writers lost updates");
}

/* Readers share: a second reader gets in while the first still holds. */
[[nodiscard]] bool readers_hold_the_lock_together()
{
	WordSharedMutex mutex;
	std::shared_lock first{mutex};
	std::atomic<bool> entered{false};
	std::thread second{[&] {
		std::shared_lock lock{mutex};
		entered.store(true);
	}};
	second.join();
	const bool writer_kept_out = !mutex.try_lock();
	first.unlock();
	const bool writer_admitted = mutex.try_lock();
	if (writer_admitted)
		mutex.unlock();
	return expect(entered.load(), "a second reader waited for the first") &&
		expect(writer_kept_out, "a writer entered beside a reader") &&
		expect(writer_admitted, "a writer was refused a free lock");
}

/* A steady stream of overlapping readers does not starve a writer: a waiting
 * writer holds new readers off. */
[[nodiscard]] bool a_waiting_writer_is_not_starved()
{
	constexpr unsigned readers = 4;
	WordSharedMutex mutex;
	std::atomic<bool> done{false};
	std::array<std::atomic<unsigned long>, readers> passes{};
	const auto hold_shared = [&] {
		std::shared_lock lock{mutex};
		for (volatile unsigned spin = 0; spin < 200; spin = spin + 1) {
		}
	};
	std::vector<std::thread> threads;
	for (unsigned index = 0; index < readers; ++index) {
		threads.emplace_back([&, index] {
			/* The first pass is made whatever `done` says. */
			do {
				hold_shared();
				++passes[index];
			} while (!done.load(std::memory_order_relaxed));
		});
	}
	const bool going = readers_are_going(passes);
	const auto started = std::chrono::steady_clock::now();
	unsigned acquired = 0;
	while (going && acquired < 1000 &&
	       std::chrono::steady_clock::now() - started <
		       std::chrono::seconds{10}) {
		std::lock_guard lock{mutex};
		++acquired;
	}
	done.store(true, std::memory_order_relaxed);
	for (auto &thread : threads)
		thread.join();
	return expect(going, "the readers never started") &&
		expect(every_reader_passed(passes), "a reader made no pass") &&
		expect(acquired == 1000,
			"a writer starved behind overlapping readers");
}

} // namespace

int main()
{
	const bool passed = writers_exclude_readers_and_each_other() &&
		readers_hold_the_lock_together() &&
		a_waiting_writer_is_not_starved();
	return passed ? 0 : 1;
}
