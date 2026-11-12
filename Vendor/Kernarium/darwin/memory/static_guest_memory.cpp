// SPDX-License-Identifier: GPL-2.0-or-later
#include "static_guest_memory.hpp"

#include "memory/mmu/execution_gate.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace uml::memory {

void StaticGuestMemory::set_generation(unsigned long generation) noexcept
{
	prepare(generation, darwin_uml_static_vm_changes(generation));
}

void StaticGuestMemory::prepare(
	unsigned long generation, std::uint64_t changes) noexcept
{
	generation_ = generation;
	changes_ = changes;
	fault_recorded_ = false;
}

bool StaticGuestMemory::read(
	std::uint64_t address, std::span<std::byte> destination)
{
	return copy_from_guest(address, destination, ::uml::memory::MemoryAccess::read);
}

bool StaticGuestMemory::fetch(
	std::uint64_t address, std::span<std::byte> destination)
{
	return copy_from_guest(address, destination, ::uml::memory::MemoryAccess::execute);
}

bool StaticGuestMemory::write(
	std::uint64_t address, std::span<const std::byte> source)
{
	/* Validate every page before changing physical memory.  This keeps a
	 * cross-page ARM store restartable when its second page faults.
	 */
	std::array<std::byte *, 2> destinations{};
	std::array<std::size_t, 2> lengths{};
	std::size_t count = 0;
	std::size_t completed = 0;
	while (completed < source.size()) {
		if (count == destinations.size())
			return record_fault(
				address + completed, ::uml::memory::MemoryAccess::write);
		const auto current = address + completed;
		const auto length = std::min<std::size_t>(
			source.size() - completed,
			page_size - (current & (page_size - 1)));
		auto *destination = translate(current, ::uml::memory::MemoryAccess::write);
		if (destination == nullptr)
			return false;
		destinations[count] = destination;
		lengths[count] = length;
		++count;
		completed += length;
	}
	completed = 0;
	for (std::size_t index = 0; index < count; ++index) {
		std::memcpy(
			destinations[index], source.data() + completed, lengths[index]);
		completed += lengths[index];
	}
	return true;
}

std::byte *StaticGuestMemory::atomic_pointer(
	std::uint64_t address, std::size_t size, ::uml::memory::MemoryAccess access)
{
	if (size == 0 || size > page_size - (address & (page_size - 1)))
		return nullptr;
	return translate(address, access);
}

bool StaticGuestMemory::is_executable(std::uint64_t address) noexcept
{
	/* This no-fault probe also primes the physical code-page handle used by
	 * executable-store publication and stale-block validation. */
	return code_page_handle(address) != nullptr;
}

bool StaticGuestMemory::code_page_stamp(std::uint64_t address,
	void *execution_context,
	::uml::memory::CodePageStamp &stamp) noexcept
{
	(void)execution_context;
	return code_page_handle(address, &stamp) != nullptr;
}

void StaticGuestMemory::invalidate_code(std::uint64_t address)
{
	darwin_uml_static_vm_invalidate_code(
		static_cast<unsigned long>(address));
}

void StaticGuestMemory::invalidate_code(
	std::uint64_t address, void *execution_context)
{
	if (execution_context != nullptr) {
		if (auto *code_page = code_page_handle(address);
		    code_page != nullptr)
			darwin_uml_static_vm_invalidate_code_page(code_page);
		if (execution_context_publish_code_change(
			    execution_context, address))
			return;
	}
	invalidate_code(address);
}

bool StaticGuestMemory::validate_access(
	std::uint64_t address, std::size_t size, ::uml::memory::MemoryAccess access)
{
	std::size_t completed = 0;
	while (completed < size) {
		const auto current = address + completed;
		const auto length = std::min<std::size_t>(
			size - completed, page_size - (current & (page_size - 1)));
		if (translate(current, access) == nullptr)
			return false;
		completed += length;
	}
	return true;
}

::uml::memory::MemoryFault StaticGuestMemory::last_fault() const noexcept
{
	return fault_;
}

