// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "memory/tlb.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

/*
 * FEAT_MOPS register formatting and transfer, shared by both engines so they
 * make the same implementation-defined choices. The virtual CPU implements
 * option B: the prologue formats the registers, the main stage transfers
 * nothing, and the epilogue transfers everything that remains. A CPY whose
 * source starts below an overlapping destination runs backward; every other
 * copy runs forward.
 *
 * The epilogue moves one region at a time, each contained in one source page
 * and one destination page, and commits the registers at every region
 * boundary, so a fault restarts the epilogue with exact progress. A region
 * moves in 64-byte blocks, each a host memmove, from its first byte in the
 * copy's direction. When its host source and destination do not overlap in a
 * way that lets one block read another's result, one memmove of the whole
 * region is equivalent and is used instead. The blocks are observable only
 * for a forward-only CPYF whose destination overlaps above its source, or
 * for aliased mappings of one page.
 */

namespace uml::cpu::arm64::detail {

enum class MopsFamily {
	copy_forward,
	copy_general,
	set,
};

enum class MopsOutcome {
	complete,
	fault,
	/* The epilogue found registers the prologue cannot have formatted. */
	illegal,
};

/* One word of the CPY/CPYF/SET allocation. SETG* is recognized but not valid:
 * it needs FEAT_MTE, which the virtual CPU does not implement. */
struct MopsInstruction final {
	bool recognized{};
	bool valid{};
	MopsFamily family{};
	/* 0 prologue, 1 main, 2 epilogue. */
	unsigned stage{};
	unsigned destination{};
	unsigned count{};
	unsigned source{};
};

[[nodiscard]] constexpr MopsInstruction decode_mops(
	std::uint32_t instruction) noexcept
{
	MopsInstruction decoded{};
	decoded.recognized = (instruction & 0x3b20'0c00U) == 0x1900'0400U;
	if (!decoded.recognized)
		return decoded;
	const unsigned op1 = (instruction >> 22) & 3U;
	const unsigned op2 = (instruction >> 12) & 15U;
	const bool set = op1 == 3U;
	const bool general = (instruction & 0x0400'0000U) != 0U;
	decoded.family = set ? MopsFamily::set :
		general ? MopsFamily::copy_general : MopsFamily::copy_forward;
	decoded.stage = set ? op2 >> 2 : op1;
	decoded.destination = instruction & 31U;
	decoded.count = (instruction >> 5) & 31U;
	decoded.source = (instruction >> 16) & 31U;
	const bool allocated = (instruction >> 30) == 0U && decoded.stage < 3U;
	const bool registers = decoded.destination != 31U &&
		decoded.count != 31U && decoded.destination != decoded.count &&
		decoded.source != decoded.destination &&
		decoded.source != decoded.count &&
		(set || decoded.source != 31U);
	decoded.valid = allocated && registers && !(set && general);
	return decoded;
}

/* The word of the same operation's given stage. */
[[nodiscard]] constexpr std::uint32_t mops_stage_word(
	std::uint32_t prologue, MopsFamily family, unsigned stage) noexcept
{
	return family == MopsFamily::set ? prologue | (stage << 14) :
		prologue | (stage << 22);
}

inline constexpr std::uint64_t mops_maximum_set = 0x7fff'ffff'ffff'ffffULL;
inline constexpr std::uint64_t mops_maximum_copy = 0x007f'ffff'ffff'ffffULL;

template <MopsFamily family>
void mops_prologue(CpuState &cpu, std::uint64_t &destination,
	std::uint64_t &count, std::uint64_t &source) noexcept
{
	auto remaining = count;
	if constexpr (family == MopsFamily::copy_general) {
		if (remaining > mops_maximum_copy)
			remaining = mops_maximum_copy;
		const auto from = source;
		const auto to = destination;
		const auto from_low = from & 0x00ff'ffff'ffff'ffffULL;
		const auto to_low = to & 0x00ff'ffff'ffff'ffffULL;
		const auto from_end =
			static_cast<unsigned __int128>(from_low) + remaining;
		const bool backward = from_low < to_low && from_end > to_low;
		if (backward) {
			source = from + remaining;
			destination = to + remaining;
			cpu.nzcv = 0xa000'0000U;
		} else {
			cpu.nzcv = 0x2000'0000U;
		}
	} else {
		if ((remaining >> 63) != 0)
			remaining = mops_maximum_set;
		cpu.nzcv = 0x2000'0000U;
	}
	count = remaining;
}

inline constexpr std::uint64_t mops_block_size = 64;

/* Translates a destination region for writing and runs transfer on its host
 * pointer. transfer returns the host range it stored. An engine that must
 * serialize or observe its stores supplies its own policy. */
struct MopsDirectStore final {
	template <typename Transfer>
	[[nodiscard]] bool operator()(::uml::memory::Tlb &tlb,
		std::uint64_t address, Transfer &&transfer) const noexcept
	{
		auto *pointer = tlb.translate(address, ::uml::memory::Access::write);
		if (pointer == nullptr)
			return false;
		(void)transfer(pointer);
		return true;
	}
};

struct MopsStoredRange final {
	std::byte *pointer;
	std::size_t size;
};

/* source is the SET fill register or the CPY source register. */
template <MopsFamily family, typename Store = MopsDirectStore>
[[nodiscard]] MopsOutcome mops_epilogue(CpuState &cpu,
	::uml::memory::Tlb &tlb, std::uint64_t &destination,
	std::uint64_t &count, std::uint64_t &source, Store &&store = {}) noexcept
{
	using ::uml::memory::Access;
	constexpr bool set = family == MopsFamily::set;
	constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
	auto remaining = count;
	const auto limit = family == MopsFamily::copy_general ?
		mops_maximum_copy : mops_maximum_set;
	const bool backward = family == MopsFamily::copy_general &&
		(cpu.nzcv & 0x8000'0000U) != 0;
	if (remaining > limit ||
	    (remaining != 0 && (cpu.nzcv & 0x2000'0000U) == 0))
		return MopsOutcome::illegal;

	auto to = destination;
	auto from = set ? std::uint64_t{} : source;
	const int fill = set ? static_cast<int>(source & 0xffU) : 0;
	const auto commit = [&] {
		count = remaining;
		destination = to;
		if constexpr (!set)
			source = from;
	};
	while (remaining != 0) {
		std::uint64_t amount;
		if (backward) {
			const auto to_available = to & (page - 1);
			amount = to_available == 0 ? page : to_available;
			const auto from_available = from & (page - 1);
			amount = std::min(amount,
				from_available == 0 ? page : from_available);
		} else {
			amount = page - (to & (page - 1));
			if constexpr (!set)
				amount = std::min(amount, page - (from & (page - 1)));
		}
		amount = std::min(amount, remaining);
		const auto to_address = cpu.untag_data_address(
			backward ? to - amount : to);

		const std::byte *read_pointer{};
		if constexpr (!set) {
			const auto from_address = cpu.untag_data_address(
				backward ? from - amount : from);
			read_pointer = tlb.translate(from_address, Access::read);
			if (read_pointer == nullptr) {
				/* Report the first byte the copy would read. */
				if (backward) {
					tlb.fault_address =
						cpu.untag_data_address(from - 1);
					tlb.fault_access = Access::read;
				}
				commit();
				return MopsOutcome::fault;
			}
		}
		auto moved = amount;
		const bool stored = store(tlb, to_address,
			[&](std::byte *write_pointer) noexcept -> MopsStoredRange {
				const auto size = static_cast<std::size_t>(amount);
				if constexpr (set) {
					std::memset(write_pointer, fill, size);
					return {write_pointer, size};
				} else {
					const auto write = reinterpret_cast<std::uintptr_t>(
						write_pointer);
					const auto read = reinterpret_cast<std::uintptr_t>(
						read_pointer);
					/* A later block would read an earlier block's
					 * result: move the first block only. */
					const bool blocks = backward ?
						write < read && read < write + size :
						read < write && write < read + size;
					if (!blocks) {
						std::memmove(write_pointer, read_pointer, size);
						return {write_pointer, size};
					}
					moved = std::min(amount, mops_block_size);
					const auto offset = backward ?
						static_cast<std::size_t>(amount - moved) : 0U;
					std::memmove(write_pointer + offset,
						read_pointer + offset,
						static_cast<std::size_t>(moved));
					return {write_pointer + offset,
						static_cast<std::size_t>(moved)};
				}
			});
		if (!stored) {
			if (backward) {
				tlb.fault_address =
					cpu.untag_data_address(to - 1);
				tlb.fault_access = Access::write;
			}
			commit();
			return MopsOutcome::fault;
		}
		if (backward) {
			to -= moved;
			from -= moved;
		} else {
			to += moved;
			if constexpr (!set)
				from += moved;
		}
		remaining -= moved;
	}
	commit();
	return MopsOutcome::complete;
}

} // namespace uml::cpu::arm64::detail
