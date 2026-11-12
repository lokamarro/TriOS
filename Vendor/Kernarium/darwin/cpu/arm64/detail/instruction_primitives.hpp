// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

/* Engine-neutral instruction field, immediate, and lane helpers shared by the
 * gadget engine, the JIT, and their tests. */

namespace uml::cpu::arm64::detail {

constexpr std::uint64_t width_mask(bool is_64_bit)
{
	return is_64_bit ? std::numeric_limits<std::uint64_t>::max() :
		0xffff'ffffULL;
}

constexpr std::int64_t sign_extend(std::uint64_t value, unsigned bits)
{
	const auto sign = std::uint64_t{1} << (bits - 1);
	return static_cast<std::int64_t>((value ^ sign) - sign);
}

inline std::uint64_t decode_little_endian(std::span<const std::byte> bytes)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < bytes.size(); ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned char>(bytes[index])) << (index * 8);
	return value;
}

inline void encode_little_endian(std::uint64_t value,
				 std::span<std::byte> bytes)
{
	for (std::size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(value >> (index * 8));
}

constexpr std::uint64_t rotate_right_width(std::uint64_t value,
					    unsigned rotate,
					    unsigned width)
{
	const auto mask = width == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << width) - 1;
	rotate %= width;
	value &= mask;
	if (rotate == 0)
		return value;
	return ((value >> rotate) | (value << (width - rotate))) & mask;
}

struct BitMasks final {
	std::uint64_t write;
	std::uint64_t test;
};

constexpr std::optional<BitMasks> decode_bit_masks(
	bool is_64_bit, unsigned n, unsigned immr, unsigned imms,
	bool immediate)
{
	if (!is_64_bit && n != 0)
		return std::nullopt;
	const unsigned pattern = (n << 6) | ((~imms) & 0x3fU);
	if (pattern == 0)
		return std::nullopt;
	const unsigned length = std::bit_width(pattern) - 1;
	if (length < 1 || (!is_64_bit && length > 5))
		return std::nullopt;
	const unsigned levels = (1U << length) - 1;
	const unsigned set_bits = imms & levels;
	if (immediate && set_bits == levels)
		return std::nullopt;
	const unsigned element_width = 1U << length;
	const unsigned count = set_bits + 1;
	const auto ones = count == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << count) - 1;
	const auto write_element = rotate_right_width(
		ones, immr & levels, element_width);
	const unsigned difference = (set_bits - (immr & levels)) & levels;
	const unsigned test_count = difference + 1;
	const auto test_element = test_count == 64 ? ~std::uint64_t{0} :
		(std::uint64_t{1} << test_count) - 1;
	const unsigned width = is_64_bit ? 64 : 32;
	BitMasks result{};
	for (unsigned position = 0; position < width;
	     position += element_width) {
		result.write |= write_element << position;
		result.test |= test_element << position;
	}
	result.write &= width_mask(is_64_bit);
	result.test &= width_mask(is_64_bit);
	return result;
}

constexpr std::optional<std::uint64_t> decode_logical_immediate(
	bool is_64_bit, unsigned n, unsigned immr, unsigned imms)
{
	const auto masks = decode_bit_masks(
		is_64_bit, n, immr, imms, true);
	return masks ? std::optional{masks->write} : std::nullopt;
}

constexpr std::optional<std::uint64_t> simd_integer_immediate(
	unsigned mode, std::uint8_t immediate)
{
	std::uint64_t element;
	if (mode == 0 || mode == 2 || mode == 4 || mode == 6) {
		element = static_cast<std::uint64_t>(immediate) <<
			((mode >> 1) * 8);
		return element | (element << 32);
	}
	if (mode == 8 || mode == 10) {
		element = static_cast<std::uint64_t>(immediate) <<
			(((mode >> 1) & 1U) * 8);
		return element | (element << 16) | (element << 32) |
			(element << 48);
	}
	if (mode == 14)
		return static_cast<std::uint64_t>(immediate) *
			0x0101'0101'0101'0101ULL;
	return std::nullopt;
}

struct SimdModifiedImmediate final {
	std::uint64_t pattern;
	unsigned action; // 0 = replace, 1 = ORR, 2 = BIC
};

