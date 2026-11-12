// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail::floating_bits {

/* The field widths of an IEEE binary16, binary32 or binary64 encoding,
 * chosen at run time by total width, for code that handles all three
 * formats in one path. */
struct Format final {
	unsigned width;
	unsigned exponent_bits;
	unsigned fraction_bits;
	int bias;
};

[[nodiscard]] constexpr Format format(unsigned width) noexcept
{
	return width == 16U ? Format{16U, 5U, 10U, 15} :
		width == 32U ? Format{32U, 8U, 23U, 127} :
			Format{64U, 11U, 52U, 1023};
}

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned bits) noexcept
{
	return bits == 64U ? ~std::uint64_t{0} :
		(std::uint64_t{1} << bits) - 1U;
}

} // namespace uml::cpu::arm64::detail::floating_bits
