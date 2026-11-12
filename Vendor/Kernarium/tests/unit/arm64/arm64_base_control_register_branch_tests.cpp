// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

constexpr std::array operations{
	RegisterBranch::branch,
	RegisterBranch::branch_link,
	RegisterBranch::return_,
};

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	std::set<std::uintptr_t> entries;
	for (const auto operation : operations)
		for (const unsigned source : {0U, 7U, 30U, 31U}) {
			const auto built = gadgets::Builder{}.build(
				base, std::array{
					encode_register_branch(operation, source)});
			require(built.complete,
				"register branch production program did not build");
			const auto expected_size =
				operation == RegisterBranch::branch_link ? 3U : 2U;
			require(built.program.words().size() == expected_size,
				"register branch retained unexpected runtime fields");
			const auto offset = source == 31U ?
				offsetof(arm64::CpuState, gadget_zero) :
				offsetof(arm64::CpuState, registers) +
					source * 8U;
			require(built.program.words()[1] == offset,
				"register branch source was not resolved during "
				"translation");
			if (operation == RegisterBranch::branch_link)
				require(built.program.words()[2] == base + 4,
					"BLR link value was not resolved during "
					"translation");
			entries.insert(built.program.words()[0]);
		}
	require(entries.size() == 3U,
		"BR, BLR, and RET did not select distinct gadgets");
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::production_specialization();
	return 0;
}
