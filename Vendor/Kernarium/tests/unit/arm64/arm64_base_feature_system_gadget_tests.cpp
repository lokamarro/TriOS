// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_feature_system_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>

namespace feature_test = arm64_base_feature_system_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace sys_test = arm64_system_test;

namespace {

void unavailable_aliases_fold_to_one_terminal()
{
	std::uintptr_t illegal_entry = 0;
	for (const auto instruction : {
	     sys_test::system_instruction(false, 6, 7, 0, 0, 0),
	     sys_test::system_instruction(false, 1, 7, 2, 4, 31),
	     sys_test::system_instruction(false, 1, 7, 2, 5, 0),
	     sys_test::system_instruction(false, 3, 7, 2, 7, 31),
	     sys_test::system_instruction(false, 3, 7, 3, 4, 0),
	     sys_test::system_instruction(false, 3, 7, 3, 5, 31),
	     sys_test::system_instruction(false, 3, 7, 3, 6, 0),
	     sys_test::system_instruction(false, 3, 7, 3, 7, 31),
	     }) {
		const auto built = gadgets::Builder{}.build(
			uml::emulator::tests::engine_test_code_base,
			std::array{instruction});
		feature_test::require(built.complete);
		feature_test::require(built.program.instruction_count() == 1);
		feature_test::require(built.program.words().size() == 2);
		if (illegal_entry == 0)
			illegal_entry = built.program.words().front();
		feature_test::require(
			built.program.words().front() == illegal_entry);
	}
	feature_test::require(illegal_entry != 0);
}

} // namespace

int main()
{
	unavailable_aliases_fold_to_one_terminal();
	std::puts("arm64-base-feature-system-gadget: specialization passed");
}
