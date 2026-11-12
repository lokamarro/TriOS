// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>

namespace gadgets = uml::cpu::arm64::gadgets;
namespace hint = arm64_hint_test;

namespace {

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(
		uml::emulator::tests::engine_test_code_base,
		std::array{instruction});
}

void feature_absent_translation_is_the_common_nop()
{
	const auto query = build(hint::primary_instruction(40));
	const auto nop = build(hint::primary_instruction(0));
	hint::require(query.complete && nop.complete,
		"CHKFEAT/NOP translation incomplete");
	hint::require(query.program.instruction_count() == 1 &&
		nop.program.instruction_count() == 1,
		"CHKFEAT/NOP instruction count");
	hint::require(query.program.words().size() ==
			nop.program.words().size() &&
		std::equal(query.program.words().begin(),
			query.program.words().end(),
			nop.program.words().begin()),
		"feature-absent CHKFEAT did not fold to the common NOP");
}

} // namespace

int main()
{
	feature_absent_translation_is_the_common_nop();
	std::puts("arm64-base-chkfeat-gadget: specialization passed");
}
