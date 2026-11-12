// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace uml::emulator::tests::base_min_max {

enum class Operation : unsigned {
	signed_maximum,
	unsigned_maximum,
	signed_minimum,
	unsigned_minimum,
};

inline constexpr std::array operations{
	Operation::signed_maximum,
	Operation::unsigned_maximum,
	Operation::signed_minimum,
	Operation::unsigned_minimum,
};

[[nodiscard]] constexpr std::uint32_t encode_immediate(
	bool wide, Operation operation, unsigned immediate, unsigned rd,
	unsigned rn)
{
	return 0x11c0'0000U | (wide ? 0x8000'0000U : 0U) |
		(static_cast<unsigned>(operation) << 18U) |
		((immediate & 0xffU) << 10U) | ((rn & 31U) << 5U) |
		(rd & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_register(
	bool wide, Operation operation, unsigned rd, unsigned rn, unsigned rm)
{
	return 0x1ac0'6000U | (wide ? 0x8000'0000U : 0U) |
		((rm & 31U) << 16U) |
		(static_cast<unsigned>(operation) << 10U) |
		((rn & 31U) << 5U) | (rd & 31U);
}

using uml::testing::require;

} // namespace uml::emulator::tests::base_min_max

namespace a64 = uml::testing::a64;

namespace test = uml::emulator::tests::base_min_max;
namespace engine_tests = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define UML_DECLARE_MIN_MAX_ROW(name) \
	void uml_arm64_gadget_integer_##name##_fast_w_r(); \
	void uml_arm64_gadget_integer_##name##_fast_w_i(); \
	void uml_arm64_gadget_integer_##name##_fast_x_r(); \
	void uml_arm64_gadget_integer_##name##_fast_x_i();
UML_DECLARE_MIN_MAX_ROW(smax)
UML_DECLARE_MIN_MAX_ROW(umax)
UML_DECLARE_MIN_MAX_ROW(smin)
UML_DECLARE_MIN_MAX_ROW(umin)
#undef UML_DECLARE_MIN_MAX_ROW
void uml_arm64_gadget_integer_min_max_copy_w();
void uml_arm64_gadget_integer_min_max_copy_x();
}

