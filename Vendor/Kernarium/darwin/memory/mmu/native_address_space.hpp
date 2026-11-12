// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../domain_heap.hpp"
#include "page_index.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_ALLOCATION_TESTING 0
#endif

namespace uml::memory {

/* Every native index is keyed by a host page or slot address, and takes its
 * nodes from the heap of the unit that owns it. */
using PageAddressSet = std::unordered_set<std::uintptr_t, PageAddressHash,
	std::equal_to<std::uintptr_t>, DomainAllocator<std::uintptr_t>>;
template <typename Value>
using PageAddressMap =
	std::unordered_map<std::uintptr_t, Value, PageAddressHash,
		std::equal_to<std::uintptr_t>,
		DomainAllocator<std::pair<const std::uintptr_t, Value>>>;

class NativeAddressSpace final {
public:
	NativeAddressSpace() noexcept;
	~NativeAddressSpace();
	int map_direct(std::uintptr_t address, int fd, std::uint64_t offset,
		std::uint64_t length, int protection);
	int protect_direct(
		std::uintptr_t address, std::uint64_t length, int protection);
	int unmap_direct(std::uintptr_t address, std::uint64_t length);
	int flush_direct(std::uintptr_t address, std::uint64_t length);
	int sync_direct(std::uintptr_t address, std::uint64_t length);
	/* sync_direct() for a range no shadow holds, all under one hold of the
	 * lock; 1, with nothing done, when a shadow holds part of it. */
	int sync_direct_unless_shadow(
		std::uintptr_t address, std::uint64_t length);
	int retry_direct(std::uintptr_t address, bool execute, bool write);
	void retire() noexcept;
	[[nodiscard]] static bool needs_shadow(
		std::uintptr_t address, std::uint64_t offset,
		std::uint64_t length) noexcept;
	[[nodiscard]] bool contains_shadow(
		std::uintptr_t address, std::uint64_t length) const;
	int map(std::uintptr_t address, int fd, std::uint64_t offset,
		std::uint64_t length, int protection);
	int publish_backing(int fd, std::uint64_t offset, std::uint64_t length);
	int protect(std::uintptr_t address, std::uint64_t length,
		    int protection);
	int unmap(std::uintptr_t address, std::uint64_t length);
	int flush(std::uintptr_t address, std::uint64_t length);
	int refresh(std::uintptr_t address, std::uint64_t length);
	int reload_from_backing(std::uintptr_t address, std::uint64_t length);
	int retry(std::uintptr_t address, bool execute, bool write);
	/* Keep the class definition identical in production and allocation-test
	 * translation units.  The definitions exist only in the separately built
	 * test variant, so ordinary products still expose no test symbols or work. */
	[[nodiscard]] std::size_t mapping_count_for_testing() const noexcept;
	[[nodiscard]] std::size_t materialized_slot_count_for_testing() const
		noexcept;
	[[nodiscard]] std::size_t direct_mapping_count_for_testing() const
		noexcept;
	/* Whether a mapping's page copy is zero once the mapping is destroyed. */
	[[nodiscard]] static bool mapping_clears_its_page_copy_for_testing()
		noexcept;

private:
	struct BackingIdentity final {
		std::uint64_t device;
		std::uint64_t inode;

		[[nodiscard]] bool operator==(
			const BackingIdentity &) const noexcept = default;
	};

	struct BackingPageKey final {
		BackingIdentity identity;
		std::uint64_t offset;

		[[nodiscard]] bool operator==(
			const BackingPageKey &) const noexcept = default;
	};

	struct BackingPageKeyHash final {
		[[nodiscard]] std::size_t operator()(
			const BackingPageKey &key) const noexcept;
	};

	struct Mapping final {
		int fd;
		std::uint64_t offset;
		int protection;
		BackingIdentity identity;
		/* A copy of the guest page as the shadow slot last agreed with its
		 * backing. It dies with the mapping and takes nothing of a guest's
		 * memory with it into freed heap, which a guest that restarts itself
		 * hands to its successor in the same process. */
		std::array<std::byte, 4096> original{};
		bool loaded{};

		~Mapping()
		{
			/* The copy holds guest data only once it is loaded. Darwin has
			 * no explicit_bzero and exposes memset_s only for C11 Annex K;
			 * the empty asm that reads the pointer and clobbers memory is
			 * what stops the compiler from dropping the clear as a dead
			 * store to a dying object. */
			if (!loaded)
				return;
			std::memset(original.data(), 0, original.size());
			__asm__ __volatile__("" : : "r"(original.data()) : "memory");
		}
	};
	struct DirectMapping final {
		int fd;
		std::uint64_t offset;
		int protection;
	};
	struct DirectTransition final {
		std::uintptr_t slot{};
		PageAddressSet::node_type direct_slot;
		PageAddressMap<DirectMapping>::node_type direct_mapping;
		bool converted{};
	};
	struct DirectTransitionGuard final {
		DirectTransitionGuard(
			NativeAddressSpace &owner, std::size_t capacity);
		~DirectTransitionGuard();
		DirectTransitionGuard(const DirectTransitionGuard &) = delete;
		DirectTransitionGuard &operator=(
			const DirectTransitionGuard &) = delete;
		void commit() noexcept { committed = true; }

