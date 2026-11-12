// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_address_space.hpp"
#include "native_retry_validation_test_support.hpp"
#include "mach_vm_region_test_support.hpp"
#include "allocation_fault.hpp"
#include "native_window_test_support.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "wait_until.hpp"
#include "memory/shared_backing.hpp"
#include "uml_darwin_memory.h"
#include "uml_darwin_static_mmu.h"
#include "uml/tlb_sync_ranges.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::size_t guest_page_size = 4096;

using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-native-mmu.XXXXXX";

using uml::testing::require;

class ReservedSlot final {
public:
	explicit ReservedSlot(bool native_bridge_address = false,
		std::size_t host_pages = 1)
		: size_{host_pages * uml::testing::host_page()}
	{
		if (native_bridge_address) {
			address_ = uml::testing::reserve_native_test_window(size_);
			require(address_ != 0,
				"native bridge reservation failed");
			require(address_ >= uml::testing::native_test_window_start &&
				address_ <= uml::testing::native_test_window_end - size_,
				"native bridge reservation escaped its window");
			require(uml::testing::release_native_test_window(address_, size_),
				"slot reservation release failed");
		} else {
			void *reservation = mmap(nullptr, size_, PROT_NONE,
				MAP_PRIVATE | MAP_ANON, -1, 0);
			require(reservation != MAP_FAILED, "slot reservation failed");
			address_ = reinterpret_cast<std::uintptr_t>(reservation);
			require(munmap(reservation, size_) == 0,
				"slot reservation release failed");
		}
	}

	~ReservedSlot()
	{
		if (address_ != 0)
			(void)munmap(reinterpret_cast<void *>(address_), size_);
	}

	ReservedSlot(const ReservedSlot &) = delete;
	ReservedSlot &operator=(const ReservedSlot &) = delete;

	[[nodiscard]] std::uintptr_t address() const noexcept { return address_; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
	std::uintptr_t address_{};
	std::size_t size_{};
};

void test_shadow_publication_precedes_software_mapping()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_next_allocation;
	constexpr unsigned long generation = 0x401;
	constexpr std::uintptr_t guest_address = 0x40'0000;
	constexpr std::size_t marker_offset = 37;
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	const auto base = slot.address();
	for (std::size_t quarter = 0; quarter < 4; ++quarter)
		require(darwin_uml_vm_map(reinterpret_cast<void *>(base +
			quarter * guest_page_size), backing.descriptor(),
			quarter * guest_page_size, guest_page_size,
			PROT_READ | PROT_WRITE, 0, 1) == 0,
			"publication fixture native shadow map failed");
	const std::array markers{
		std::byte{0x21}, std::byte{0x42}, std::byte{0x63}, std::byte{0x74}};
	for (std::size_t quarter = 0; quarter < markers.size(); ++quarter)
		*reinterpret_cast<volatile std::byte *>(base +
			quarter * guest_page_size + marker_offset) = markers[quarter];
	/* The shared page's untouched neighbors must survive publication of a
	 * dirty quarter and the other dirty quarters in its 16 KiB host slot. */
	backing.put_byte(2 * guest_page_size + marker_offset + 1,
		std::byte{0x5a});
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(base), slot.size(),
		PROT_READ) == 0, "publication fixture protect failed");

	fail_next_allocation(AllocationSite::native_backing_index);
	errno = 0;
	require(darwin_uml_vm_map_generation(generation,
		reinterpret_cast<void *>(guest_address), backing.descriptor(),
		2 * guest_page_size, guest_page_size, PROT_READ | PROT_EXEC, 1, 0) ==
		-1 && errno == ENOMEM,
		"backing-index allocation failure escaped or published a mapping");
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, guest_address,
		DARWIN_UML_STATIC_READ, &translated) == DARWIN_UML_STATIC_UNMAPPED,
		"failed native publication left a software mapping");
	for (std::size_t quarter = 0; quarter < markers.size(); ++quarter)
		require(backing.get_byte(
			quarter * guest_page_size + marker_offset) == std::byte{0},
			"failed native publication partially changed shared backing");

	require(darwin_uml_vm_map_generation(generation,
		reinterpret_cast<void *>(guest_address), backing.descriptor(),
		2 * guest_page_size, guest_page_size, PROT_READ | PROT_EXEC, 1, 0) == 0,
		"software mapping retry after native publication failed");
	for (std::size_t quarter = 0; quarter < markers.size(); ++quarter)
		require(backing.get_byte(
			quarter * guest_page_size + marker_offset) == markers[quarter],
			"publication omitted a dirty sibling quarter");
	require(backing.get_byte(
		2 * guest_page_size + marker_offset + 1) == std::byte{0x5a},
		"publication damaged an untouched partial-page neighbor");
	const auto restored = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((restored & PROT_WRITE) == 0,
		"publication did not restore native slot protection");
	translated = nullptr;
	require(darwin_uml_static_vm_translate(generation,
		guest_address + marker_offset, DARWIN_UML_STATIC_EXECUTE,
		&translated) == DARWIN_UML_STATIC_TRANSLATED && translated != nullptr &&
		*static_cast<const std::byte *>(translated) == markers[2],
		"translated execution did not observe the published native bytes");

	const std::byte replacement{0x6e};
	backing.put_byte(2 * guest_page_size + marker_offset, replacement);
	require(darwin_uml_vm_sync_from_backing(
		base + 2 * guest_page_size) == 0,
		"reverse backing-to-shadow synchronization failed");
	require(*reinterpret_cast<volatile std::byte *>(base +
		2 * guest_page_size + marker_offset) == replacement &&
		*reinterpret_cast<volatile std::byte *>(base +
			guest_page_size + marker_offset) == markers[1],
		"reverse synchronization damaged a sibling quarter");

	require(darwin_uml_static_vm_destroy(generation) == 0,
		"publication fixture static cleanup failed");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base), slot.size()) == 0,
		"publication fixture native cleanup failed");
}

/* The bridge reports a batch that an invalidation rejected as
 * DARWIN_UML_VM_BATCH_REJECTED.  The static API's Darwin EAGAIN (35) would
 * reach the kernel as EDEADLK, and its retry of the batch would never run. */
void test_bridge_names_a_rejected_batch()
{
	constexpr unsigned long generation = 0x402;
	constexpr std::uintptr_t guest_address = 0x40'0000;
	ScratchBacking backing{backing_name, guest_page_size};
	const auto map_page = [&] {
		return darwin_uml_vm_map_generation(generation,
			reinterpret_cast<void *>(guest_address), backing.descriptor(),
			0, guest_page_size, PROT_READ | PROT_WRITE, 0, 1);
	};

	require(darwin_uml_vm_batch_begin(generation) == 0,
		"rejected-batch fixture could not begin a batch");
	require(map_page() == 0, "rejected-batch fixture map failed");
	std::thread invalidator{[&] {
		require(darwin_uml_vm_unmap_generation(generation,
			reinterpret_cast<void *>(guest_address),
			guest_page_size) == 0,
			"rejected-batch fixture invalidation failed");
	}};
	invalidator.join();
	require(darwin_uml_vm_batch_end() == DARWIN_UML_VM_BATCH_REJECTED,
		"the bridge did not name a rejected batch");
	require(darwin_uml_vm_batch_begin(generation) == 0 && map_page() == 0 &&
		darwin_uml_vm_batch_end() == 0,
		"the walk after the invalidation was not published");
	require(darwin_uml_static_vm_destroy(generation) == 0,
		"rejected-batch fixture cleanup failed");
}

