// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct BFloatMultiplyLong final {
	bool recognized{};
	bool by_element{};
	bool top{};
	unsigned index{};
};

[[nodiscard]] constexpr BFloatMultiplyLong decode_bfloat_multiply_long(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xbfe0'fc00U) == 0x2ec0'fc00U)
		return {true, false, (instruction & 0x4000'0000U) != 0U, 0U};
	if ((instruction & 0xbfc0'f400U) == 0x0fc0'f000U) {
		const unsigned index = ((instruction >> 20) & 3U) |
			((instruction >> 9) & 4U);
		return {true, true, (instruction & 0x4000'0000U) != 0U, index};
	}
	return {};
}

namespace bfloat_multiply_detail {

[[nodiscard]] inline std::uint32_t fmadd(std::uint32_t addend,
	std::uint32_t left, std::uint32_t right) noexcept
{
	const float addend_value = std::bit_cast<float>(addend);
	const float left_value = std::bit_cast<float>(left);
	const float right_value = std::bit_cast<float>(right);
	float result;
	__asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(result) :
		"w"(left_value), "w"(right_value), "w"(addend_value));
	return std::bit_cast<std::uint32_t>(result);
}

} // namespace bfloat_multiply_detail

/* BFMulAddH expands both BFloat16 multiplicands exactly to binary32, so it
 * is scalar FMADD under the guest FPCR, which the caller has installed. */
[[nodiscard]] inline std::uint32_t bfloat_multiply_add(
	std::uint32_t addend, std::uint16_t left, std::uint16_t right) noexcept
{
	return bfloat_multiply_detail::fmadd(addend,
		static_cast<std::uint32_t>(left) << 16,
		static_cast<std::uint32_t>(right) << 16);
}

} // namespace uml::cpu::arm64::detail
