// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::int64_t displacement = 0x1234;
	const auto target = base + displacement;
	const auto branch = gadgets::Builder{}.build(
		base, std::array{encode_direct_branch(false, displacement)});
	const auto link = gadgets::Builder{}.build(
		base, std::array{encode_direct_branch(true, displacement)});
	require(branch.complete && link.complete,
		"direct branch production program did not build");
	require(branch.program.words().size() == 2U &&
			branch.program.words()[1] == target,
		"B target was not resolved during translation");
	require(link.program.words().size() == 3U &&
			link.program.words()[1] == target &&
			link.program.words()[2] == base + 4,
		"BL target/link values were not resolved during translation");
	require(branch.program.words()[0] != link.program.words()[0],
		"B and BL did not select distinct straight-line gadgets");
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::production_specialization();
	return 0;
}