		NativeAddressSpace &owner;
		std::vector<DirectTransition> transitions;
		bool committed{};
	};

	static int identify_backing(int fd, BackingIdentity &identity) noexcept;
	int convert_direct_slot_to_shadow_locked(std::uintptr_t slot);
	int convert_shadow_slot_to_direct_locked(std::uintptr_t slot, int fd,
		std::uint64_t offset, int protection);
	int ensure_shadow_range_locked(
		std::uintptr_t address, std::uint64_t length,
		DirectTransitionGuard &guard);
	void restore_direct_transitions_locked(
		std::vector<DirectTransition> &transitions) noexcept;
	int ensure_backing_index_locked();
	void erase_backing_index_entry_locked(
		const BackingPageKey &key, std::uintptr_t page) noexcept;
	int flush_page_locked(std::uintptr_t page);
	int flush_slot_locked(std::uintptr_t slot);
	int reconcile_page_locked(std::uintptr_t page);
	int load_page_locked(std::uintptr_t page);
	int reload_page_from_backing_locked(std::uintptr_t page);
	int materialize_slot_locked(std::uintptr_t slot);
	int set_slot_protection_locked(std::uintptr_t slot, int protection);
	int update_slot_protection_locked(std::uintptr_t slot);
	[[nodiscard]] bool contains_shadow_locked(
		std::uintptr_t address, std::uint64_t length) const;
	/* Every index below takes its nodes from this heap, which retire() gives
	 * back whole. It is declared first so that it outlives them. */
	DomainHeap heap_;
	mutable std::mutex mutex_;
	PageAddressMap<Mapping> mappings_{DomainAllocator<char>{&heap_}};
	std::unordered_multimap<BackingPageKey, std::uintptr_t,
		BackingPageKeyHash, std::equal_to<BackingPageKey>,
		DomainAllocator<std::pair<const BackingPageKey, std::uintptr_t>>>
		backing_index_{DomainAllocator<char>{&heap_}};
	PageAddressMap<int> slot_protections_{DomainAllocator<char>{&heap_}};
	PageAddressSet materialized_slots_{DomainAllocator<char>{&heap_}};
	PageAddressMap<DirectMapping> direct_mappings_{
		DomainAllocator<char>{&heap_}};
	bool backing_index_ready_{};

	/* Every software mapping of a guest page publishes its backing first, and
	 * almost none of those pages has a shadow alias: two multi-core workloads
	 * published 1.8 million pages and 93 had one.  Asking cost an fstat and
	 * this object's mutex per page, from every vCPU at once.  The filter holds
	 * one hashed bit per backing page that some shadow maps; a clear bit lets
	 * publish_backing() return without either.  Bits are only set while
	 * mappings come and go, so a filter that has outgrown the live mappings is
	 * rebuilt from them, under an odd sequence that sends readers the slow
	 * way.  A reader that finds its bits clear and the sequence unchanged has
	 * published before any alias created later, which the mutex allowed too. */
	static constexpr std::size_t shadow_filter_bits = std::size_t{1} << 20;
	static constexpr std::uint64_t shadow_filter_scan_pages = 64;
	std::array<std::atomic<std::uint64_t>, shadow_filter_bits / 64>
		shadow_filter_{};
	std::atomic<std::uint64_t> shadow_filter_sequence_{};
	std::size_t shadow_filter_population_{};
	[[nodiscard]] static std::size_t shadow_filter_bit(
		std::uint64_t offset) noexcept;
	[[nodiscard]] bool may_alias_shadow(
		std::uint64_t offset, std::uint64_t length) const noexcept;
	void note_shadow_offset_locked(std::uint64_t offset) noexcept;
	void rebuild_shadow_filter_locked() noexcept;
	/* Every operation asks this under mutex_ and fails with ESHUTDOWN once
	 * retire() has run.  It is not noexcept: errno's accessor may throw as far
	 * as the compiler knows, and a noexcept wrapper would add a terminate
	 * handler to every caller. */
	int sync_direct_locked(std::uintptr_t address, std::uint64_t length);
	[[nodiscard]] bool refuse_retired_locked()
	{
		if (!retired_.load(std::memory_order_acquire))
			return false;
		errno = ESHUTDOWN;
		return true;
	}

	PageAddressSet direct_slots_{DomainAllocator<char>{&heap_}};
	std::uint64_t ownership_id_{};
	std::atomic_bool retired_{};
};

[[nodiscard]] std::shared_ptr<NativeAddressSpace>
native_address_space() noexcept;

} // namespace uml::memory