namespace {

using uml::emulator::tests::address;

[[nodiscard]] constexpr std::uintptr_t register_offset(unsigned index)
{
	return index == 31U ? offsetof(arm64::CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

void require_one_operand_stream(const gadgets::BuildResult &built,
	std::uintptr_t expected)
{
	test::require(built.complete && built.program.words().size() == 4U,
		"min/max instruction has an unexpected gadget stream shape");
	test::require(built.program.words()[1] == expected,
		"min/max operands were not resolved at translation time");
}

[[nodiscard]] bool branch_free(std::uintptr_t entry)
{
	unsigned direct_branches = 0;
	const auto shape = a64::body_shape(entry, {.window = 64U},
		[&](std::uint32_t instruction) {
			direct_branches +=
				(instruction & 0x7c00'0000U) == 0x1400'0000U;
		});
	return shape.reached_dispatch && shape.conditional_branches == 0U &&
		direct_branches == 0U;
}

void add_row(std::set<std::uintptr_t> &entries,
	std::array<void (*)(), 4> row)
{
	for (const auto entry : row)
		entries.insert(address(entry));
}

#define UML_MIN_MAX_ROW(name) \
	std::array<void (*)(), 4>{ \
		uml_arm64_gadget_integer_##name##_fast_w_r, \
		uml_arm64_gadget_integer_##name##_fast_w_i, \
		uml_arm64_gadget_integer_##name##_fast_x_r, \
		uml_arm64_gadget_integer_##name##_fast_x_i}

[[nodiscard]] std::set<std::uintptr_t> expected_entries()
{
	std::set<std::uintptr_t> entries;
	add_row(entries, UML_MIN_MAX_ROW(smax));
	add_row(entries, UML_MIN_MAX_ROW(umax));
	add_row(entries, UML_MIN_MAX_ROW(smin));
	add_row(entries, UML_MIN_MAX_ROW(umin));
	entries.insert(address(
		uml_arm64_gadget_integer_min_max_copy_w));
	entries.insert(address(
		uml_arm64_gadget_integer_min_max_copy_x));
	return entries;
}

#undef UML_MIN_MAX_ROW

void translation_specialization()
{
	constexpr auto base = engine_tests::engine_test_code_base;
	const auto nop = gadgets::Builder{}.build(
		base, std::array{0xd503'201fU});
	test::require(nop.complete && nop.program.words().size() == 3U,
		"NOP program did not build");

	std::set<std::uintptr_t> selected;
	for (const bool wide : {false, true})
		for (const auto operation : test::operations) {
			const unsigned encoded = 0x81U;
			const auto immediate = gadgets::Builder{}.build(
				base, std::array{test::encode_immediate(
					wide, operation, encoded, 7, 3)});
			std::uint32_t translated = encoded;
			if ((static_cast<unsigned>(operation) & 1U) == 0U)
				translated |= 0xffff'ff00U;
			require_one_operand_stream(immediate,
				std::uintptr_t{7U * 8U} |
					(register_offset(3U) << 16U) |
					(static_cast<std::uintptr_t>(
						 translated)
					 << 32U));
			selected.insert(immediate.program.words().front());

			const auto registers = gadgets::Builder{}.build(
				base, std::array{test::encode_register(
					wide, operation, 7, 3, 5)});
			require_one_operand_stream(registers,
				std::uintptr_t{7U * 8U} |
					(register_offset(3U) << 16U) |
					(register_offset(5U) << 32U));
			selected.insert(registers.program.words().front());
		}

	for (const bool wide : {false, true}) {
		const auto immediate_copy = gadgets::Builder{}.build(
			base, std::array{test::encode_immediate(wide,
				test::Operation::unsigned_maximum, 0, 7,
				3)});
		require_one_operand_stream(immediate_copy,
			std::uintptr_t{7U * 8U} |
				(register_offset(3U) << 16U));
		const auto copy_entry = address(wide ?
			uml_arm64_gadget_integer_min_max_copy_x :
			uml_arm64_gadget_integer_min_max_copy_w);
		test::require(immediate_copy.program.words().front() ==
				copy_entry,
			"UMAX #0 retained a compare/select hot path");
		selected.insert(copy_entry);

		const auto equal_copy = gadgets::Builder{}.build(
			base, std::array{test::encode_register(wide,
				test::Operation::signed_minimum, 7, 3,
				3)});
		require_one_operand_stream(equal_copy,
			std::uintptr_t{7U * 8U} |
				(register_offset(3U) << 16U));
		test::require(equal_copy.program.words().front() == copy_entry,
			"equal min/max sources retained a duplicate load and compare");

		const auto discarded = gadgets::Builder{}.build(
			base, std::array{test::encode_register(wide,
				test::Operation::signed_maximum, 31, 3,
				5)});
		test::require(discarded.complete &&
				discarded.program.words().size() == 3U &&
				discarded.program.words().front() ==
					nop.program.words().front(),
			"discarded min/max result was not folded to NOP");

		const auto inplace_copy = gadgets::Builder{}.build(
			base, std::array{test::encode_immediate(wide,
				test::Operation::unsigned_maximum, 0, 3,
				3)});
		if (wide) {
			test::require(inplace_copy.complete &&
					inplace_copy.program.words().size() ==
						3U &&
					inplace_copy.program.words().front() ==
						nop.program.words().front(),
				"64-bit in-place identity was not folded to NOP");
		} else {
			require_one_operand_stream(inplace_copy,
				std::uintptr_t{3U * 8U} |
					(register_offset(3U) << 16U));
		}
	}

	/* A non-identity ZR source still carries a direct CpuState offset. */
	const auto zero_source = gadgets::Builder{}.build(
		base, std::array{test::encode_register(true,
			test::Operation::signed_maximum, 7, 31, 5)});
	require_one_operand_stream(zero_source,
		std::uintptr_t{7U * 8U} |
			(register_offset(31U) << 16U) |
			(register_offset(5U) << 32U));

	const auto expected = expected_entries();
	test::require(selected == expected,
		"min/max gadget bank has an unselected or missing entry");
	for (const auto entry : expected)
		test::require(branch_free(entry),
			"min/max gadget retained a runtime branch");
}

} // namespace

int main()
{
	translation_specialization();
	return 0;
}
