// SPDX-License-Identifier: GPL-2.0-or-later
/* An unbound thread reads the published route as a sequence-stamped copy,
 * without a lock. Its payload loads must stay ahead of the closing sequence
 * check, or a reader pairs one publication's runtime with another's network
 * and still accepts the snapshot. Readers here race a writer that alternates
 * two runs for a bounded time, against the production object, and must never
 * see a mixed route. Without the reader's fence they see thousands a second.
 */
#include "run_routing.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main()
{
	using namespace std::chrono_literals;
	static int runtime_a;
	static int network_a;
	static int runtime_b;
	static int network_b;
	std::atomic<bool> stop{};
	std::atomic<unsigned> started{};
	std::atomic<unsigned long> reads{};
	std::atomic<unsigned long> torn{};
	std::atomic<unsigned long> writes{};
	constexpr unsigned reader_count = 4;
	std::vector<unsigned long> passes(reader_count);
	std::vector<std::thread> readers;
	for (unsigned index = 0; index < reader_count; ++index)
		readers.emplace_back([&, index] {
			unsigned long local_reads = 0;
			unsigned long local_torn = 0;
			started.fetch_add(1, std::memory_order_release);
			/* The first pass is made whatever `stop` says. */
			do {
				const auto route = uml::routing::current_route();
				++local_reads;
				const bool coherent =
					(route.runtime == &runtime_a &&
					 route.network == &network_a) ||
					(route.runtime == &runtime_b &&
					 route.network == &network_b) ||
					(route.runtime == nullptr &&
					 route.network == nullptr);
				if (!coherent)
					++local_torn;
			} while (!stop.load(std::memory_order_relaxed));
			passes[index] = local_reads;
			reads.fetch_add(local_reads, std::memory_order_relaxed);
			torn.fetch_add(local_torn, std::memory_order_relaxed);
		});
	std::thread writer{[&] {
		unsigned long local_writes = 0;
		started.fetch_add(1, std::memory_order_release);
		do {
			{
				uml::routing::ActiveRouteScope scope{
					&runtime_a, &network_a};
			}
			{
				uml::routing::ActiveRouteScope scope{
					&runtime_b, &network_b};
			}
			++local_writes;
		} while (!stop.load(std::memory_order_relaxed));
		writes.store(local_writes, std::memory_order_relaxed);
	}};
	/* The run is timed once every thread is going: a loaded host can
	 * schedule them only after it is over, and the test would then pass
	 * having read nothing. */
	const bool going = uml::testing::waitUntil([&] {
			return started.load(std::memory_order_acquire) ==
				reader_count + 1U;
		}, 10s);
	if (going)
		std::this_thread::sleep_for(400ms);
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	for (auto &reader : readers)
		reader.join();
	if (!going) {
		std::fprintf(stderr, "the route threads never started\n");
		return EXIT_FAILURE;
	}
	if (torn.load() != 0) {
		std::fprintf(stderr, "%lu of %lu unbound route reads were torn\n",
			torn.load(), reads.load());
		return EXIT_FAILURE;
	}
	for (const auto count : passes) {
		if (count == 0) {
			std::fprintf(stderr, "a route reader made no pass\n");
			return EXIT_FAILURE;
		}
	}
	if (writes.load() == 0) {
		std::fprintf(stderr, "the route writer made no pass\n");
		return EXIT_FAILURE;
	}
	std::printf("PASS: %lu unbound route reads, %lu republications, none "
		"torn\n", reads.load(), writes.load());
	return EXIT_SUCCESS;
}
