// SPDX-License-Identifier: GPL-2.0-or-later
/* The heaps a unit of the engine maps for itself: what they allocate, that
 * they give every byte back to the process, and that the region they share
 * never opens a hole for the next mmap(NULL). */
#include "domain_heap.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace {

using uml::memory::DomainAllocator;
using uml::memory::DomainHeap;
using uml::memory::HeapRegion;
using uml::memory::ScratchAllocator;
using uml::memory::ScratchArena;
using uml::memory::ScratchScope;
using uml::testing::require;
using uml::testing::requireThrows;

[[nodiscard]] std::size_t committed() noexcept
{
	return HeapRegion::instance().committed();
}

[[nodiscard]] std::size_t host_page() noexcept
{
	return static_cast<std::size_t>(getpagesize());
}

/* A block's contents name the block, so that an overlap or a stale reuse is a
 * visible difference rather than a silent one. */
void stamp(void *block, std::size_t bytes) noexcept
{
	auto *bytes_ptr = static_cast<unsigned char *>(block);
	const auto seed = static_cast<unsigned char>(
		(reinterpret_cast<std::uintptr_t>(block) >> 4) ^ bytes);
	for (std::size_t index = 0; index < bytes; ++index)
		bytes_ptr[index] = static_cast<unsigned char>(seed + index);
}

[[nodiscard]] bool stamped(const void *block, std::size_t bytes) noexcept
{
	const auto *bytes_ptr = static_cast<const unsigned char *>(block);
	const auto seed = static_cast<unsigned char>(
		(reinterpret_cast<std::uintptr_t>(block) >> 4) ^ bytes);
	for (std::size_t index = 0; index < bytes; ++index)
		if (bytes_ptr[index] != static_cast<unsigned char>(seed + index))
			return false;
	return true;
}

struct Block final {
	void *pointer;
	std::size_t bytes;
	std::size_t alignment;
};

/* A reusable barrier from a mutex and a condition variable, which the
 * thread sanitizer models. */
class Gate final {
public:
	explicit Gate(unsigned parties) noexcept : parties_{parties} {}

	void arrive_and_wait()
	{
		std::unique_lock lock{mutex_};
		const auto generation = generation_;
		if (++arrived_ == parties_) {
			arrived_ = 0;
			++generation_;
			changed_.notify_all();
			return;
		}
		changed_.wait(lock, [&] { return generation_ != generation; });
	}

private:
	const unsigned parties_;
	unsigned arrived_{};
	unsigned generation_{};
	std::mutex mutex_;
	std::condition_variable changed_;
};

void every_size_class_hands_out_aligned_distinct_blocks()
{
	DomainHeap heap;
	std::vector<Block> blocks;
	/* Every size to the end of the medium classes, and a few beyond. */
	for (std::size_t bytes = 1; bytes <= 8192 + 40; bytes += 1 + bytes / 64) {
		for (const std::size_t alignment : {std::size_t{1}, std::size_t{8},
			     std::size_t{16}}) {
			void *pointer = heap.allocate(bytes, alignment);
			require(reinterpret_cast<std::uintptr_t>(pointer) % 16 == 0);
			stamp(pointer, bytes);
			blocks.push_back({pointer, bytes, alignment});
		}
	}
	for (const auto &block : blocks)
		require(stamped(block.pointer, block.bytes),
			"a block's contents changed while others were allocated");
	std::vector<std::uintptr_t> starts;
	for (const auto &block : blocks)
		starts.push_back(reinterpret_cast<std::uintptr_t>(block.pointer));
	std::sort(starts.begin(), starts.end());
	require(std::adjacent_find(starts.begin(), starts.end()) == starts.end(),
		"two live blocks share an address");
	for (const auto &block : blocks)
		heap.deallocate(block.pointer, block.bytes, block.alignment);
	require(heap.mapped_bytes() != 0);
}