void test_backing_index_tracks_rebind_unmap_and_retire()
{
	ScratchBacking first{backing_name, 4 * guest_page_size};
	ScratchBacking second{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	uml::memory::NativeAddressSpace space;
	const auto base = slot.address();
	require(space.map(base, first.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "index rebind fixture map failed");
	*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x31};
	require(space.publish_backing(first.descriptor(), 0, guest_page_size) == 0 &&
		first.get_byte(0) == std::byte{0x31},
		"initial indexed publication failed");

	*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x41};
	require(space.map(base, second.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0 &&
		first.get_byte(0) == std::byte{0x41},
		"native rebind did not finish the old backing");
	*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x52};
	require(space.publish_backing(first.descriptor(), 0, guest_page_size) == 0 &&
		first.get_byte(0) == std::byte{0x41} &&
		second.get_byte(0) == std::byte{0},
		"rebind left the native page indexed under its old backing");
	require(space.publish_backing(second.descriptor(), 0, guest_page_size) == 0 &&
		second.get_byte(0) == std::byte{0x52},
		"rebind did not index the replacement backing");

	*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x63};
	require(space.unmap(base, guest_page_size) == 0 &&
		second.get_byte(0) == std::byte{0x63},
		"indexed unmap did not finish the native page");
	second.put_byte(0, std::byte{0x74});
	require(space.publish_backing(second.descriptor(), 0, guest_page_size) == 0 &&
		second.get_byte(0) == std::byte{0x74},
		"unmap retained a stale backing-index alias");

	require(space.map(base + guest_page_size, second.descriptor(), guest_page_size,
		guest_page_size, PROT_READ | PROT_WRITE) == 0,
		"index retirement fixture map failed");
	*reinterpret_cast<volatile std::byte *>(base + guest_page_size) =
		std::byte{0x85};
	space.retire();
	require(second.get_byte(guest_page_size) == std::byte{0x85},
		"retirement did not finish an indexed native page");
	errno = 0;
	require(space.publish_backing(second.descriptor(), guest_page_size,
		guest_page_size) == -1 && errno == ESHUTDOWN,
		"retired backing index accepted a publication");
}

/* publish_backing() asks a lock-free filter whether any shadow aliases the
 * page before it takes the mutex.  The filter only gains bits, so it is rebuilt
 * from the live mappings once it has outgrown them.  Rebind one quarter through
 * more backing pages than it tolerates: every new alias must be published the
 * moment it exists, including the one whose bit a rebuild immediately precedes,
 * and an alias that lives through every rebuild must still be found. */
void test_publication_filter_survives_its_rebuilds()
{
	constexpr std::uint64_t churned_pages = 10000;
	ScratchBacking backing{
		backing_name, (churned_pages + 1) * guest_page_size};
	ReservedSlot slot;
	uml::memory::NativeAddressSpace space;
	const auto base = slot.address();
	const auto churned = base + guest_page_size;
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "filter fixture map failed");
	for (std::uint64_t page = 1; page <= churned_pages; ++page) {
		const auto offset = page * guest_page_size;
		const auto marker = static_cast<std::byte>(page % 251 + 1);
		require(space.map(churned, backing.descriptor(), offset, guest_page_size,
			PROT_READ | PROT_WRITE) == 0, "filter churn map failed");
		*reinterpret_cast<volatile std::byte *>(churned) = marker;
		require(space.publish_backing(backing.descriptor(), offset,
			guest_page_size) == 0 && backing.get_byte(offset) == marker,
			"a new alias was not published");
	}
	*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x5a};
	require(space.publish_backing(backing.descriptor(), 0, guest_page_size) == 0 &&
		backing.get_byte(0) == std::byte{0x5a},
		"a filter rebuild lost a live alias");
	require(space.unmap(base, 2 * guest_page_size) == 0,
		"filter fixture cleanup failed");
}

void test_absent_quarter_and_alias_coherency()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	require(slot.size() >= 4 * guest_page_size,
		"host page is smaller than the emulated slot");
	auto space_owner = uml::memory::native_address_space();
	require(space_owner != nullptr, "native address-space domain unavailable");
	auto &space = *space_owner;
	const auto base = slot.address();
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "first quarter map failed");
	auto page = std::span{
		reinterpret_cast<std::byte *>(base), guest_page_size};
	page[0] = std::byte{0x11};

	/* Darwin must not unmap the live sibling when Linux removes a hole. */
	require(space.unmap(base + guest_page_size, guest_page_size) == 0,
		"absent sibling unmap failed");
	require(page[0] == std::byte{0x11},
		"absent sibling unmap discarded the live quarter");

	/* An identical PTE synchronization must preserve live shadow bytes. */
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "duplicate map failed");
	require(page[0] == std::byte{0x11},
		"duplicate map rebuilt an unchanged quarter");

	require(space.map(base + 2 * guest_page_size, backing.descriptor(),
		2 * guest_page_size, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "third quarter map failed");
	page[1] = std::byte{0x22};
	const std::byte authoritative{0x33};
	require(pwrite(backing.descriptor(), &authoritative, 1, 2) == 1,
		"authoritative alias write failed");
	require(space.unmap(base + 2 * guest_page_size, guest_page_size) == 0,
		"third quarter unmap failed");
	require(page[0] == std::byte{0x11} && page[1] == std::byte{0x22},
		"sibling unmap recopied the live quarter");
	require(space.refresh(base, guest_page_size) == 0,
		"authoritative alias refresh failed");
	require(page[1] == std::byte{0x22},
		"refresh discarded a disjoint native-shadow update");
	require(page[2] == authoritative,
		"slot rebuild did not load the authoritative physical alias");
	require(space.unmap(base, guest_page_size) == 0,
		"final quarter unmap failed");
}

void test_authoritative_reload_discards_stale_shadow_bytes()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	uml::memory::NativeAddressSpace space;
	const auto address = slot.address() + guest_page_size;
	constexpr std::size_t marker_offset = 97;
	const std::byte authoritative{0x5a};
	require(space.map(address, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"authoritative reload fixture map failed");
	backing.put_byte(marker_offset, authoritative);
	require(space.reload_from_backing(address, guest_page_size) == 0 &&
		*reinterpret_cast<volatile std::byte *>(address + marker_offset) ==
			authoritative,
		"authoritative reload did not import backing data");
	*reinterpret_cast<volatile std::byte *>(address + marker_offset) =
		std::byte{};
	require(space.reload_from_backing(address, guest_page_size) == 0 &&
		*reinterpret_cast<volatile std::byte *>(address + marker_offset) ==
			authoritative &&
		backing.get_byte(marker_offset) == authoritative,
		"authoritative reload published a stale shadow byte");
	require(space.unmap(address, guest_page_size) == 0,
		"authoritative reload fixture cleanup failed");
}

