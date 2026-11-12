// SPDX-License-Identifier: GPL-2.0-or-later
#include "fill_index.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace {

using uml::memory::FillIndex;
using uml::testing::require;

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint64_t address_limit = DARWIN_UML_GUEST_ADDRESS_LIMIT;
/* 64 pages share a leaf. */
constexpr std::uint64_t leaf_span = 64 * page_size;

std::atomic_size_t allocations{};

/* A mapped page's word: any nonzero value that is not a mapping the test
 * cares about, distinct per page. */
[[nodiscard]] std::uintptr_t word_of(std::uint64_t page) noexcept
{
	return static_cast<std::uintptr_t>(page | FillIndex::readable);
}

void store_reserved(FillIndex &index, std::uint64_t page)
{
	require(index.reserve(page));
	index.store(page, word_of(page));
}

/* The pages of `pages` in [first, end), by brute force. */
[[nodiscard]] std::vector<std::uint64_t> expected_in(
	const std::map<std::uint64_t, std::uintptr_t> &pages,
	std::uint64_t first, std::uint64_t end)
{
	std::vector<std::uint64_t> found;
	for (const auto &[page, word] : pages) {
		(void)word;
		if (page >= first && page < end)
			found.push_back(page);
	}
	return found;
}

[[nodiscard]] std::vector<std::uint64_t> scanned(const FillIndex &index,
	std::uint64_t first, std::uint64_t end)
{
	std::vector<std::uint64_t> found;
	(void)index.scan(first, end, [&](std::uint64_t page, std::uintptr_t word) {
		require(word == word_of(page));
		found.push_back(page);
		return false;
	});
	return found;
}

