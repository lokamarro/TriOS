// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_dot_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Mode {
	signed_signed,
	unsigned_unsigned,
	signed_unsigned,
	unsigned_signed,
};

enum class Form {
	vector,
	indexed,
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

inline constexpr std::array vector_modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::unsigned_signed,
};

inline constexpr std::array indexed_modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::signed_unsigned,
	Mode::unsigned_signed,
};

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr bool extended(Mode mode) noexcept
{
	return mode == Mode::signed_unsigned || mode == Mode::unsigned_signed;
}

[[nodiscard]] constexpr std::uint32_t vector_instruction(Mode mode, bool q,
	unsigned size, Registers registers) noexcept
{
	/*
	 * The mixed vector allocation has only the unsigned-by-signed form.
	 * Its fixed size field is kept explicit so allocation tests can exercise
	 * all four values without borrowing an implementation decoder.
	 */
	const bool ordinary = !extended(mode);
	const bool unsigned_group = mode == Mode::unsigned_unsigned;
	return (ordinary ? 0x0e00'9400U : 0x0e00'9c00U) |
		(q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t indexed_instruction_with_size(
	Mode mode, bool q, unsigned size, Registers registers,
	unsigned index) noexcept
{
	const bool ordinary = !extended(mode);
	const bool unsigned_group = mode == Mode::unsigned_unsigned;
	std::uint32_t instruction = 0x0f00'0000U |
		(q ? 0x4000'0000U : 0U) |
		(unsigned_group ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		((ordinary ? 14U : 15U) << 12) |
		((registers.right & 15U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
	/*
	 * For every instruction in this family, M extends Rm and H:L selects
	 * one of the four groups. Other allocations sharing opcode 1111 give
	 * these bits different meanings and are intentionally not classified
	 * as this family.
	 */
	instruction |= ((registers.right >> 4) & 1U) << 20;
	instruction |= (index & 1U) << 21;
	instruction |= (index & 2U) << 10;
	return instruction;
}

[[nodiscard]] constexpr std::uint32_t indexed_instruction(
	Mode mode, bool q, Registers registers, unsigned index) noexcept
{
	return indexed_instruction_with_size(mode, q,
		mode == Mode::signed_unsigned ? 0U : 2U, registers, index);
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form, Mode mode,
	bool q, Registers registers, unsigned index = 0U) noexcept
{
	return form == Form::vector ?
		vector_instruction(mode, q, 2U, registers) :
		indexed_instruction(mode, q, registers, index);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(Mode mode)
{
	return engine_tests::NativeRequirements::features(
		extended(mode) ? ::uml::cpu::ArmFeature::i8mm :
			::uml::cpu::ArmFeature::dotprod);
}

} // namespace arm64_simd_dot_test
