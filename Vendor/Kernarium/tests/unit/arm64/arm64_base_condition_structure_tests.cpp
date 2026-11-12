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

namespace uml::emulator::tests::base_condition {

enum class SelectOperation : unsigned {
	select,
	increment,
	invert,
	negate,
};

inline constexpr std::array select_operations{
	SelectOperation::select,
	SelectOperation::increment,
	SelectOperation::invert,
	SelectOperation::negate,
};

[[nodiscard]] constexpr std::uint32_t encode_select(
	bool wide, SelectOperation operation, unsigned condition, unsigned rd,
	unsigned rn, unsigned rm)
{
	const auto op = static_cast<unsigned>(operation);
	return 0x1a80'0000U | (wide ? 0x8000'0000U : 0U) |
		((op & 2U) << 29U) | ((rm & 31U) << 16U) |
		((condition & 15U) << 12U) | ((op & 1U) << 10U) |
		((rn & 31U) << 5U) | (rd & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_compare(
	bool wide, bool subtract, bool immediate, unsigned condition,
	unsigned alternative, unsigned rn, unsigned operand)
{
	return 0x3a40'0000U | (wide ? 0x8000'0000U : 0U) |
		(subtract ? 0x4000'0000U : 0U) |
		((operand & 31U) << 16U) | ((condition & 15U) << 12U) |
		(immediate ? 0x0000'0800U : 0U) |
		((rn & 31U) << 5U) | (alternative & 15U);
}

} // namespace uml::emulator::tests::base_condition

namespace a64 = uml::testing::a64;

namespace test = uml::emulator::tests::base_condition;
namespace engine_tests = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define UML_DECLARE_SELECT_ROW(name) \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_sel(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_sel(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_inc(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_inc(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_inv(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_inv(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_neg(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_neg();
UML_DECLARE_SELECT_ROW(eq)
UML_DECLARE_SELECT_ROW(ne)
UML_DECLARE_SELECT_ROW(hs)
UML_DECLARE_SELECT_ROW(lo)
UML_DECLARE_SELECT_ROW(mi)
UML_DECLARE_SELECT_ROW(pl)
UML_DECLARE_SELECT_ROW(vs)
UML_DECLARE_SELECT_ROW(vc)
UML_DECLARE_SELECT_ROW(hi)
UML_DECLARE_SELECT_ROW(ls)
UML_DECLARE_SELECT_ROW(ge)
UML_DECLARE_SELECT_ROW(lt)
UML_DECLARE_SELECT_ROW(gt)
UML_DECLARE_SELECT_ROW(le)
#undef UML_DECLARE_SELECT_ROW
	void uml_arm64_gadget_conditional_select_fast_always_w();
	void uml_arm64_gadget_conditional_select_fast_always_x();

#define UML_DECLARE_COMPARE_ROW(name) \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_i();
UML_DECLARE_COMPARE_ROW(eq)
UML_DECLARE_COMPARE_ROW(ne)
UML_DECLARE_COMPARE_ROW(hs)
UML_DECLARE_COMPARE_ROW(lo)
UML_DECLARE_COMPARE_ROW(mi)
UML_DECLARE_COMPARE_ROW(pl)
UML_DECLARE_COMPARE_ROW(vs)
UML_DECLARE_COMPARE_ROW(vc)
UML_DECLARE_COMPARE_ROW(hi)
UML_DECLARE_COMPARE_ROW(ls)
UML_DECLARE_COMPARE_ROW(ge)
UML_DECLARE_COMPARE_ROW(lt)
UML_DECLARE_COMPARE_ROW(gt)
UML_DECLARE_COMPARE_ROW(le)
UML_DECLARE_COMPARE_ROW(always)
#undef UML_DECLARE_COMPARE_ROW
}

namespace {

using uml::testing::require;
using uml::emulator::tests::address;

[[nodiscard]] constexpr std::uintptr_t register_offset(unsigned index)
{
	return index == 31U ? offsetof(arm64::CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

void require_one_operand_stream(const gadgets::BuildResult &built,
	std::uintptr_t expected)
{
	require(built.complete && built.program.words().size() == 4U,
		"condition instruction has an unexpected gadget stream shape");
	require(built.program.words()[1] == expected,
		"condition operands were not fully resolved at translation time");
}

struct BranchCounts final {
	unsigned conditional{};
	unsigned direct{};
	bool terminal{};
};

[[nodiscard]] BranchCounts branch_counts(std::uintptr_t entry)
{
	BranchCounts result{};
	const auto shape = a64::body_shape(entry, {.window = 64U},
		[&](std::uint32_t instruction) {
			result.direct +=
				(instruction & 0x7c00'0000U) == 0x1400'0000U;
		});
	result.conditional = shape.conditional_branches;
	result.terminal = shape.reached_dispatch;
	return result;
}

void insert_select_row(std::set<std::uintptr_t> &entries,
	std::array<void (*)(), 8> row)
{
	for (const auto entry : row)
		entries.insert(address(entry));
}

#define UML_SELECT_ROW(name) \
	std::array<void (*)(), 8>{ \
		uml_arm64_gadget_conditional_select_fast_##name##_w_sel, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_sel, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_inc, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_inc, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_inv, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_inv, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_neg, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_neg}

[[nodiscard]] std::set<std::uintptr_t> expected_select_entries()
{
	std::set<std::uintptr_t> entries;
	insert_select_row(entries, UML_SELECT_ROW(eq));
	insert_select_row(entries, UML_SELECT_ROW(ne));
	insert_select_row(entries, UML_SELECT_ROW(hs));
	insert_select_row(entries, UML_SELECT_ROW(lo));
	insert_select_row(entries, UML_SELECT_ROW(mi));
	insert_select_row(entries, UML_SELECT_ROW(pl));
	insert_select_row(entries, UML_SELECT_ROW(vs));
	insert_select_row(entries, UML_SELECT_ROW(vc));
	insert_select_row(entries, UML_SELECT_ROW(hi));
	insert_select_row(entries, UML_SELECT_ROW(ls));
	insert_select_row(entries, UML_SELECT_ROW(ge));
	insert_select_row(entries, UML_SELECT_ROW(lt));
	insert_select_row(entries, UML_SELECT_ROW(gt));
	insert_select_row(entries, UML_SELECT_ROW(le));
	entries.insert(address(
		uml_arm64_gadget_conditional_select_fast_always_w));
	entries.insert(address(
		uml_arm64_gadget_conditional_select_fast_always_x));
	return entries;
}

#undef UML_SELECT_ROW

void select_translation_and_dead_gadgets()
{
	constexpr auto base = engine_tests::engine_test_code_base;
	const auto nop = gadgets::Builder{}.build(
		base, std::array{0xd503'201fU});
	require(nop.complete && nop.program.words().size() == 3U,
		"NOP program did not build");

	std::set<std::uintptr_t> selected;
	for (const bool wide : {false, true})
		for (const auto operation : test::select_operations)
			for (unsigned condition = 0; condition < 16U;
			     ++condition) {
				const auto instruction = test::encode_select(
					wide, operation, condition, 7, 3, 5);
				const auto built = gadgets::Builder{}.build(
					base, std::array{instruction});
				const bool unconditional = condition >= 14U;
				const auto expected =
					std::uintptr_t{7U * 8U} |
					(register_offset(3U) << 16U) |
					((unconditional ?
						  std::uintptr_t{0} :
						  register_offset(5U))
					 << 32U);
				require_one_operand_stream(built, expected);
				selected.insert(built.program.words().front());
			}

	/* Equal CSEL sources are also an unconditional copy at translation. */
	for (const bool wide : {false, true}) {
		const auto built = gadgets::Builder{}.build(
			base, std::array{test::encode_select(wide,
				test::SelectOperation::select, 5, 7, 3, 3)});
		require_one_operand_stream(built,
			std::uintptr_t{7U * 8U} |
				(register_offset(3U) << 16U));
		require(built.program.words().front() ==
				address(wide ?
					uml_arm64_gadget_conditional_select_fast_always_x :
					uml_arm64_gadget_conditional_select_fast_always_w),
			"equal CSEL sources retained an NZCV-dependent hot path");
	}

	for (const bool wide : {false, true}) {
		const auto discarded = gadgets::Builder{}.build(
			base, std::array{test::encode_select(wide,
				test::SelectOperation::negate, 5, 31, 3, 5)});
		require(discarded.complete &&
				discarded.program.words().size() == 3U &&
				discarded.program.words().front() ==
					nop.program.words().front(),
			"discarded conditional-select result was not folded to NOP");
	}

	const auto expected = expected_select_entries();
	require(selected == expected,
		"conditional-select gadget bank has an unselected or missing entry");
	for (const auto entry : expected) {
		const auto branches = branch_counts(entry);
		require(branches.terminal && branches.conditional == 0U &&
				branches.direct == 0U,
			"conditional-select gadget retained a runtime branch");
	}
}

void insert_compare_row(std::set<std::uintptr_t> &entries,
	std::array<void (*)(), 8> row)
{
	for (const auto entry : row)
		entries.insert(address(entry));
}

#define UML_COMPARE_ROW(name) \
	std::array<void (*)(), 8>{ \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_i}

[[nodiscard]] std::set<std::uintptr_t> expected_compare_entries()
{
	std::set<std::uintptr_t> entries;
	insert_compare_row(entries, UML_COMPARE_ROW(eq));
	insert_compare_row(entries, UML_COMPARE_ROW(ne));
	insert_compare_row(entries, UML_COMPARE_ROW(hs));
	insert_compare_row(entries, UML_COMPARE_ROW(lo));
	insert_compare_row(entries, UML_COMPARE_ROW(mi));
	insert_compare_row(entries, UML_COMPARE_ROW(pl));
	insert_compare_row(entries, UML_COMPARE_ROW(vs));
	insert_compare_row(entries, UML_COMPARE_ROW(vc));
	insert_compare_row(entries, UML_COMPARE_ROW(hi));
	insert_compare_row(entries, UML_COMPARE_ROW(ls));
	insert_compare_row(entries, UML_COMPARE_ROW(ge));
	insert_compare_row(entries, UML_COMPARE_ROW(lt));
	insert_compare_row(entries, UML_COMPARE_ROW(gt));
	insert_compare_row(entries, UML_COMPARE_ROW(le));
	insert_compare_row(entries, UML_COMPARE_ROW(always));
	return entries;
}

#undef UML_COMPARE_ROW

void compare_translation_and_dead_gadgets()
{
	constexpr auto base = engine_tests::engine_test_code_base;
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> always_entries;
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool immediate : {false, true})
				for (unsigned condition = 0;
				     condition < 16U; ++condition) {
					const unsigned alternative =
						(condition * 5U + 3U) & 15U;
					const unsigned operand =
						immediate ? 29U : 5U;
					const auto built =
						gadgets::Builder{}.build(
							base,
							std::array{
								test::encode_compare(
									wide,
									subtract,
									immediate,
									condition,
									alternative,
									3,
									operand)});
					const auto expected =
						register_offset(3U) |
						((immediate ?
							  std::uintptr_t{
								  operand} :
							  register_offset(
								  operand))
						 << 16U) |
						(static_cast<std::uintptr_t>(
							 alternative)
						 << 60U);
					require_one_operand_stream(
						built, expected);
					selected.insert(
						built.program.words()
							.front());
					if (condition >= 14U)
						always_entries.insert(
							built.program.words()
								.front());
				}

	const auto expected = expected_compare_entries();
	require(selected == expected,
		"conditional-compare gadget bank has an unselected or missing entry");
	require(always_entries.size() == 8U,
		"AL/NV compare forms did not share the eight unconditional gadgets");
	for (const auto entry : expected) {
		const auto branches = branch_counts(entry);
		const bool always = always_entries.contains(entry);
		require(branches.terminal && branches.direct == 0U &&
				branches.conditional == (always ? 0U : 1U),
			always ?
				"unconditional compare retained a runtime branch" :
				"conditional compare has an unexpected branch shape");
	}
}

} // namespace

int main()
{
	select_translation_and_dead_gadgets();
	compare_translation_and_dead_gadgets();
	return 0;
}
