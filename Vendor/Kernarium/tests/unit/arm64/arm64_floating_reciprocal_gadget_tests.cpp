// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace arm64_floating_reciprocal_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	reciprocal_estimate,
	reciprocal_step,
	reciprocal_exponent,
	reciprocal_sqrt_estimate,
	reciprocal_sqrt_step,
};

struct Form final {
	Operation operation;
	bool scalar;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array widths{16U, 32U, 64U};
inline constexpr std::array vector_shapes{
	std::array{16U, 8U}, std::array{16U, 16U},
	std::array{32U, 8U}, std::array{32U, 16U},
	std::array{64U, 16U},
};

[[nodiscard]] constexpr bool unary(Operation operation) noexcept
{
	return operation != Operation::reciprocal_step &&
		operation != Operation::reciprocal_sqrt_step;
}

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto operation : {
		     Operation::reciprocal_estimate,
		     Operation::reciprocal_step,
		     Operation::reciprocal_exponent,
		     Operation::reciprocal_sqrt_estimate,
		     Operation::reciprocal_sqrt_step,
	     }) {
		for (const auto width : widths)
			function(Form{operation, true, width, 16U});
		if (operation == Operation::reciprocal_exponent)
			continue;
		for (const auto shape : vector_shapes)
			function(Form{operation, false, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr unsigned scalar_shape(unsigned width) noexcept
{
	return width == 16U ? 0U : width == 32U ? 1U : 2U;
}

[[nodiscard]] constexpr unsigned vector_shape(unsigned width,
	unsigned vector_bytes) noexcept
{
	return width == 16U ? (vector_bytes == 16U ? 1U : 0U) :
		width == 32U ? (vector_bytes == 16U ? 3U : 2U) : 4U;
}

/*
 * Direct transcription of the five encoding diagrams. It deliberately neither
 * includes nor calls the production decoder.
 */
[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array scalar{
		std::array{0x5ef9'd800U, 0x5ea1'd800U, 0x5ee1'd800U},
		std::array{0x5e40'3c00U, 0x5e20'fc00U, 0x5e60'fc00U},
		std::array{0x5ef9'f800U, 0x5ea1'f800U, 0x5ee1'f800U},
		std::array{0x7ef9'd800U, 0x7ea1'd800U, 0x7ee1'd800U},
		std::array{0x5ec0'3c00U, 0x5ea0'fc00U, 0x5ee0'fc00U},
	};
	constexpr std::array vector{
		std::array{0x0ef9'd800U, 0x4ef9'd800U, 0x0ea1'd800U,
			0x4ea1'd800U, 0x4ee1'd800U},
		std::array{0x0e40'3c00U, 0x4e40'3c00U, 0x0e20'fc00U,
			0x4e20'fc00U, 0x4e60'fc00U},
		std::array<std::uint32_t, 5>{},
		std::array{0x2ef9'd800U, 0x6ef9'd800U, 0x2ea1'd800U,
			0x6ea1'd800U, 0x6ee1'd800U},
		std::array{0x0ec0'3c00U, 0x4ec0'3c00U, 0x0ea0'fc00U,
			0x4ea0'fc00U, 0x4ee0'fc00U},
	};
	const unsigned operation = static_cast<unsigned>(form.operation);
	std::uint32_t result = form.scalar ?
		scalar[operation][scalar_shape(form.width)] :
		vector[operation][vector_shape(form.width, form.vector_bytes)];
	result |= registers.destination & 31U;
	result |= (registers.first & 31U) << 5;
	if (!unary(form.operation))
		result |= (registers.second & 31U) << 16;
	return result;
}

} // namespace arm64_floating_reciprocal_test

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_RECIPROCAL_SCALAR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d();
#define UML_DECLARE_RECIPROCAL_VECTOR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d();
#define UML_DECLARE_RECIPROCAL_SCALAR_PAIR(name) \
	UML_DECLARE_RECIPROCAL_SCALAR(native, name)
#define UML_DECLARE_RECIPROCAL_VECTOR_PAIR(name) \
	UML_DECLARE_RECIPROCAL_VECTOR(native, name)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frecpe)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frecpe)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frecps)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frecps)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frecpx)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frsqrte)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frsqrte)
UML_DECLARE_RECIPROCAL_SCALAR_PAIR(frsqrts)
UML_DECLARE_RECIPROCAL_VECTOR_PAIR(frsqrts)
#undef UML_DECLARE_RECIPROCAL_VECTOR_PAIR
#undef UML_DECLARE_RECIPROCAL_SCALAR_PAIR
#undef UML_DECLARE_RECIPROCAL_VECTOR
#undef UML_DECLARE_RECIPROCAL_SCALAR
}

namespace test = arm64_floating_reciprocal_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

