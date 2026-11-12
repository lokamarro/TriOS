// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_bfloat_conversion_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Form : unsigned { scalar, lower, upper };

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array forms{Form::scalar, Form::lower, Form::upper};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U}) noexcept
{
	const std::uint32_t base = form == Form::scalar ? 0x1e63'4000U :
		form == Form::upper ? 0x4ea1'6800U : 0x0ea1'6800U;
	return base | (registers.destination & 31U) |
		((registers.source & 31U) << 5);
}

} // namespace arm64_bfloat_conversion_test

extern "C" {
void uml_arm64_gadget_bfloat_convert_scalar_native();
void uml_arm64_gadget_bfloat_convert_lower_native();
void uml_arm64_gadget_bfloat_convert_upper_native();
void uml_arm64_gadget_bfloat_convert_software();
}

namespace test = arm64_bfloat_conversion_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array<Gadget, 3> native_gadgets{
	uml_arm64_gadget_bfloat_convert_scalar_native,
	uml_arm64_gadget_bfloat_convert_lower_native,
	uml_arm64_gadget_bfloat_convert_upper_native,
};

using uml::testing::require;
using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Form form, test::Registers registers)
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.source) << 16U) |
		(static_cast<std::uintptr_t>(form == test::Form::scalar) << 32U) |
		(static_cast<std::uintptr_t>(form == test::Form::upper) << 33U);
}

void translation_selects_shape_host_path_and_direct_offsets()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U};
	constexpr std::array tail{std::uint32_t{0xd400'0001U}};
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	const bool native = host.has(::uml::cpu::ArmFeature::bf16);
	std::set<std::uintptr_t> selected;
	for (unsigned index = 0U; index < test::forms.size(); ++index) {
		const auto form = test::forms[index];
		const std::array code{
			test::instruction(form, registers), tail[0]};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid reduced-precision conversion did not build");
		require(built.program.instruction_count() == 2U,
			"reduced-precision conversion changed block length");
		require(built.program.accesses_fp_state(),
			"reduced-precision conversion lost FP-state tracking");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"reduced-precision gadget stream is truncated");
		const auto expected = native ? native_gadgets[index] :
			uml_arm64_gadget_bfloat_convert_software;
		require(words[0] == address(expected),
			"host path or conversion shape remained runtime-decoded");
		require(words[1] == translated_operand(form, registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
	}
	require(selected.size() == (native ? 3U : 1U),
		"conversion gadget selection contains a dead duplicate");
}

void every_declared_entry_is_distinct_and_owned()
{
	std::set<std::uintptr_t> declared{
		address(uml_arm64_gadget_bfloat_convert_software)};
	for (const auto gadget : native_gadgets)
		declared.insert(address(gadget));
	require(declared.size() == 4U,
		"conversion gadget declarations contain an unused duplicate");
}

} // namespace

int main()
{
	translation_selects_shape_host_path_and_direct_offsets();
	every_declared_entry_is_distinct_and_owned();
	return 0;
}
