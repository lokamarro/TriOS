/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/software_tlb.hpp"
#include "require.hpp"

#include <malloc/malloc.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#include <sanitizer/allocator_interface.h>
#define UML_TEST_SANITIZER_ALLOCATOR 1
#endif
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using uml::cpu::x86_64::CodePageRegistry;
using uml::cpu::x86_64::NativeTlbEntry;
using uml::cpu::x86_64::native_tlb_entry_count;
using uml::cpu::x86_64::native_tlb_index;
using uml::testing::require;

constexpr std::uint64_t page_at(std::uint64_t number) noexcept
{
	return 0x400000U + number * 0x1000U;
}

void test_membership_is_counted()
{
	CodePageRegistry registry;
	const auto page = page_at(3);
	require(!registry.holds(page), "an empty registry holds a page");
	const auto before = registry.changes();
	registry.add(page, nullptr);
	require(registry.holds(page), "an added page is not held");
	require(registry.changes() != before,
		"a page that gained code did not change the registry");
	const auto counted = registry.changes();
	registry.add(page, nullptr);
	require(registry.changes() == counted,
		"a second holder of a page changed the registry");
	registry.remove(page);
	require(registry.holds(page), "one of two holders released the page");
	registry.remove(page);
	require(!registry.holds(page), "the last holder did not release the page");
	registry.remove(page);
	require(!registry.holds(page), "a stray removal underflowed the count");
	registry.add(page, nullptr);
	require(registry.holds(page), "a page cannot be held again");
}

/* The bit in front of the counts keeps neighbours adjacent, so a run of code
 * pages shadows a run of data pages. A shadowed page holds no code: a store to
 * it has nothing to publish, however hot the page is.
 */
void test_membership_is_exact_behind_a_shared_bit()
{
	CodePageRegistry registry;
	const std::uint64_t code = std::uint64_t{0x400} << 12U;
	/* (page ^ page >> 18) of both is 0x400. */
	const std::uint64_t shadowed = std::uint64_t{0x40401} << 12U;
	registry.add(code, nullptr);
	require(registry.holds(code), "the code page is not held");
	require(!registry.holds(shadowed),
		"a data page sharing the code page's bit is held");
	const auto before = registry.changes();
	registry.add(shadowed, nullptr);
	require(registry.holds(shadowed), "the second page of a bit is not held");
	require(registry.changes() != before,
		"a page gained code behind a set bit without changing the registry");
	registry.remove(code);
	require(!registry.holds(code), "a released page is held through its bit");
	require(registry.holds(shadowed),
		"releasing one page of a bit released the other");
	registry.remove(shadowed);
	require(!registry.holds(shadowed), "the bit outlived both of its pages");
}

void test_membership_retires_write_translations()
{
	CodePageRegistry registry;
	auto translations =
		std::make_unique<NativeTlbEntry[]>(native_tlb_entry_count);
	CodePageRegistry::Participant participant;
	participant.write_translations = translations.get();
	registry.join(participant);
	const auto page = page_at(9);
	auto &entry = translations[native_tlb_index(page)];
	entry.write_page = page;
	registry.add(page, &participant);
	require(entry.write_page == 1,
		"a page gained code under a live write translation");
	entry.write_page = page;
	registry.remove(page);
	require(entry.write_page == 1,
		"a tagged write translation outlived the page's code");
	registry.leave(participant);
	entry.write_page = page;
	registry.add(page, nullptr);
	require(entry.write_page == page,
		"a participant that left was still retired");
}

/* A participant whose software TLB has its own layout retires its own
 * entries: the registry calls it for every page that gains or loses code, and
 * never once it has left.
 */
