/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The guest memory the x86-64 unit tests share when they need nothing but one
 * flat mapping from address zero: a vector that answers every access inside
 * it and records a fault for any outside. */
#pragma once

#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

namespace uml::test::x86_64_flat_memory {

class FlatMemory final : public uml::memory::GuestAddressSpace {
public:
	explicit FlatMemory(std::size_t size = 16384) : bytes_(size) {}

	void load(std::uint64_t address, std::initializer_list<std::uint8_t> bytes)
	{
		for (const auto byte : bytes)
			bytes_[address++] = static_cast<std::byte>(byte);
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!inside(address, destination.size()))
			return fault(address, uml::memory::MemoryAccess::read);
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!inside(address, destination.size()))
			return fault(address, uml::memory::MemoryAccess::execute);
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (!inside(address, source.size()))
			return fault(address, uml::memory::MemoryAccess::write);
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		if (!inside(address, size)) {
			(void)fault(address, access);
			return nullptr;
		}
		return bytes_.data() + address;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

#if UML_X86_64_SSE_LOOP_JIT
	/* The JIT fetches only from executable pages. */
	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return address < bytes_.size();
	}
#endif

	[[nodiscard]] std::byte *at(std::uint64_t address) noexcept
	{
		return bytes_.data() + address;
	}

private:
	[[nodiscard]] bool inside(std::uint64_t address,
		std::size_t size) const noexcept
	{
		return address <= bytes_.size() && size <= bytes_.size() - address;
	}

	[[nodiscard]] bool fault(std::uint64_t address,
		uml::memory::MemoryAccess access) noexcept
	{
		fault_ = {address, access};
		faulted_ = true;
		return false;
	}

	std::vector<std::byte> bytes_;
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

} // namespace uml::test::x86_64_flat_memory
