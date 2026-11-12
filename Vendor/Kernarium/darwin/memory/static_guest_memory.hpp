// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::memory {

class StaticGuestMemory final : public ::uml::memory::GuestAddressSpace {
public:
	void set_generation(unsigned long generation) noexcept;
	void prepare(unsigned long generation, std::uint64_t changes) noexcept;

	[[nodiscard]] bool read(
		std::uint64_t address, std::span<std::byte> destination) override;
	[[nodiscard]] bool fetch(
		std::uint64_t address, std::span<std::byte> destination) override;
	[[nodiscard]] bool write(
		std::uint64_t address, std::span<const std::byte> source) override;
	[[nodiscard]] bool validate_access(
		std::uint64_t address, std::size_t size,
		::uml::memory::MemoryAccess access) override;
	[[nodiscard]] std::byte *atomic_pointer(
		std::uint64_t address, std::size_t size,
		::uml::memory::MemoryAccess access) override;
	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override;
	[[nodiscard]] bool code_page_stamp(std::uint64_t address,
		void *execution_context,
		::uml::memory::CodePageStamp &stamp) noexcept override;
	void invalidate_code(std::uint64_t address) override;
	void invalidate_code(
		std::uint64_t address, void *execution_context) override;
	[[nodiscard]] ::uml::memory::MemoryFault last_fault() const noexcept override;
	[[nodiscard]] bool has_fault() const noexcept override;

private:
	static constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	static constexpr std::size_t tlb_index_bits = 13U;
	static constexpr std::size_t tlb_size = 1U << tlb_index_bits;

	struct TlbEntry final {
		std::uint64_t page{1};
		std::uint64_t generation{};
		std::uint64_t changes{};
		std::array<std::byte *, 3> bases{};
		void *code_page{};
	};

	[[nodiscard]] static constexpr std::size_t index_for(
		std::uint64_t address) noexcept;
	[[nodiscard]] static constexpr std::size_t access_index(
		::uml::memory::MemoryAccess access) noexcept;
	[[nodiscard]] static constexpr darwin_uml_static_access host_access(
		::uml::memory::MemoryAccess access) noexcept;
	[[nodiscard]] std::byte *translate(
		std::uint64_t address, ::uml::memory::MemoryAccess access);
	[[nodiscard]] void *code_page_handle(
		std::uint64_t address,
		::uml::memory::CodePageStamp *stamp = nullptr) noexcept;
	[[nodiscard]] bool copy_from_guest(
		std::uint64_t address, std::span<std::byte> destination,
		::uml::memory::MemoryAccess access);
	bool record_fault(std::uint64_t address, ::uml::memory::MemoryAccess access);

	unsigned long generation_{};
	std::uint64_t changes_{};
	std::array<TlbEntry, tlb_size> tlb_{};
	::uml::memory::MemoryFault fault_{};
	bool fault_recorded_{};
};

} // namespace uml::memory