void test_duplicate_map_reconciles_reused_backing_page()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	uml::memory::NativeAddressSpace space;
	const auto address = slot.address() + guest_page_size;
	constexpr std::size_t marker_offset = 113;
	const std::byte stale{0x6b};
	require(space.map(address, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"duplicate-map reuse fixture map failed");
	auto *page = reinterpret_cast<volatile std::byte *>(address);
	page[marker_offset] = stale;
	require(space.flush(address, guest_page_size) == 0 &&
		backing.get_byte(marker_offset) == stale,
		"duplicate-map reuse fixture did not publish its old object");

	/* Model the allocator zeroing a freed physical page through the shared
	 * direct alias before vmalloc installs the same physical page and PTE at
	 * the same virtual address. */
	backing.put_byte(marker_offset, std::byte{});
	require(space.map(address, backing.descriptor(), 0, guest_page_size,
		PROT_READ) == 0 &&
		page[marker_offset] == std::byte{} &&
		backing.get_byte(marker_offset) == std::byte{},
		"same-backing remap retained the previous object's private shadow");
	require(space.unmap(address, guest_page_size) == 0,
		"duplicate-map reuse fixture cleanup failed");
}

void test_prot_none_shadow_flush_and_release()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "PROT_NONE shadow domain unavailable");
	const auto initial_mappings = space->mapping_count_for_testing();
	const auto initial_slots = space->materialized_slot_count_for_testing();
	const auto address = slot.address() + guest_page_size;
	constexpr std::size_t marker_offset = 83;
	const auto no_access = [&] {
		return (uml::testing::current_vm_protection(
			reinterpret_cast<void *>(address)) &
			(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
	};

	require(darwin_uml_vm_map(reinterpret_cast<void *>(address), backing.descriptor(),
		guest_page_size, guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0,
		"PROT_NONE dirty-drop fixture map failed");
	*reinterpret_cast<volatile std::byte *>(address + marker_offset) =
		std::byte{0x6b};
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(address),
		guest_page_size, PROT_NONE) == 0 && no_access(),
		"PROT_NONE dirty-drop fixture remained accessible");
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(address),
		guest_page_size) == 0,
		"PROT_NONE dirty shadow drop failed");
	require(backing.get_byte(guest_page_size + marker_offset) ==
			std::byte{0x6b},
		"PROT_NONE dirty shadow drop omitted the modified byte");
	require(no_access(),
		"PROT_NONE dirty shadow drop did not restore exact protection");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(address),
		guest_page_size) == 0,
		"PROT_NONE dirty-drop fixture cleanup failed");

	require(darwin_uml_vm_map(reinterpret_cast<void *>(address), backing.descriptor(),
		guest_page_size, guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0 &&
		darwin_uml_vm_protect(reinterpret_cast<void *>(address),
			guest_page_size, PROT_NONE) == 0,
		"PROT_NONE all-absent fixture setup failed");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(address),
		guest_page_size) == 0,
		"PROT_NONE sole-quarter unmap failed");
	require(space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		uml::testing::released_into_native_window(
			address, guest_page_size),
		"PROT_NONE all-absent unmap retained its host slot");

	require(darwin_uml_vm_map(reinterpret_cast<void *>(address), backing.descriptor(),
		2 * guest_page_size, guest_page_size,
		PROT_READ | PROT_WRITE, 0, 1) == 0,
		"PROT_NONE flush-error fixture map failed");
	*reinterpret_cast<volatile std::byte *>(address + marker_offset) =
		std::byte{0x7c};
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(address),
		guest_page_size, PROT_NONE) == 0,
		"PROT_NONE flush-error fixture protect failed");
	const int backing_fd = backing.descriptor();
	const int rescue_fd = dup(backing_fd);
	require(rescue_fd >= 0, "PROT_NONE flush-error rescue dup failed");
	if (close(backing_fd) != 0) {
		(void)close(rescue_fd);
		require(false, "PROT_NONE flush-error close failed");
	}
	errno = 0;
	const int failed_drop = darwin_uml_vm_drop(
		reinterpret_cast<void *>(address), guest_page_size);
	const int failed_drop_errno = errno;
	const int restored_fd = dup2(rescue_fd, backing_fd);
	const int rescue_close = close(rescue_fd);
	require(restored_fd == backing_fd && rescue_close == 0,
		"PROT_NONE flush-error backing restore failed");
	require(failed_drop == -1 && failed_drop_errno == EBADF,
		"PROT_NONE flush read failure did not propagate EBADF");
	require(no_access(),
		"failed PROT_NONE shadow drop did not restore exact protection");
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(address),
		guest_page_size) == 0 &&
		backing.get_byte(2 * guest_page_size + marker_offset) ==
			std::byte{0x7c} && no_access(),
		"PROT_NONE shadow drop did not retry after protection restoration");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(address),
		guest_page_size) == 0 &&
		space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		uml::testing::released_into_native_window(
			address, guest_page_size),
		"PROT_NONE flush-error fixture retained its host slot");
}

void test_shadow_sync_publishes_stable_machine_words()
{
	constexpr std::size_t marker_offset = 128;
	constexpr std::uint64_t first = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t second = 0x8877'6655'4433'2211ULL;
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	const auto address = slot.address() + guest_page_size;
	require(darwin_uml_vm_map(reinterpret_cast<void *>(address), backing.descriptor(),
		guest_page_size, guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0,
		"stable-shadow fixture map failed");
	auto &word = *reinterpret_cast<std::uint64_t *>(address + marker_offset);
	std::atomic_ref<std::uint64_t> shared_word{word};
	std::atomic_uint64_t writes{};
	std::jthread writer{[&](std::stop_token stop) {
		while (!stop.stop_requested()) {
			const auto value =
				(writes.load(std::memory_order_relaxed) & 1U) == 0 ?
					first : second;
			shared_word.store(value, std::memory_order_relaxed);
			writes.fetch_add(1, std::memory_order_release);
		}
	}};
	while (writes.load(std::memory_order_acquire) == 0)
		std::this_thread::yield();

	bool sync_ok = true;
	const char *sync_failure = nullptr;
	for (unsigned int iteration = 0; iteration < 256; ++iteration) {
		if (darwin_uml_vm_drop(reinterpret_cast<void *>(address),
			guest_page_size) != 0) {
			sync_ok = false;
			sync_failure = "stable-shadow flush failed";
			break;
		}
		const auto backing_value = backing.get64(
			guest_page_size + marker_offset);
		if (backing_value != first && backing_value != second) {
			sync_ok = false;
			sync_failure =
				"shadow publication emitted a torn machine word";
			break;
		}
	}
	writer.request_stop();
	writer.join();
	require(sync_ok, sync_failure);
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(address),
		guest_page_size) == 0,
		"stable-shadow fixture cleanup failed");
}

