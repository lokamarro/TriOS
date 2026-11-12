// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "bfloat_dot.hpp"
#include "instruction_primitives.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::detail {

[[nodiscard]] constexpr bool decode_bfloat_matrix_multiply(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xffe0'fc00U) == 0x6e40'ec00U;
}

[[nodiscard]] inline std::array<std::byte, 16> bfloat_matrix_multiply_add(
	const std::array<std::byte, 16> &addends,
	const std::array<std::byte, 16> &left,
	const std::array<std::byte, 16> &right) noexcept
{
	const auto read_bfloat = [](const auto &source, unsigned element) {
		return static_cast<std::uint16_t>(decode_little_endian(
			std::span{source}.subspan(element * 2U, 2U)));
	};
	std::array<std::byte, 16> result{};
	for (unsigned row = 0; row < 2U; ++row) {
		for (unsigned column = 0; column < 2U; ++column) {
			const unsigned lane = row * 2U + column;
			auto sum = static_cast<std::uint32_t>(decode_little_endian(
				std::span{addends}.subspan(lane * 4U, 4U)));
			for (unsigned pair = 0; pair < 2U; ++pair) {
				const unsigned left_element = row * 4U + pair * 2U;
				const unsigned right_element = column * 4U + pair * 2U;
				sum = bfloat_dot_add(sum,
					read_bfloat(left, left_element),
					read_bfloat(left, left_element + 1U),
					read_bfloat(right, right_element),
					read_bfloat(right, right_element + 1U));
			}
			encode_little_endian(sum,
				std::span{result}.subspan(lane * 4U, 4U));
		}
	}
	return result;
}

} // namespace uml::cpu::arm64::detail
