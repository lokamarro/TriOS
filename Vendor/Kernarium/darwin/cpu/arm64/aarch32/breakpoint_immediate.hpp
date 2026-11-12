// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstdint>
#include <span>

namespace uml::cpu::arm64::aarch32 {

/* BKPT's immediate, from the instruction at `key`: imm12:imm4 for A32 and
 * imm8 for T32. An engine reads it back from the guest on the cold exit
 * rather than carrying it in the translation; 0 when the instruction cannot
 * be fetched. */
[[nodiscard]] inline std::uint32_t read_breakpoint_immediate(
	::uml::memory::StaticGuestMemory &memory, std::uint64_t key) noexcept
{
	if (key_is_thumb(key)) {
		std::uint16_t halfword = 0;
		if (!memory.fetch(key_pc(key),
				  std::as_writable_bytes(std::span{&halfword, 1})))
			return 0;
		return halfword & 0xffU;
	}
	std::uint32_t word = 0;
	if (!memory.fetch(key_pc(key),
			  std::as_writable_bytes(std::span{&word, 1})))
		return 0;
	return (word >> 4 & 0xfff0U) | (word & 0xfU);
}

} // namespace uml::cpu::arm64::aarch32
