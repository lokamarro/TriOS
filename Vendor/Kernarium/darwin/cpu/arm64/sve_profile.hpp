// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>

namespace uml::cpu::arm64 {

/* The predicate arithmetic streaming mode and the SM = 0 vector-length
 * family share. The virtual CPU implements no SVE outside streaming mode and
 * advertises none (platform::without_sve), so it keeps no non-streaming
 * predicate state; CpuState reserves the storage the removed profile held. */

/* DecodePredCount(): how many of a vector's `elements` elements a
 * predicate constraint names (POW2, VL1-VL256, MUL4, MUL3, ALL); the
 * reserved patterns name none. It serves the streaming forms at SVL too. */
[[nodiscard]] constexpr unsigned predicate_pattern_count(unsigned pattern,
	unsigned elements) noexcept
{
	switch (pattern) {
	case 0: /* POW2 */
		return std::bit_floor(elements);
	case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
		return elements >= pattern ? pattern : 0U;
	case 9: case 10: case 11: case 12: case 13: {
		const unsigned count = 16U << (pattern - 9U);
		return elements >= count ? count : 0U;
	}
	case 29: /* MUL4 */
		return elements - elements % 4U;
	case 30: /* MUL3 */
		return elements - elements % 3U;
	case 31: /* ALL */
		return elements;
	default:
		return 0;
	}
}

static_assert(predicate_pattern_count(0, 16) == 16 &&
	predicate_pattern_count(30, 8) == 6 &&
	predicate_pattern_count(9, 8) == 0 &&
	predicate_pattern_count(14, 16) == 0);

} // namespace uml::cpu::arm64
