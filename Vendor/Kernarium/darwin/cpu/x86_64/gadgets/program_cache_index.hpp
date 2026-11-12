// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/cpu_state.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64::gadgets {

/* The set of the main gadget cache that holds a translation, the only
 * definition of it: one multiply of the address space's generation, the
 * byte-granular instruction pointer and the execution mode, whose top bits
 * pick one of `sets` (a power of two greater than one).
 *
 * Guest code clusters at alignments and strides, and two xor folds of the
 * address are linear: keys that differ by a multiple of their kernel share a
 * set whatever else they hold, so such code filled some sets while others
 * stayed empty. A multiply carries every key bit into the bits the set is
 * taken from. */
[[nodiscard]] constexpr std::size_t program_cache_set(
	std::uint64_t generation, std::uint64_t address, ExecutionMode mode,
	std::size_t sets) noexcept
{
	const auto key = address ^ (generation * 0x9e37'79b9'7f4a'7c15ULL) ^
		(static_cast<std::uint64_t>(mode) << 32U);
	return static_cast<std::size_t>((key * 0xbf58'476d'1ce4'e5b9ULL) >>
		(64U - static_cast<unsigned>(std::countr_zero(sets))));
}

} // namespace uml::cpu::x86_64::gadgets
