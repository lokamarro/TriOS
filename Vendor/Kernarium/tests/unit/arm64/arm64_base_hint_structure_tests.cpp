// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_base_hint_test_support.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace a64 = uml::testing::a64;

namespace {

namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine_tests = uml::emulator::tests;

extern "C" {
void uml_arm64_gadget_nop();
void uml_arm64_gadget_yield();
void uml_arm64_gadget_dgh();
void uml_arm64_gadget_csdb();
void uml_arm64_gadget_clrbhb();
void uml_arm64_gadget_illegal();
}

using uml::emulator::tests::address;

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(
		engine_tests::engine_test_code_base,
		std::array{instruction});
}

[[nodiscard]] std::uintptr_t expected_primary_entry(unsigned immediate)
{
	switch (immediate) {
	case 1:
		return address(uml_arm64_gadget_yield);
	case 6:
		return address(uml_arm64_gadget_dgh);
	case 20:
		return address(uml_arm64_gadget_csdb);
	case 22:
		return address(uml_arm64_gadget_clrbhb);
	default:
		return address(uml_arm64_gadget_nop);
	}
}

void primary_translation_and_dead_gadget_audit()
{
	std::set<std::uintptr_t> selected;
	for (unsigned immediate = 0; immediate < 128U; ++immediate) {
		if (!arm64_hint_test::is_owned_primary_hint(immediate))
			continue;
		const auto built =
			build(arm64_hint_test::primary_instruction(immediate));
		const auto expected = expected_primary_entry(immediate);
		arm64_hint_test::require(
			built.complete && !built.program.words().empty(),
			"primary HINT did not translate completely");
		arm64_hint_test::require(
			built.program.words().front() == expected,
			"primary HINT did not resolve its operation at translation");
		selected.insert(built.program.words().front());
	}

	const std::set<std::uintptr_t> expected{
		address(uml_arm64_gadget_nop),
		address(uml_arm64_gadget_yield),
		address(uml_arm64_gadget_dgh),
		address(uml_arm64_gadget_csdb),
		address(uml_arm64_gadget_clrbhb),
	};
	arm64_hint_test::require(selected == expected,
		"HINT gadget bank contains an unselected or missing entry");

	for (const auto entry : expected) {
		const auto shape = a64::body_shape(entry, {.window = 32U});
		arm64_hint_test::require(shape.reached_dispatch,
			"HINT gadget did not terminate in the inspection window");
		arm64_hint_test::require(shape.conditional_branches == 0U,
			"HINT gadget retained a runtime conditional branch");
		arm64_hint_test::require(shape.instructions <= 2U,
			"HINT gadget retained work beyond one resolved operation");
	}
}

void pointer_authentication_hints_are_nops()
{
	for (const auto immediate : arm64_hint_test::pointer_auth_hints) {
		const auto built =
			build(arm64_hint_test::primary_instruction(immediate));
		arm64_hint_test::require(
			built.complete && built.program.words().size() >= 1U &&
				built.program.words().front() ==
					address(uml_arm64_gadget_nop),
			"a pointer-authentication hint did not translate to NOP");
	}
}

void gsb_profile_translation()
{
	for (unsigned operation = 0; operation < 2U; ++operation)
		for (const unsigned transfer : {0U, 31U}) {
			const auto gsb = build(
				arm64_hint_test::gsb_instruction(
					operation, transfer));
			arm64_hint_test::require(
				gsb.complete && !gsb.program.words().empty() &&
					gsb.program.words().front() ==
						address(uml_arm64_gadget_illegal),
				"GSB EL0 profile exception was not resolved in translation");
		}
}

} // namespace

int main()
{
	primary_translation_and_dead_gadget_audit();
	pointer_authentication_hints_are_nops();
	gsb_profile_translation();
	return 0;
}
