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
void uml_arm64_gadget_cache_zero_r();
void uml_arm64_gadget_cache_zero_z();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_nop();
}

namespace {

void translation_resolves_operation_and_source()
{
	for (const auto &operation : operations::dc_operations)
		for (const unsigned source : {0U, 31U}) {
			const auto instruction = sys_test::system_instruction(false,
				operation.op1, 7, operation.crm, operation.op2,
				source);
			const auto built = gadgets::Builder{}.build(
				engine::engine_test_code_base,
				std::array{instruction});
			feature_test::require(
				built.complete &&
				built.program.instruction_count() == 1U &&
				!built.program.words().empty());
			std::uintptr_t expected;
			if (operation.op1 == 3U && operation.crm == 4U &&
			    operation.op2 == 1U)
				expected = reinterpret_cast<std::uintptr_t>(
					source == 31U ?
						uml_arm64_gadget_cache_zero_z :
						uml_arm64_gadget_cache_zero_r);
			else if (operation.accessible_at_el0)
				expected = reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_nop);
			else
				expected = reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_illegal);
			feature_test::require(
				built.program.words().front() == expected);
		}
}

} // namespace

int main()
{
	translation_resolves_operation_and_source();
	std::puts("arm64-base-dc-gadget: specialization passed");
}