void test_bridge_uses_direct_shared_mapping_for_aligned_ranges()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "native address-space domain unavailable");
	const auto base = slot.address();
	const auto initial_mappings = space->mapping_count_for_testing();
	const auto initial_slots = space->materialized_slot_count_for_testing();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base), backing.descriptor(), 0,
		slot.size(), PROT_READ | PROT_WRITE, 0, 1) == 0,
		"host-page-aligned native bridge map failed");
	require(space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct + 1,
		"aligned native bridge map did not select MAP_SHARED direct mode");
	require(darwin_uml_vm_map(reinterpret_cast<void *>(
		base + guest_page_size), backing.descriptor(), guest_page_size,
		guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0 &&
		space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct + 1,
		"compatible 4 KiB remap replaced its shared direct slot");
	for (std::size_t quarter = 0; quarter < 4; ++quarter) {
		const auto marker = static_cast<std::byte>(0x31 + quarter);
		*reinterpret_cast<volatile std::byte *>(
			base + quarter * guest_page_size + 17) = marker;
		require(backing.get_byte(quarter * guest_page_size + 17) == marker,
			"direct native write was not immediately shared");
	}
	const std::byte replacement{0x6d};
	backing.put_byte(2 * guest_page_size + 29, replacement);
	require(*reinterpret_cast<volatile std::byte *>(
		base + 2 * guest_page_size + 29) == replacement,
		"shared backing write was not immediately visible in direct mode");
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(
		base + guest_page_size), guest_page_size) == 0,
		"partial drop rejected an owned direct mapping");
	require(darwin_uml_vm_retry_fault(base + guest_page_size,
		0, 1) == 1, "direct mapping did not authorize a write retry");
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(base), slot.size(),
		PROT_READ) == 0,
		"full aligned direct protect failed");
	require((uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base)) & PROT_WRITE) == 0,
		"full aligned direct protect did not reach the host mapping");
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(base), slot.size(),
		PROT_READ | PROT_WRITE) == 0,
		"full aligned direct protect restore failed");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		slot.size()) == 0 &&
		space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"aligned direct-routing fixture retained state");
}

void test_whole_slot_map_replaces_shadow_with_shared_page()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "shadow replacement domain unavailable");
	const auto base = slot.address();
	const auto initial_mappings = space->mapping_count_for_testing();
	const auto initial_slots = space->materialized_slot_count_for_testing();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	for (std::size_t quarter = 0; quarter < 4; ++quarter)
		require(darwin_uml_vm_map(reinterpret_cast<void *>(
			base + quarter * guest_page_size), backing.descriptor(),
			quarter * guest_page_size, guest_page_size,
			PROT_READ | PROT_WRITE, 0, 1) == 0,
			"shadow replacement quarter map failed");
	require(space->mapping_count_for_testing() == initial_mappings + 4 &&
		space->materialized_slot_count_for_testing() == initial_slots + 1,
		"shadow replacement fixture did not start as a shadow");
	const std::byte shadow_marker{0x5a};
	const std::byte backing_marker{0x6b};
	*reinterpret_cast<volatile std::byte *>(base + guest_page_size + 7) =
		shadow_marker;
	backing.put_byte(2 * guest_page_size + 9, backing_marker);
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base),
		backing.descriptor(), 0, slot.size(), PROT_READ | PROT_WRITE,
		0, 1) == 0,
		"whole-slot map did not replace its shadow");
	require(space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct + 1,
		"whole-slot map left shadow state behind");
	require(backing.get_byte(guest_page_size + 7) == shadow_marker,
		"replaced shadow lost a write it had not published");
	require(*reinterpret_cast<volatile std::byte *>(
		base + 2 * guest_page_size + 9) == backing_marker,
		"shared page did not show a backing write");
	*reinterpret_cast<volatile std::byte *>(base + 3 * guest_page_size + 5) =
		std::byte{0x7c};
	require(backing.get_byte(3 * guest_page_size + 5) == std::byte{0x7c},
		"shared page write did not reach the backing");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		slot.size()) == 0 &&
		space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"shadow replacement fixture retained state");
}

void test_whole_slot_map_publishes_the_shadow_to_its_own_backing()
{
	ScratchBacking old_backing{backing_name, 4 * guest_page_size};
	ScratchBacking new_backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "shadow replacement domain unavailable");
	const auto base = slot.address();
	const auto initial_mappings = space->mapping_count_for_testing();
	const auto initial_slots = space->materialized_slot_count_for_testing();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	for (std::size_t quarter : {0u, 2u})
		require(darwin_uml_vm_map(reinterpret_cast<void *>(
			base + quarter * guest_page_size), old_backing.descriptor(),
			quarter * guest_page_size, guest_page_size,
			PROT_READ | PROT_WRITE, 0, 1) == 0,
			"replacement quarter map failed");
	const std::byte first_marker{0x31};
	const std::byte third_marker{0x32};
	new_backing.put_byte(5, std::byte{0x41});
	*reinterpret_cast<volatile std::byte *>(base + 11) = first_marker;
	*reinterpret_cast<volatile std::byte *>(base + 2 * guest_page_size + 13) =
		third_marker;
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base),
		new_backing.descriptor(), 0, slot.size(), PROT_READ, 0, 0) == 0,
		"whole-slot map of other backing did not replace its shadow");
	require(old_backing.get_byte(11) == first_marker &&
		old_backing.get_byte(2 * guest_page_size + 13) == third_marker,
		"replaced shadow wrote its stores to the wrong backing");
	require(new_backing.get_byte(11) == std::byte{0} &&
		new_backing.get_byte(2 * guest_page_size + 13) == std::byte{0},
		"replaced shadow wrote its stores to the new backing");
	require(*reinterpret_cast<volatile std::byte *>(base + 5) == std::byte{0x41},
		"shared page did not show the new backing");
	require(space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct + 1,
		"replacement left shadow state behind");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		slot.size()) == 0 &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"replacement fixture retained state");
}

void test_direct_retry_validates_ownership_mapping_and_permissions()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot arena{true, 4};
	const auto page = uml::testing::host_page();
	uml::testing::exercise_native_retry_direct(arena.address(),
		arena.address() + page, arena.address() + 2 * page, page,
		backing.descriptor());
}

