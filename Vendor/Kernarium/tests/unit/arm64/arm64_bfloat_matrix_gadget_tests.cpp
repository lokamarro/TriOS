// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_bfloat_matrix_test {

namespace dot = arm64_bfloat_dot_test;
namespace shared = dot::shared;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

using Registers = dot::Registers;

[[nodiscard]] constexpr std::uint32_t instruction(
	Registers registers = {0U, 1U, 2U}) noexcept
{
	return 0x6e40'ec00U |
		((registers.second & 31U) << 16U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

} // namespace arm64_bfloat_matrix_test

extern "C" {
void uml_arm64_gadget_bfloat_matrix_native();
void uml_arm64_gadget_bfloat_matrix_software();
}

namespace test = arm64_bfloat_matrix_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;
using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Registers registers)
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U);
}

void translation_selects_host_path_and_direct_offsets()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	const std::array code{
		test::instruction(registers),
		std::uint32_t{0xd400'0001U},
	};
	const auto built = gadgets::Builder{}.build(base, code);
	test::shared::require(built.complete,
		"BFMMLA valid block did not build");
	test::shared::require(built.program.instruction_count() == 2U,
		"BFMMLA changed block instruction count");
	test::shared::require(built.program.accesses_fp_state(),
		"BFMMLA lost FP-state tracking");
	const auto words = built.program.words();
	test::shared::require(words.size() >= 2U,
		"BFMMLA gadget stream is truncated");
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	const bool native = host.has(::uml::cpu::ArmFeature::bf16);
	const auto expected = native ?
		uml_arm64_gadget_bfloat_matrix_native :
		uml_arm64_gadget_bfloat_matrix_software;
	test::shared::require(words[0] == address(expected),
		"BFMMLA host path remained runtime-decoded");
	test::shared::require(words[1] == translated_operand(registers),
		"BFMMLA registers were not translated to direct offsets");
}

void every_declared_entry_is_distinct()
{
	const std::set<std::uintptr_t> declared{
		address(uml_arm64_gadget_bfloat_matrix_native),
		address(uml_arm64_gadget_bfloat_matrix_software),
	};
	test::shared::require(declared.size() == 2U,
		"BFMMLA gadget declarations contain a duplicate");
}

} // namespace

int main()
{
	translation_selects_host_path_and_direct_offsets();
	every_declared_entry_is_distinct();
	return 0;
}
