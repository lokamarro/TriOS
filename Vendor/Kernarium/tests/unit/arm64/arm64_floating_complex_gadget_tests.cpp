// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>

namespace arm64_floating_complex_test {

namespace basic = arm64_basic_floating_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	add,
	multiply_accumulate,
};

struct Form final {
	Operation operation;
	bool by_element;
	unsigned width;
	unsigned vector_bytes;
	unsigned rotation;
	unsigned index;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto shape : basic::vector_shapes)
		for (const unsigned rotation : {1U, 3U})
			function(Form{Operation::add, false, shape[0], shape[1],
				rotation, 0U});
	for (const auto shape : basic::vector_shapes)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, false,
				shape[0], shape[1], rotation, 0U});
	for (unsigned index = 0; index < 2U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				16U, 8U, rotation, index});
	for (unsigned index = 0; index < 4U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				16U, 16U, rotation, index});
	for (unsigned index = 0; index < 2U; ++index)
		for (unsigned rotation = 0; rotation < 4U; ++rotation)
			function(Form{Operation::multiply_accumulate, true,
				32U, 16U, rotation, index});
}

[[nodiscard]] constexpr unsigned vector_shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array add{
		0x2e40'e400U, 0x6e40'e400U, 0x2e80'e400U,
		0x6e80'e400U, 0x6ec0'e400U,
	};
	constexpr std::array multiply{
		0x2e40'c400U, 0x6e40'c400U, 0x2e80'c400U,
		0x6e80'c400U, 0x6ec0'c400U,
	};
	std::uint32_t value;
	if (form.operation == Operation::add) {
		value = add[vector_shape(form.width, form.vector_bytes)];
		if (form.rotation == 3U)
			value |= 0x1000U;
	} else if (!form.by_element) {
		value = multiply[vector_shape(form.width, form.vector_bytes)] |
			(form.rotation << 11);
	} else {
		value = form.width == 32U ? 0x6f80'1000U :
			form.vector_bytes == 16U ? 0x6f40'1000U :
			0x2f40'1000U;
		if (form.width == 16U) {
			value |= (form.index & 1U) << 21;
			value |= (form.index >> 1) << 11;
		} else {
			value |= form.index << 11;
		}
		value |= form.rotation << 13;
	}
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	if (form.by_element) {
		value |= (registers.second & 15U) << 16;
		value |= ((registers.second >> 4) & 1U) << 20;
	} else {
		value |= (registers.second & 31U) << 16;
	}
	return value;
}

using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t gadget_operand(
	Registers registers) noexcept
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U);
}

} // namespace arm64_floating_complex_test

extern "C" {
#define UML_DECLARE_ROTATIONS(name) \
	void uml_arm64_gadget_floating_complex_##name##_0(); \
	void uml_arm64_gadget_floating_complex_##name##_90(); \
	void uml_arm64_gadget_floating_complex_##name##_180(); \
	void uml_arm64_gadget_floating_complex_##name##_270();
#define UML_DECLARE_ADD(shape) \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_90(); \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_270();
UML_DECLARE_ADD(4h)
UML_DECLARE_ADD(8h)
UML_DECLARE_ADD(2s)
UML_DECLARE_ADD(4s)
UML_DECLARE_ADD(2d)
UML_DECLARE_ROTATIONS(fcmla_4h)
UML_DECLARE_ROTATIONS(fcmla_8h)
UML_DECLARE_ROTATIONS(fcmla_2s)
UML_DECLARE_ROTATIONS(fcmla_4s)
UML_DECLARE_ROTATIONS(fcmla_2d)
UML_DECLARE_ROTATIONS(fcmla_element_4h_0)
UML_DECLARE_ROTATIONS(fcmla_element_4h_1)
UML_DECLARE_ROTATIONS(fcmla_element_8h_0)
UML_DECLARE_ROTATIONS(fcmla_element_8h_1)
UML_DECLARE_ROTATIONS(fcmla_element_8h_2)
UML_DECLARE_ROTATIONS(fcmla_element_8h_3)
UML_DECLARE_ROTATIONS(fcmla_element_4s_0)
UML_DECLARE_ROTATIONS(fcmla_element_4s_1)
#undef UML_DECLARE_ADD
#undef UML_DECLARE_ROTATIONS
}

namespace test = arm64_floating_complex_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;
using uml::testing::require;
using uml::emulator::tests::address;

[[nodiscard]] Gadget expected_gadget(const test::Form &form)
{
#define UML_ADD_PAIR(shape) std::array<Gadget, 2>{ \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_90, \
	uml_arm64_gadget_floating_complex_fcadd_##shape##_270}
	static constexpr std::array add{
		UML_ADD_PAIR(4h),
		UML_ADD_PAIR(8h),
		UML_ADD_PAIR(2s),
		UML_ADD_PAIR(4s),
		UML_ADD_PAIR(2d),
	};
#undef UML_ADD_PAIR
#define UML_ROTATIONS(name) std::array<Gadget, 4>{ \
	uml_arm64_gadget_floating_complex_##name##_0, \
	uml_arm64_gadget_floating_complex_##name##_90, \
	uml_arm64_gadget_floating_complex_##name##_180, \
	uml_arm64_gadget_floating_complex_##name##_270}
	static constexpr std::array multiply{
		UML_ROTATIONS(fcmla_4h),
		UML_ROTATIONS(fcmla_8h),
		UML_ROTATIONS(fcmla_2s),
		UML_ROTATIONS(fcmla_4s),
		UML_ROTATIONS(fcmla_2d),
	};
	static constexpr std::array half64{
		UML_ROTATIONS(fcmla_element_4h_0),
		UML_ROTATIONS(fcmla_element_4h_1),
	};
	static constexpr std::array half128{
		UML_ROTATIONS(fcmla_element_8h_0),
		UML_ROTATIONS(fcmla_element_8h_1),
		UML_ROTATIONS(fcmla_element_8h_2),
		UML_ROTATIONS(fcmla_element_8h_3),
	};
	static constexpr std::array single128{
		UML_ROTATIONS(fcmla_element_4s_0),
		UML_ROTATIONS(fcmla_element_4s_1),
	};
#undef UML_ROTATIONS
	const unsigned shape =
		test::vector_shape(form.width, form.vector_bytes);
	if (form.operation == test::Operation::add)
		return add[shape][form.rotation == 3U ? 1U : 0U];
	if (!form.by_element)
		return multiply[shape][form.rotation];
	if (form.width == 32U)
		return single128[form.index][form.rotation];
	return (form.vector_bytes == 16U ?
		half128[form.index] : half64[form.index])[form.rotation];
}

void translation_selects_every_exact_hot_path()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31, 30, 29};
	std::set<std::uintptr_t> selected;
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		const std::array code{
			test::instruction(form, registers),
			std::uint32_t{0xd400'0001U},
		};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid complex instruction did not build");
		require(built.program.instruction_count() == 2U,
			"complex instruction changed translated block length");
		require(built.program.accesses_fp_state(),
			"complex instruction lost FP-state tracking");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"complex gadget stream is truncated");
		const auto gadget = expected_gadget(form);
		require(words[0] == address(gadget),
			"shape, rotation, or element remained runtime-decoded");
		require(words[1] == test::gadget_operand(registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
		++forms;
	});
	require(forms == 62U,
		"complex form inventory changed unexpectedly");
	require(selected.size() == 62U,
		"complex gadget declarations contain a dead duplicate");
}

} // namespace

int main()
{
	translation_selects_every_exact_hot_path();
}