void test_direct_mapping_transitions_to_isolated_quarters()
{
	ScratchBacking first{backing_name, 4 * guest_page_size};
	ScratchBacking second{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "hybrid transition domain unavailable");
	const auto base = slot.address();
	const auto initial_mappings = space->mapping_count_for_testing();
	const auto initial_slots = space->materialized_slot_count_for_testing();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base), first.descriptor(), 0,
		slot.size(), PROT_READ | PROT_WRITE, 0, 1) == 0,
		"hybrid transition direct map failed");
	const std::array markers{
		std::byte{0x41}, std::byte{0x52}, std::byte{0x63}, std::byte{0x74}};
	for (std::size_t quarter = 0; quarter < markers.size(); ++quarter)
		*reinterpret_cast<volatile std::byte *>(base +
			quarter * guest_page_size + 11) = markers[quarter];

	require(darwin_uml_vm_protect(reinterpret_cast<void *>(
		base + guest_page_size), guest_page_size, PROT_READ) == 0,
		"partial direct protect did not transition to quarters");
	require(space->direct_mapping_count_for_testing() == initial_direct &&
		space->mapping_count_for_testing() == initial_mappings + 4 &&
		space->materialized_slot_count_for_testing() == initial_slots + 1,
		"partial protect left mixed direct/shadow ownership");
	for (std::size_t quarter = 0; quarter < markers.size(); ++quarter)
		require(*reinterpret_cast<volatile std::byte *>(base +
			quarter * guest_page_size + 11) == markers[quarter],
			"direct-to-shadow transition changed a sibling byte");
	require(space->retry(base + guest_page_size, false, true) == 0 &&
		space->retry(base + 2 * guest_page_size, false, true) == 1,
		"partial protection was not isolated after transition");

	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(
		base + 2 * guest_page_size), guest_page_size) == 0,
		"partial shadow unmap after direct transition failed");
	require(*reinterpret_cast<volatile std::byte *>(
		base + 3 * guest_page_size + 11) == markers[3],
		"partial unmap damaged a live sibling quarter");
	second.put_byte(guest_page_size + 11, std::byte{0x7e});
	require(darwin_uml_vm_map(reinterpret_cast<void *>(
		base + 2 * guest_page_size), second.descriptor(), guest_page_size,
		guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0,
		"partial quarter rebind after direct transition failed");
	require(*reinterpret_cast<volatile std::byte *>(
		base + 2 * guest_page_size + 11) == std::byte{0x7e} &&
		*reinterpret_cast<volatile std::byte *>(
			base + 3 * guest_page_size + 11) == markers[3],
		"partial quarter rebind changed its live sibling");
	*reinterpret_cast<volatile std::byte *>(
		base + 2 * guest_page_size + 13) = std::byte{0x6c};
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(
		base + 2 * guest_page_size), guest_page_size) == 0 &&
		second.get_byte(guest_page_size + 13) == std::byte{0x6c},
		"partial shadow drop did not publish the rebound quarter");

	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		slot.size()) == 0 &&
		space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"direct-to-shadow transition fixture retained ownership");
}

void test_failed_direct_transition_is_atomic_and_retryable()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_next_allocation;
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "transition failure domain unavailable");
	const auto base = slot.address();
	const auto initial_mappings = space->mapping_count_for_testing();
	const auto initial_slots = space->materialized_slot_count_for_testing();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base), backing.descriptor(), 0,
		slot.size(), PROT_READ | PROT_WRITE, 0, 1) == 0,
		"transition failure direct map failed");
	*reinterpret_cast<volatile std::byte *>(base + guest_page_size + 19) =
		std::byte{0x5d};
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(base), slot.size(),
		PROT_NONE) == 0,
		"transition failure could not make the direct slot inaccessible");
	fail_next_allocation(AllocationSite::native_mapping);
	errno = 0;
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(
		base + guest_page_size), guest_page_size, PROT_READ) == -1 &&
		errno == ENOMEM,
		"injected direct-transition failure escaped the C wrapper");
	require(space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct + 1 &&
		backing.get_byte(guest_page_size + 19) == std::byte{0x5d},
		"failed direct transition changed ownership or shared bytes");
	const auto failed_protection = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((failed_protection & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0,
		"failed transition broadened an inaccessible direct slot");
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(base), slot.size(),
		PROT_READ | PROT_WRITE) == 0 &&
		*reinterpret_cast<volatile std::byte *>(
			base + guest_page_size + 19) == std::byte{0x5d},
		"failed transition could not restore its direct mapping");
	fail_next_allocation(AllocationSite::native_operation);
	errno = 0;
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(
		base + guest_page_size), guest_page_size, PROT_READ) == -1 &&
		errno == ENOMEM,
		"post-transition operation failure escaped the C wrapper");
	require(space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct + 1 &&
		*reinterpret_cast<volatile std::byte *>(
			base + guest_page_size + 19) == std::byte{0x5d},
		"post-transition failure did not restore direct ownership and bytes");
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(
		base + guest_page_size), guest_page_size, PROT_READ) == 0,
		"direct transition did not succeed on immediate retry");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		slot.size()) == 0 &&
		space->mapping_count_for_testing() == initial_mappings &&
		space->materialized_slot_count_for_testing() == initial_slots &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"transition retry fixture retained ownership");
}

void test_shadow_protect_rejects_holes_without_mutation()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	uml::memory::NativeAddressSpace space;
	const auto base = slot.address();
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"protect-hole fixture map failed");
	errno = 0;
	require(space.protect(base, 2 * guest_page_size, PROT_READ) == -1 &&
		errno == ENOMEM && space.retry(base, false, true) == 1,
		"shadow protect accepted a hole or changed the live page");
	const auto host_protection = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((host_protection & PROT_WRITE) != 0,
		"failed shadow protect changed host-slot protection");
	errno = 0;
	require(space.protect(base, guest_page_size,
		PROT_READ | 0x4000'0000) == -1 && errno == EINVAL &&
		space.retry(base, false, true) == 1,
		"invalid shadow protection changed mapping metadata");
	require(space.unmap(base, guest_page_size) == 0,
		"protect-hole fixture cleanup failed");
}

void test_sibling_churn_keeps_live_quarter_stable()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	auto space_owner = uml::memory::native_address_space();
	require(space_owner != nullptr, "native address-space domain unavailable");
	auto &space = *space_owner;
	const auto base = slot.address();
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "live quarter map failed");
	require(space.map(base + 2 * guest_page_size, backing.descriptor(),
		2 * guest_page_size, guest_page_size,
		PROT_READ | PROT_WRITE) == 0, "churn quarter map failed");

	auto &counter = *reinterpret_cast<std::uint64_t *>(base);
	std::atomic_ref<std::uint64_t> value{counter};
	std::atomic_bool stop{};
	std::atomic_uint regressions{};
	std::atomic_ulong passes{};
	std::thread writer{[&] {
		auto previous = value.fetch_add(1, std::memory_order_relaxed);
		/* The first pass is made whatever `stop` says. */
		do {
			const auto current = value.fetch_add(1,
				std::memory_order_relaxed);
			if (current <= previous)
				regressions.fetch_add(1, std::memory_order_relaxed);
			previous = current;
			passes.fetch_add(1, std::memory_order_release);
		} while (!stop.load(std::memory_order_relaxed));
	}};
	/* The churn begins once the writer is going: a loaded host can
	 * schedule it only after the churn is over, and the test would then
	 * pass having watched nothing. */
	require(uml::testing::waitUntil([&] {
			return passes.load(std::memory_order_acquire) != 0;
		}, std::chrono::seconds{10}),
		"the sibling writer never started");
	for (int iteration = 0; iteration < 2000; ++iteration) {
		require(space.unmap(base + 2 * guest_page_size,
			guest_page_size) == 0, "churn quarter unmap failed");
		require(space.map(base + 2 * guest_page_size, backing.descriptor(),
			2 * guest_page_size, guest_page_size,
			PROT_READ | PROT_WRITE) == 0,
			"churn quarter remap failed");
	}
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	require(passes.load(std::memory_order_acquire) != 0,
		"the sibling writer made no pass");
	require(regressions.load(std::memory_order_relaxed) == 0,
		"sibling churn replaced a concurrently accessed live quarter");
	require(space.unmap(base + 2 * guest_page_size, guest_page_size) == 0,
		"churn quarter final unmap failed");
	require(space.unmap(base, guest_page_size) == 0,
		"live quarter final unmap failed");
}

