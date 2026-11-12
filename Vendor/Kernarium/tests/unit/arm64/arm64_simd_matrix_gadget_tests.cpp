// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_gadget_test_support.hpp"
#include "arm64_simd_dot_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_simd_matrix_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace dot = arm64_simd_dot_test;

using Registers = dot::Registers;
using dot::fail;
using dot::require;

enum class Mode {
	signed_signed,
	unsigned_unsigned,
	unsigned_signed,
};

inline constexpr std::array modes{
	Mode::signed_signed,
	Mode::unsigned_unsigned,
	Mode::unsigned_signed,
};

[[nodiscard]] constexpr std::uint32_t raw_instruction(bool q, bool u,
	unsigned size, bool alternate, Registers registers) noexcept
{
	return 0x0e00'a400U |
		(q ? 0x4000'0000U : 0U) |
		(u ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		(alternate ? 0x0000'0800U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(Mode mode,
	Registers registers) noexcept
{
	return raw_instruction(true, mode == Mode::unsigned_unsigned, 2U,
		mode == Mode::unsigned_signed, registers);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements()
{
	return engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::i8mm);
}

} // namespace arm64_simd_matrix_test

namespace a64 = uml::testing::a64;

extern "C" {
void uml_arm64_gadget_simd_matrix_smmla();
void uml_arm64_gadget_simd_matrix_ummla();
void uml_arm64_gadget_simd_matrix_usmmla();
void uml_arm64_gadget_simd_matrix_smmla_soft();
void uml_arm64_gadget_simd_matrix_ummla_soft();
void uml_arm64_gadget_simd_matrix_usmmla_soft();
}

namespace test = arm64_simd_matrix_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;
using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr unsigned mode_index(test::Mode mode) noexcept
{
	switch (mode) {
	case test::Mode::signed_signed:
		return 0U;
	case test::Mode::unsigned_unsigned:
		return 1U;
	case test::Mode::unsigned_signed:
		return 2U;
	}
	return 0U;
}

[[nodiscard]] Gadget selected_gadget(test::Mode mode, bool portable)
{
	static constexpr std::array native{
		uml_arm64_gadget_simd_matrix_smmla,
		uml_arm64_gadget_simd_matrix_ummla,
		uml_arm64_gadget_simd_matrix_usmmla,
	};
	static constexpr std::array software{
		uml_arm64_gadget_simd_matrix_smmla_soft,
		uml_arm64_gadget_simd_matrix_ummla_soft,
		uml_arm64_gadget_simd_matrix_usmmla_soft,
	};
	const bool host_native =
		test::engine_tests::host_supports(test::native_requirements());
	return (portable || !host_native ? software : native)[mode_index(mode)];
}

void require_straight_line(Gadget gadget)
{
	const auto shape = a64::body_shape(
		reinterpret_cast<std::uintptr_t>(gadget),
		{.window = 64U}, [](std::uint32_t instruction) {
			test::require(!a64::conditional_branch(instruction),
				"matrix gadget contains a conditional branch");
			test::require((instruction & 0xfc00'0000U) != 0x9400'0000U,
				"matrix gadget contains a helper call");
		});
	if (!shape.reached_dispatch)
		test::fail("matrix gadget did not reach indirect dispatch");
}

void translation_resolves_host_choice_and_register_offsets()
{
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	for (const bool portable : {false, true})
		for (const auto mode : test::modes) {
			const auto options = gadgets::BuildOptions{
				.force_software_i8mm = portable,
			};
			const auto built = gadgets::Builder{}.build(base,
				std::array{test::instruction(mode, registers)},
				{}, std::pmr::get_default_resource(), {}, options);
			const auto words = built.program.words();
			test::require(built.complete && words.size() == 4U,
				"matrix stream has an unexpected shape");
			test::require(words[0] == address(
					selected_gadget(mode, portable)),
				"matrix host/form choice was not made in translation");
			test::require(words[1] ==
					gadgets::selection::packed_u16_operands(
						vector_offset(registers.destination),
						vector_offset(registers.left),
						vector_offset(registers.right)),
				"matrix offsets were not packed during translation");
		}
}

void all_live_entries_are_straight_line_and_unique()
{
	const std::array<Gadget, 6> entries{
		uml_arm64_gadget_simd_matrix_smmla,
		uml_arm64_gadget_simd_matrix_ummla,
		uml_arm64_gadget_simd_matrix_usmmla,
		uml_arm64_gadget_simd_matrix_smmla_soft,
		uml_arm64_gadget_simd_matrix_ummla_soft,
		uml_arm64_gadget_simd_matrix_usmmla_soft,
	};
	std::set<std::uintptr_t> unique;
	for (const auto entry : entries) {
		unique.insert(address(entry));
		require_straight_line(entry);
	}
	test::require(unique.size() == entries.size(),
		"matrix gadget entries are duplicated");
}

} // namespace

int main()
{
	translation_resolves_host_choice_and_register_offsets();
	all_live_entries_are_straight_line_and_unique();
	return 0;
}
