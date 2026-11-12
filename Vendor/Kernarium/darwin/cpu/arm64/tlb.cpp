// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/tlb.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] constexpr std::size_t index_for(
	std::uint64_t address) noexcept
{
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	return static_cast<std::size_t>(page & (tlb_entry_count - 1));
}

static_assert(sizeof(TlbEntry) == 32);
static_assert(offsetof(TlbEntry, read_page) == 0);
static_assert(offsetof(TlbEntry, read_data_minus_address) == 8);
static_assert(offsetof(TlbEntry, write_page) == 16);
static_assert(offsetof(TlbEntry, write_data_minus_address) == 24);
static_assert(offsetof(Tlb, entries) == 0);
static_assert(offsetof(Tlb, generation) ==
	tlb_entry_count * sizeof(TlbEntry));
static_assert(offsetof(Tlb, changes) ==
	offsetof(Tlb, generation) + sizeof(std::uint64_t));
static_assert(offsetof(Tlb, fault_address) ==
	offsetof(Tlb, changes) + sizeof(std::uint64_t));
static_assert(offsetof(Tlb, fault_access) ==
	offsetof(Tlb, fault_address) + sizeof(std::uint64_t));

} // namespace

Tlb::Tlb()
{
	dirty_count = tlb_dirty_capacity + 1;
	flush();
}

void Tlb::flush()
{
	if (dirty_count > tlb_dirty_capacity) {
		std::ranges::fill(entries, TlbEntry{});
	} else {
		for (std::uint32_t index = 0; index < dirty_count; ++index)
			entries[dirty_entries[index]] = TlbEntry{};
	}
	dirty_count = 0;
}

void Tlb::invalidate(std::uint64_t address, std::uint64_t length)
{
	/* Keys may carry a logical tag in the top byte (translate_tagged), and
	 * the index never depends on that byte, so a masked compare finds every
	 * key of the page in its own entry. */
	constexpr std::uint64_t untagged =
		0x00ff'ffff'ffff'ffffULL >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto end = address + length;
	/* A range wider than the table is cheaper to find by its keys than by
	 * its pages: a large unmap would otherwise cost every vCPU a loop over
	 * pages the table cannot even hold. */
	if (length / page_size >= tlb_entry_count) {
		const auto first_key = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
		const auto last_key = (end - 1) >> DARWIN_UML_GUEST_PAGE_SHIFT;
		const auto in_range = [&](std::uint64_t key) {
			const auto page = key & untagged;
			return key != tlb_empty_key && page >= first_key &&
				page <= last_key;
		};
		for (auto &entry : entries) {
			if (in_range(entry.read_page))
				entry.read_page = tlb_empty_key;
			if (in_range(entry.write_page))
				entry.write_page = tlb_empty_key;
		}
		return;
	}
	for (auto page = address; page < end; page += page_size) {
		auto &entry = entries[index_for(page)];
		const auto key = page >> DARWIN_UML_GUEST_PAGE_SHIFT;
		if ((entry.read_page & untagged) == key)
			entry.read_page = tlb_empty_key;
		if ((entry.write_page & untagged) == key)
			entry.write_page = tlb_empty_key;
	}
}

std::byte *Tlb::translate_tagged(std::uint64_t address, Access access)
{
	constexpr std::uint64_t untagged_mask = 0x00ff'ffff'ffff'ffffULL;
	if (!tagged_addresses || (address & ~untagged_mask) == 0 ||
	    access == Access::execute)
		return translate(address, access);
	const auto untagged = address & untagged_mask;
	auto *host = translate(untagged, access);
	if (host == nullptr)
		return nullptr;
	/* Re-key the entry translate() just filled or hit so the tagged form
	 * the guest actually uses hits next time. */
	const auto page = untagged & ~(page_size - 1);
	const auto tagged_page = address & ~(page_size - 1);
	const auto key = page >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto tagged_key = tagged_page >> DARWIN_UML_GUEST_PAGE_SHIFT;
	auto &entry = entries[index_for(untagged)];
	if (access == Access::write && entry.write_page == key) {
		entry.write_page = tagged_key;
		entry.write_data_minus_address -= (tagged_page - page);
	} else if (access != Access::write && entry.read_page == key) {
		entry.read_page = tagged_key;
		entry.read_data_minus_address -= (tagged_page - page);
	}
	return host;
}

