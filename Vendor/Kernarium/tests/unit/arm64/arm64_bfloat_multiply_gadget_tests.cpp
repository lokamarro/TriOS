// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_arithmetic_test_support.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_bfloat_multiply_test {

namespace shared = arm64_bfloat_arithmetic_test;
namespace arm64 = shared::arm64;
namespace engine_tests = shared::engine_tests;

enum class Form : unsigned {
	vector_bottom,
	vector_top,
	indexed_bottom,
	indexed_top,
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array forms{
	Form::vector_bottom, Form::vector_top,
	Form::indexed_bottom, Form::indexed_top,
};

[[nodiscard]] constexpr bool top(Form form) noexcept
{
	return form == Form::vector_top || form == Form::indexed_top;
}

[[nodiscard]] constexpr bool by_element(Form form) noexcept
{
	return form == Form::indexed_bottom ||
		form == Form::indexed_top;
}

[[nodiscard]] constexpr unsigned index_count(Form form) noexcept
{
	return by_element(form) ? 8U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	unsigned index = 0U,
	Registers registers = {0U, 1U, 2U}) noexcept
{
	std::uint32_t encoding;
	if (by_element(form)) {
		encoding = 0x0fc0'f000U |
			((index & 3U) << 20U) |
			((index & 4U) << 9U) |
			((registers.second & 15U) << 16U);
	} else {
		encoding = 0x2ec0'fc00U |
			((registers.second & 31U) << 16U);
	}
	return encoding |
		(static_cast<std::uint32_t>(top(form)) << 30U) |
		((registers.first & 31U) << 5U) |
		(registers.destination & 31U);
}

} // namespace arm64_bfloat_multiply_test

extern "C" {
void uml_arm64_gadget_bfloat_multiply_long_native_vector_b();
void uml_arm64_gadget_bfloat_multiply_long_native_vector_t();
#define UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(half, index) \
	void uml_arm64_gadget_bfloat_multiply_long_native_indexed_##half##_##index();
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 0)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 1)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 2)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 3)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 4)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 5)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 6)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(b, 7)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 0)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 1)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 2)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 3)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 4)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 5)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 6)
UML_DECLARE_BFLOAT_MULTIPLY_INDEXED(t, 7)
#undef UML_DECLARE_BFLOAT_MULTIPLY_INDEXED
void uml_arm64_gadget_bfloat_multiply_long_software();
}

namespace test = arm64_bfloat_multiply_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array<Gadget, 18> native_gadgets{
	uml_arm64_gadget_bfloat_multiply_long_native_vector_b,
	uml_arm64_gadget_bfloat_multiply_long_native_vector_t,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_0,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_1,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_2,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_3,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_4,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_5,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_6,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_b_7,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_0,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_1,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_2,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_3,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_4,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_5,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_6,
	uml_arm64_gadget_bfloat_multiply_long_native_indexed_t_7,
};

using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Form form, unsigned index, test::Registers registers)
{
	const unsigned descriptor =
		static_cast<unsigned>(test::top(form)) |
		(static_cast<unsigned>(test::by_element(form)) << 1U) |
		(index << 2U);
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
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	const bool native = host.has(::uml::cpu::ArmFeature::bf16);
	std::set<std::uintptr_t> selected;
	unsigned native_index{};
	for (const auto form : test::forms)
		for (unsigned index = 0U;
		     index < test::index_count(form); ++index) {
			const test::Registers registers{
				31U, 30U,
				test::by_element(form) ? 15U : 29U,
			};
			const std::array code{
				test::instruction(form, index, registers),
				std::uint32_t{0xd400'0001U},
			};
			const auto built =
				gadgets::Builder{}.build(base, code);
			test::shared::require(built.complete,
				"BFMLAL valid block did not build");
			test::shared::require(
				built.program.instruction_count() == 2U,
				"BFMLAL changed block instruction count");
			test::shared::require(
				built.program.accesses_fp_state(),
				"BFMLAL lost FP-state tracking");
			const auto words = built.program.words();
			test::shared::require(words.size() >= 2U,
				"BFMLAL gadget stream is truncated");
			const auto expected = native ?
				native_gadgets[native_index] :
				uml_arm64_gadget_bfloat_multiply_long_software;
			test::shared::require(words[0] == address(expected),
				"BFMLAL host path or shape remained runtime-decoded");
			test::shared::require(words[1] ==
				translated_operand(form, index, registers),
				"BFMLAL registers were not translated to direct offsets");
			selected.insert(words[0]);
			++native_index;
		}
	test::shared::require(native_index == native_gadgets.size(),
		"BFMLAL native shape table is incomplete");
	test::shared::require(selected.size() == (native ? 18U : 1U),
		"BFMLAL translation contains a dead selected duplicate");
}

void every_declared_entry_is_distinct()
{
	std::set<std::uintptr_t> declared{
		address(uml_arm64_gadget_bfloat_multiply_long_software),
	};
	for (const auto gadget : native_gadgets)
		declared.insert(address(gadget));
	test::shared::require(declared.size() == 19U,
		"BFMLAL gadget declarations contain a duplicate");
}

} // namespace

int main()
{
	translation_selects_shape_host_path_and_direct_offsets();
	every_declared_entry_is_distinct();
	return 0;
}
