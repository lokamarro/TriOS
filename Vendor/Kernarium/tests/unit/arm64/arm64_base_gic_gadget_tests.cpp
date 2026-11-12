// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_system_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace arm64_base_gic_test {

using uml::testing::require;

} // namespace arm64_base_gic_test

namespace gic = arm64_base_gic_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace sys_test = arm64_system_test;

namespace {

void feature_disabled_aliases_fold_to_illegal()
{
	std::uintptr_t illegal_entry = 0;
	for (const auto instruction : {
	     sys_test::system_instruction(false, 0, 12, 1, 0, 0),
	     sys_test::system_instruction(false, 6, 12, 2, 1, 31),
	     sys_test::system_instruction(true, 0, 12, 3, 0, 0),
	     sys_test::system_instruction(true, 0, 12, 3, 1, 31),
	     }) {
		const auto built = gadgets::Builder{}.build(
			uml::emulator::tests::engine_test_code_base,
			std::array{instruction});
		gic::require(built.complete);
		gic::require(built.program.instruction_count() == 1);
		gic::require(built.program.words().size() == 2);
		if (illegal_entry == 0)
			illegal_entry = built.program.words().front();
		gic::require(built.program.words().front() == illegal_entry);
	}
	gic::require(illegal_entry != 0);
}

} // namespace

int main()
{
	feature_disabled_aliases_fold_to_illegal();
	std::puts("arm64-base-gic-gadget: specialization passed");
}
