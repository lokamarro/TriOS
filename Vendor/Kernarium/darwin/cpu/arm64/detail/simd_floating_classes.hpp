// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace uml::cpu::arm64::detail {

struct SimdFloatingRegion final {
	std::uint32_t mask;
	std::uint32_t value;
};

/*
 * The unallocated leaves of the A64 "Data Processing -- Scalar
 * Floating-Point and Advanced SIMD" class table (op0 bits 28:25 = x111), as
 * pinned in tests/unit/arm64/a64_instruction_decode_partitions.tsv. Fields
 * below name the table's op0 (31:28), op1 (24:23), op2 (22:19) and op3
 * (18:10) columns.
 */
inline constexpr std::array unallocated_simd_floating_regions{
	SimdFloatingRegion{0xde00'0000U, 0x8e00'0000U}, // op0 10x0
	SimdFloatingRegion{0xde00'0000U, 0xde00'0000U}, // op0 11x1
	SimdFloatingRegion{0xfe00'0000U, 0xee00'0000U}, // op0 1110
	SimdFloatingRegion{0xcf80'0400U, 0x4f80'0400U}, // op0 01xx, op1 11, op3 xxxxxxxx1
	SimdFloatingRegion{0xdf80'0400U, 0x0f80'0400U}, // op0 00x0, op1 11, op3 xxxxxxxx1
	SimdFloatingRegion{0xff00'0000U, 0xcf00'0000U}, // op0 1100, op1 1x
	SimdFloatingRegion{0x9f20'8400U, 0x0e00'8000U}, // op0 0xx0, op1 0x, op2 x0xx, op3 xxx1xxxx0
	SimdFloatingRegion{0xff20'8400U, 0x5e00'8000U}, // op0 0101, op1 0x, op2 x0xx, op3 xxx1xxxx0
	SimdFloatingRegion{0xff20'8c00U, 0x5e00'0800U}, // op0 0101, op1 0x, op2 x0xx, op3 xxx0xxx10
	SimdFloatingRegion{0xff20'0400U, 0x7e00'0000U}, // op0 0111, op1 0x, op2 x0xx, op3 xxxxxxxx0
	SimdFloatingRegion{0x9f24'0c00U, 0x0e24'0800U}, // op0 0xx0, op1 0x, op2 x1xx, op3 1xxxxxx10
	SimdFloatingRegion{0x9f26'0c00U, 0x0e22'0800U}, // op0 0xx0, op1 0x, op2 x1xx, op3 01xxxxx10
	SimdFloatingRegion{0xdf24'0c00U, 0x5e24'0800U}, // op0 01x1, op1 0x, op2 x1xx, op3 1xxxxxx10
	SimdFloatingRegion{0xdf26'0c00U, 0x5e22'0800U}, // op0 01x1, op1 0x, op2 x1xx, op3 01xxxxx10
	SimdFloatingRegion{0xffa0'0000U, 0xcea0'0000U}, // op0 1100, op1 01, op2 x1xx
	SimdFloatingRegion{0xfff0'0000U, 0xced0'0000U}, // op0 1100, op1 01, op2 101x
	SimdFloatingRegion{0xfff8'0000U, 0xcec8'0000U}, // op0 1100, op1 01, op2 1001
	SimdFloatingRegion{0x9fe0'8400U, 0x0e80'0400U}, // op0 0xx0, op1 01, op2 00xx, op3 xxx0xxxx1
	SimdFloatingRegion{0xdfe0'8400U, 0x5e80'0400U}, // op0 01x1, op1 01, op2 00xx, op3 xxx0xxxx1
	SimdFloatingRegion{0x9f60'c400U, 0x0e40'4400U}, // op0 0xx0, op1 0x, op2 10xx, op3 xxx01xxx1
	SimdFloatingRegion{0xdf60'c400U, 0x5e40'4400U}, // op0 01x1, op1 0x, op2 10xx, op3 xxx01xxx1
	SimdFloatingRegion{0xffc0'8000U, 0xce00'8000U}, // op0 1100, op1 00, op2 0xxx, op3 xxx1xxxxx
	SimdFloatingRegion{0xffe0'c000U, 0xce40'c000U}, // op0 1100, op1 00, op2 10xx, op3 xxx11xxxx
	SimdFloatingRegion{0xffe0'a000U, 0xce60'a000U}, // op0 1100, op1 00, op2 11xx, op3 xxx1x1xxx
	SimdFloatingRegion{0xffe0'b000U, 0xce60'9000U}, // op0 1100, op1 00, op2 11xx, op3 xxx1x01xx
	SimdFloatingRegion{0x5f20'fc00U, 0x1e20'8000U}, // op0 x0x1, op1 0x, op2 x1xx, op3 xxx100000
	SimdFloatingRegion{0xdf3e'0c00U, 0x0e28'0800U}, // op0 00x0, op1 0x, op2 x101, op3 00xxxxx10
	SimdFloatingRegion{0xef3e'0c00U, 0x6e28'0800U}, // op0 011x, op1 0x, op2 x101, op3 00xxxxx10
	SimdFloatingRegion{0x9f7e'0c00U, 0x0e38'0800U}, // op0 0xx0, op1 0x, op2 0111, op3 00xxxxx10
	SimdFloatingRegion{0xdf7e'0c00U, 0x5e38'0800U}, // op0 01x1, op1 0x, op2 0111, op3 00xxxxx10
};

/*
 * True for a word in one of these regions, which no operand field can
 * allocate. Every engine rejects such a word before its class decoders run:
 * those decoders test only the bits that separate one allocation from
 * another, so a word outside every class would otherwise execute as a
 * neighbour. Bits 20:17, for example, must be 0000 for two-register
 * miscellaneous and 1000 for across-lanes words, and neither class decoder
 * reads them.
 */
[[nodiscard]] constexpr bool unallocated_simd_floating_class(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0x0e00'0000U) != 0x0e00'0000U)
		return false;
	for (const auto region : unallocated_simd_floating_regions)
		if ((instruction & region.mask) == region.value)
			return true;
	/* Op0 1100, op1 01, op2 1000 is the two-register SHA512 class only
	 * where op3 is 0001000xx. */
	return (instruction & 0xfff8'0000U) == 0xcec0'0000U &&
		(instruction & 0x0007'f000U) != 0x0000'8000U;
}

} // namespace uml::cpu::arm64::detail