void a_freed_block_is_the_next_of_its_class()
{
	DomainHeap heap;
	void *first = heap.allocate(100, 16);
	void *second = heap.allocate(100, 16);
	heap.deallocate(first, 100, 16);
	/* 100 and 112 are one class: both round up to 112 bytes. */
	require(heap.allocate(112, 16) == first);
	heap.deallocate(second, 100, 16);
	require(heap.allocate(97, 16) == second);
	/* A different class does not take it. */
	void *other = heap.allocate(500, 16);
	require(other != first && other != second);
}

void large_and_over_aligned_blocks_are_mappings_of_their_own()
{
	DomainHeap heap;
	const auto baseline = committed();
	std::vector<Block> blocks;
	for (const std::size_t bytes : {std::size_t{8193}, std::size_t{20000},
		     std::size_t{1} << 20, (std::size_t{5} << 20) + 3}) {
		for (const std::size_t alignment : {std::size_t{16}, std::size_t{64},
			     std::size_t{256}, std::size_t{4096}, host_page()}) {
			void *pointer = heap.allocate(bytes, alignment);
			require(reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0);
			stamp(pointer, bytes);
			blocks.push_back({pointer, bytes, alignment});
		}
	}
	/* Small blocks with a large alignment are large blocks too. */
	for (const std::size_t alignment : {std::size_t{32}, std::size_t{4096}}) {
		void *pointer = heap.allocate(24, alignment);
		require(reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0);
		stamp(pointer, 24);
		blocks.push_back({pointer, 24, alignment});
	}
	for (const auto &block : blocks)
		require(stamped(block.pointer, block.bytes));
	for (const auto &block : blocks)
		heap.deallocate(block.pointer, block.bytes, block.alignment);
	/* A large block's pages go back when it is freed, before any release. */
	require(committed() == baseline,
		"freeing every large block left their pages committed");
	require(heap.mapped_bytes() == 0);
}

void release_returns_every_byte_to_the_region()
{
	auto &region = HeapRegion::instance();
	const auto baseline = committed();
	DomainHeap heap;
	void *small = heap.allocate(100, 16);
	void *medium = heap.allocate(8000, 16);
	void *large = heap.allocate(300000, 16);
	(void)medium;
	(void)large;
	require(heap.mapped_bytes() != 0);
	require(committed() - baseline == heap.mapped_bytes());
	require(region.contains(small));
	/* A chunk grows by doubling: enough small blocks to need several. */
	for (int index = 0; index < 40000; ++index)
		(void)heap.allocate(256, 16);
	require(committed() - baseline == heap.mapped_bytes());
	require(heap.mapped_bytes() > (std::size_t{512} << 10));

	heap.release();
	require(heap.mapped_bytes() == 0);
	require(committed() == baseline,
		"release left pages of the heap committed");

	/* The heap is as new, and the first block lands where the first did:
	 * the region hands out its lowest free range first. */
	void *again = heap.allocate(100, 16);
	require(again == small, "a released range was not reused lowest first");
}

