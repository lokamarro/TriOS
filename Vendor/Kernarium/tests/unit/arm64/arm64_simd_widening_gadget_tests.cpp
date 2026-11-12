// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_simd_widening_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add_long = 0U,
	add_wide = 1U,
	subtract_long = 2U,
	subtract_wide = 3U,
	absolute_accumulate = 5U,
	absolute_long = 7U,
};

inline constexpr std::array operations{
	Operation::add_long,
	Operation::add_wide,
	Operation::subtract_long,
	Operation::subtract_wide,
	Operation::absolute_accumulate,
	Operation::absolute_long,
};

struct Shape final {
	unsigned size;
	bool upper;
};

inline constexpr std::array shapes{
	Shape{0U, false}, Shape{1U, false}, Shape{2U, false},
	Shape{0U, true}, Shape{1U, true}, Shape{2U, true},
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr unsigned encoding(Operation operation)
{
	return static_cast<unsigned>(operation);
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	bool unsigned_values, Shape shape, Registers registers)
{
	return 0x0e20'0000U |
		(shape.upper ? 0x4000'0000U : 0U) |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(encoding(operation) << 12) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

/*
 * The scalar three-different allocation shares the operation field, but none
 * of the twelve vector instructions audited by this suite has a scalar form.
 */
[[nodiscard]] constexpr std::uint32_t scalar_instruction(
	Operation operation, bool unsigned_values, unsigned size,
	Registers registers)
{
	return 0x5e20'0000U |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) |
		(encoding(operation) << 12) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_simd_widening_test

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_WIDENING(name) \
	void uml_arm64_gadget_simd_widening_##name##_8b(); \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_16b(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s();
UML_DECLARE_WIDENING(saddl)
UML_DECLARE_WIDENING(uaddl)
UML_DECLARE_WIDENING(saddw)
UML_DECLARE_WIDENING(uaddw)
UML_DECLARE_WIDENING(ssubl)
UML_DECLARE_WIDENING(usubl)
UML_DECLARE_WIDENING(ssubw)
UML_DECLARE_WIDENING(usubw)
UML_DECLARE_WIDENING(sabal)
UML_DECLARE_WIDENING(uabal)
UML_DECLARE_WIDENING(sabdl)
UML_DECLARE_WIDENING(uabdl)
#undef UML_DECLARE_WIDENING
}

namespace test = arm64_simd_widening_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;
using GadgetSet = std::array<Gadget, 6>;

#define UML_WIDENING_SET(name) GadgetSet{ \
	uml_arm64_gadget_simd_widening_##name##_8b, \
	uml_arm64_gadget_simd_widening_##name##_4h, \
	uml_arm64_gadget_simd_widening_##name##_2s, \
	uml_arm64_gadget_simd_widening_##name##_16b, \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s}

constexpr std::array expected_gadgets{
	std::array{UML_WIDENING_SET(saddl), UML_WIDENING_SET(uaddl)},
	std::array{UML_WIDENING_SET(saddw), UML_WIDENING_SET(uaddw)},
	std::array{UML_WIDENING_SET(ssubl), UML_WIDENING_SET(usubl)},
	std::array{UML_WIDENING_SET(ssubw), UML_WIDENING_SET(usubw)},
	std::array{UML_WIDENING_SET(sabal), UML_WIDENING_SET(uabal)},
	std::array{UML_WIDENING_SET(sabdl), UML_WIDENING_SET(uabdl)},
};

#undef UML_WIDENING_SET

using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] unsigned instructions_to_dispatch(Gadget gadget)
{
	const auto shape = a64::body_shape(
		reinterpret_cast<std::uintptr_t>(gadget), {.window = 64U});
	test::require(shape.conditional_branches == 0U,
		"widening gadget contains a conditional branch");
	if (!shape.reached_dispatch)
		test::fail("widening gadget did not reach indirect dispatch");
	return shape.instructions + 1U;
}

void every_form_uses_one_direct_offset_gadget()
{
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &operation : expected_gadgets)
		for (const auto &signedness : operation)
			for (const auto gadget : signedness)
				expected.insert(address(gadget));

	for (unsigned operation = 0; operation < test::operations.size();
	     ++operation)
		for (unsigned unsigned_values = 0; unsigned_values < 2U;
		     ++unsigned_values)
			for (unsigned shape = 0; shape < test::shapes.size();
			     ++shape) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::instruction(
						test::operations[operation],
						unsigned_values != 0U,
						test::shapes[shape], registers)});
				test::require(built.complete,
					"allocated widening instruction did not build");
				const auto words = built.program.words();
				test::require(words.size() == 4U,
					"widening gadget stream has an unexpected shape");
				const auto selected_gadget =
					expected_gadgets[operation][unsigned_values][shape];
				test::require(words[0] == address(selected_gadget),
					"operation or arrangement was not selected in translation");
				test::require(words[1] ==
						gadgets::selection::packed_u16_operands(
							vector_offset(registers.destination),
							vector_offset(registers.left),
							vector_offset(registers.right)),
					"register offsets were not packed during translation");
				selected.insert(words[0]);
			}
	test::require(expected.size() == 72U,
		"expected widening gadget table is incomplete");
	test::require(selected == expected,
		"widening gadget table contains dead or unselected entries");
}

void reserved_forms_never_select_a_widening_gadget()
{
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> expected;
	for (const auto &operation : expected_gadgets)
		for (const auto &signedness : operation)
			for (const auto gadget : signedness)
				expected.insert(address(gadget));
	for (const auto operation : test::operations)
		for (const bool unsigned_values : {false, true}) {
			for (const bool upper : {false, true}) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::instruction(operation,
						unsigned_values, {3U, upper}, registers)});
				test::require(built.complete,
					"reserved widening size did not terminate");
				test::require(expected.count(
					built.program.words()[0]) == 0U,
					"reserved widening size selected a valid gadget");
			}
			for (unsigned size = 0; size < 4U; ++size) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::scalar_instruction(operation,
						unsigned_values, size, registers)});
				test::require(built.complete,
					"reserved scalar widening form did not terminate");
				test::require(expected.count(
					built.program.words()[0]) == 0U,
					"reserved scalar form selected a valid gadget");
			}
		}
}

void every_hot_path_is_minimal_and_branch_free()
{
	for (unsigned operation = 0; operation < expected_gadgets.size();
	     ++operation)
		for (const auto &signedness : expected_gadgets[operation])
			for (const auto gadget : signedness) {
				const unsigned expected_count =
					test::operations[operation] ==
						test::Operation::absolute_accumulate ?
					11U : 10U;
				test::require(instructions_to_dispatch(gadget) ==
						expected_count,
					"widening gadget has unexpected runtime work");
			}
}

} // namespace

int main()
{
	every_form_uses_one_direct_offset_gadget();
	reserved_forms_never_select_a_widening_gadget();
	every_hot_path_is_minimal_and_branch_free();
	return 0;
}
