// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory_transfer.hpp"

#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" std::uint32_t uml_arm64_memory_transfer(
	::uml::memory::Tlb *tlb, std::uint64_t address,
	void *data, std::size_t size, std::uint32_t access) noexcept
{
	using ::uml::memory::Access;
	constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	if (tlb == nullptr || data == nullptr || size == 0 || size > 64)
		return 0;

	const auto direction = access == static_cast<std::uint32_t>(Access::write) ?
		Access::write : Access::read;
	std::array<std::byte *, 2> pages{};
	std::array<std::size_t, 2> lengths{};
	std::size_t count = 0;
	std::size_t completed = 0;
	while (completed < size) {
		if (count == pages.size())
			return 0;
		const auto current = address + completed;
		auto *translated = tlb->translate_tagged(current, direction);
		if (translated == nullptr)
			return 0;
		const auto length = std::min(size - completed,
			page_size - static_cast<std::size_t>(current % page_size));
		pages[count] = translated;
		lengths[count] = length;
		++count;
		completed += length;
	}

	completed = 0;
	auto *bytes = static_cast<std::byte *>(data);
	for (std::size_t index = 0; index < count; ++index) {
		if (direction == Access::write)
			std::memcpy(pages[index], bytes + completed, lengths[index]);
		else
			std::memcpy(bytes + completed, pages[index], lengths[index]);
		completed += lengths[index];
	}
	return 1;
}

extern "C" std::uint32_t uml_arm64_simd_structure_memory_transfer(
	uml::cpu::arm64::CpuState *cpu,
	::uml::memory::Tlb *tlb, std::uint64_t tagged_address,
	void *data, std::size_t size, std::uint32_t access) noexcept
{
	using ::uml::memory::Access;
	if (cpu == nullptr || tlb == nullptr)
		return 0;
	const auto direction =
		access == static_cast<std::uint32_t>(Access::write) ?
			Access::write : Access::read;
	const auto address = cpu->untag_data_address(tagged_address);
	const auto transferred = uml_arm64_memory_transfer(
		tlb, address, data, size, access);
	if (transferred != 0U && direction == Access::write)
		cpu->exclusive_size = 0;
	return transferred;
}

extern "C" void uml_arm64_alignment_fault(
	::uml::memory::Tlb *tlb, std::uint64_t address,
	std::uint32_t access) noexcept
{
	if (tlb == nullptr)
		return;
	tlb->fault_alignment = 1;
	tlb->fault_address = address;
	tlb->fault_access = access == static_cast<std::uint32_t>(
		::uml::memory::Access::write) ?
		::uml::memory::Access::write : ::uml::memory::Access::read;
}

static_assert(uml_arm64_tlb_miss_exclusive_load >
	static_cast<std::uint32_t>(::uml::memory::Access::execute));

extern "C" void *uml_arm64_tlb_miss(
	::uml::memory::Tlb *tlb, std::uint64_t address,
	std::uint32_t access)
{
	if (tlb == nullptr)
		return nullptr;
	if (access == uml_arm64_tlb_miss_exclusive_load)
		return tlb->translate_exclusive_load(address);
	return tlb->translate_tagged(address,
		access == static_cast<std::uint32_t>(
			::uml::memory::Access::write) ?
			::uml::memory::Access::write : ::uml::memory::Access::read);
}
