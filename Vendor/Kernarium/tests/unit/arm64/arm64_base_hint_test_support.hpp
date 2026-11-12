// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_hint_test {

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t primary_instruction(
	unsigned immediate)
{
	return 0xd503'201fU | ((immediate & 0x7fU) << 5);
}

[[nodiscard]] constexpr std::uint32_t gsb_instruction(
	unsigned operation, unsigned transfer)
{
	return 0xd508'c000U | ((operation & 7U) << 5) |
		(transfer & 31U);
}

/*
 * The virtual CPU implements neither FEAT_PAuth nor FEAT_PAuth_LR, so their
 * hint-space forms (XPACLRI, PAC*1716, AUT*1716, PAC*Z, PAC*SP, AUT*Z,
 * AUT*SP, and PACM) are NOPs.
 */
inline constexpr unsigned pointer_auth_hints[] = {
	7U, 8U, 10U, 12U, 14U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U, 39U,
};

[[nodiscard]] constexpr bool is_owned_primary_hint(unsigned immediate)
{
	return immediate < 128U &&
		immediate != 40U &&
		immediate != 48U &&
		immediate != 49U &&
		immediate != 52U;
}

} // namespace arm64_hint_test
