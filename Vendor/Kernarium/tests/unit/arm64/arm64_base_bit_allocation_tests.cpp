// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace base_bit_test {

[[nodiscard]] constexpr bool allocated(bool wide, unsigned operation) noexcept
{
	if (operation <= 2U)
		return true;
	if (operation == 3U)
		return wide;
	return operation <= 8U;
}

[[nodiscard]] constexpr std::uint32_t instruction(bool wide,
	unsigned operation, unsigned source, unsigned destination) noexcept
{
	return 0x5ac0'0000U | (wide ? 0x8000'0000U : 0U) |
		((operation & 31U) << 10) | ((source & 31U) << 5) |
		(destination & 31U);
}

} // namespace base_bit_test

namespace test = base_bit_test;
namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::testing::require;

void encoding_examples()
{
	require(test::instruction(false, 0, 5, 13) == 0x5ac0'00adU,
		"RBIT W encoding mismatch");
	require(test::instruction(true, 2, 3, 9) == 0xdac0'0869U,
		"REV32 X encoding mismatch");
	require(test::instruction(false, 2, 5, 15) == 0x5ac0'08afU,
		"REV W encoding mismatch");
	require(test::instruction(true, 3, 4, 10) == 0xdac0'0c8aU,
		"REV X encoding mismatch");
	require(test::instruction(true, 8, 31, 31) == 0xdac0'23ffU,
		"optional zero/discard encoding mismatch");
}

void production_specialization()
{
	constexpr auto base = engine_tests::engine_test_code_base;
	constexpr std::uint32_t nop = 0xd503'201fU;
	const auto folded_nop = gadgets::Builder{}.build(
		base, std::array{nop});
	require(folded_nop.complete, "NOP program did not build");

	std::set<std::uintptr_t> entries;
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 9U; ++operation) {
			if (!test::allocated(wide, operation))
				continue;
			const auto regular = gadgets::Builder{}.build(
				base, std::array{test::instruction(
					wide, operation, 3, 7)});
			require(regular.complete &&
					regular.program.words().size() == 4U,
				"ordinary bit operation has unexpected packed stream shape");
			require(regular.program.words()[1] ==
					gadgets::selection::packed_u16_operands(
						offsetof(uml::cpu::arm64::CpuState,
							registers) +
							3U * sizeof(std::uint64_t),
						offsetof(uml::cpu::arm64::CpuState,
							registers) +
							7U * sizeof(std::uint64_t)),
				"register offsets were not packed at translation time");
			entries.insert(regular.program.words().front());

			const auto zero_source = gadgets::Builder{}.build(
				base, std::array{test::instruction(
					wide, operation, 31, 7)});
			require(zero_source.complete &&
					zero_source.program.words().size() == 4U,
				"zero-source bit operation has unexpected stream shape");
			require(zero_source.program.words().front() ==
					regular.program.words().front(),
				"zero source selected a redundant runtime gadget");
			require(zero_source.program.words()[1] ==
					gadgets::selection::packed_u16_operands(
						offsetof(uml::cpu::arm64::CpuState,
							gadget_zero),
						offsetof(uml::cpu::arm64::CpuState,
							registers) +
							7U * sizeof(std::uint64_t)),
				"zero source was not resolved at translation time");

			const auto discarded = gadgets::Builder{}.build(
				base, std::array{test::instruction(
					wide, operation, 3, 31)});
			require(discarded.complete &&
					discarded.program.words().size() == 3U,
				"discarded destination retained runtime operands");
			require(discarded.program.words().front() ==
					folded_nop.program.words().front(),
				"discarded destination was not folded to NOP");
		}
	require(entries.size() == 17U,
		"allocated width/operation forms did not select distinct gadgets");
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
	return 0;
}
