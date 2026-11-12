// SPDX-License-Identifier: GPL-2.0-or-later
/* The native window's bounds are read as a sequence-stamped copy, without a
 * lock. Both loads must stay ahead of the closing revision check, or a reader
 * takes the start of one window and the end of another and still accepts the
 * snapshot. Readers here classify an address that lies outside both windows a
 * writer alternates between, and must never find it inside one. Without the
 * reader's fence they do so tens of thousands of times a second.
 */
#include "address_layout.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main()
{
	using namespace std::chrono_literals;
	using uml::memory::AddressDomain;
	using uml::memory::ConfiguredAddressLayout;
	constexpr std::uint64_t gib = 1ULL << 30U;
	constexpr std::uint64_t base = DARWIN_UML_NATIVE_ADDRESS_START;
	static_assert(base + 6U * gib <= DARWIN_UML_GUEST_ADDRESS_LIMIT);
	/* Inside only the mixed window [first start, second end). */
	constexpr std::uint64_t between = base + 3U * gib;
	std::atomic<bool> stop{};
	std::atomic<bool> refused{};
	std::atomic<unsigned> started{};
	std::atomic<unsigned long> reads{};
	std::atomic<unsigned long> torn{};
	std::atomic<unsigned long> writes{};
	/* The default window covers `between`, so the readers start after the
	 * first of the two is in place. */
	if (!uml::memory::configure_native_address_window(base, base + 2U * gib)) {
		std::fprintf(stderr, "a native window was refused\n");
		return EXIT_FAILURE;
	}
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
				++local_reads;
				if (ConfiguredAddressLayout::classify(between, 1) !=
				    AddressDomain::guest)
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
			if (!uml::memory::configure_native_address_window(
				    base, base + 2U * gib) ||
			    !uml::memory::configure_native_address_window(
				    base + 4U * gib, base + 6U * gib))
				refused.store(true, std::memory_order_relaxed);
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
	uml::memory::reset_native_address_window();
	if (!going) {
		std::fprintf(stderr, "the window threads never started\n");
		return EXIT_FAILURE;
	}
	if (refused.load()) {
		std::fprintf(stderr, "a native window was refused\n");
		return EXIT_FAILURE;
	}
	if (torn.load() != 0) {
		std::fprintf(stderr, "%lu of %lu window reads were torn\n",
			torn.load(), reads.load());
		return EXIT_FAILURE;
	}
	for (const auto count : passes) {
		if (count == 0) {
			std::fprintf(stderr, "a window reader made no pass\n");
			return EXIT_FAILURE;
		}
	}
	if (writes.load() == 0) {
		std::fprintf(stderr, "the window writer made no pass\n");
		return EXIT_FAILURE;
	}
	std::printf("PASS: %lu window reads, %lu rewrites, none torn\n",
		reads.load(), writes.load());
	return EXIT_SUCCESS;
}
