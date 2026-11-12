// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace uml::memory {

/* std::hash of an integer is the identity, and libc++ keeps a power-of-two
 * bucket count whenever one is requested through reserve(), constraining a
 * hash by masking rather than by a modulus.  A page address carries
 * log2(page_size) low zero bits, so an identity hash under a power-of-two mask
 * reaches only bucket_count / page_size distinct buckets: at 4 KiB pages a
 * 65,536-bucket table uses sixteen of them, every lookup walks a chain
 * thousands of nodes long, and filling the table becomes quadratic.  Fold the
 * page number so the distribution no longer depends on the bucket count the
 * container happens to choose.
 */
[[nodiscard]] constexpr std::uint64_t fold_page_key(
	std::uint64_t value) noexcept
{
	value ^= value >> 33;
	value *= 0xff51'afd7'ed55'8ccdULL;
	value ^= value >> 29;
	return value;
}

struct PageAddressHash final {
	[[nodiscard]] std::size_t operator()(
		const std::uint64_t address) const noexcept
	{
		return static_cast<std::size_t>(
			fold_page_key(address / DARWIN_UML_GUEST_PAGE_SIZE));
	}
};

/* Make room for additional elements so that the insertions which follow
 * cannot rehash.  reserve() is not that operation: it rehashes whenever the
 * requested count selects another bucket count, a smaller one included, so
 * asking an index of half a million pages for size() + 1 walks every node on
 * most single-page mappings.  Rehash only when the present buckets cannot hold
 * the request, and then grow geometrically.
 */
template <typename Index>
void reserve_additional(Index &index, const std::size_t additional)
{
	const auto required = index.size() + additional;
	if (static_cast<float>(required) <=
	    static_cast<float>(index.bucket_count()) * index.max_load_factor())
		return;
	index.reserve(std::max(required, 2 * index.size()));
}

} // namespace uml::memory
