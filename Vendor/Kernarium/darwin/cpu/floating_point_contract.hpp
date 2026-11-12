// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu {

inline constexpr std::uint32_t floating_point_status_exception_mask =
	0x0000'009fU;
inline constexpr std::uint32_t floating_point_syndrome_base =
	(0x2cU << 26U) | (1U << 25U) | (1U << 23U);

[[nodiscard]] constexpr std::uint32_t floating_point_syndrome(
	std::uint32_t raised_enabled) noexcept
{
	return floating_point_syndrome_base |
		(raised_enabled & floating_point_status_exception_mask);
}

} // namespace uml::cpu