bool StaticGuestMemory::has_fault() const noexcept
{
	return fault_recorded_;
}

constexpr std::size_t StaticGuestMemory::index_for(
	std::uint64_t address) noexcept
{
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	return static_cast<std::size_t>(
		(page ^ (page >> tlb_index_bits)) & (tlb_size - 1));
}

constexpr std::size_t StaticGuestMemory::access_index(
	::uml::memory::MemoryAccess access) noexcept
{
	return access == ::uml::memory::MemoryAccess::read ? 0 :
		access == ::uml::memory::MemoryAccess::write ? 1 : 2;
}

constexpr darwin_uml_static_access StaticGuestMemory::host_access(
	::uml::memory::MemoryAccess access) noexcept
{
	return access == ::uml::memory::MemoryAccess::read ? DARWIN_UML_STATIC_READ :
		access == ::uml::memory::MemoryAccess::write ? DARWIN_UML_STATIC_WRITE :
		DARWIN_UML_STATIC_EXECUTE;
}

std::byte *StaticGuestMemory::translate(
	std::uint64_t address, ::uml::memory::MemoryAccess access)
{
	const auto page = address & ~(page_size - 1);
	auto &entry = tlb_[index_for(address)];
	auto &base = entry.bases[access_index(access)];
	if (entry.page == page && entry.generation == generation_ &&
	    entry.changes == changes_ && base != nullptr)
		return base + (address & (page_size - 1));

	void *translated = nullptr;
	const int result = darwin_uml_static_vm_translate(
		generation_, static_cast<unsigned long>(page), host_access(access),
		&translated);
	if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
		record_fault(address, access);
		return nullptr;
	}
	if (entry.page != page || entry.generation != generation_ ||
	    entry.changes != changes_) {
		entry = {};
		entry.page = page;
		entry.generation = generation_;
		entry.changes = changes_;
	}
	base = static_cast<std::byte *>(translated);
	return base + (address & (page_size - 1));
}

void *StaticGuestMemory::code_page_handle(
	std::uint64_t address, ::uml::memory::CodePageStamp *stamp) noexcept
{
	const auto page = address & ~(page_size - 1);
	auto &entry = tlb_[index_for(address)];
	darwin_uml_static_code_page_stamp observed{};
	if (entry.page == page && entry.generation == generation_ &&
	    entry.changes == changes_ && entry.code_page != nullptr) {
		if (darwin_uml_static_vm_code_page_stamp(
			entry.code_page, &observed) != 0)
			return nullptr;
	} else {
		void *handle = nullptr;
		if (darwin_uml_static_vm_code_page(
			generation_, static_cast<unsigned long>(page), &handle,
			&observed) != DARWIN_UML_STATIC_TRANSLATED ||
		    handle == nullptr)
			return nullptr;
		if (entry.page != page || entry.generation != generation_ ||
		    entry.changes != changes_) {
			entry = {};
			entry.page = page;
			entry.generation = generation_;
			entry.changes = changes_;
		}
		entry.code_page = handle;
	}
	if (stamp != nullptr)
		*stamp = {observed.identity, observed.version};
	return entry.code_page;
}

bool StaticGuestMemory::copy_from_guest(
	std::uint64_t address, std::span<std::byte> destination,
	::uml::memory::MemoryAccess access)
{
	std::size_t completed = 0;
	while (completed < destination.size()) {
		const auto current = address + completed;
		const auto length = std::min<std::size_t>(
			destination.size() - completed,
			page_size - (current & (page_size - 1)));
		const auto *source = translate(current, access);
		if (source == nullptr)
			return false;
		std::memcpy(destination.data() + completed, source, length);
		completed += length;
	}
	return true;
}

bool StaticGuestMemory::record_fault(
	std::uint64_t address, ::uml::memory::MemoryAccess access)
{
	fault_ = {address, access};
	fault_recorded_ = true;
	return false;
}

} // namespace uml::memory