#define UML_RECIPROCAL_SCALAR(host, name) std::array<Gadget, 3>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d}
#define UML_RECIPROCAL_VECTOR(host, name) std::array<Gadget, 5>{ \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s, \
	uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d}
#define UML_RECIPROCAL_SCALAR_PAIR(name) UML_RECIPROCAL_SCALAR(native, name)
#define UML_RECIPROCAL_VECTOR_PAIR(name) UML_RECIPROCAL_VECTOR(native, name)

constexpr std::array scalar_gadgets{
	UML_RECIPROCAL_SCALAR_PAIR(frecpe),
	UML_RECIPROCAL_SCALAR_PAIR(frecps),
	UML_RECIPROCAL_SCALAR_PAIR(frecpx),
	UML_RECIPROCAL_SCALAR_PAIR(frsqrte),
	UML_RECIPROCAL_SCALAR_PAIR(frsqrts),
};
constexpr std::array vector_gadgets{
	UML_RECIPROCAL_VECTOR_PAIR(frecpe),
	UML_RECIPROCAL_VECTOR_PAIR(frecps),
	std::array<Gadget, 5>{},
	UML_RECIPROCAL_VECTOR_PAIR(frsqrte),
	UML_RECIPROCAL_VECTOR_PAIR(frsqrts),
};

#undef UML_RECIPROCAL_VECTOR_PAIR
#undef UML_RECIPROCAL_SCALAR_PAIR
#undef UML_RECIPROCAL_VECTOR
#undef UML_RECIPROCAL_SCALAR

using uml::testing::fail;
using uml::testing::require;
using uml::emulator::tests::address;
using uml::emulator::tests::emitted_entry;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	const test::Form &form, test::Registers registers)
{
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(test::unary(form.operation) ? 0U :
			(vector_offset(registers.second) << 32U));
}

[[nodiscard]] unsigned conditional_branches(Gadget gadget)
{
	const auto shape = a64::body_shape(reinterpret_cast<std::uintptr_t>(gadget), {.window = 64U});
	if (!shape.reached_dispatch)
		fail("reciprocal gadget did not reach threaded dispatch");
	return shape.conditional_branches;
}

[[nodiscard]] unsigned instruction_count(Gadget gadget)
{
	const auto shape = a64::body_shape(reinterpret_cast<std::uintptr_t>(gadget), {.window = 64U});
	if (!shape.reached_dispatch)
		fail("reciprocal gadget did not reach threaded dispatch");
	return shape.instructions + 1U;
}

[[nodiscard]] Gadget expected_gadget(const test::Form &form)
{
	const unsigned operation = static_cast<unsigned>(form.operation);
	return form.scalar ?
		scalar_gadgets[operation][test::scalar_shape(form.width)] :
		vector_gadgets[operation][
			test::vector_shape(form.width, form.vector_bytes)];
}

[[nodiscard]] std::size_t valid_form_count()
{
	std::size_t count = 0;
	test::for_each_form([&](const test::Form &) {
		++count;
	});
	require(count == 35U,
		"reciprocal form formula omitted an allocated shape");
	return count;
}

void translation_selects_every_shape_and_direct_offset()
{
	constexpr test::Registers registers{31, 30, 29};
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr std::array code_tail{std::uint32_t{0xd400'0001U}};
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	test::for_each_form([&](const test::Form &form) {
		const std::array code{
			test::instruction(form, registers), code_tail[0]};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid reciprocal instruction did not build");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"reciprocal gadget stream is truncated");
		const auto gadget = emitted_entry(address(expected_gadget(form)));
		require(words[0] == gadget,
			"operation or shape remained in the runtime stream");
		require(words[1] == translated_operand(form, registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
		expected.insert(gadget);
	});
	require(expected.size() == valid_form_count(),
		"reciprocal gadget declaration table is incomplete");
	require(selected == expected,
		"reciprocal gadget table contains dead or unselected entries");
}

void hot_paths_have_only_the_required_branches()
{
	std::set<std::uintptr_t> checked;
	unsigned maximum = 0U;
	test::for_each_form([&](const test::Form &form) {
		const auto gadget = expected_gadget(form);
		if (!checked.insert(address(gadget)).second)
			return;
		require(conditional_branches(gadget) == 1U,
			"reciprocal hot path has an unexpected branch");
		const auto instructions = instruction_count(gadget);
		require(instructions <= 17U,
			"reciprocal hot path exceeds its instruction budget");
		maximum = std::max(maximum, instructions);
	});
	require(checked.size() == valid_form_count(),
		"reciprocal hot-path audit missed a gadget");
	require(maximum == 17U,
		"reciprocal hot-path audit did not cover the largest form");
}

} // namespace

int main()
{
	translation_selects_every_shape_and_direct_offset();
	hot_paths_have_only_the_required_branches();
	return 0;
}