void test_participant_retires_its_own_translations()
{
	struct OwnTlb final {
		std::vector<std::uint64_t> retired;
	};
	CodePageRegistry registry;
	OwnTlb tlb;
	CodePageRegistry::Participant participant;
	registry.join(participant, [](void *context, std::uint64_t page) noexcept {
		static_cast<OwnTlb *>(context)->retired.push_back(page);
	}, &tlb);
	const auto page = page_at(11);
	registry.add(page, &participant);
	require(tlb.retired.size() == 1 && tlb.retired[0] == page,
		"a page gained code without retiring the participant's entry");
	registry.add(page, nullptr);
	require(tlb.retired.size() == 1,
		"a second holder retired the participant's entry again");
	registry.remove(page);
	registry.remove(page);
	require(tlb.retired.size() == 2 && tlb.retired[1] == page,
		"a page lost its code without retiring the participant's entry");
	registry.leave(participant);
	registry.add(page, nullptr);
	require(tlb.retired.size() == 2,
		"a participant that left was still retired");
}

/* Every executor of an address space counts pages in and out at once. The
 * counts stay exact across stripes, a page one holder never releases stays
 * held throughout, and nothing is left behind.
 */
void test_concurrent_holders_stay_exact()
{
	constexpr unsigned holders = 8;
	constexpr unsigned pages = 96;
	constexpr unsigned rounds = 20000;
	CodePageRegistry registry;
	const auto pinned = page_at(pages + 1);
	registry.add(pinned, nullptr);

	std::atomic<bool> start{false};
	std::atomic<bool> lost{false};
	std::vector<std::thread> threads;
	for (unsigned holder = 0; holder < holders; ++holder) {
		threads.emplace_back([&, holder] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			std::uint64_t random = 0x9e3779b97f4a7c15ULL * (holder + 1);
			for (unsigned round = 0; round < rounds; ++round) {
				random = random * 6364136223846793005ULL +
					1442695040888963407ULL;
				const auto page = page_at((random >> 33U) % pages);
				registry.add(page, nullptr);
				if (!registry.holds(page) || !registry.holds(pinned))
					lost.store(true, std::memory_order_release);
				registry.remove(page);
			}
		});
	}
	start.store(true, std::memory_order_release);
	for (auto &thread : threads)
		thread.join();
	require(!lost.load(std::memory_order_acquire),
		"a held page was not held while others came and went");
	for (unsigned page = 0; page < pages; ++page)
		require(!registry.holds(page_at(page)),
			"a page stayed held after every holder released it");
	require(registry.holds(pinned), "the pinned page was released");
}

/* The bytes the process's live allocations hold. A sanitizer replaces the
 * allocator, and the malloc zone statistics it reports then measure something
 * else: AddressSanitizer's count every byte ever allocated, freed or not, and
 * ThreadSanitizer's do not move at all. Its own count of live bytes answers
 * instead. */
std::size_t heap_in_use() noexcept
{
#if defined(UML_TEST_SANITIZER_ALLOCATOR)
	return __sanitizer_get_current_allocated_bytes();
#else
	malloc_statistics_t statistics{};
	malloc_zone_statistics(nullptr, &statistics);
	return statistics.size_in_use;
#endif
}

/* Executors of one address space share its registry, and a guest process
 * never reuses a generation. The registry of an address space nobody runs any
 * more must be released in full, or a guest that runs many short processes
 * grows the host without bound.
 */
void test_acquired_registries_are_released()
{
	constexpr std::uint64_t first = std::uint64_t{1} << 40U;
	constexpr unsigned generations = 4096;
	{
		const auto held = CodePageRegistry::acquire(first);
		require(CodePageRegistry::acquire(first) == held,
			"two holders of a generation got different registries");
	}
	/* Warm the allocator's size classes before measuring. */
	for (unsigned index = 1; index <= 64; ++index)
		(void)CodePageRegistry::acquire(first + index);
	const auto before = heap_in_use();
	for (unsigned index = 0; index < generations; ++index)
		(void)CodePageRegistry::acquire(first + 65 + index);
	const auto after = heap_in_use();
	/* A retained registry costs more than 4 KiB. */
	require(after < before + (std::size_t{1} << 20U),
		"registries of released generations stayed allocated");
}

} // namespace

int main()
{
	test_membership_is_counted();
	test_membership_is_exact_behind_a_shared_bit();
	test_membership_retires_write_translations();
	test_participant_retires_its_own_translations();
	test_concurrent_holders_stay_exact();
	test_acquired_registries_are_released();
	return 0;
}
