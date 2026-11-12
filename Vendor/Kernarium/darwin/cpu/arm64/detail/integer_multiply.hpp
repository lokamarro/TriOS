// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../cpu_state.hpp"
#include "floating_bits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::detail {

namespace integer_multiply_detail {

using Wide = __int128;
using UnsignedWide = unsigned __int128;

using floating_bits::low_mask;

[[nodiscard]] constexpr Wide signed_value(
	std::uint64_t raw, unsigned bits) noexcept
{
	const auto sign = std::uint64_t{1} << (bits - 1U);
	raw &= low_mask(bits);
	return (raw & sign) != 0 ?
		static_cast<Wide>(raw) - (Wide{1} << bits) :
		static_cast<Wide>(raw);
}

[[nodiscard]] constexpr Wide arithmetic_shift_right(
	Wide value, unsigned shift) noexcept
{
	if (value >= 0)
		return value >> shift;
	const Wide divisor = Wide{1} << shift;
	return -((-value + divisor - 1) / divisor);
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes) noexcept
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) <<
			(byte * 8U);
	return value;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes, std::uint64_t value) noexcept
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[offset + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] inline std::uint64_t saturate_signed(
	Wide value, unsigned bits, bool &saturated) noexcept
{
	const Wide minimum = -(Wide{1} << (bits - 1U));
	const Wide maximum = (Wide{1} << (bits - 1U)) - 1;
	if (value < minimum) {
		value = minimum;
		saturated = true;
	} else if (value > maximum) {
		value = maximum;
		saturated = true;
	}
	return static_cast<std::uint64_t>(
		static_cast<UnsignedWide>(value)) & low_mask(bits);
}

struct Decoded final {
	bool valid{};
	bool indexed{};
	bool scalar{};
	bool subtract{};
	unsigned size{};
	unsigned destination{};
	unsigned left{};
	unsigned right{};
	unsigned index{};
};

[[nodiscard]] constexpr Decoded decode(std::uint32_t instruction) noexcept
{
	const bool indexed_vector =
		(instruction & 0x9f00'0400U) == 0x0f00'0000U;
	const bool indexed_scalar =
		(instruction & 0xdf00'0400U) == 0x5f00'0000U;
	const bool indexed = indexed_vector || indexed_scalar;
	if (indexed) {
		const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
		const unsigned operation = (instruction >> 12) & 15U;
		if (!unsigned_group || (operation != 13U && operation != 15U))
			return {};
		const unsigned size = (instruction >> 22) & 3U;
		if (size != 1U && size != 2U)
			return Decoded{.indexed = true};
		unsigned right = (instruction >> 16) & 15U;
		unsigned index;
		if (size == 1U) {
			index = ((instruction >> 9) & 4U) |
				((instruction >> 20) & 1U) |
				((instruction >> 20) & 2U);
		} else {
			right |= ((instruction >> 20) & 1U) << 4;
			index = ((instruction >> 10) & 2U) |
				((instruction >> 21) & 1U);
		}
		return {
			.valid = true,
			.indexed = true,
			.scalar = indexed_scalar,
			.subtract = operation == 15U,
			.size = size,
			.destination = instruction & 31U,
			.left = (instruction >> 5) & 31U,
			.right = right,
			.index = index,
		};
	}

	const bool vector =
		(instruction & 0xbf20'f400U) == 0x2e00'8400U;
	const bool scalar =
		(instruction & 0xff20'f400U) == 0x7e00'8400U;
	if (!vector && !scalar)
		return {};
	const unsigned size = (instruction >> 22) & 3U;
	return {
		.valid = size == 1U || size == 2U,
		.indexed = false,
		.scalar = scalar,
		.subtract = (instruction & 0x800U) != 0,
		.size = size,
		.destination = instruction & 31U,
		.left = (instruction >> 5) & 31U,
		.right = (instruction >> 16) & 31U,
	};
}

} // namespace integer_multiply_detail

/*
 * Implements the exact FEAT_RDM accumulator expression for both the
 * three-register and by-element encodings. The caller performs decode-event
 * handling and PC advancement; this helper only mutates architectural data.
 */
[[nodiscard]] inline bool execute_saturating_rounding_multiply_accumulate(
	CpuState &cpu, std::uint32_t instruction) noexcept
{
	using namespace integer_multiply_detail;
	const auto decoded = decode(instruction);
	if (!decoded.valid)
		return false;

	const unsigned lane_bytes = 1U << decoded.size;
	const unsigned lane_bits = lane_bytes * 8U;
	const unsigned vector_bytes = decoded.scalar ? lane_bytes :
		(instruction & 0x4000'0000U) != 0 ? 16U : 8U;
	const auto left = cpu.vectors[decoded.left].bytes;
	const auto right = cpu.vectors[decoded.right].bytes;
	const auto accumulator = cpu.vectors[decoded.destination].bytes;
	std::array<std::byte, 16> result{};
	const auto indexed_right = decoded.indexed ?
		signed_value(read_lane(right, decoded.index * lane_bytes,
			lane_bytes), lane_bits) : Wide{};
	bool saturated = false;

	for (unsigned offset = 0; offset < vector_bytes;
	     offset += lane_bytes) {
		const Wide left_value = signed_value(
			read_lane(left, offset, lane_bytes), lane_bits);
		const Wide right_value = decoded.indexed ? indexed_right :
			signed_value(read_lane(right, offset, lane_bytes), lane_bits);
		const Wide accumulator_value = signed_value(
			read_lane(accumulator, offset, lane_bytes), lane_bits);
		const Wide product = left_value * right_value * 2;
		Wide combined =
			accumulator_value * (Wide{1} << lane_bits);
		combined += decoded.subtract ? -product : product;
		combined += Wide{1} << (lane_bits - 1U);
		const auto value = saturate_signed(
			arithmetic_shift_right(combined, lane_bits),
			lane_bits, saturated);
		write_lane(result, offset, lane_bytes, value);
	}

	cpu.vectors[decoded.destination].bytes = result;
	if (saturated)
		cpu.fpsr |= 1U << 27;
	return true;
}

} // namespace uml::cpu::arm64::detail