std::byte *Tlb::translate_exclusive_load(std::uint64_t address)
{
	/* The load itself decides: an unreadable page faults as a read. */
	auto *host = translate_tagged(address, Access::read);
	if (host == nullptr)
		return nullptr;
	/* Then the write entry, for the store-exclusive that completes the pair.
	 * A page that is not writable now fails the lookup, which must leave no
	 * trace of it: no fault is the load's to take. */
	const auto saved_address = fault_address;
	const auto saved_access = fault_access;
	if (auto *writable = translate_tagged(address, Access::write))
		return writable;
	fault_address = saved_address;
	fault_access = saved_access;
	return host;
}

void Tlb::prepare(std::uint64_t next_generation,
	std::uint64_t next_changes)
{
	if (generation != next_generation) {
		flush();
		generation = next_generation;
		changes = next_changes;
		return;
	}
	if (changes != next_changes)
		reconcile_changes(next_generation, next_changes);
}

void Tlb::discard(std::uint64_t address)
{
	invalidate(address, 1);
}

__attribute__((noinline)) void Tlb::reconcile_changes(
	std::uint64_t next_generation, std::uint64_t next_changes)
{
	std::array<darwin_uml_static_mapping_change, 256> changed_ranges{};
	std::uint64_t current_changes = next_changes;
	const int count = darwin_uml_static_vm_mapping_changes(
		next_generation, changes, changed_ranges.data(),
		changed_ranges.size(), &current_changes);
	if (count < 0) {
		flush();
	} else {
		for (int index = 0; index < count; ++index)
			invalidate(changed_ranges[index].address,
				changed_ranges[index].length);
	}
	generation = next_generation;
	changes = current_changes;
}

std::byte *Tlb::translate(std::uint64_t address, Access access)
{
	const auto page = address & ~(page_size - 1);
	if (access == Access::execute) {
		/* Translated engines normally fetch through StaticGuestMemory before
		 * entering generated code. Keep execute translation precise here as
		 * well without changing the assembly-visible data TLB layout. */
		void *translated = nullptr;
		const auto result = darwin_uml_static_vm_execution_context_translate(
			execution_context, static_cast<unsigned long>(generation),
			static_cast<unsigned long>(page), DARWIN_UML_STATIC_EXECUTE,
			&translated);
		if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
			fault_address = address;
			fault_access = Access::execute;
			return nullptr;
		}
		return static_cast<std::byte *>(translated) +
			(address & (page_size - 1));
	}
	const auto key = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto index = index_for(address);
	auto &entry = entries[index];
	const auto cached_page = access == Access::write ?
		entry.write_page : entry.read_page;
	if (cached_page == key) {
		const auto base = access == Access::write ?
			entry.write_data_minus_address :
			entry.read_data_minus_address;
		return reinterpret_cast<std::byte *>(base + address);
	}

	void *translated = nullptr;
	const auto host_access = access == Access::write ?
		DARWIN_UML_STATIC_WRITE : DARWIN_UML_STATIC_READ;
	const auto result = darwin_uml_static_vm_execution_context_translate(
		execution_context, static_cast<unsigned long>(generation),
		static_cast<unsigned long>(page), host_access, &translated);
	if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
		fault_address = address;
		fault_access = access;
		return nullptr;
	}
	/* A fill into a wholly empty entry records it for the next flush. An
	 * entry that invalidation emptied may be recorded twice, which only
	 * costs a redundant clear. */
	if (dirty_count <= tlb_dirty_capacity &&
	    entry.read_page == tlb_empty_key &&
	    entry.write_page == tlb_empty_key) {
		if (dirty_count < tlb_dirty_capacity)
			dirty_entries[dirty_count] =
				static_cast<std::uint16_t>(index);
		++dirty_count;
	}
	const auto host_page = reinterpret_cast<std::uintptr_t>(translated);
	if (access == Access::write) {
		entry.write_data_minus_address = host_page - page;
		entry.write_page = key;
	} else {
		entry.read_data_minus_address = host_page - page;
		entry.read_page = key;
	}
	return static_cast<std::byte *>(translated) +
		(address & (page_size - 1));
}

} // namespace uml::memory
