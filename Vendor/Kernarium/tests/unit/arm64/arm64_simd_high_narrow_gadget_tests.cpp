// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_simd_high_narrow_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add,
	rounded_add,
	subtract,
	rounded_subtract,
};

inline constexpr std::array operations{
	Operation::add,
	Operation::rounded_add,
	Operation::subtract,
	Operation::rounded_subtract,
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

using uml::testing::require;

[[nodiscard]] constexpr bool rounding(Operation operation)
{
	return operation == Operation::rounded_add ||
		operation == Operation::rounded_subtract;
}

[[nodiscard]] constexpr bool subtracting(Operation operation)
{
	return operation == Operation::subtract ||
		operation == Operation::rounded_subtract;
}

[[nodiscard]] constexpr std::uint32_t instruction(Operation operation,
	Shape shape, Registers registers)
{
	return 0x0e20'4000U | (shape.upper ? 0x4000'0000U : 0U) |
		(rounding(operation) ? 0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(subtracting(operation) ? 0x2000U : 0U) |
		((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
}

} // namespace arm64_simd_high_narrow_test

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_HIGH_NARROW(name) \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_2d(); \
	void uml_arm64_gadget_simd_widening_##name##_8h2(); \
	void uml_arm64_gadget_simd_widening_##name##_4s2(); \
	void uml_arm64_gadget_simd_widening_##name##_2d2();
UML_DECLARE_HIGH_NARROW(addhn)
UML_DECLARE_HIGH_NARROW(raddhn)
UML_DECLARE_HIGH_NARROW(subhn)
UML_DECLARE_HIGH_NARROW(rsubhn)
#undef UML_DECLARE_HIGH_NARROW
}

namespace test = arm64_simd_high_narrow_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

#define UML_HIGH_NARROW_SET(name) std::array<Gadget, 6>{ \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s, \
	uml_arm64_gadget_simd_widening_##name##_2d, \
	uml_arm64_gadget_simd_widening_##name##_8h2, \
	uml_arm64_gadget_simd_widening_##name##_4s2, \
	uml_arm64_gadget_simd_widening_##name##_2d2}

constexpr std::array expected_gadgets{
	UML_HIGH_NARROW_SET(addhn),
	UML_HIGH_NARROW_SET(raddhn),
	UML_HIGH_NARROW_SET(subhn),
	UML_HIGH_NARROW_SET(rsubhn),
};

#undef UML_HIGH_NARROW_SET

using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

void require_branch_free(Gadget gadget)
{
	const auto shape = a64::body_shape(
		reinterpret_cast<std::uintptr_t>(gadget), {.window = 64U});
	test::require(shape.conditional_branches == 0U,
		"high-narrow gadget contains a conditional branch");
	test::require(shape.reached_dispatch,
		"high-narrow gadget did not reach indirect dispatch");
}

void every_form_uses_one_direct_offset_gadget()
{
	static_assert(vector_offset(31U) <= 0xffffU);
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &operation : expected_gadgets)
		for (const auto gadget : operation)
			expected.insert(address(gadget));

	for (unsigned operation = 0; operation < test::operations.size();
	     ++operation)
		for (unsigned shape = 0; shape < test::shapes.size(); ++shape) {
			const auto built = gadgets::Builder{}.build(base,
				std::array{test::instruction(
					test::operations[operation], test::shapes[shape],
					registers)});
			test::require(built.complete,
				"allocated high-narrow instruction did not build");
			const auto words = built.program.words();
			test::require(words.size() == 4U,
				"high-narrow gadget stream has an unexpected shape");
			test::require(words[0] ==
					address(expected_gadgets[operation][shape]),
				"operation or arrangement was not selected in translation");
			test::require(words[1] ==
					gadgets::selection::packed_u16_operands(
						vector_offset(registers.destination),
						vector_offset(registers.left),
						vector_offset(registers.right)),
				"register offsets were not packed during translation");
			selected.insert(words[0]);
		}
	test::require(expected.size() == 24U,
		"expected high-narrow gadget table is incomplete");
	test::require(selected == expected,
		"high-narrow gadget table contains dead or unselected entries");
}

void every_hot_path_is_branch_free()
{
	for (const auto &operation : expected_gadgets)
		for (const auto gadget : operation)
			require_branch_free(gadget);
}

} // namespace

int main()
{
	every_form_uses_one_direct_offset_gadget();
	every_hot_path_is_branch_free();
	return 0;
}
