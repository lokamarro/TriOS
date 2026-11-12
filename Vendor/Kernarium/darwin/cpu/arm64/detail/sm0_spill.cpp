// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/sm0_spill.hpp"

#include "cpu/arm64/detail/sm0_vector_length.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace uml::cpu::arm64::detail {

namespace {

using ::uml::memory::MemoryAccess;

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] Sm0Result fault_at(std::uint64_t address, bool load,
	bool alignment = false) noexcept
{
	return {.event = Sm0Event::memory_fault,
		.alignment = alignment,
		.access = load ? MemoryAccess::read : MemoryAccess::write,
		.fault_address = address};
}

/* The first byte of [address, address + size) that does not translate for
 * `access`, or nothing when every byte does. */
[[nodiscard]] bool reachable(Sm0Memory &memory, std::uint64_t address,
	unsigned size, MemoryAccess access, std::uint64_t &first) noexcept
{
	for (unsigned done = 0; done < size;) {
		const std::uint64_t current = address + done;
		if (memory.translate(current, access) == nullptr) {
			first = current;
			return false;
		}
		const std::uint64_t left =
			page_size - (current & (page_size - 1U));
		done += static_cast<unsigned>(
			std::min<std::uint64_t>(left, size - done));
	}
	return true;
}

/* Copies between translated guest bytes and `data`; a store reports each
 * host range it wrote. */
void move(Sm0Memory &memory, std::uint64_t address, std::byte *data,
	unsigned size, bool load) noexcept
{
	for (unsigned done = 0; done < size;) {
		const std::uint64_t current = address + done;
		std::byte *host = memory.translate(current,
			load ? MemoryAccess::read : MemoryAccess::write);
		const std::uint64_t left =
			page_size - (current & (page_size - 1U));
		const auto length = static_cast<unsigned>(
			std::min<std::uint64_t>(left, size - done));
		if (load) {
			std::memcpy(data + done, host, length);
		} else {
			std::memcpy(host, data + done, length);
			memory.stored(host, length);
		}
		done += length;
	}
}

} // namespace

Sm0Result execute_sm0_spill(CpuState &cpu, std::uint32_t word,
	Sm0Memory &memory) noexcept
{
	const auto spill = sm0_spill_access(word);
	if (!spill)
		return {.event = Sm0Event::fatal};
	const bool load = !spill->store;
	const bool sp_base = spill->base == 31U;
	if (sp_base && (cpu.sp & 15U) != 0U)
		return fault_at(cpu.sp, load, true);
	cpu.access_fp_state();
	const std::uint64_t base =
		sp_base ? cpu.sp : cpu.registers[spill->base];
	const std::uint64_t address = cpu.untag_data_address(
		base + static_cast<std::uint64_t>(
			static_cast<std::int64_t>(spill->offset)));
	const unsigned size = spill->bytes();
	std::array<std::byte, sm0_vector_bytes> image{};
	std::uint64_t unreachable = 0;
	if (load) {
		if (!reachable(memory, address, size, MemoryAccess::read,
			    unreachable))
			return fault_at(unreachable, true);
		move(memory, address, image.data(), size, true);
		if (!spill->predicate)
			std::memcpy(cpu.vectors[spill->transfer & 31U]
					    .bytes.data(),
				image.data(), size);
		return {};
	}
	if (!spill->predicate)
		std::memcpy(image.data(),
			cpu.vectors[spill->transfer & 31U].bytes.data(), size);
	if (!reachable(memory, address, size, MemoryAccess::write,
		    unreachable))
		return fault_at(unreachable, false);
	move(memory, address, image.data(), size, false);
	return {};
}

} // namespace uml::cpu::arm64::detail
