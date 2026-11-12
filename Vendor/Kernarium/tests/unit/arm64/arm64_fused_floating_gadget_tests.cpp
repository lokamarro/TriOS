// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_basic_floating_test_support.hpp"
#include "require.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string_view>

namespace arm64_fused_floating_test {

namespace basic = arm64_basic_floating_test;
namespace common = uml::testing;
namespace engine_tests = uml::emulator::tests;

struct Form final {
	bool scalar;
	bool negate;
	bool subtract;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
	unsigned addend;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2, 3}, Registers{1, 1, 2, 3},
	Registers{2, 1, 2, 3}, Registers{3, 1, 2, 3},
	Registers{1, 1, 1, 1}, Registers{31, 30, 29, 28},
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true})
			for (const auto width : basic::widths)
				function(Form{true, negate, subtract, width, 16U});
	for (const bool subtract : {false, true})
		for (const auto shape : basic::vector_shapes)
			function(Form{false, false, subtract, shape[0], shape[1]});
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2, 3}) noexcept
{
	std::uint32_t value;
	if (form.scalar) {
		value = 0x1f00'0000U |
			(basic::scalar_type(form.width) << 22) |
			(form.negate ? 0x0020'0000U : 0U) |
			(form.subtract ? 0x0000'8000U : 0U) |
			((registers.addend & 31U) << 10);
	} else {
		const bool q = form.vector_bytes == 16U;
		if (form.width == 16U)
			value = 0x0e40'0c00U;
		else
			value = 0x0e20'cc00U |
				(form.width == 64U ? 0x0040'0000U : 0U);
		value |= q ? 0x4000'0000U : 0U;
		value |= form.subtract ? 0x0080'0000U : 0U;
	}
	return value | ((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

} // namespace arm64_fused_floating_test

namespace a64 = uml::testing::a64;

namespace test = arm64_fused_floating_test;

namespace {

namespace gadgets = uml::cpu::arm64::gadgets;
namespace arm64 = uml::cpu::arm64;

extern "C" {
#define DECLARE_SCALAR(operation) \
	void uml_arm64_gadget_floating_##operation##_half(); \
	void uml_arm64_gadget_floating_##operation##_single(); \
	void uml_arm64_gadget_floating_##operation##_double();
DECLARE_SCALAR(madd)
DECLARE_SCALAR(msub)
DECLARE_SCALAR(nmadd)
DECLARE_SCALAR(nmsub)
#undef DECLARE_SCALAR
}

using uml::emulator::tests::address;
using uml::emulator::tests::emitted_entry;

[[nodiscard]] std::uintptr_t scalar_entry(const test::Form &form)
{
	const unsigned width = form.width == 16U ? 0U :
		form.width == 32U ? 1U : 2U;
#define SCALAR_ROW(operation) std::array<std::uintptr_t, 3>{ \
	address(uml_arm64_gadget_floating_##operation##_half), \
	address(uml_arm64_gadget_floating_##operation##_single), \
	address(uml_arm64_gadget_floating_##operation##_double)}
	static const std::array table{
		SCALAR_ROW(madd), SCALAR_ROW(msub),
		SCALAR_ROW(nmadd), SCALAR_ROW(nmsub),
	};
#undef SCALAR_ROW
	const unsigned operation = form.negate * 2U + form.subtract;
	return table[operation][width];
}

/* Every vector FMLA/FMLS accumulates into Vd, so the translator always
 * reaches the baked-accumulator entry for that register. */
[[nodiscard]] std::uintptr_t vector_entry(const test::Form &form,
	unsigned destination)
{
	const unsigned shape = form.width == 16U ?
		(form.vector_bytes == 16U ? 1U : 0U) :
		form.width == 32U ?
		(form.vector_bytes == 16U ? 3U : 2U) : 4U;
	const unsigned row = (form.subtract ? 5U : 0U) + shape;
	return reinterpret_cast<std::uintptr_t>(
		gadgets::selection::uml_arm64_gadget_simd_fused_inplace_table[
			row * 32U + destination]);
}

using uml::emulator::tests::vector_offset;

void direct_operands_and_live_bank()
{
	std::set<std::uintptr_t> selected;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases) {
			const auto built = gadgets::Builder{}.build(
				test::engine_tests::engine_test_code_base,
				std::array{test::instruction(form, registers)});
			test::common::require(
				built.complete && !built.program.words().empty());
			const auto expected_entry = form.scalar ?
				scalar_entry(form) :
				vector_entry(form, registers.destination);
			test::common::require(built.program.words()[0] ==
				emitted_entry(expected_entry));
			test::common::require(
				built.program.words().size() == 4U);
			test::common::require(built.program.words()[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.left),
					vector_offset(registers.right),
					form.scalar ?
						vector_offset(registers.addend) : 0U));
			selected.insert(expected_entry);
		}
	});

	std::set<std::uintptr_t> expected;
	test::for_each_form([&](const test::Form &form) {
		if (form.scalar) {
			expected.insert(scalar_entry(form));
			return;
		}
		/* A vector form reaches one entry per destination register when
		 * the accumulator displacement is baked, and one entry in all
		 * for the established bank. */
		for (const auto registers : test::register_cases)
			expected.insert(vector_entry(form, registers.destination));
	});
	test::common::require(selected == expected);

	for (const auto entry : expected) {
		const auto shape = a64::body_shape(entry, {.window = 32U});
		test::common::require(shape.reached_dispatch);
		const bool scalar =
			entry == address(uml_arm64_gadget_floating_madd_half) ||
			entry == address(uml_arm64_gadget_floating_madd_single) ||
			entry == address(uml_arm64_gadget_floating_madd_double) ||
			entry == address(uml_arm64_gadget_floating_msub_half) ||
			entry == address(uml_arm64_gadget_floating_msub_single) ||
			entry == address(uml_arm64_gadget_floating_msub_double) ||
			entry == address(uml_arm64_gadget_floating_nmadd_half) ||
			entry == address(uml_arm64_gadget_floating_nmadd_single) ||
			entry == address(uml_arm64_gadget_floating_nmadd_double) ||
			entry == address(uml_arm64_gadget_floating_nmsub_half) ||
			entry == address(uml_arm64_gadget_floating_nmsub_single) ||
			entry == address(uml_arm64_gadget_floating_nmsub_double);
		test::common::require(shape.conditional_branches <=
			(scalar ? 2U : 1U));
		test::common::require(shape.instructions <=
			(scalar ? 25U : 16U));
	}
}

} // namespace

int main()
{
	direct_operands_and_live_bank();
}