void test_native_shadows_are_data_only()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	require(slot.size() >= 4 * guest_page_size,
		"host page is smaller than the emulated slot");
	auto space_owner = uml::memory::native_address_space();
	require(space_owner != nullptr, "native address-space domain unavailable");
	auto &space = *space_owner;
	const auto base = slot.address();
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_EXEC) == 0, "executable quarter map failed");
	require(space.map(base + guest_page_size, backing.descriptor(), guest_page_size,
		guest_page_size, PROT_READ | PROT_WRITE) == 0,
		"writable sibling map failed");

	require(space.retry(base, true, false) == 0,
		"data-only native shadow authorized execute retry");
	const auto protection = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((protection & PROT_EXEC) == 0,
		"native shadow gained execute permission");
	require(space.retry(base + guest_page_size, false, true) == 1,
		"data-only native shadow rejected write retry");
	require(!uml::testing::writable_and_executable(
		uml::testing::current_vm_protection(
			reinterpret_cast<void *>(base))),
		"write retry produced W+X memory");
	*reinterpret_cast<volatile std::byte *>(base + guest_page_size) =
		std::byte{0x5a};

	require(space.unmap(base, guest_page_size) == 0,
		"executable quarter unmap failed");
	require(space.unmap(base + guest_page_size, guest_page_size) == 0,
		"writable sibling unmap failed");
}

void test_writable_kernel_alias_does_not_gain_execute()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot;
	auto space_owner = uml::memory::native_address_space();
	require(space_owner != nullptr, "native address-space domain unavailable");
	auto &space = *space_owner;
	const auto base = slot.address();
	require(space.map(base, backing.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE | PROT_EXEC) == 0,
		"writable kernel alias map failed");
	const auto initial = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((initial & PROT_WRITE) != 0 && (initial & PROT_EXEC) == 0,
		"writable kernel alias was not sanitized to RW");
	require(space.retry(base, true, false) == 0,
		"writable kernel alias incorrectly authorized execute retry");
	require(space.retry(base, false, true) == 1,
		"writable kernel alias rejected write retry");
	const auto after_retry = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((after_retry & PROT_WRITE) != 0 &&
		(after_retry & PROT_EXEC) == 0,
		"writable kernel alias retry produced executable memory");
	require(space.unmap(base, guest_page_size) == 0,
		"writable kernel alias unmap failed");
}

void test_allocation_failures_leave_no_native_slot_or_partial_pages()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_allocation_at;
	using uml::memory::testing::fail_next_allocation;
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space_owner = uml::memory::native_address_space();
	require(space_owner != nullptr, "native address-space domain unavailable");
	auto &space = *space_owner;
	const auto base = slot.address();
	const auto initial_mappings = space.mapping_count_for_testing();
	const auto initial_slots = space.materialized_slot_count_for_testing();

	fail_next_allocation(AllocationSite::native_after_mmap);
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base), backing.descriptor(),
		0, guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == -1,
		"post-mmap allocation failure escaped or succeeded");
	require(space.mapping_count_for_testing() == initial_mappings &&
		space.materialized_slot_count_for_testing() == initial_slots,
		"post-mmap failure left an orphan native slot");

	for (const auto occurrence : {std::size_t{1}, std::size_t{2}}) {
		fail_allocation_at(AllocationSite::native_slot, occurrence);
		require(darwin_uml_vm_map(reinterpret_cast<void *>(base),
			backing.descriptor(), 0, guest_page_size,
			PROT_READ | PROT_WRITE, 0, 1) == -1,
			"native slot-container failure escaped or succeeded");
		require(space.mapping_count_for_testing() == initial_mappings &&
			space.materialized_slot_count_for_testing() == initial_slots,
			"slot-container failure retained bookkeeping or a host slot");
	}

	require(darwin_uml_vm_map(reinterpret_cast<void *>(base), backing.descriptor(),
		0, guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0,
		"live sibling setup failed after injected failures");
	*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x6d};
	const auto live_mappings = space.mapping_count_for_testing();
	const auto live_slots = space.materialized_slot_count_for_testing();

	fail_allocation_at(AllocationSite::native_operation, 2);
	require(darwin_uml_vm_map(
		reinterpret_cast<void *>(base + guest_page_size), backing.descriptor(),
		guest_page_size, 2 * guest_page_size,
		PROT_READ | PROT_WRITE, 0, 1) == -1,
		"Nth native-page failure escaped or partially succeeded");
	require(space.mapping_count_for_testing() == live_mappings &&
		space.materialized_slot_count_for_testing() == live_slots &&
		*reinterpret_cast<volatile std::byte *>(base) == std::byte{0x6d},
		"Nth native-page rollback damaged a live sibling");

	fail_allocation_at(AllocationSite::native_mapping, 2);
	require(darwin_uml_vm_map(
		reinterpret_cast<void *>(base + guest_page_size), backing.descriptor(),
		guest_page_size, 2 * guest_page_size,
		PROT_READ | PROT_WRITE, 0, 1) == -1,
		"Nth native mapping-container failure partially succeeded");
	require(space.mapping_count_for_testing() == live_mappings &&
		space.materialized_slot_count_for_testing() == live_slots &&
		*reinterpret_cast<volatile std::byte *>(base) == std::byte{0x6d},
		"mapping-container rollback damaged a live sibling");

	require(darwin_uml_vm_map(
		reinterpret_cast<void *>(base + guest_page_size), backing.descriptor(),
		guest_page_size, 2 * guest_page_size,
		PROT_READ | PROT_WRITE, 0, 1) == 0,
		"multi-page native retry failed");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		3 * guest_page_size) == 0, "native failure-test cleanup failed");
	require(space.mapping_count_for_testing() == initial_mappings &&
		space.materialized_slot_count_for_testing() == initial_slots,
		"native failure-test cleanup retained state");
}

