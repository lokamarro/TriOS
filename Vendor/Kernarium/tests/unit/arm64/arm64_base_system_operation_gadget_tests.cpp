// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_system_operation_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>

namespace operations = arm64_base_system_operation_test;
namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace sys_test = arm64_system_test;

extern "C" {
void uml_arm64_gadget_illegal();
}

namespace {

void all_profile_illegal_families_fold_at_translation()
{
	for (const auto instruction : {
	     sys_test::system_instruction(false, 0, 1, 2, 3, 4),
	     sys_test::system_instruction(true, 7, 15, 15, 7, 31),
	     sys_test::system_instruction(false, 0, 12, 0, 0, 0),
	     sys_test::system_instruction(false, 0, 12, 0, 1, 31),
	     sys_test::system_instruction(false, 0, 8, 3, 1, 0),
	     operations::system_instruction_pair(0, 8, 1, 1, 0),
	     operations::system_instruction_pair(7, 15, 15, 7, 31),
	     operations::system_register_pair(false, 0, 0, 0, 0, 0, 0),
	     operations::system_register_pair(true, 1, 7, 15, 15, 7, 30),
	     }) {
		const auto built = gadgets::Builder{}.build(
			engine::engine_test_code_base,
			std::array{instruction});
		feature_test::require(
			built.complete &&
			built.program.instruction_count() == 1U &&
			built.program.words().size() == 2U &&
			built.program.words().front() ==
				reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_illegal));
	}
}

} // namespace

int main()
{
	all_profile_illegal_families_fold_at_translation();
	std::puts("arm64-base-system-operation-gadget: specialization passed");
}
