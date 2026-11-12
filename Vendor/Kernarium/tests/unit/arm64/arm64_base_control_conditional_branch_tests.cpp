// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_illegal();

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::int64_t displacement = 0x100;
	const auto target = base + displacement;
	std::set<std::uintptr_t> entries;
	for (unsigned condition = 0; condition < 16; ++condition) {
		const auto instruction = encode_conditional_branch(
			false, condition, displacement);
		const auto built =
			gadgets::Builder{}.build(base, std::array{instruction});
		require(built.complete && built.program.words().size() == 3U,
			"B.cond retained unexpected runtime operands");
		require(built.program.words()[1] == target &&
				built.program.words()[2] == base + 4,
			"B.cond targets were not resolved during translation");
		entries.insert(built.program.words()[0]);

		/* BC.cond needs FEAT_HBC, which the virtual CPU does not
		 * implement. */
		const auto consistent = gadgets::Builder{}.build(
			base, std::array{instruction | 0x10U});
		require(consistent.complete &&
				consistent.program.words().size() == 2U &&
				consistent.program.words()[0] ==
					reinterpret_cast<std::uintptr_t>(
						uml_arm64_gadget_illegal) &&
				consistent.program.words()[1] == base,
			"BC.cond did not select strict illegal");
	}
	require(entries.size() == 15U,
		"B.cond did not select 14 conditions plus the always form");
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::production_specialization();
	return 0;
}