void test_non_map_wrapper_allocation_failures_are_transactional()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::fail_allocation_at;
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	auto space_owner = uml::memory::native_address_space();
	require(space_owner != nullptr, "native address-space domain unavailable");
	auto &space = *space_owner;
	const auto base = slot.address();
	const auto initial_mappings = space.mapping_count_for_testing();
	const auto initial_slots = space.materialized_slot_count_for_testing();
	const auto map_pages = [&] {
		require(darwin_uml_vm_map(reinterpret_cast<void *>(base), backing.descriptor(),
			0, 2 * guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == 0,
			"non-map failure fixture mapping failed");
		*reinterpret_cast<volatile std::byte *>(base) = std::byte{0x41};
		*reinterpret_cast<volatile std::byte *>(base + guest_page_size) =
			std::byte{0x52};
	};
	const auto require_live_state = [&] {
		require(space.mapping_count_for_testing() == initial_mappings + 2 &&
			space.materialized_slot_count_for_testing() == initial_slots + 1,
			"injected non-map operation changed native bookkeeping");
		require(*reinterpret_cast<volatile std::byte *>(base) ==
				std::byte{0x41} &&
			*reinterpret_cast<volatile std::byte *>(
				base + guest_page_size) == std::byte{0x52},
			"injected non-map operation damaged sibling bytes");
		const auto protection = uml::testing::current_vm_protection(
			reinterpret_cast<void *>(base));
		require((protection & PROT_WRITE) != 0 &&
			(protection & PROT_EXEC) == 0,
			"injected non-map operation changed host-slot protection");
	};

	map_pages();
	for (const auto occurrence : {std::size_t{1}, std::size_t{2}}) {
		fail_allocation_at(AllocationSite::native_operation, occurrence);
		errno = 0;
		require(darwin_uml_vm_protect(reinterpret_cast<void *>(base),
			2 * guest_page_size, PROT_READ) == -1 && errno == ENOMEM,
			"native protect allocation failure escaped the C wrapper");
		require_live_state();
		require(darwin_uml_vm_protect(reinterpret_cast<void *>(base),
			2 * guest_page_size, PROT_READ) == 0,
			"native protect did not retry immediately");
		const auto read_only = uml::testing::current_vm_protection(
			reinterpret_cast<void *>(base));
		require((read_only & (PROT_WRITE | PROT_EXEC)) == 0,
			"native protect retry installed unsafe permissions");
		require(darwin_uml_vm_protect(reinterpret_cast<void *>(base),
			2 * guest_page_size, PROT_READ | PROT_WRITE) == 0,
			"native protect fixture restore failed");
	}

	for (const auto occurrence : {std::size_t{1}, std::size_t{2}}) {
		fail_allocation_at(AllocationSite::native_operation, occurrence);
		errno = 0;
		require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
			2 * guest_page_size) == -1 && errno == ENOMEM,
			"native unmap allocation failure escaped the C wrapper");
		require_live_state();
		require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
			2 * guest_page_size) == 0,
			"native unmap did not retry immediately");
		require(space.mapping_count_for_testing() == initial_mappings &&
			space.materialized_slot_count_for_testing() == initial_slots,
			"native unmap retry retained state");
		map_pages();
	}

	const std::byte authoritative{0x7c};
	require(pwrite(backing.descriptor(), &authoritative, 1, 0) == 1,
		"refresh fixture backing write failed");
	fail_allocation_at(AllocationSite::native_operation, 1);
	errno = 0;
	require(darwin_uml_vm_sync_from_backing(base) == -1 && errno == ENOMEM,
		"native refresh allocation failure escaped the C wrapper");
	require_live_state();
	require(darwin_uml_vm_sync_from_backing(base) == 0,
		"native refresh did not retry immediately");
	require(*reinterpret_cast<volatile std::byte *>(base) == authoritative &&
		*reinterpret_cast<volatile std::byte *>(base + guest_page_size) ==
			std::byte{0x52},
		"native refresh retry damaged an unrelated sibling");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		2 * guest_page_size) == 0,
		"non-map failure fixture cleanup failed");
	require(space.mapping_count_for_testing() == initial_mappings &&
		space.materialized_slot_count_for_testing() == initial_slots,
		"non-map failure fixture retained state");
}

void test_shadow_slots_and_direct_maps_share_global_ownership()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	const auto base = slot.address();
	uml::memory::NativeAddressSpace shadow_owner;
	uml::memory::NativeAddressSpace direct_owner;
	require(shadow_owner.map(base + guest_page_size, backing.descriptor(), 0,
		guest_page_size, PROT_READ | PROT_WRITE) == 0,
		"shadow ownership fixture map failed");
	*reinterpret_cast<volatile std::byte *>(base + guest_page_size) =
		std::byte{0x65};
	errno = 0;
	require(direct_owner.map_direct(base, backing.descriptor(), 0, slot.size(),
		PROT_READ | PROT_WRITE) == -1 && errno == EBUSY,
		"foreign direct map replaced a shadow slot");
	errno = 0;
	require(direct_owner.protect_direct(base, slot.size(), PROT_NONE) == -1 &&
		errno == EBUSY,
		"foreign direct protect changed a shadow slot");
	errno = 0;
	require(direct_owner.unmap_direct(base, slot.size()) == -1 &&
		errno == EBUSY,
		"foreign direct unmap removed a shadow slot");
	require(*reinterpret_cast<volatile std::byte *>(base + guest_page_size) ==
			std::byte{0x65},
		"foreign direct operation damaged the shadow owner");
	shadow_owner.retire();
	require(direct_owner.map_direct(base, backing.descriptor(), 0, slot.size(),
		PROT_READ | PROT_WRITE) == 0,
		"direct owner could not acquire a retired shadow slot");
	auto *first = reinterpret_cast<volatile std::byte *>(base);
	auto *second = reinterpret_cast<volatile std::byte *>(
		base + guest_page_size);
	*first = std::byte{0x71};
	*second = std::byte{0x82};
	errno = 0;
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base), backing.descriptor(),
		0, guest_page_size, PROT_READ | PROT_WRITE, 0, 1) == -1 &&
		errno == EBUSY,
		"foreign bridge remap replaced a direct slot");
	errno = 0;
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(base),
		guest_page_size, PROT_NONE) == -1 && errno == ENOMEM,
		"foreign bridge protect accepted an absent shadow quarter");
	errno = 0;
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		guest_page_size) == 0,
		"foreign bridge unmap rejected an absent shadow quarter");
	require(*first == std::byte{0x71} && *second == std::byte{0x82},
		"foreign bridge operation damaged a direct sibling");
	errno = 0;
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(base),
		guest_page_size) == -1 && errno == EBUSY,
		"foreign bridge drop accepted another domain's direct slot");
	require(*first == std::byte{0x71} && *second == std::byte{0x82},
		"foreign partial drop damaged a direct sibling");
	/* The bridge's space neither maps the slot nor shadows it, so only the
	 * ownership registry can tell that it is another domain's. */
	errno = 0;
	require(darwin_uml_vm_sync_from_backing(base) == -1 && errno == EBUSY,
		"foreign bridge sync accepted another domain's direct slot");
	require(direct_owner.sync_direct(base, guest_page_size) == 0,
		"direct owner could not sync its own slot");
	require(direct_owner.protect_direct(base, slot.size(), PROT_READ) == 0,
		"valid full-slot direct protect failed");
	const auto read_only = uml::testing::current_vm_protection(
		reinterpret_cast<void *>(base));
	require((read_only & PROT_WRITE) == 0,
		"valid full-slot direct protect did not become read-only");
	require(direct_owner.protect_direct(
		base, slot.size(), PROT_READ | PROT_WRITE) == 0,
		"valid full-slot direct protect restore failed");
	constexpr auto overflowing_length =
		std::numeric_limits<std::uint64_t>::max();
	errno = 0;
	require(direct_owner.map_direct(base, backing.descriptor(), 0,
		overflowing_length, PROT_READ | PROT_WRITE) == -1 &&
		errno == EINVAL,
		"overflowing direct map did not report EINVAL");
	errno = 0;
	require(direct_owner.protect_direct(base, overflowing_length,
		PROT_NONE) == -1 && errno == EINVAL,
		"overflowing direct protect did not report EINVAL");
	errno = 0;
	require(direct_owner.unmap_direct(base, overflowing_length) == -1 &&
		errno == EINVAL,
		"overflowing direct unmap did not report EINVAL");
	errno = 0;
	require(direct_owner.flush_direct(base, overflowing_length) == -1 &&
		errno == EINVAL,
		"overflowing direct flush did not report EINVAL");
	require(*first == std::byte{0x71} && *second == std::byte{0x82},
		"rejected overflowing direct operation damaged a sibling");
	errno = 0;
	require(direct_owner.map_direct(base, backing.descriptor(), 0, 0,
		PROT_READ | PROT_WRITE) == -1 && errno == EINVAL,
		"zero-length direct map did not report EINVAL");
	errno = 0;
	require(direct_owner.protect_direct(base, 0, PROT_NONE) == -1 &&
		errno == EINVAL,
		"zero-length direct protect did not report EINVAL");
	errno = 0;
	require(direct_owner.unmap_direct(base, 0) == -1 && errno == EINVAL,
		"zero-length direct unmap did not report EINVAL");
	require(direct_owner.map(base + guest_page_size, backing.descriptor(), 0,
		guest_page_size, PROT_READ | PROT_WRITE) == 0 &&
		direct_owner.direct_mapping_count_for_testing() == 0 &&
		direct_owner.mapping_count_for_testing() == 4,
		"same-owner partial remap did not transition direct ownership");
	require(direct_owner.unmap(base, slot.size()) == 0,
		"transitioned ownership fixture cleanup failed");
}

