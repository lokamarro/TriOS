// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_ic_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace ic = arm64_base_ic_test;
namespace feature_test = arm64_base_feature_system_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

extern "C" {
void uml_arm64_gadget_code_synchronize_r();
void uml_arm64_gadget_code_synchronize_z();
}

namespace {

void translation_resolves_operation_and_profile()
{
	const auto canonical_illegal = gadgets::Builder{}.build(
		engine::engine_test_code_base,
		std::array{arm64_system_test::system_instruction(
			false, 0, 7, 0, 0, 0)});
	feature_test::require(canonical_illegal.complete);
	for (const auto &operation : ic::operations)
		for (const unsigned source : {0U, 1U, 16U, 30U, 31U}) {
			const auto built = gadgets::Builder{}.build(
				engine::engine_test_code_base,
				std::array{ic::instruction(operation, source)});
			feature_test::require(
				built.complete &&
				built.program.instruction_count() == 1U);
			if (operation.accessible_at_el0) {
				const auto expected =
					reinterpret_cast<std::uintptr_t>(
						source == 31U ?
							uml_arm64_gadget_code_synchronize_z :
							uml_arm64_gadget_code_synchronize_r);
				const auto &words = built.program.words();
				feature_test::require(
					words.size() == (source == 31U ? 2U : 3U) &&
					words.front() == expected &&
					words.back() ==
						engine::engine_test_code_base +
							sizeof(std::uint32_t));
				if (source != 31U)
					feature_test::require(
						words[1] ==
						source * sizeof(std::uint64_t));
				continue;
			}
			const auto &canonical = canonical_illegal.program.words();
			feature_test::require(
				built.program.words().size() == canonical.size() &&
				std::equal(built.program.words().begin(),
					built.program.words().end(),
					canonical.begin()));
		}
}

} // namespace

int main()
{
	translation_resolves_operation_and_profile();
	std::puts("arm64-base-ic-gadget: specialization passed");
}
