// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"
#include "address_layout.hpp"
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "scratch_backing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace {

using namespace std::chrono_literals;

using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-static-mmu-batch-XXXXXX";

using uml::testing::require;

class HeldExecution final {
public:
	HeldExecution(unsigned long generation, unsigned long guest_pc)
	{
		context_ =
			darwin_uml_static_vm_execution_context_create(generation);
		require(context_ != nullptr);
		require(darwin_uml_static_vm_execution_context_begin(
				context_, guest_pc) == 0);
	}

	HeldExecution(const HeldExecution &) = delete;
	HeldExecution &operator=(const HeldExecution &) = delete;

	~HeldExecution()
	{
		if (context_ == nullptr)
			return;
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	[[nodiscard]] void *context() const noexcept
	{
		return context_;
	}

private:
	void *context_{};
};

void wait_until_started(const std::atomic_bool &started)
{
	started.wait(false, std::memory_order_acquire);
}

void test_additive_batch_does_not_stop_execution()
{
	constexpr unsigned long generation = 201;
	constexpr unsigned long base = 0x4'0000;
	constexpr unsigned long added = base + DARWIN_UML_GUEST_PAGE_SIZE;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};

	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);
	{
		HeldExecution execution{generation, base};
		std::atomic_bool started{};
		std::atomic_bool complete{};
		std::thread publisher{[&] {
			require(darwin_uml_static_vm_batch_begin(generation) == 0);
			require(darwin_uml_static_vm_map_generation(
					generation, reinterpret_cast<void *>(added),
					backing.descriptor(), DARWIN_UML_GUEST_PAGE_SIZE,
					DARWIN_UML_GUEST_PAGE_SIZE,
					PROT_READ | PROT_WRITE, 0, 1) == 0);
			started.store(true, std::memory_order_release);
			started.notify_all();
			require(darwin_uml_static_vm_batch_end() == 0);
			complete.store(true, std::memory_order_release);
			complete.notify_all();
		}};
		wait_until_started(started);
		complete.wait(false, std::memory_order_acquire);
		require(darwin_uml_static_vm_execution_context_sequences(
				execution.context()).execution_interrupt == 0);
		publisher.join();
	}

	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
			generation, added, DARWIN_UML_STATIC_WRITE, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(translated != nullptr);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_replacing_batch_still_quiesces_execution()
{
	constexpr unsigned long generation = 202;
	constexpr unsigned long base = 0x5'0000;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);

	void *context =
		darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	require(darwin_uml_static_vm_execution_context_begin(context, base) == 0);
	std::atomic_bool started{};
	std::atomic_bool complete{};
	std::thread replacer{[&] {
		require(darwin_uml_static_vm_batch_begin(generation) == 0);
		require(darwin_uml_static_vm_map_generation(
				generation, reinterpret_cast<void *>(base),
				backing.descriptor(), DARWIN_UML_GUEST_PAGE_SIZE,
				DARWIN_UML_GUEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
		started.store(true, std::memory_order_release);
		started.notify_all();
		require(darwin_uml_static_vm_batch_end() == 0);
		complete.store(true, std::memory_order_release);
		complete.notify_all();
	}};
	wait_until_started(started);
	std::this_thread::sleep_for(20ms);
	require(!complete.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_execution_context_sequences(
			context).execution_interrupt != 0);
	require(darwin_uml_static_vm_execution_context_end(context) == 0);
	darwin_uml_static_vm_execution_context_destroy(context);
	replacer.join();
	require(complete.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_cross_thread_destroy_recreate_replaces_stale_activation()
{
	constexpr unsigned long generation = 203;
	constexpr unsigned long base = 0x6'0000;
	ScratchBacking original{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	ScratchBacking replacement{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	original.put_byte(0, std::byte{0x31});
	replacement.put_byte(0, std::byte{0x72});
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), original.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
			generation, base, DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(*static_cast<const std::uint8_t *>(translated) == 0x31);

	std::thread recreator{[&] {
		require(darwin_uml_static_vm_destroy(generation) == 0);
		require(darwin_uml_static_vm_map_generation(
				generation, reinterpret_cast<void *>(base),
				replacement.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	recreator.join();
	translated = nullptr;
	require(darwin_uml_static_vm_translate(
			generation, base, DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(*static_cast<const std::uint8_t *>(translated) == 0x72);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void require_unmapped(unsigned long generation, unsigned long address)
{
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
			generation, address, DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_UNMAPPED);
	require(translated == nullptr);
}

void test_unaligned_unmap_covers_the_complete_range()
{
	constexpr unsigned long generation = 204;
	constexpr unsigned long base = 0x7'0000;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);

	/* The two-byte interval straddles a guest-page boundary. Normalizing the
	 * address and length independently used to remove only the first page. */
	require(darwin_uml_static_vm_unmap_generation(
			generation,
			reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE - 1),
			2) == 0);
	require_unmapped(generation, base);
	require_unmapped(generation, base + DARWIN_UML_GUEST_PAGE_SIZE);

	const auto near_limit =
		std::numeric_limits<std::uintptr_t>::max() - 3U;
	require(darwin_uml_static_vm_unmap_generation(
			generation, reinterpret_cast<void *>(near_limit), 8) == -1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_ranges_cannot_cross_generation_boundary_or_wrap()
{
	constexpr unsigned long generation = 205;
	constexpr unsigned long address_limit = DARWIN_UML_GUEST_ADDRESS_LIMIT;
	constexpr unsigned long low_page =
		address_limit - DARWIN_UML_GUEST_PAGE_SIZE;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};

	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(low_page), backing.descriptor(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == -1);
	require_unmapped(generation, low_page);
	require_unmapped(generation, address_limit);

	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(low_page), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address_limit), backing.descriptor(),
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(low_page),
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ) == -1);
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(address_limit - 1), 2) == -1);

	void *low = nullptr;
	void *high = nullptr;
	require(darwin_uml_static_vm_translate(generation, low_page,
		DARWIN_UML_STATIC_WRITE, &low) == DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_translate(generation, address_limit,
		DARWIN_UML_STATIC_WRITE, &high) == DARWIN_UML_STATIC_TRANSLATED);
	require(low != nullptr && high != nullptr);

	constexpr unsigned long ordinary_page = 0xc'0000;
	const unsigned long overflowing_length = 0UL - ordinary_page;
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(ordinary_page), overflowing_length,
		PROT_READ) == -1);

	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(low_page), DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(address_limit),
		DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_split_ios_address_layout_keeps_native_kernel_hole_disjoint()
{
	using Layout = uml::memory::AddressLayout<
		128ULL << 40, 56ULL << 30, 62ULL << 30>;
	using uml::memory::AddressDomain;
	constexpr std::uint64_t page = 16ULL << 10;

	require(Layout::classify((56ULL << 30) - page, page) ==
		AddressDomain::guest);
	require(Layout::classify(56ULL << 30, 6ULL << 30) ==
		AddressDomain::native);
	require(Layout::classify(62ULL << 30, page) == AddressDomain::guest);
	require(Layout::classify(256ULL << 30, 32ULL << 30) ==
		AddressDomain::guest);
	require(Layout::classify((56ULL << 30) - page, 2 * page) ==
		AddressDomain::invalid);
	require(Layout::classify((62ULL << 30) - page, 2 * page) ==
		AddressDomain::invalid);
	require(Layout::classify((128ULL << 40) - page, 2 * page) ==
		AddressDomain::invalid);
	require(Layout::is_native_window(56ULL << 30, 6ULL << 30));
}

void test_allocation_failures_are_balanced_and_retryable()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::active_backing_mappings;
	using uml::memory::testing::fail_next_allocation;
	constexpr unsigned long base = 0x8'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	require(active_backing_mappings() == 0);

	constexpr unsigned long backing_generation = 301;
	fail_next_allocation(AllocationSite::backing);
	require(darwin_uml_static_vm_map_generation(
			backing_generation, reinterpret_cast<void *>(base), backing.descriptor(),
			0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		-1);
	require(active_backing_mappings() == 0);
	require_unmapped(backing_generation, base);
	require(darwin_uml_static_vm_map_generation(
			backing_generation, reinterpret_cast<void *>(base), backing.descriptor(),
			0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);
	require(darwin_uml_static_vm_destroy(backing_generation) == 0);
	require(active_backing_mappings() == 0);

	constexpr unsigned long generation_failure = 302;
	fail_next_allocation(AllocationSite::generation);
	require(darwin_uml_static_vm_activate(generation_failure) == -1);
	require(darwin_uml_static_vm_activate(generation_failure) == 0);
	require(darwin_uml_static_vm_destroy(generation_failure) == 0);

	constexpr unsigned long walker_generation = 303;
	fail_next_allocation(AllocationSite::generation);
	require(darwin_uml_static_vm_walker_lock(walker_generation) == -1);
	require(darwin_uml_static_vm_walker_lock(walker_generation) == 0);
	require(darwin_uml_static_vm_walker_unlock(walker_generation) == 0);
	require(darwin_uml_static_vm_destroy(walker_generation) == 0);

	constexpr unsigned long mapping_generation = 304;
	fail_next_allocation(AllocationSite::reserve);
	require(darwin_uml_static_vm_map_generation(
			mapping_generation, reinterpret_cast<void *>(base), backing.descriptor(),
			0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		-1);
	require(active_backing_mappings() == 0);
	require_unmapped(mapping_generation, base);
	require(darwin_uml_static_vm_map_generation(
			mapping_generation, reinterpret_cast<void *>(base), backing.descriptor(),
			0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) ==
		0);
	require(darwin_uml_static_vm_destroy(mapping_generation) == 0);
	require(active_backing_mappings() == 0);

	constexpr unsigned long batch_generation = 305;
	require(darwin_uml_static_vm_batch_begin(batch_generation) == 0);
	fail_next_allocation(AllocationSite::batch);
	require(darwin_uml_static_vm_map_generation(
			batch_generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == -1);
	require(darwin_uml_static_vm_batch_end() == -1);
	require(active_backing_mappings() == 0);
	require(darwin_uml_static_vm_batch_begin(batch_generation) == 0);
	require(darwin_uml_static_vm_map_generation(
			batch_generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	fail_next_allocation(AllocationSite::reserve);
	require(darwin_uml_static_vm_batch_end() == -1);
	require(active_backing_mappings() == 0);
	require_unmapped(batch_generation, base);
	require(darwin_uml_static_vm_batch_begin(batch_generation) == 0);
	require(darwin_uml_static_vm_map_generation(
			batch_generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_batch_end() == 0);
	require(darwin_uml_static_vm_destroy(batch_generation) == 0);
	require(active_backing_mappings() == 0);
}

void test_multi_page_and_history_failures_are_atomic()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_allocation_at;
	using uml::memory::testing::fail_next_allocation;
	constexpr unsigned long generation = 306;
	constexpr unsigned long base = 0x9'0000;
	constexpr auto pages = 3UL;
	ScratchBacking backing{backing_name, pages * DARWIN_UML_GUEST_PAGE_SIZE};
	const auto initial = darwin_uml_static_vm_sequences(generation);

	fail_allocation_at(AllocationSite::reserve, 2);
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		pages * DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_EXEC, 1, 0) == -1);
	for (unsigned long page = 0; page < pages; ++page)
		require_unmapped(generation,
			base + page * DARWIN_UML_GUEST_PAGE_SIZE);
	auto after_page_failure = darwin_uml_static_vm_sequences(generation);
	require(after_page_failure.mapping_changes == initial.mapping_changes &&
		after_page_failure.code_changes == initial.code_changes);

	fail_next_allocation(AllocationSite::history);
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		pages * DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_EXEC, 1, 0) == -1);
	for (unsigned long page = 0; page < pages; ++page)
		require_unmapped(generation,
			base + page * DARWIN_UML_GUEST_PAGE_SIZE);
	auto after_history_failure = darwin_uml_static_vm_sequences(generation);
	require(after_history_failure.mapping_changes == initial.mapping_changes &&
		after_history_failure.code_changes == initial.code_changes);

	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 0,
		pages * DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_EXEC, 1, 0) == 0);
	/* Pages that were not mapped held no translation, so mapping them
	 * moves the mapping sequence and publishes no code change. */
	const auto retried = darwin_uml_static_vm_sequences(generation);
	require(retried.mapping_changes == initial.mapping_changes + 1 &&
		retried.code_changes == initial.code_changes);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_nth_batch_operation_failure_rolls_back_every_operation()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_allocation_at;
	constexpr unsigned long generation = 307;
	constexpr unsigned long base = 0xa'0000;
	constexpr auto pages = 3UL;
	ScratchBacking backing{backing_name, pages * DARWIN_UML_GUEST_PAGE_SIZE};
	const auto initial = darwin_uml_static_vm_sequences(generation);
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	for (unsigned long page = 0; page < pages; ++page)
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base +
				page * DARWIN_UML_GUEST_PAGE_SIZE), backing.descriptor(),
			page * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	fail_allocation_at(AllocationSite::batch_commit, 2);
	require(darwin_uml_static_vm_batch_end() == -1);
	require(darwin_uml_static_vm_batch_end() == -1);
	for (unsigned long page = 0; page < pages; ++page)
		require_unmapped(generation,
			base + page * DARWIN_UML_GUEST_PAGE_SIZE);
	const auto failed = darwin_uml_static_vm_sequences(generation);
	require(failed.mapping_changes == initial.mapping_changes &&
		failed.code_changes == initial.code_changes);

	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	for (unsigned long page = 0; page < pages; ++page)
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base +
				page * DARWIN_UML_GUEST_PAGE_SIZE), backing.descriptor(),
			page * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	require(darwin_uml_static_vm_batch_end() == 0);
	for (unsigned long page = 0; page < pages; ++page) {
		void *translated = nullptr;
		require(darwin_uml_static_vm_translate(generation,
			base + page * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_STATIC_READ, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED);
		require(translated != nullptr);
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_large_overlapping_batch_preserves_last_operation()
{
	constexpr unsigned long generation = 310;
	constexpr unsigned long base = 0x20'0000;
	constexpr std::size_t page_count = 8192;
	constexpr std::size_t length =
		page_count * DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::size_t quarter = page_count / 4;
	ScratchBacking backing{backing_name, length};

	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
			length, PROT_READ | PROT_WRITE, 0, 1) == 0);
	/* Revisit thousands of already planned pages twice. The middle half is
	 * first removed, then its upper quarter is restored read-only. */
	require(darwin_uml_static_vm_unmap_generation(
			generation,
			reinterpret_cast<void *>(base + quarter * DARWIN_UML_GUEST_PAGE_SIZE),
			2 * quarter * DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(darwin_uml_static_vm_map_generation(
			generation,
			reinterpret_cast<void *>(base + 2 * quarter *
				DARWIN_UML_GUEST_PAGE_SIZE), backing.descriptor(),
			2 * quarter * DARWIN_UML_GUEST_PAGE_SIZE,
			quarter * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ, 0, 0) == 0);
	require(darwin_uml_static_vm_batch_end() == 0);
	for (std::size_t index = 0; index < page_count; ++index) {
		void *translated = nullptr;
		const auto address = base + index * DARWIN_UML_GUEST_PAGE_SIZE;
		const bool absent = index >= quarter && index < 2 * quarter;
		const bool read_only = index >= 2 * quarter && index < 3 * quarter;
		const auto read_result = darwin_uml_static_vm_translate(
			generation, address, DARWIN_UML_STATIC_READ, &translated);
		require(read_result == (absent ? DARWIN_UML_STATIC_UNMAPPED :
			DARWIN_UML_STATIC_TRANSLATED));
		translated = nullptr;
		const auto write_result = darwin_uml_static_vm_translate(
			generation, address, DARWIN_UML_STATIC_WRITE, &translated);
		require(write_result == (absent ? DARWIN_UML_STATIC_UNMAPPED :
			read_only ? DARWIN_UML_STATIC_PERMISSION :
			DARWIN_UML_STATIC_TRANSLATED));
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_batch_index_allocation_failure_is_atomic_and_retryable()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_allocation_at;
	constexpr unsigned long generation = 311;
	constexpr unsigned long base = 0x30'0000;
	constexpr std::size_t pages = 3;
	ScratchBacking backing{backing_name, pages * DARWIN_UML_GUEST_PAGE_SIZE};
	const auto initial = darwin_uml_static_vm_sequences(generation);

	const auto stage_batch = [&] {
		require(darwin_uml_static_vm_batch_begin(generation) == 0);
		require(darwin_uml_static_vm_map_generation(
				generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
				pages * DARWIN_UML_GUEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
	};
	stage_batch();
	/* The first checkpoint covers the index reserve. Fail while inserting the
	 * second unique page, after an earlier plan entry exists but before any
	 * GenerationState mapping or sequence has been published. */
	fail_allocation_at(AllocationSite::batch_index, 3);
	require(darwin_uml_static_vm_batch_end() == -1);
	for (std::size_t index = 0; index < pages; ++index)
		require_unmapped(generation,
			base + index * DARWIN_UML_GUEST_PAGE_SIZE);
	const auto failed = darwin_uml_static_vm_sequences(generation);
	require(failed.mapping_changes == initial.mapping_changes &&
		failed.code_changes == initial.code_changes);

	stage_batch();
	require(darwin_uml_static_vm_batch_end() == 0);
	for (std::size_t index = 0; index < pages; ++index) {
		void *translated = nullptr;
		require(darwin_uml_static_vm_translate(generation,
			base + index * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_STATIC_READ, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED);
		require(translated != nullptr);
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_large_sparse_unmap_visits_only_mapped_pages()
{
	constexpr unsigned long generation = 312;
	constexpr unsigned long base =
		DARWIN_UML_NATIVE_ADDRESS_END + 0x40'0000;
	constexpr unsigned long length = 1ULL << 40;
	constexpr std::array<unsigned long, 3> addresses{
		base, base + length / 2,
		base + length - DARWIN_UML_GUEST_PAGE_SIZE};
	ScratchBacking backing{backing_name, addresses.size() * DARWIN_UML_GUEST_PAGE_SIZE};

	for (std::size_t index = 0; index < addresses.size(); ++index)
		require(darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(addresses[index]),
			backing.descriptor(), index * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	const auto before = darwin_uml_static_vm_sequences(generation);
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base), length) == 0);
	require(darwin_uml_static_vm_batch_end() == 0);
	for (const auto address : addresses)
		require_unmapped(generation, address);
	const auto after = darwin_uml_static_vm_sequences(generation);
	require(after.mapping_changes == before.mapping_changes + 1 &&
		after.code_changes == before.code_changes);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_batch_unmaps_change_only_pages_they_find()
{
	constexpr unsigned long generation = 322;
	constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long held = 0x40'0000;
	constexpr unsigned long absent = 0x50'0000;
	constexpr unsigned long transient = 0x60'0000;
	constexpr std::size_t held_pages = 8;
	ScratchBacking backing{backing_name, (held_pages + 2) * page};
	const auto require_mapped = [&](const unsigned long address) {
		void *translated = nullptr;
		require(darwin_uml_static_vm_translate(generation, address,
				DARWIN_UML_STATIC_READ, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED);
		require(translated != nullptr);
	};
	const auto map_page = [&](const unsigned long address,
				  const std::size_t backing_page) {
		require(darwin_uml_static_vm_map_generation(generation,
				reinterpret_cast<void *>(address), backing.descriptor(),
				backing_page * page, page, PROT_READ | PROT_WRITE,
				0, 1) == 0);
	};
	const auto unmap = [&](const unsigned long address,
			       const unsigned long length) {
		require(darwin_uml_static_vm_unmap_generation(generation,
				reinterpret_cast<void *>(address), length) == 0);
	};

	/* Hold more pages than any unmap below names, so each is walked page by
	 * page rather than through the generation's mappings. */
	require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(held), backing.descriptor(), 0,
			held_pages * page, PROT_READ | PROT_WRITE, 0, 1) == 0);
	const auto before = darwin_uml_static_vm_sequences(generation);
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	unmap(absent, 4 * page);
	require(darwin_uml_static_vm_batch_end() == 0);
	const auto after = darwin_uml_static_vm_sequences(generation);
	require(after.mapping_changes == before.mapping_changes &&
		after.code_changes == before.code_changes);

	/* An unmap still clears what the generation holds and what an earlier
	 * operation of the same batch mapped, and an absent page it names can be
	 * mapped later in the batch. */
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	unmap(absent, page);
	map_page(absent, held_pages);
	map_page(transient, held_pages + 1);
	unmap(transient, page);
	unmap(held, page);
	require(darwin_uml_static_vm_batch_end() == 0);
	require_mapped(absent);
	require_unmapped(generation, absent + page);
	require_unmapped(generation, transient);
	require_unmapped(generation, held);
	for (std::size_t index = 1; index < held_pages; ++index)
		require_mapped(held + index * page);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_generation_flush_is_complete_and_reusable()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_next_allocation;
	constexpr unsigned long generation = 313;
	constexpr unsigned long other_generation = 314;
	constexpr unsigned long base = 0x50'0000;
	ScratchBacking backing{backing_name, 3 * DARWIN_UML_GUEST_PAGE_SIZE};

	require(darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 1, 1) == 0);
	require(darwin_uml_static_vm_map_generation(
		other_generation, reinterpret_cast<void *>(base), backing.descriptor(),
		2 * DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	const auto before = darwin_uml_static_vm_sequences(generation);
	fail_next_allocation(AllocationSite::history);
	require(darwin_uml_static_vm_flush_generation(generation) == -1);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, base, DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(translated != nullptr);
	require(darwin_uml_static_vm_sequences(generation).mapping_changes ==
		before.mapping_changes);
	require(darwin_uml_static_vm_flush_generation(generation) == 0);
	require_unmapped(generation, base);
	require_unmapped(generation, base + DARWIN_UML_GUEST_PAGE_SIZE);
	translated = nullptr;
	require(darwin_uml_static_vm_translate(
		other_generation, base, DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(translated != nullptr);
	const auto after = darwin_uml_static_vm_sequences(generation);
	require(after.mapping_changes == before.mapping_changes + 1);
	require(after.code_changes == before.code_changes + 1);
	std::array<darwin_uml_static_mapping_change, 1> changes{};
	std::uint64_t current = 0;
	require(darwin_uml_static_vm_mapping_changes(
		generation, before.mapping_changes, changes.data(), changes.size(),
		&current) == -1);
	require(current == after.mapping_changes);

	/* Flushing does not retire the generation; the synchronized Linux walker
	 * must be able to republish the current PTEs immediately. */
	require(darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	translated = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, base, DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(translated != nullptr);
	require(darwin_uml_static_vm_destroy(generation) == 0);
	require(darwin_uml_static_vm_destroy(other_generation) == 0);
}

void test_empty_generation_flush_rejects_an_older_batch()
{
	constexpr unsigned long generation = 315;
	constexpr unsigned long base = 0x60'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};

	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::thread flusher{[&] {
		require(darwin_uml_static_vm_flush_generation(generation) == 0);
	}};
	flusher.join();
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	require_unmapped(generation, base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* Linux invalidates a range after it changes the PTE and before it frees the
 * page.  A walker that sampled the old PTE may not have published it yet, so
 * the invalidation finds nothing to remove; it must still reject that batch,
 * or the walker maps a page Linux has already given away. */
void test_empty_range_unmap_rejects_an_older_batch()
{
	constexpr unsigned long generation = 316;
	constexpr unsigned long base = 0x60'0000;
	ScratchBacking backing{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};

	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::thread invalidator{[&] {
		require(darwin_uml_static_vm_unmap_generation(
			generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	}};
	invalidator.join();
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	require_unmapped(generation, base);

	/* The replayed walk starts after the invalidation and is accepted. */
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(darwin_uml_static_vm_map_generation(
		generation, reinterpret_cast<void *>(base), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_batch_end() == 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A uaccess pins its page from a PTE whose lock it has already dropped, so
 * when the host names other backing either of the two can be the stale one: a
 * sibling's copy-on-write may have replaced the page in between.  The repair
 * discards the mapping and never installs the caller's backing, and it rejects
 * an older walk as any other invalidation does. */
void test_a_stale_mapping_is_discarded_instead_of_replaced()
{
	constexpr unsigned long generation = 318;
	constexpr unsigned long base = 0x60'0000;
	ScratchBacking published{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	ScratchBacking pinned{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	published.put_byte(0, std::byte{0x5a});
	pinned.put_byte(0, std::byte{0xa5});
	const auto map_published = [&] {
		return darwin_uml_static_vm_map_generation(
			generation, reinterpret_cast<void *>(base), published.descriptor(),
			0, DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1);
	};
	const auto require_published = [&] {
		void *translated = nullptr;
		require(darwin_uml_static_vm_translate(generation, base,
				DARWIN_UML_STATIC_READ, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED);
		require(*static_cast<const std::uint8_t *>(translated) == 0x5a);
	};

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_discard_if_stale(base, pinned.descriptor(), 0) == 0);
	require_unmapped(generation, base);

	require(map_published() == 0);
	require_published();
	require(darwin_uml_static_vm_discard_if_stale(base, published.descriptor(), 0) == 0);
	require_published();

	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(map_published() == 0);
	std::thread uaccess{[&] {
		require(darwin_uml_static_vm_activate(generation) == 0);
		require_published();
		require(darwin_uml_static_vm_discard_if_stale(base, pinned.descriptor(), 0) == 0);
		require_unmapped(generation, base);
	}};
	uaccess.join();
	require_unmapped(generation, base);
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	require_unmapped(generation, base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* Linux walkers of one generation run side by side, each bound to its thread
 * with its own batch.  Neither waits for the other, and with no invalidation
 * between them both publish. */
void test_sibling_walkers_publish_side_by_side()
{
	constexpr unsigned long generation = 317;
	constexpr unsigned long base = 0x60'0000;
	ScratchBacking backing{backing_name, 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	const auto map_page = [&](unsigned long page) {
		return darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(
				base + page * DARWIN_UML_GUEST_PAGE_SIZE),
			backing.descriptor(), page * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1);
	};
	const auto require_mapped = [&](unsigned long page) {
		void *translated = nullptr;
		require(darwin_uml_static_vm_translate(generation,
				base + page * DARWIN_UML_GUEST_PAGE_SIZE,
				DARWIN_UML_STATIC_READ, &translated) ==
			DARWIN_UML_STATIC_TRANSLATED);
	};

	require(darwin_uml_static_vm_walker_lock(generation) == 0);
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(map_page(0) == 0);
	std::thread sibling{[&] {
		require(darwin_uml_static_vm_walker_lock(generation) == 0);
		require(darwin_uml_static_vm_batch_begin(generation) == 0);
		require(map_page(1) == 0);
		require(darwin_uml_static_vm_batch_end() == 0);
		require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	}};
	sibling.join();
	require_mapped(1);
	require(darwin_uml_static_vm_batch_end() == 0);
	require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	require_mapped(0);
	require_mapped(1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A batch plans one entry per touched page and indexes them by page address,
 * reserving the index up to a power-of-two cap.  libc++ keeps a power-of-two
 * bucket count for such a reservation and constrains the hash by masking, so
 * an identity hash of page-aligned keys would reach only a handful of buckets
 * and make planning quadratic.  Assert the distribution rather than the time
 * it takes, so the guarantee does not depend on how fast the host is.
 */
void test_page_index_hash_spreads_page_aligned_addresses()
{
	constexpr std::size_t entries = 64 * 1024;
	constexpr unsigned long base = 0x40'0000;
	std::unordered_map<std::uint64_t, std::size_t,
		uml::memory::GuestPageHash> index;

	index.reserve(entries);
	require((index.bucket_count() & (index.bucket_count() - 1)) == 0);
	for (std::size_t page = 0; page < entries; ++page)
		index.emplace(base + page * DARWIN_UML_GUEST_PAGE_SIZE, page);

	std::size_t occupied = 0;
	std::size_t longest = 0;
	for (std::size_t bucket = 0; bucket < index.bucket_count(); ++bucket) {
		const auto size = index.bucket_size(bucket);
		occupied += size != 0 ? 1 : 0;
		longest = size > longest ? size : longest;
	}
	/* An identity hash yields four occupied buckets and chains of 16,384 here.
	 * A sound hash keeps chains at a small constant, so these bounds separate
	 * the two by three orders of magnitude without being tight. */
	require(occupied >= index.bucket_count() / 4);
	require(longest <= 16);
}

/* The native ownership index holds one entry per 16 KiB host page of guest
 * physical memory, about half a million for an 8 GiB guest, and the kernel
 * then maps and unmaps single pages.  Asking reserve() for size() + n rehashed
 * that index on most of those calls, and a request for exactly a power of two
 * selected a masked bucket count under which page addresses shared a few
 * buckets, so that scanning one empty 32 MiB span took a fifth of a second and
 * a guest shutdown stopped for seconds.
 */
void test_page_index_reservation_never_shrinks_or_rehashes_needlessly()
{
	constexpr std::size_t pages = 64 * 1024 - 8;
	constexpr std::uint64_t host_page = 16 * 1024;
	constexpr std::uint64_t base = 0x6000'0280'0000ULL;
	std::unordered_map<std::uint64_t, int, uml::memory::PageAddressHash>
		index;

	uml::memory::reserve_additional(index, pages);
	for (std::size_t page = 0; page < pages; ++page)
		index.emplace(base + page * host_page, 1);
	const auto settled = index.bucket_count();

	/* Single-page churn below the settled capacity leaves the buckets alone,
	 * including the step on which size() + 1 is exactly a power of two. */
	for (std::size_t page = pages; page < 64 * 1024; ++page) {
		uml::memory::reserve_additional(index, 1);
		require(index.bucket_count() == settled);
		index.emplace(base + page * host_page, 1);
		require(index.bucket_count() == settled);
	}
	for (std::size_t page = 0; page < 1024; ++page)
		index.erase(base + page * host_page);
	uml::memory::reserve_additional(index, 1);
	require(index.bucket_count() == settled);

	/* Growth is geometric and the insertions it covers do not rehash. */
	uml::memory::reserve_additional(index, 4 * pages);
	const auto grown = index.bucket_count();
	require(grown >= 2 * settled);
	for (std::size_t page = 0; page < 4 * pages; ++page)
		index.emplace(base + (64 * 1024 + page) * host_page, 1);
	require(index.bucket_count() == grown);
}

void test_page_index_hash_spreads_host_pages_under_a_masked_bucket_count()
{
	constexpr std::size_t entries = 64 * 1024;
	constexpr std::uint64_t host_page = 16 * 1024;
	constexpr std::uint64_t base = 0x6000'0280'0000ULL;
	std::unordered_map<std::uint64_t, int, uml::memory::PageAddressHash>
		index;

	index.reserve(entries);
	require((index.bucket_count() & (index.bucket_count() - 1)) == 0);
	for (std::size_t page = 0; page < entries; ++page)
		index.emplace(base + page * host_page, 1);
	std::size_t longest = 0;
	for (std::size_t bucket = 0; bucket < index.bucket_count(); ++bucket)
		longest = std::max(longest, index.bucket_size(bucket));
	require(longest <= 16);
}

void test_retired_batch_cannot_commit_into_replacement_generation()
{
	constexpr unsigned long generation = 308;
	constexpr unsigned long base = 0xb'0000;
	ScratchBacking retired{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	ScratchBacking replacement{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	retired.put_byte(0, std::byte{0x41});
	replacement.put_byte(0, std::byte{0x92});
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), retired.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);

	std::thread recreator{[&] {
		require(darwin_uml_static_vm_destroy(generation) == 0);
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base), replacement.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	}};
	recreator.join();
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_READ, &translated) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(translated != nullptr &&
		*static_cast<const std::uint8_t *>(translated) == 0x92);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void test_waiting_execution_context_cannot_enter_retired_generation()
{
	constexpr unsigned long generation = 309;
	constexpr unsigned long guest_pc = 0xd'0000;
	void *active = darwin_uml_static_vm_execution_context_create(generation);
	void *waiting = darwin_uml_static_vm_execution_context_create(generation);
	require(active != nullptr && waiting != nullptr);
	require(darwin_uml_static_vm_execution_context_begin(
		active, guest_pc) == 0);

	std::atomic_int destroy_result{-2};
	std::thread destroyer{[&] {
		destroy_result.store(darwin_uml_static_vm_destroy(generation),
			std::memory_order_release);
	}};
	require(uml::testing::waitUntil([&] {
		return darwin_uml_static_vm_execution_context_sequences(
			active).execution_interrupt != 0;
	}, 5s, 0ms), "destroying the generation never interrupted the executor");

	std::atomic_int waiting_result{-2};
	std::thread waiter{[&] {
		waiting_result.store(
			darwin_uml_static_vm_execution_context_begin(waiting, guest_pc),
			std::memory_order_release);
	}};
	std::this_thread::sleep_for(20ms);
	require(waiting_result.load(std::memory_order_acquire) == -2);
	require(darwin_uml_static_vm_execution_context_end(active) == 0);
	waiter.join();
	destroyer.join();
	require(destroy_result.load(std::memory_order_acquire) == 0);
	require(waiting_result.load(std::memory_order_acquire) == -1);
	darwin_uml_static_vm_execution_context_destroy(active);
	darwin_uml_static_vm_execution_context_destroy(waiting);
}

} // namespace

/* A TLB miss resolves through its executor's execution context while the
 * context is entered: same answer as the generation's own translation, for
 * every access and every kind of page. Anything else (a context that is not
 * entered, another generation, no context) is the generation's translation. */
void test_an_entered_context_translates_as_its_generation_does()
{
	constexpr unsigned long generation = 319;
	constexpr unsigned long other_generation = 320;
	constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long base = 0x70'0000;
	ScratchBacking backing{backing_name, 4 * page};
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 0, page,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base + page), backing.descriptor(), page,
		page, PROT_READ, 0, 0) == 0);
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base + 2 * page), backing.descriptor(),
		2 * page, page, PROT_READ | PROT_EXEC, 1, 0) == 0);
	require(darwin_uml_static_vm_map_generation(other_generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 3 * page,
		page, PROT_READ | PROT_WRITE, 0, 1) == 0);

	constexpr std::array accesses{DARWIN_UML_STATIC_READ,
		DARWIN_UML_STATIC_WRITE, DARWIN_UML_STATIC_EXECUTE};
	const auto agrees = [&](void *context, unsigned long asked) {
		for (unsigned long offset = 0; offset < 4 * page;
		     offset += page / 2) {
			for (const auto access : accesses) {
				void *expected = &backing;
				void *found = &backing;
				const int want = darwin_uml_static_vm_translate(
					asked, base + offset + 8, access, &expected);
				const int got =
					darwin_uml_static_vm_execution_context_translate(
						context, asked, base + offset + 8, access,
						&found);
				require(got == want && found == expected);
			}
		}
	};

	void *context = darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	agrees(context, generation);
	require(darwin_uml_static_vm_execution_context_begin(context, base) == 0);
	agrees(context, generation);
	agrees(context, other_generation);
	agrees(nullptr, generation);
	void *own = nullptr;
	void *other = nullptr;
	require(darwin_uml_static_vm_execution_context_translate(context,
		generation, base, DARWIN_UML_STATIC_WRITE, &own) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_execution_context_translate(context,
		other_generation, base, DARWIN_UML_STATIC_WRITE, &other) ==
		DARWIN_UML_STATIC_TRANSLATED);
	require(own != nullptr && other != nullptr && own != other);
	require(darwin_uml_static_vm_execution_context_end(context) == 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
	agrees(context, generation);
	darwin_uml_static_vm_execution_context_destroy(context);
	require(darwin_uml_static_vm_destroy(other_generation) == 0);
}

/* The entered context's fill reads a lock-free copy of the mappings. Every
 * change, failed or not, keeps that copy's answer the generation's: a remap,
 * a protection change, a batch that adds pages while a vCPU runs, a batch
 * that fails part way, an unmap, one that empties a part of the copy, a
 * whole-generation flush and pages far apart in the address space. */
void test_an_entered_context_fill_follows_every_mapping_change()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_allocation_at;
	constexpr unsigned long generation = 321;
	constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long base = 0x90'0000;
	/* Far enough apart for separate nodes at every level of the index. */
	constexpr std::array<unsigned long, 3> bases{base, 0x7f'f000'0000UL,
		DARWIN_UML_GUEST_ADDRESS_LIMIT - 4 * page};
	ScratchBacking backing{backing_name, 8 * page};
	constexpr std::array accesses{DARWIN_UML_STATIC_READ,
		DARWIN_UML_STATIC_WRITE, DARWIN_UML_STATIC_EXECUTE};

	void *context = darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	const auto agrees = [&] {
		require(darwin_uml_static_vm_execution_context_begin(
				context, base) == 0);
		for (const auto first : bases) {
			for (unsigned long offset = 0; offset < 4 * page;
			     offset += page / 2) {
				for (const auto access : accesses) {
					void *expected = &backing;
					void *found = &backing;
					const int want = darwin_uml_static_vm_translate(
						generation, first + offset + 8, access,
						&expected);
					const int got =
						darwin_uml_static_vm_execution_context_translate(
							context, generation, first + offset + 8,
							access, &found);
					require(got == want && found == expected);
				}
			}
		}
		require(darwin_uml_static_vm_execution_context_end(context) == 0);
	};
	const auto map = [&](unsigned long address, unsigned long offset,
				 unsigned long length, int protection, int code) {
		return darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(address), backing.descriptor(),
			offset, length, protection, code, 1);
	};

	for (const auto first : bases) {
		require(map(first, 0, page, PROT_READ | PROT_WRITE, 0) == 0);
		require(map(first + page, page, page, PROT_READ | PROT_EXEC, 1) ==
			0);
	}
	agrees();
	/* Another backing page at the same address. */
	require(map(base, 4 * page, page, PROT_READ | PROT_WRITE, 0) == 0);
	agrees();
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_protect(reinterpret_cast<void *>(base),
			page, PROT_READ) == 0);
	agrees();
	require(darwin_uml_static_vm_protect(
			reinterpret_cast<void *>(base + page), page, PROT_READ) == 0);
	agrees();
	{
		/* Added beside a running vCPU, without stopping it. */
		HeldExecution running{generation, base};
		require(darwin_uml_static_vm_batch_begin(generation) == 0);
		require(map(base + 2 * page, 2 * page, page,
				PROT_READ | PROT_WRITE, 0) == 0);
		require(darwin_uml_static_vm_batch_end() == 0);
		void *found = nullptr;
		require(darwin_uml_static_vm_execution_context_translate(
				running.context(), generation, base + 2 * page,
				DARWIN_UML_STATIC_WRITE, &found) ==
			DARWIN_UML_STATIC_TRANSLATED);
		require(found != nullptr);
	}
	agrees();
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	require(map(base + 3 * page, 3 * page, page, PROT_READ, 0) == 0);
	require(map(base, 5 * page, 2 * page, PROT_READ | PROT_WRITE, 0) == 0);
	/* The second page's insertion fails after the first one's. */
	fail_allocation_at(AllocationSite::reserve, 2);
	require(darwin_uml_static_vm_batch_end() == -1);
	agrees();
	require(darwin_uml_static_vm_unmap_generation(generation,
			reinterpret_cast<void *>(base + page), 2 * page) == 0);
	agrees();
	require(darwin_uml_static_vm_unmap_generation(generation,
			reinterpret_cast<void *>(bases[2]), 4 * page) == 0);
	agrees();
	/* Into the range whose last page has just gone. */
	require(map(bases[2] + page, 7 * page, page, PROT_READ, 0) == 0);
	agrees();
	require(darwin_uml_static_vm_flush_generation(generation) == 0);
	agrees();
	for (const auto first : bases)
		require(map(first, 6 * page, page, PROT_READ | PROT_WRITE, 0) == 0);
	agrees();
	darwin_uml_static_vm_execution_context_destroy(context);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

int main()
{
	test_additive_batch_does_not_stop_execution();
	test_replacing_batch_still_quiesces_execution();
	test_cross_thread_destroy_recreate_replaces_stale_activation();
	test_unaligned_unmap_covers_the_complete_range();
	test_ranges_cannot_cross_generation_boundary_or_wrap();
	test_split_ios_address_layout_keeps_native_kernel_hole_disjoint();
	test_allocation_failures_are_balanced_and_retryable();
	test_multi_page_and_history_failures_are_atomic();
	test_nth_batch_operation_failure_rolls_back_every_operation();
	test_large_overlapping_batch_preserves_last_operation();
	test_batch_index_allocation_failure_is_atomic_and_retryable();
	test_large_sparse_unmap_visits_only_mapped_pages();
	test_batch_unmaps_change_only_pages_they_find();
	test_generation_flush_is_complete_and_reusable();
	test_empty_generation_flush_rejects_an_older_batch();
	test_empty_range_unmap_rejects_an_older_batch();
	test_a_stale_mapping_is_discarded_instead_of_replaced();
	test_sibling_walkers_publish_side_by_side();
	test_page_index_hash_spreads_page_aligned_addresses();
	test_page_index_reservation_never_shrinks_or_rehashes_needlessly();
	test_page_index_hash_spreads_host_pages_under_a_masked_bucket_count();
	test_retired_batch_cannot_commit_into_replacement_generation();
	test_waiting_execution_context_cannot_enter_retired_generation();
	test_an_entered_context_translates_as_its_generation_does();
	test_an_entered_context_fill_follows_every_mapping_change();
	std::puts("Static MMU batch tests passed");
	return 0;
}
