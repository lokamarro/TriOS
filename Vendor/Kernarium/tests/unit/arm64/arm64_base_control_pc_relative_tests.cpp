// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

void production_specialization()
{
	constexpr auto base = engine_test_code_base + 0xffc;
	constexpr std::uint32_t svc = terminal_system_call;
	const auto build = [&](bool page, std::int64_t immediate,
			       unsigned destination) {
		const std::array instructions{
			encode_pc_relative(page, immediate, destination), svc};
		const auto built =
			gadgets::Builder{}.build(base, instructions);
		require(built.complete,
			"PC-relative production program did not build");
		return std::move(built.program);
	};

	const auto adr_program = build(false, -12345, 7);
	const auto adrp_program = build(true, 12345, 7);
	const auto adr = adr_program.words();
	const auto adrp = adrp_program.words();
	require(adr.size() == 4U && adrp.size() == 4U,
		"PC-relative stream retained unexpected runtime operands");
	/* The destination's offset over the address, in one word. */
	const auto offset = std::uint64_t{
		offsetof(arm64::CpuState, registers) + 7U * 8U};
	require(adr[1] == (offset << 48 |
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(base) - 12345)),
		"ADR fields were not resolved during translation");
	require(adr[0] == adrp[0],
		"ADR and ADRP did not share the constant-store gadget");
	require(adr[1] != adrp[1],
		"ADRP page calculation was not resolved during translation");

	const auto discard_program = build(false, 123, 31);
	const auto discard = discard_program.words();
	const auto nop = gadgets::Builder{}.build(
		base, std::array{std::uint32_t{0xd503'201fU}, svc});
	require(nop.complete && discard.size() == 3U &&
			discard[0] == nop.program.words()[0] &&
			discard[1] == nop.program.words()[1],
		"discarded PC-relative result was not folded to NOP");

	std::set<std::uint64_t> basis_values;
	for (unsigned bit = 0; bit < 21; ++bit) {
		const auto program =
			build(false, std::int64_t{1} << bit, 9);
		basis_values.insert(program.words()[1]);
	}
	require(basis_values.size() == 21U,
		"PC-relative immediate basis bits were lost in translation");
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::production_specialization();
	return 0;
}
