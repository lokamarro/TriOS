// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::aarch32 {

/* Fetches the guest units that start at `address` into `bytes`, which holds
 * `bytes.size() / unit` of them, one page-bounded run at a time through
 * `fetch(address, bytes)`. A unit is a word for A64 and A32, a halfword for
 * T32 (isa_unit_bytes) and a byte for a caller that wants a byte prefix.
 *
 * A run that cannot be fetched ends the window: the result is the number of
 * whole units in the runs before it, zero when not even the first can be
 * read. Both engines fetch a translation window through this loop. */
template <typename Fetch>
[[nodiscard]] std::size_t fetch_guest_units(std::uint64_t address,
	std::size_t unit, std::span<std::byte> bytes, std::uint64_t page_size,
	Fetch &&fetch)
{
	const std::size_t capacity = bytes.size() / unit;
	std::size_t count = 0;
	while (count < capacity) {
		const auto at = address + count * unit;
		const auto page_remaining = page_size - (at & (page_size - 1));
		auto run = std::min<std::size_t>(capacity - count,
			static_cast<std::size_t>(page_remaining / unit));
		/* A valid PC is aligned to its unit. Preserve deterministic
		 * fault handling for a malformed one (an odd Thumb address at
		 * the last byte of a page) instead of constructing an empty
		 * span. */
		if (run == 0)
			run = 1;
		if (!fetch(at, bytes.subspan(count * unit, run * unit)))
			break;
		count += run;
	}
	return count;
}

} // namespace uml::cpu::arm64::aarch32
