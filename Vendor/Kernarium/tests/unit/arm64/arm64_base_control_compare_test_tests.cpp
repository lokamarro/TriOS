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

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::int64_t displacement = 0x100;
	constexpr auto target = base + displacement;
	std::set<std::uintptr_t> compare_entries;
	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true}) {
			const auto built = gadgets::Builder{}.build(
				base, std::array{encode_compare_branch(
					wide, nonzero, displacement, 7)});
			require(built.complete &&
					built.program.words().size() == 4U,
				"compare branch retained unexpected runtime fields");
			require(built.program.words()[1] ==
					offsetof(arm64::CpuState, registers) +
						7U * 8U &&
					built.program.words()[2] == target &&
					built.program.words()[3] == base + 4,
				"compare branch fields were not resolved during "
				"translation");
			compare_entries.insert(built.program.words()[0]);
		}
	require(compare_entries.size() == 4U,
		"compare branch width/condition did not select four gadgets");

	std::set<std::uintptr_t> test_entries;
	for (const bool nonzero : {false, true})
		for (const unsigned bit : {0U, 1U, 31U, 32U, 63U}) {
			const auto built = gadgets::Builder{}.build(
				base, std::array{encode_test_branch(
					nonzero, bit, displacement, 11)});
			require(built.complete &&
					built.program.words().size() == 4U,
				"test branch retained unexpected runtime fields");
			/* The bit in bits 5:0, the register's offset in 31:16. */
			require(built.program.words()[1] ==
					(bit | (offsetof(arm64::CpuState, registers) +
						11U * 8U) << 16) &&
					built.program.words()[2] == target &&
					built.program.words()[3] == base + 4,
				"test branch fields were not resolved during "
				"translation");
			test_entries.insert(built.program.words()[0]);
		}
	require(test_entries.size() == 2U,
		"test branch condition did not select two gadgets");

	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true}) {
			const auto zero = gadgets::Builder{}.build(
				base, std::array{encode_compare_branch(
					wide, nonzero, displacement, 31)});
			require(zero.complete &&
					zero.program.words().size() == 2U &&
					zero.program.words()[1] ==
						(nonzero ? base + 4 : target),
				"XZR compare branch was not folded to a fixed edge");
		}
	for (const bool nonzero : {false, true}) {
		const auto zero = gadgets::Builder{}.build(
			base, std::array{encode_test_branch(
				nonzero, 63, displacement, 31)});
		require(zero.complete && zero.program.words().size() == 2U &&
				zero.program.words()[1] ==
					(nonzero ? base + 4 : target),
			"XZR test branch was not folded to a fixed edge");
	}
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::production_specialization();
	return 0;
}