void a_word_round_trips_and_keeps_its_documented_layout()
{
	/* The layout is the contract between the writers, the readers and the
	 * assembly-free fill path, so pin it: read, write, executable, mapped
	 * executable, code, guest writable, then six bits of slot. */
	const FillIndex::Fields all{0x2000, true, true, true, true, true, 5};
	require(FillIndex::encode(all) ==
		(0x2000U | 1U | 2U | 4U | 8U | 16U | 32U | (5U << 6)));
	require(FillIndex::address_mask == ~std::uintptr_t{0xfff});
	require(FillIndex::overflow_slot == 63);
	/* Executable, as the fill judges it, is PROT_EXEC on a code page. */
	require((FillIndex::encode({0x2000, true, false, true, false, false, 0}) &
		FillIndex::permissions) == FillIndex::readable);
	require((FillIndex::encode({0x2000, true, false, true, true, false, 0}) &
		FillIndex::permissions) ==
		(FillIndex::readable | FillIndex::executable));

	const std::uintptr_t hosts[] = {
		0x1000, 0x7fff'ffff'f000, 0x0001'0000'0000'0000,
		0xffff'ffff'ffff'f000};
	const unsigned slots[] = {0, 1, 31, 62, 63};
	for (const auto host : hosts)
		for (const auto slot : slots)
			for (unsigned flags = 0; flags < 32; ++flags) {
				const FillIndex::Fields fields{host, (flags & 1U) != 0,
					(flags & 2U) != 0, (flags & 4U) != 0,
					(flags & 8U) != 0, (flags & 16U) != 0, slot};
				const auto word = FillIndex::encode(fields);
				require(word != 0);
				require((word & FillIndex::address_mask) == host);
				const auto back = FillIndex::decode(word);
				require(back.host == host && back.read == fields.read &&
					back.write == fields.write &&
					back.exec == fields.exec &&
					back.guest_code == fields.guest_code &&
					back.guest_writable == fields.guest_writable &&
					back.slot == slot);
				/* A word encoded from what decode returned is the word. */
				require(FillIndex::encode(back) == word);
			}

	require(FillIndex::representable(0x1000));
	require(FillIndex::representable(0x7fff'ffff'f000));
	require(!FillIndex::representable(0));
	require(!FillIndex::representable(0x1010));
	require(!FillIndex::representable(0x1800));
	require(!FillIndex::representable(0x1fff));
}

void a_page_is_absent_until_reserved_and_stored()
{
	FillIndex index;
	const std::uint64_t pages[] = {0, page_size, address_limit - page_size,
		0x1'0000'0000, 0xc'0000'0000, 0x7fff'0000'0000};
	for (const auto page : pages) {
		require(index.find(page) == 0 && index.lookup(page) == 0);
		/* Nothing was reserved, so the word has nowhere to live and reads
		 * as absent, which is always sound. */
		index.store(page, word_of(page));
		require(index.find(page) == 0);
	}
	for (const auto page : pages) {
		require(index.reserve(page));
		require(index.find(page) == 0);
		index.store(page, word_of(page));
		require(index.find(page) == word_of(page));
		require(index.lookup(page) == word_of(page));
	}
	/* A neighbour in the same leaf stays absent. */
	require(index.find(page_size * 2) == 0);
	for (const auto page : pages) {
		index.erase(page);
		require(index.find(page) == 0 && index.lookup(page) == 0);
	}
}

void a_page_above_the_guest_limit_is_found_only_under_the_lock()
{
	FillIndex index;
	const std::uint64_t high[] = {address_limit, address_limit + page_size,
		0xffff'ffff'ffff'f000};
	for (const auto page : high) {
		index.store(page, word_of(page));
		require(index.lookup(page) == 0);
		require(index.reserve(page));
		index.store(page, word_of(page));
		/* The fill of an entered context never asks for such a page. */
		require(index.find(page) == 0);
		require(index.lookup(page) == word_of(page));
	}
	require(scanned(index, address_limit, ~std::uint64_t{0}).size() == 3);
	std::vector<std::uint64_t> noted;
	index.erase_range(address_limit, address_limit + 2 * page_size,
		[&](std::uint64_t page, std::uintptr_t word) {
			require(word == word_of(page));
			noted.push_back(page);
		});
	std::sort(noted.begin(), noted.end());
	require(noted == std::vector<std::uint64_t>(
		{address_limit, address_limit + page_size}));
	require(index.lookup(address_limit) == 0);
	require(index.lookup(0xffff'ffff'ffff'f000) ==
		word_of(0xffff'ffff'ffff'f000));
	index.erase(0xffff'ffff'ffff'f000);
	require(index.lookup(0xffff'ffff'ffff'f000) == 0);
}

void a_committed_change_allocates_nothing()
{
	FillIndex index;
	std::vector<std::uint64_t> pages;
	for (std::uint64_t cluster = 0; cluster < 40; ++cluster)
		for (std::uint64_t page = 0; page < 30; ++page)
			pages.push_back(cluster * (std::uint64_t{1} << 36) +
				page * page_size);
	pages.push_back(address_limit);
	pages.push_back(address_limit + 5 * page_size);
	for (const auto page : pages)
		require(index.reserve(page));

	const auto before = allocations.load();
	for (const auto page : pages)
		index.store(page, word_of(page));
	for (const auto page : pages)
		index.store(page, word_of(page) | FillIndex::writable);
	std::size_t seen = 0;
	require(!index.scan(0, ~std::uint64_t{0},
		[&](std::uint64_t, std::uintptr_t) {
			++seen;
			return false;
		}));
	require(seen == pages.size());
	std::size_t noted = 0;
	index.erase_range(0, 20 * (std::uint64_t{1} << 36),
		[&](std::uint64_t, std::uintptr_t) { ++noted; });
	require(noted == 20 * 30);
	for (const auto page : pages)
		index.erase(page);
	require(allocations.load() == before);
	for (const auto page : pages)
		require(index.lookup(page) == 0);
}

void a_leaf_is_freed_only_when_its_last_word_goes()
{
	FillIndex index;
	const std::uint64_t first = 7 * leaf_span;
	store_reserved(index, first);
	store_reserved(index, first + page_size);
	/* A neighbour's word keeps the leaf. */
	index.trim(first + 2 * page_size);
	auto before = allocations.load();
	require(index.reserve(first));
	require(allocations.load() == before);

	index.store(first, 0);
	index.trim(first);
	require(index.find(first + page_size) == word_of(first + page_size));
	before = allocations.load();
	require(index.reserve(first));
	require(allocations.load() == before);

	index.erase(first + page_size);
	index.erase(first);
	/* Gone: reserving the page allocates its leaf again, and only that. */
	before = allocations.load();
	require(index.reserve(first));
	require(allocations.load() == before + 1);
	index.trim(first);
	before = allocations.load();
	require(index.reserve(first));
	require(allocations.load() == before + 1);
}

void a_sparse_range_walk_visits_only_what_is_mapped()
{
	FillIndex index;
	/* One page in each corner of the tree: both ends of a leaf, a leaf
	 * apart, and under different nodes at every level, in both halves of the
	 * root and in its last entry. */
	const std::uint64_t pages[] = {
		page_size, leaf_span - page_size, leaf_span,
		std::uint64_t{1} << 26, std::uint64_t{1} << 34,
		std::uint64_t{1} << 42, (std::uint64_t{1} << 42) + page_size,
		address_limit - page_size};
	std::map<std::uint64_t, std::uintptr_t> mapped;
	for (const auto page : pages) {
		store_reserved(index, page);
		mapped.emplace(page, word_of(page));
	}

	/* The whole address space has 2^35 pages; walking it page by page takes
	 * minutes. */
	const auto started = std::chrono::steady_clock::now();
	const auto everything = scanned(index, 0, ~std::uint64_t{0});
	require(std::chrono::steady_clock::now() - started <
		std::chrono::seconds(2));
	require(everything == expected_in(mapped, 0, ~std::uint64_t{0}));
	require(everything.size() == std::size(pages));
	require(std::is_sorted(everything.begin(), everything.end()));

	const std::pair<std::uint64_t, std::uint64_t> ranges[] = {
		{page_size, 2 * page_size},
		{2 * page_size, leaf_span - page_size},
		{leaf_span - page_size, leaf_span + page_size},
		{leaf_span - page_size, leaf_span},
		{leaf_span + page_size, std::uint64_t{1} << 26},
		{std::uint64_t{1} << 26, (std::uint64_t{1} << 26) + 1},
		{(std::uint64_t{1} << 26) + page_size, std::uint64_t{1} << 34},
		{(std::uint64_t{1} << 42) - page_size,
			(std::uint64_t{1} << 42) + 2 * page_size},
		{address_limit - 2 * page_size, address_limit},
		{address_limit - page_size, address_limit + page_size},
		{5, 5}, {leaf_span, page_size}, {0, 0}, {0, 1},
	};
	for (const auto &[first, end] : ranges)
		require(scanned(index, first, end) == expected_in(mapped, first, end));

	/* The visit can stop the walk. */
	std::size_t visits = 0;
	require(index.scan(0, ~std::uint64_t{0},
		[&](std::uint64_t, std::uintptr_t) {
			++visits;
			return true;
		}));
	require(visits == 1);

	/* Erasing a range clears what is in it and nothing else, and frees the
	 * leaves it empties. */
	std::vector<std::uint64_t> noted;
	index.erase_range(leaf_span, std::uint64_t{1} << 34,
		[&](std::uint64_t page, std::uintptr_t word) {
			require(word == word_of(page));
			noted.push_back(page);
		});
	require(noted == std::vector<std::uint64_t>(
		{leaf_span, std::uint64_t{1} << 26}));
	for (const auto page : noted)
		mapped.erase(page);
	require(scanned(index, 0, ~std::uint64_t{0}) ==
		expected_in(mapped, 0, ~std::uint64_t{0}));
}

void a_walk_agrees_with_a_brute_force_scan_of_random_pages()
{
	FillIndex index;
	std::map<std::uint64_t, std::uintptr_t> mapped;
	std::uint64_t state = 0x9e3779b97f4a7c15ULL;
	const auto next = [&] {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	};
	/* Clusters of neighbouring pages at random places, as a guest maps. */
	for (unsigned cluster = 0; cluster < 300; ++cluster) {
		const auto base = (next() % (address_limit / page_size)) * page_size;
		const auto length = 1 + next() % 90;
		for (std::uint64_t page = 0; page < length; ++page) {
			const auto address = base + page * page_size;
			if (address >= address_limit)
				break;
			store_reserved(index, address);
			mapped.emplace(address, word_of(address));
		}
	}
	for (unsigned round = 0; round < 400; ++round) {
		const auto first = (next() % (address_limit / page_size)) * page_size;
		/* Short, medium and very wide ranges, ending on a page or not. */
		const std::uint64_t span[] = {page_size, 70 * page_size,
			(std::uint64_t{1} << 30), address_limit};
		auto end = first + span[next() % 4] + (next() % 3) * 17;
		if (round % 7 == 0)
			end = ~std::uint64_t{0};
		require(scanned(index, first, end) == expected_in(mapped, first, end));
	}
	for (unsigned round = 0; round < 100; ++round) {
		const auto first =
			(next() % (address_limit / page_size)) * page_size;
		const auto end = first + (std::uint64_t{1} << 31);
		std::vector<std::uint64_t> noted;
		index.erase_range(first, end,
			[&](std::uint64_t page, std::uintptr_t) {
				noted.push_back(page);
			});
		require(noted == expected_in(mapped, first, end));
		for (const auto page : noted)
			mapped.erase(page);
		require(scanned(index, 0, ~std::uint64_t{0}) ==
			expected_in(mapped, 0, ~std::uint64_t{0}));
	}
}

void a_reader_without_the_lock_sees_a_word_whole_or_not_at_all()
{
	/* What an entered context relies on: a mapping added beside it only
	 * fills a word it cannot have read as present, so it reads the whole word
	 * or none. Removals quiesce execution and are not part of this. */
	constexpr unsigned pages = 20000;
	constexpr unsigned readers = 3;
	FillIndex index;
	std::atomic_bool done{false};
	std::atomic_size_t found{};
	std::atomic_uint started{};
	std::vector<std::thread> threads;
	for (unsigned reader = 0; reader < readers; ++reader) {
		threads.emplace_back([&, reader] {
			std::uint64_t state = 0x2545f4914f6cdd1dULL + reader;
			started.fetch_add(1, std::memory_order_acq_rel);
			while (!done.load(std::memory_order_acquire)) {
				state ^= state << 13;
				state ^= state >> 7;
				state ^= state << 17;
				/* Pages spread over several leaves and nodes. */
				const auto page = (state % pages) * 5 * page_size;
				const auto word = index.find(page);
				require(word == 0 || word == word_of(page));
				if (word != 0)
					found.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	while (started.load(std::memory_order_acquire) < readers)
		std::this_thread::yield();
	for (std::uint64_t page = 0; page < pages; ++page)
		store_reserved(index, page * 5 * page_size);
	/* Let the readers see them all at least once. */
	while (found.load(std::memory_order_relaxed) < 1000)
		std::this_thread::yield();
	done.store(true, std::memory_order_release);
	for (auto &thread : threads)
		thread.join();
	for (std::uint64_t page = 0; page < pages; ++page)
		require(index.find(page * 5 * page_size) ==
			word_of(page * 5 * page_size));
}

void clear_empties_the_index_and_it_is_reusable()
{
	FillIndex index;
	for (std::uint64_t page = 0; page < 200; ++page)
		store_reserved(index, page * leaf_span);
	store_reserved(index, address_limit);
	index.clear();
	for (std::uint64_t page = 0; page < 200; ++page)
		require(index.lookup(page * leaf_span) == 0);
	require(index.lookup(address_limit) == 0);
	require(scanned(index, 0, ~std::uint64_t{0}).empty());
	store_reserved(index, 5 * leaf_span);
	require(index.lookup(5 * leaf_span) == word_of(5 * leaf_span));
}

} // namespace

void *operator new(std::size_t size)
{
	allocations.fetch_add(1, std::memory_order_relaxed);
	if (void *memory = std::malloc(size))
		return memory;
	throw std::bad_alloc{};
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	allocations.fetch_add(1, std::memory_order_relaxed);
	return std::malloc(size);
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept
{
	std::free(memory);
}

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"a word round-trips and keeps its documented layout",
			a_word_round_trips_and_keeps_its_documented_layout},
		{"a page is absent until reserved and stored",
			a_page_is_absent_until_reserved_and_stored},
		{"a page above the guest limit is found only under the lock",
			a_page_above_the_guest_limit_is_found_only_under_the_lock},
		{"a committed change allocates nothing",
			a_committed_change_allocates_nothing},
		{"a leaf is freed only when its last word goes",
			a_leaf_is_freed_only_when_its_last_word_goes},
		{"a sparse range walk visits only what is mapped",
			a_sparse_range_walk_visits_only_what_is_mapped},
		{"a walk agrees with a brute force scan of random pages",
			a_walk_agrees_with_a_brute_force_scan_of_random_pages},
		{"a reader without the lock sees a word whole or not at all",
			a_reader_without_the_lock_sees_a_word_whole_or_not_at_all},
		{"clear empties the index and it is reusable",
			clear_empties_the_index_and_it_is_reusable},
	};
	return uml::testing::runNamedTests(tests);
}
