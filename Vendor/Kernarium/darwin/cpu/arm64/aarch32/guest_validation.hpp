// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace uml::cpu::arm64::aarch32 {

/* What re-reading the guest bytes a cached translation was made from found:
 * every unit equal, the first unit that differs, or a unit that cannot be
 * read before any differs. */
enum class GuestValidation : std::uint8_t {
	match,
	mismatch,
	fault,
};

/* Compares `units` saved guest units against guest memory, reading them
 * where the translation's key says they lie. A unit is a word for A64 and A32
 * and a halfword for T32 (isa_unit_bytes), so a key-aware validation is the
 * same loop for all three: it fetches page-bounded runs and compares each run
 * once, and a run never splits a unit because units are naturally aligned.
 *
 * `scratch` holds the bytes of the whole span; `fetch(address, bytes)`
 * returns whether it filled `bytes` from guest memory. A failed run is a
 * fault, or, with `per_unit_fallback`, is retried one unit at a time so that
 * a difference reached before the unreadable unit is still reported as one.
 *
 * Both engines run this only on a stale or validating lookup, so it is not on
 * a path a block entry pays for. */
template <typename Fetch>
[[nodiscard]] GuestValidation validate_guest_units(std::uint64_t key,
	std::size_t units, const std::byte *saved, std::span<std::byte> scratch,
	std::uint64_t page_size, Fetch &&fetch, bool per_unit_fallback)
{
	const std::size_t unit = isa_unit_bytes(key_isa(key));
	assert(scratch.size() >= units * unit);
	const auto base = key_fetch_address(key);
	std::size_t index = 0;
	while (index < units) {
		const auto address = base + index * unit;
		const auto page_remaining =
			page_size - (address & (page_size - 1));
		auto run = std::min<std::size_t>(units - index,
			static_cast<std::size_t>(page_remaining / unit));
		/* A valid PC is aligned to its unit; keep a malformed one
		 * deterministic rather than constructing an empty span. */
		if (run == 0)
			run = 1;
		const auto bytes = scratch.subspan(index * unit, run * unit);
		if (fetch(address, bytes)) {
			if (std::memcmp(bytes.data(), saved + index * unit,
					run * unit) != 0)
				return GuestValidation::mismatch;
			index += run;
			continue;
		}
		if (!per_unit_fallback)
			return GuestValidation::fault;
		for (std::size_t step = 0; step < run; ++step) {
			std::array<std::byte, sizeof(std::uint32_t)> word{};
			const auto one = std::span{word}.first(unit);
			if (!fetch(address + step * unit, one))
				return GuestValidation::fault;
			if (std::memcmp(one.data(),
					saved + (index + step) * unit, unit) != 0)
				return GuestValidation::mismatch;
		}
		index += run;
	}
	return GuestValidation::match;
}

} // namespace uml::cpu::arm64::aarch32
