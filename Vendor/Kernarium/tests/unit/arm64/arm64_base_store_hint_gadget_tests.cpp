// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_base_hint_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace arm64_base_store_hint_test {

/* The hint immediates of STSHH KEEP and STSHH STRM (FEAT_PCDPHINT) and of
 * STCPH (FEAT_CMH). */
inline constexpr std::array immediates{48U, 49U, 52U};

} // namespace arm64_base_store_hint_test

namespace a64 = uml::testing::a64;

namespace store_hint = arm64_base_store_hint_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

extern "C" void uml_arm64_gadget_nop();

namespace {

void all_forms_fold_to_nop()
{
	const auto nop = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_nop);
	const auto nop_program = gadgets::Builder{}.build(
		engine::engine_test_code_base,
		std::array{arm64_hint_test::primary_instruction(0)});
	arm64_hint_test::require(nop_program.complete,
		"common NOP did not translate completely");
	for (const auto immediate : store_hint::immediates) {
		const auto built = gadgets::Builder{}.build(
			engine::engine_test_code_base,
			std::array{
				arm64_hint_test::primary_instruction(immediate)});
		arm64_hint_test::require(
			built.complete && built.program.instruction_count() == 1U,
			"store hint did not translate completely");
		arm64_hint_test::require(
			built.program.words().size() ==
				nop_program.program.words().size() &&
				std::equal(built.program.words().begin(),
					built.program.words().end(),
					nop_program.program.words().begin()) &&
				built.program.words().front() == nop,
			"store hint retained runtime decode or a dedicated gadget");
	}

	const auto shape = a64::body_shape(nop, {.window = 4U});
	arm64_hint_test::require(shape.conditional_branches == 0U,
		"NOP gadget contains a conditional branch");
	arm64_hint_test::require(shape.instructions <= 1U,
		"NOP gadget contains unexpected runtime work");
}

} // namespace

int main()
{
	all_forms_fold_to_nop();
}
