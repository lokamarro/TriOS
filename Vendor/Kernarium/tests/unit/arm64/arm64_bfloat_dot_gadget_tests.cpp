// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

extern "C" {
void uml_arm64_gadget_bfloat_dot_native_vector_2s();
void uml_arm64_gadget_bfloat_dot_native_vector_4s();
void uml_arm64_gadget_bfloat_dot_native_indexed_2s_0();
void uml_arm64_gadget_bfloat_dot_native_indexed_2s_1();
void uml_arm64_gadget_bfloat_dot_native_indexed_2s_2();
void uml_arm64_gadget_bfloat_dot_native_indexed_2s_3();
void uml_arm64_gadget_bfloat_dot_native_indexed_4s_0();
void uml_arm64_gadget_bfloat_dot_native_indexed_4s_1();
void uml_arm64_gadget_bfloat_dot_native_indexed_4s_2();
void uml_arm64_gadget_bfloat_dot_native_indexed_4s_3();
void uml_arm64_gadget_bfloat_dot_software();
}

namespace test = arm64_bfloat_dot_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array<Gadget, 10> native_gadgets{
	uml_arm64_gadget_bfloat_dot_native_vector_2s,
	uml_arm64_gadget_bfloat_dot_native_vector_4s,
	uml_arm64_gadget_bfloat_dot_native_indexed_2s_0,
	uml_arm64_gadget_bfloat_dot_native_indexed_2s_1,
	uml_arm64_gadget_bfloat_dot_native_indexed_2s_2,
	uml_arm64_gadget_bfloat_dot_native_indexed_2s_3,
	uml_arm64_gadget_bfloat_dot_native_indexed_4s_0,
	uml_arm64_gadget_bfloat_dot_native_indexed_4s_1,
	uml_arm64_gadget_bfloat_dot_native_indexed_4s_2,
	uml_arm64_gadget_bfloat_dot_native_indexed_4s_3,
};

using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Form form, test::Registers registers)
{
	const unsigned descriptor =
		static_cast<unsigned>(form.by_element) |
		(static_cast<unsigned>(form.wide) << 1U) |
		(form.index << 2U);
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U) |
		(static_cast<std::uintptr_t>(descriptor) << 48U);
}

void translation_selects_shape_host_path_and_direct_offsets()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	const bool native = host.has(::uml::cpu::ArmFeature::bf16);
	std::set<std::uintptr_t> selected;
	for (unsigned index = 0U; index < test::forms.size(); ++index) {
		const auto form = test::forms[index];
		const std::array code{
			test::instruction(form, registers),
			std::uint32_t{0xd400'0001U},
		};
		const auto built = gadgets::Builder{}.build(base, code);
		test::shared::require(built.complete,
			"BFDOT valid block did not build");
		test::shared::require(
			built.program.instruction_count() == 2U,
			"BFDOT changed block instruction count");
		test::shared::require(built.program.accesses_fp_state(),
			"BFDOT lost FP-state tracking");
		const auto words = built.program.words();
		test::shared::require(words.size() >= 2U,
			"BFDOT gadget stream is truncated");
		const auto expected = native ? native_gadgets[index] :
			uml_arm64_gadget_bfloat_dot_software;
		test::shared::require(words[0] == address(expected),
			"BFDOT host path or shape remained runtime-decoded");
		test::shared::require(words[1] ==
			translated_operand(form, registers),
			"BFDOT registers were not translated to direct offsets");
		selected.insert(words[0]);
	}
	test::shared::require(selected.size() == (native ? 10U : 1U),
		"BFDOT translation contains a dead selected duplicate");
}

void every_declared_entry_is_distinct()
{
	std::set<std::uintptr_t> declared{
		address(uml_arm64_gadget_bfloat_dot_software),
	};
	for (const auto gadget : native_gadgets)
		declared.insert(address(gadget));
	test::shared::require(declared.size() == 11U,
		"BFDOT gadget declarations contain a duplicate");
}

} // namespace

int main()
{
	translation_selects_shape_host_path_and_direct_offsets();
	every_declared_entry_is_distinct();
	return 0;
}