struct SimdLaneIndex final {
	unsigned element_shift;
	unsigned element_size;
	unsigned index;
	unsigned offset;
};

/* Decode the shared imm5 lane encoding.  Bit 4 selects the upper 64-bit
 * source half; at least one of bits [3:0] must select an element size. */
constexpr std::optional<SimdLaneIndex> decode_simd_lane_index(
	unsigned immediate)
{
	immediate &= 0x1fU;
	if ((immediate & 0xfU) == 0)
		return std::nullopt;
	const unsigned element_shift = std::countr_zero(immediate);
	const unsigned index = immediate >> (element_shift + 1U);
	return SimdLaneIndex{element_shift, 1U << element_shift, index,
		index << element_shift};
}

constexpr std::optional<SimdModifiedImmediate>
decode_simd_modified_immediate(std::uint32_t instruction)
{
	const bool q = (instruction & 0x4000'0000U) != 0;
	const bool invert = (instruction & 0x2000'0000U) != 0;
	const unsigned mode = (instruction >> 12) & 0xfU;
	const bool o2 = (instruction & 0x800U) != 0;
	const auto immediate = static_cast<std::uint8_t>(
		(((instruction >> 16) & 0x7U) << 5) |
		((instruction >> 5) & 0x1fU));

	/* o2 == 1 has exactly one allocated encoding: FP16 FMOV.  Validate
	 * this before applying any of the integer/immediate expansion rules. */
	if (o2) {
		if (invert || mode != 15U)
			return std::nullopt;
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t fp = (sign << 15) |
			((1U - exponent_bit) << 14) |
			(exponent_bit * 0x3000U) |
			(static_cast<std::uint64_t>(immediate & 0x3fU) << 6);
		return SimdModifiedImmediate{
			fp * 0x0001'0001'0001'0001ULL, 0U};
	}

	std::uint64_t pattern;
	const bool combine = (mode & 1U) != 0 && mode != 13U && mode != 15U;
	if (mode == 14U) {
		if (!invert) {
			pattern = static_cast<std::uint64_t>(immediate) *
				0x0101'0101'0101'0101ULL;
		} else {
			pattern = 0;
			for (unsigned index = 0; index < 8; ++index)
				if ((immediate & (1U << index)) != 0)
					pattern |= 0xffULL << (index * 8);
		}
	} else if (mode == 15U) {
		const std::uint64_t sign = immediate >> 7;
		const std::uint64_t exponent_bit = (immediate >> 6) & 1U;
		const std::uint64_t fraction = immediate & 0x3fU;
		if (!invert) {
			const std::uint32_t exponent = ((1U - exponent_bit) << 7) |
				(exponent_bit * 0x7cU) | ((fraction >> 4) & 3U);
			const std::uint32_t fp =
				(static_cast<std::uint32_t>(sign) << 31) |
				(exponent << 23) |
				(static_cast<std::uint32_t>(fraction & 15U) << 19);
			pattern = fp | (static_cast<std::uint64_t>(fp) << 32);
		} else {
			if (!q)
				return std::nullopt;
			const std::uint64_t exponent = ((1U - exponent_bit) << 10) |
				(exponent_bit * 0x3fcU) | ((fraction >> 4) & 3U);
			pattern = (sign << 63) | (exponent << 52) |
				((fraction & 15U) << 48);
		}
	} else if (mode == 12U) {
		const auto element =
			(static_cast<std::uint64_t>(immediate) << 8) | 0xffU;
		pattern = element | (element << 32);
	} else if (mode == 13U) {
		const auto element =
			(static_cast<std::uint64_t>(immediate) << 16) | 0xffffU;
		pattern = element | (element << 32);
	} else {
		const auto decoded = simd_integer_immediate(
			combine ? mode - 1U : mode, immediate);
		if (!decoded)
			return std::nullopt;
		pattern = *decoded;
	}
	if (!combine && invert && mode != 14U && mode != 15U)
		pattern = ~pattern;
	return SimdModifiedImmediate{
		pattern, combine ? (invert ? 2U : 1U) : 0U};
}

} // namespace uml::cpu::arm64::detail