void test_shadow_failures_report_errno()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	ReservedSlot slot{true};
	const auto address = slot.address() + guest_page_size;
	errno = 0;
	require(darwin_uml_vm_unmap(
		reinterpret_cast<void *>(slot.address()), 0) == -1 &&
		errno == EINVAL,
		"aligned zero-length shadow unmap did not report EINVAL");
	errno = 0;
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(address), 0) == -1 &&
		errno == EINVAL,
		"unaligned zero-length shadow unmap did not report EINVAL");
	errno = 0;
	require(darwin_uml_vm_map(reinterpret_cast<void *>(address), backing.descriptor(),
		0, 0, PROT_READ, 0, 0) == -1 && errno == EINVAL,
		"invalid shadow map returned a zero or stale errno");
	errno = 0;
	require(darwin_uml_vm_protect(reinterpret_cast<void *>(address),
		guest_page_size, PROT_READ) == -1 && errno == ENOMEM,
		"absent shadow protect returned a zero or stale errno");
	errno = 0;
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(address), 1) == -1 &&
		errno == EINVAL,
		"misaligned shadow unmap returned a zero or stale errno");
	errno = 0;
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(address), 1) == -1 &&
		errno == EINVAL,
		"misaligned shadow drop returned a zero or stale errno");
}

void test_anonymous_backing_shadow_uses_root_memory()
{
	ScratchBacking token{backing_name, 4 * guest_page_size};
	ReservedSlot physical;
	require(uml::memory::register_anonymous_shared_backing(token.descriptor()),
		"anonymous backing registration failed");
	void *root = uml::memory::map_shared_backing(
		reinterpret_cast<void *>(physical.address()), physical.size(),
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, token.descriptor(), 0);
	require(root == reinterpret_cast<void *>(physical.address()),
		"anonymous backing root establishment failed");

	ReservedSlot shadow;
	uml::memory::NativeAddressSpace space;
	const auto address = shadow.address() + guest_page_size;
	constexpr std::size_t marker_offset = 137;
	auto *physical_page = reinterpret_cast<volatile std::byte *>(
		physical.address());
	physical_page[marker_offset] = std::byte{0x31};
	auto root_view = uml::memory::acquire_shared_backing_view(
		token.descriptor(), 0, physical.size(), PROT_READ | PROT_WRITE);
	require(root_view && root_view.data() == root &&
		static_cast<const std::byte *>(root_view.data())[marker_offset] ==
			std::byte{0x31},
		"anonymous backing view did not borrow the established root");
	require(space.map(address, token.descriptor(), 0, guest_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"anonymous-root shadow map failed");
	auto *shadow_page = reinterpret_cast<volatile std::byte *>(address);
	require(shadow_page[marker_offset] == std::byte{0x31},
		"anonymous-root shadow did not load root memory");
	require(token.get_byte(marker_offset) == std::byte{},
		"anonymous-root shadow read modified the descriptor token");

	shadow_page[marker_offset] = std::byte{0x42};
	require(space.flush(address, guest_page_size) == 0 &&
		physical_page[marker_offset] == std::byte{0x42} &&
		token.get_byte(marker_offset) == std::byte{},
		"anonymous-root shadow flush updated the descriptor token");

	physical_page[marker_offset] = std::byte{0x53};
	require(space.map(address, token.descriptor(), 0, guest_page_size,
		PROT_READ) == 0 &&
		shadow_page[marker_offset] == std::byte{0x53} &&
		token.get_byte(marker_offset) == std::byte{},
		"anonymous-root same-backing remap retained stale shadow data");
	require(space.unmap(address, guest_page_size) == 0,
		"anonymous-root shadow cleanup failed");
}

} // namespace

/* A guest that restarts itself hands its process to the next one, so the copy
 * of a guest page a shadow mapping keeps for the merge must not outlive the
 * mapping in freed heap. */
void test_destroyed_mapping_keeps_no_copy_of_its_page()
{
	require(uml::memory::NativeAddressSpace::
		mapping_clears_its_page_copy_for_testing(),
		"a destroyed shadow mapping left its page copy in memory");
}

int main()
{
	try {
		uml::testing::NativeTestWindowScope native_window;
		require(native_window.entered(),
			"could not configure native MMU test window");
		test_shadow_publication_precedes_software_mapping();
		test_bridge_names_a_rejected_batch();
		test_backing_index_tracks_rebind_unmap_and_retire();
		test_publication_filter_survives_its_rebuilds();
		test_absent_quarter_and_alias_coherency();
		test_authoritative_reload_discards_stale_shadow_bytes();
		test_duplicate_map_reconciles_reused_backing_page();
		test_prot_none_shadow_flush_and_release();
		test_shadow_sync_publishes_stable_machine_words();
		test_bridge_uses_direct_shared_mapping_for_aligned_ranges();
		test_whole_slot_map_replaces_shadow_with_shared_page();
		test_whole_slot_map_publishes_the_shadow_to_its_own_backing();
		test_direct_retry_validates_ownership_mapping_and_permissions();
		test_direct_mapping_transitions_to_isolated_quarters();
		test_failed_direct_transition_is_atomic_and_retryable();
		test_shadow_protect_rejects_holes_without_mutation();
		test_sibling_churn_keeps_live_quarter_stable();
		test_native_shadows_are_data_only();
		test_writable_kernel_alias_does_not_gain_execute();
		test_allocation_failures_leave_no_native_slot_or_partial_pages();
		test_non_map_wrapper_allocation_failures_are_transactional();
		test_shadow_slots_and_direct_maps_share_global_ownership();
		test_shadow_failures_report_errno();
		test_destroyed_mapping_keeps_no_copy_of_its_page();
		/* Anonymous backing registration is process-global and one-shot. */
		test_anonymous_backing_shadow_uses_root_memory();
		std::puts("native 4K shadow MMU tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "native MMU test failed: %s\n", error.what());
		return EXIT_FAILURE;
	}
}
