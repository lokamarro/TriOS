// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

/* darwin/cpu/CMakeLists.txt defines both for every includer. A default here
 * would let a translation unit see a geometry other engines do not. */
#ifndef UML_GADGET_CACHE_SLOTS
#error "UML_GADGET_CACHE_SLOTS must come from the build"
#endif
#ifndef UML_GADGET_RESIDENT_GENERATIONS
#error "UML_GADGET_RESIDENT_GENERATIONS must come from the build"
#endif

namespace uml::cpu::gadget_cache {

/* Both ISAs keep executor-owned links in identically shaped private caches.
 * The translated payload and its validation remain owned by the ISA.
 *
 * Hashed blocks load sets unevenly, so a four-way set evicts code it will run
 * again while other sets still have room. Eight ways keep that code at the
 * cost of a longer probe; at 131,072 slots multi-core Geekbench stored a
 * quarter fewer translations and ran 2% faster (G231). */
inline constexpr std::size_t slots = UML_GADGET_CACHE_SLOTS;
inline constexpr std::size_t ways = 8;
inline constexpr std::size_t slab_slots = 256;
inline constexpr std::size_t resident_generations = UML_GADGET_RESIDENT_GENERATIONS;
static_assert((slots & (slots - 1U)) == 0);
static_assert(slots >= slab_slots * 4U && slots % slab_slots == 0);
static_assert(slots % ways == 0);
static_assert(resident_generations != 0);

/* An address space sweeps its link records and indirect sites once they
 * number half again what its last sweep kept, and at least `sweep_floor`,
 * and once at least a quarter as many of its bodies have left the cache as it
 * has made records since. A sweep reads every stream the address space holds;
 * short-lived address spaces never make that many records, and only a body
 * that leaves can leave a record that nothing names, so an address space
 * that is only growing would sweep for nothing. */
inline constexpr std::size_t sweep_floor = 4096;

[[nodiscard]] constexpr bool sweep_due(std::size_t records,
	std::size_t kept, std::size_t departed) noexcept
{
	const auto due = kept + kept / 2 > sweep_floor ? kept + kept / 2 :
		sweep_floor;
	return records >= due && departed * 4 >= records - kept;
}

} // namespace uml::cpu::gadget_cache