void a_released_range_stays_reserved()
{
	auto &region = HeapRegion::instance();
	const auto baseline = committed();
	void *start;
	{
		DomainHeap heap;
		start = heap.allocate(100, 16);
		stamp(start, 100);
	}
	require(committed() == baseline);
	/* The pages are not resident, and the range is still a mapping: it
	 * is reserved, not unmapped, so the next mmap(NULL) cannot take it. */
	unsigned char residency = 1;
	const auto page_start = reinterpret_cast<void *>(
		reinterpret_cast<std::uintptr_t>(start) & ~(host_page() - 1));
	require(mincore(page_start, host_page(),
			reinterpret_cast<char *>(&residency)) == 0,
		"the released range is no longer mapped");
	require((residency & MINCORE_INCORE) == 0);

	std::vector<void *> mappings;
	for (int index = 0; index < 64; ++index) {
		void *memory = mmap(nullptr, std::size_t{4} << 20,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		require(memory != MAP_FAILED);
		require(!region.contains(memory),
			"mmap(NULL) landed inside the heap region");
		mappings.push_back(memory);
	}
	for (auto *memory : mappings)
		munmap(memory, std::size_t{4} << 20);
}

void the_region_throws_when_it_has_no_room_and_recovers()
{
	const auto baseline = committed();
	DomainHeap heap;
	requireThrows<std::bad_alloc>([&] {
		(void)heap.allocate(HeapRegion::reservation(), 16);
	});
	requireThrows<std::bad_alloc>([&] {
		(void)heap.allocate(static_cast<std::size_t>(-1) - 8, 16);
	});
	requireThrows<std::bad_alloc>([&] {
		(void)heap.allocate(100, host_page() * 2);
	});
	require(committed() == baseline);

	/* Fill the region with large blocks until it refuses, then free them. */
	std::vector<void *> blocks;
	const std::size_t block_bytes = std::size_t{256} << 20;
	bool exhausted = false;
	try {
		for (std::size_t count = 0;
		     count <= HeapRegion::reservation() / block_bytes + 1; ++count)
			blocks.push_back(heap.allocate(block_bytes, 16));
	} catch (const std::bad_alloc &) {
		exhausted = true;
	}
	require(exhausted, "the region never ran out");
	require(!blocks.empty());
	for (auto *block : blocks)
		heap.deallocate(block, block_bytes, 16);
	require(committed() == baseline);
	/* Its space is back: a block as large as the last that fit is served. */
	void *again = heap.allocate(block_bytes, 16);
	heap.deallocate(again, block_bytes, 16);
}

void eight_threads_share_one_heap_and_free_each_others_blocks()
{
	const auto baseline = committed();
	DomainHeap heap;
	constexpr unsigned threads = 8;
	constexpr unsigned per_round = 4000;
	constexpr unsigned rounds = 3;
	static std::vector<Block> slots[threads];
	Gate sync{threads};
	std::vector<std::thread> workers;
	for (unsigned self = 0; self < threads; ++self)
		workers.emplace_back([&, self] {
			std::mt19937 random{self + 1};
			for (unsigned round = 0; round < rounds; ++round) {
				auto &mine = slots[self];
				mine.clear();
				for (unsigned index = 0; index < per_round; ++index) {
					const std::size_t bytes = 1 + random() %
						(index % 16 == 0 ? 20000 : 1200);
					void *pointer = heap.allocate(bytes, 16);
					stamp(pointer, bytes);
					mine.push_back({pointer, bytes, 16});
				}
				sync.arrive_and_wait();
				for (const auto &block : slots[(self + 1) % threads]) {
					require(stamped(block.pointer, block.bytes),
						"a block was overwritten by another thread");
					heap.deallocate(block.pointer, block.bytes, 16);
				}
				sync.arrive_and_wait();
			}
		});
	for (auto &worker : workers)
		worker.join();
	heap.release();
	require(committed() == baseline);
}

void a_nested_scratch_scope_resets_only_at_the_outermost_end()
{
	void *first;
	{
		ScratchScope outer;
		first = outer.arena().allocate(100, 16);
		std::memset(first, 0xa5, 100);
		{
			ScratchScope inner;
			void *inside = inner.arena().allocate(100, 16);
			require(inside >= static_cast<char *>(first) + 100,
				"an inner allocation overlaps the outer one");
			std::memset(inside, 0x5a, 100);
		}
		/* The inner scope ended; the outer allocation is intact and the
		 * arena has not rewound past it. */
		for (int index = 0; index < 100; ++index)
			require(static_cast<unsigned char *>(first)[index] == 0xa5);
		void *after = outer.arena().allocate(100, 16);
		require(after >= static_cast<char *>(first) + 100);
	}
	ScratchScope again;
	require(again.arena().allocate(100, 16) == first,
		"the outermost scope did not rewind the arena");
}

void scratch_keeps_one_chunk_and_returns_the_rest_at_the_reset()
{
	const auto kept = []() {
		ScratchScope scope;
		(void)scope.arena().allocate(1, 16);
		return committed();
	}();
	auto &arena = uml::memory::thread_scratch_arena();
	require(arena.mapped_bytes() == ScratchArena::kept_bytes);
	{
		ScratchScope scope;
		/* Past the kept chunk, an alignment that needs padding, and a
		 * block far larger than any chunk. */
		(void)scope.arena().allocate(60000, 16);
		void *aligned = scope.arena().allocate(100, 4096);
		require(reinterpret_cast<std::uintptr_t>(aligned) % 4096 == 0);
		void *large = scope.arena().allocate(3 << 20, 64);
		std::memset(large, 1, 3 << 20);
		require(committed() >= kept + (3 << 20));
		require(arena.mapped_bytes() > ScratchArena::kept_bytes);
		requireThrows<std::bad_alloc>([&] {
			(void)scope.arena().allocate(HeapRegion::reservation(), 16);
		});
	}
	require(committed() == kept, "the reset kept more than one chunk");
	require(arena.mapped_bytes() == ScratchArena::kept_bytes);

	/* A thread's kept chunk goes back when the thread ends. */
	const auto before_thread = committed();
	std::size_t during = 0;
	std::thread worker([&] {
		ScratchScope scope;
		(void)scope.arena().allocate(100, 16);
		during = committed();
	});
	worker.join();
	require(during == before_thread + ScratchArena::kept_bytes);
	require(committed() == before_thread);
}

void scratch_containers_never_free_and_reset_together()
{
	ScratchScope scope;
	std::vector<int, ScratchAllocator<int>> numbers{
		ScratchAllocator<int>{scope.arena()}};
	for (int index = 0; index < 100000; ++index)
		numbers.push_back(index);
	for (int index = 0; index < 100000; ++index)
		require(numbers[static_cast<std::size_t>(index)] == index);
	std::vector<char, ScratchAllocator<char>> other{
		ScratchAllocator<char>{scope.arena()}};
	require(numbers.get_allocator() == ScratchAllocator<int>{other.get_allocator()});
}

void an_allocator_follows_its_container_and_falls_back_without_a_heap()
{
	DomainHeap first_heap;
	DomainHeap second_heap;
	using Vector = std::vector<int, DomainAllocator<int>>;
	Vector first{DomainAllocator<int>{&first_heap}};
	Vector second{DomainAllocator<int>{&second_heap}};
	first.assign(1000, 7);
	second.assign(10, 1);

	/* Move assignment, swap and copy assignment carry the heap with the
	 * nodes, so nothing is ever freed into the wrong heap. */
	second = std::move(first);
	require(second.get_allocator().heap() == &first_heap);
	require(second.size() == 1000 && second[999] == 7);
	Vector third{DomainAllocator<int>{&second_heap}};
	third.assign(5, 3);
	std::swap(second, third);
	require(second.get_allocator().heap() == &second_heap);
	require(third.get_allocator().heap() == &first_heap);
	Vector copy{DomainAllocator<int>{&second_heap}};
	copy = third;
	require(copy.get_allocator().heap() == &first_heap);
	require(copy.size() == 1000);

	require(DomainAllocator<int>{&first_heap} != DomainAllocator<int>{&second_heap});
	require(DomainAllocator<int>{&first_heap} == DomainAllocator<long>{&first_heap});

	/* A container built outside any domain is an ordinary one. */
	const auto baseline = committed();
	Vector plain;
	plain.assign(100000, 1);
	require(plain.get_allocator().heap() == nullptr);
	require(committed() == baseline);
}

void a_leased_heap_goes_back_with_its_last_lease()
{
	auto &leased = uml::memory::LeasedHeap::engine();
	const auto before = committed();
	require(leased.leases() == 0);
	{
		uml::memory::HeapLease first;
		void *block = first.heap().allocate(100, 16);
		stamp(block, 100);
		require(committed() > before);
		{
			/* A copy holds a lease of its own. */
			const uml::memory::HeapLease second{first};
			require(leased.leases() == 2);
		}
		require(leased.leases() == 1 && stamped(block, 100),
			"a lease that ended took the heap from the one that is left");
		first.heap().deallocate(block, 100, 16);
	}
	require(leased.leases() == 0);
	require(committed() == before, "the last lease left the heap committed");

	/* Zeroed, and the size it was made with frees it. */
	{
		const uml::memory::HeapLease lease;
		auto numbers = uml::memory::make_leased_array<std::uint32_t>(
			100'000, true);
		for (std::size_t index = 0; index < 100'000; ++index)
			require(numbers[index] == 0);
		numbers[99'999] = 7;
		auto overwritten =
			uml::memory::make_leased_array<std::uint32_t>(10, false);
		overwritten[9] = 1;
	}
	require(committed() == before);
}

/* Taking a lease as the last one ends is safe: the count and the release are
 * one critical section, so a block a new holder allocates is never released
 * from under it. Each holder writes its block and reads it back across a
 * yield, over many generations of the heap. */
void leases_taken_as_the_last_ends_keep_their_blocks()
{
	constexpr unsigned threads = 3;
	constexpr int rounds = 300'000;
	std::atomic_bool failed{};
	Gate start{threads};
	std::vector<std::thread> workers;
	for (unsigned worker = 0; worker < threads; ++worker)
		workers.emplace_back([&, worker] {
			start.arrive_and_wait();
			for (int round = 0; round < rounds; ++round) {
				const uml::memory::HeapLease lease;
				auto *block = static_cast<unsigned char *>(
					lease.heap().allocate(64, 16));
				for (unsigned index = 0; index < 64; ++index)
					block[index] = static_cast<unsigned char>(
						worker + index);
				std::this_thread::yield();
				for (unsigned index = 0; index < 64; ++index)
					if (block[index] !=
					    static_cast<unsigned char>(worker + index))
						failed.store(true);
				lease.heap().deallocate(block, 64, 16);
			}
		});
	for (auto &worker : workers)
		worker.join();
	require(!failed.load(), "a block changed under its holder");
	require(uml::memory::LeasedHeap::engine().leases() == 0);
}

void heaps_are_counted_while_they_live()
{
	const auto before = DomainHeap::live_heaps();
	{
		DomainHeap first;
		DomainHeap second;
		require(DomainHeap::live_heaps() == before + 2);
	}
	require(DomainHeap::live_heaps() == before);
}

} // namespace

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"every size class hands out aligned, distinct blocks",
			every_size_class_hands_out_aligned_distinct_blocks},
		{"a freed block is the next of its class",
			a_freed_block_is_the_next_of_its_class},
		{"large and over-aligned blocks are mappings of their own",
			large_and_over_aligned_blocks_are_mappings_of_their_own},
		{"release returns every byte to the region",
			release_returns_every_byte_to_the_region},
		{"a released range stays reserved",
			a_released_range_stays_reserved},
		{"the region throws when it has no room and recovers",
			the_region_throws_when_it_has_no_room_and_recovers},
		{"eight threads share one heap and free each other's blocks",
			eight_threads_share_one_heap_and_free_each_others_blocks},
		{"a nested scratch scope resets only at the outermost end",
			a_nested_scratch_scope_resets_only_at_the_outermost_end},
		{"scratch keeps one chunk and returns the rest at the reset",
			scratch_keeps_one_chunk_and_returns_the_rest_at_the_reset},
		{"scratch containers never free and reset together",
			scratch_containers_never_free_and_reset_together},
		{"an allocator follows its container and falls back without a heap",
			an_allocator_follows_its_container_and_falls_back_without_a_heap},
		{"a leased heap goes back with its last lease",
			a_leased_heap_goes_back_with_its_last_lease},
		{"leases taken as the last ends keep their blocks",
			leases_taken_as_the_last_ends_keep_their_blocks},
		{"heaps are counted while they live",
			heaps_are_counted_while_they_live},
	};
	return uml::testing::runNamedTests(tests);
}
