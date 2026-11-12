// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_basic_floating_test_support.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/detail/floating_horizontal.hpp"
#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <span>
#include <string_view>

namespace arm64_floating_horizontal_test {

namespace basic = arm64_basic_floating_test;
namespace common = uml::testing;
namespace arm64 = uml::cpu::arm64;
namespace detail = uml::cpu::arm64::detail;
namespace engine_tests = uml::emulator::tests;

using Operation = detail::FloatingHorizontalOperation;
using Kind = detail::FloatingHorizontalForm;

struct Form final {
	Operation operation;
	Kind kind;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
};

inline constexpr std::array minmax_operations{
	Operation::maximum, Operation::maximum_number,
	Operation::minimum, Operation::minimum_number};
inline constexpr std::array register_cases{
	Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
	Registers{1, 1, 1}, Registers{31, 30, 29}, Registers{30, 31, 30},
	Registers{30, 15, 30}};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const auto width : basic::widths)
		function(Form{Operation::absolute_difference, Kind::scalar_binary,
			width, 16U});
	for (const auto shape : basic::vector_shapes)
		function(Form{Operation::absolute_difference, Kind::vector_binary,
			shape[0], shape[1]});
	for (const auto width : basic::widths)
		function(Form{Operation::add, Kind::scalar_pairwise, width, 16U});
	for (const auto shape : basic::vector_shapes)
		function(Form{Operation::add, Kind::vector_pairwise,
			shape[0], shape[1]});
	for (const auto operation : minmax_operations) {
		for (const auto width : basic::widths)
			function(Form{operation, Kind::scalar_binary, width, 16U});
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, Kind::vector_binary,
				shape[0], shape[1]});
		for (const auto width : basic::widths)
			function(Form{operation, Kind::scalar_pairwise, width, 16U});
		for (const auto shape : basic::vector_shapes)
			function(Form{operation, Kind::vector_pairwise,
				shape[0], shape[1]});
		for (const auto shape : {std::array{16U, 8U},
			std::array{16U, 16U}, std::array{32U, 16U}})
			function(Form{operation, Kind::reduction, shape[0], shape[1]});
	}
}

[[nodiscard]] constexpr unsigned operation_index(Operation operation) noexcept
{
	return operation == Operation::maximum ? 0U :
		operation == Operation::maximum_number ? 1U :
		operation == Operation::minimum ? 2U : 3U;
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

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2}) noexcept
{
	constexpr std::array fabd_scalar{0x7ec0'1400U, 0x7ea0'd400U,
		0x7ee0'd400U};
	constexpr std::array fabd_vector{0x2ec0'1400U, 0x6ec0'1400U,
		0x2ea0'd400U, 0x6ea0'd400U, 0x6ee0'd400U};
	constexpr std::array faddp_scalar{0x5e30'd800U, 0x7e30'd800U,
		0x7e70'd800U};
	constexpr std::array faddp_vector{0x2e40'1400U, 0x6e40'1400U,
		0x2e20'd400U, 0x6e20'd400U, 0x6e60'd400U};
	constexpr std::array minmax_scalar_binary{
		std::array{0x1ee0'4800U, 0x1e20'4800U, 0x1e60'4800U},
		std::array{0x1ee0'6800U, 0x1e20'6800U, 0x1e60'6800U},
		std::array{0x1ee0'5800U, 0x1e20'5800U, 0x1e60'5800U},
		std::array{0x1ee0'7800U, 0x1e20'7800U, 0x1e60'7800U}};
	constexpr std::array minmax_vector_binary{
		std::array{0x0e40'3400U, 0x4e40'3400U, 0x0e20'f400U,
			0x4e20'f400U, 0x4e60'f400U},
		std::array{0x0e40'0400U, 0x4e40'0400U, 0x0e20'c400U,
			0x4e20'c400U, 0x4e60'c400U},
		std::array{0x0ec0'3400U, 0x4ec0'3400U, 0x0ea0'f400U,
			0x4ea0'f400U, 0x4ee0'f400U},
		std::array{0x0ec0'0400U, 0x4ec0'0400U, 0x0ea0'c400U,
			0x4ea0'c400U, 0x4ee0'c400U}};
	constexpr std::array minmax_scalar_pair{
		std::array{0x5e30'f800U, 0x7e30'f800U, 0x7e70'f800U},
		std::array{0x5e30'c800U, 0x7e30'c800U, 0x7e70'c800U},
		std::array{0x5eb0'f800U, 0x7eb0'f800U, 0x7ef0'f800U},
		std::array{0x5eb0'c800U, 0x7eb0'c800U, 0x7ef0'c800U}};
	constexpr std::array minmax_vector_pair{
		std::array{0x2e40'3400U, 0x6e40'3400U, 0x2e20'f400U,
			0x6e20'f400U, 0x6e60'f400U},
		std::array{0x2e40'0400U, 0x6e40'0400U, 0x2e20'c400U,
			0x6e20'c400U, 0x6e60'c400U},
		std::array{0x2ec0'3400U, 0x6ec0'3400U, 0x2ea0'f400U,
			0x6ea0'f400U, 0x6ee0'f400U},
		std::array{0x2ec0'0400U, 0x6ec0'0400U, 0x2ea0'c400U,
			0x6ea0'c400U, 0x6ee0'c400U}};
	constexpr std::array minmax_reduce{
		std::array{0x0e30'f800U, 0x4e30'f800U, 0x6e30'f800U},
		std::array{0x0e30'c800U, 0x4e30'c800U, 0x6e30'c800U},
		std::array{0x0eb0'f800U, 0x4eb0'f800U, 0x6eb0'f800U},
		std::array{0x0eb0'c800U, 0x4eb0'c800U, 0x6eb0'c800U}};

	std::uint32_t value{};
	if (form.operation == Operation::absolute_difference)
		value = form.kind == Kind::scalar_binary ?
			fabd_scalar[scalar_shape(form.width)] :
			fabd_vector[vector_shape(form.width, form.vector_bytes)];
	else if (form.operation == Operation::add)
		value = form.kind == Kind::scalar_pairwise ?
			faddp_scalar[scalar_shape(form.width)] :
			faddp_vector[vector_shape(form.width, form.vector_bytes)];
	else {
		const unsigned operation = operation_index(form.operation);
		if (form.kind == Kind::scalar_binary)
			value = minmax_scalar_binary[operation][scalar_shape(form.width)];
		else if (form.kind == Kind::vector_binary)
			value = minmax_vector_binary[operation][vector_shape(
				form.width, form.vector_bytes)];
		else if (form.kind == Kind::scalar_pairwise)
			value = minmax_scalar_pair[operation][scalar_shape(form.width)];
		else if (form.kind == Kind::vector_pairwise)
			value = minmax_vector_pair[operation][vector_shape(
				form.width, form.vector_bytes)];
		else
			value = minmax_reduce[operation][form.width == 32U ? 2U :
				form.vector_bytes == 16U ? 1U : 0U];
	}
	value |= registers.destination & 31U;
	value |= (registers.first & 31U) << 5;
	if (form.kind == Kind::scalar_binary ||
	    form.kind == Kind::vector_binary ||
	    form.kind == Kind::vector_pairwise)
		value |= (registers.second & 31U) << 16;
	return value;
}

} // namespace arm64_floating_horizontal_test

namespace a64 = uml::testing::a64;

namespace test = arm64_floating_horizontal_test;

namespace {

namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define DECLARE_SCALAR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_d();
#define DECLARE_VECTOR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2d();
#define DECLARE_BINARY(name) \
	DECLARE_SCALAR(name##_scalar) \
	DECLARE_VECTOR(name##_vector)
	DECLARE_BINARY(fabd)
DECLARE_BINARY(fmax)
DECLARE_BINARY(fmaxnm)
DECLARE_BINARY(fmin)
DECLARE_BINARY(fminnm)
#undef DECLARE_BINARY
#define DECLARE_PAIR(name) \
	DECLARE_SCALAR(name##_scalar) \
	DECLARE_VECTOR(name##_vector)
	DECLARE_PAIR(faddp)
DECLARE_PAIR(fmaxp)
DECLARE_PAIR(fmaxnmp)
DECLARE_PAIR(fminp)
DECLARE_PAIR(fminnmp)
#undef DECLARE_PAIR
#define DECLARE_REDUCE(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s();
DECLARE_REDUCE(fmaxv)
DECLARE_REDUCE(fmaxnmv)
DECLARE_REDUCE(fminv)
DECLARE_REDUCE(fminnmv)
#undef DECLARE_REDUCE
#undef DECLARE_VECTOR
#undef DECLARE_SCALAR
}

using uml::emulator::tests::address;

[[nodiscard]] std::uintptr_t entry_for(const test::Form &form)
{
	const unsigned scalar_shape = test::scalar_shape(form.width);
	const unsigned vector_shape =
		test::vector_shape(form.width, form.vector_bytes);
#define SCALAR(name) std::array<std::uintptr_t, 3>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_d)}
#define VECTOR(name) std::array<std::uintptr_t, 5>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_4h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_8h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_2s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_4s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_2d)}
#define REDUCE(name) std::array<std::uintptr_t, 3>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_4h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_8h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_4s)}
	static const std::array scalar_binary{
		SCALAR(fabd_scalar), std::array<std::uintptr_t, 3>{},
		SCALAR(fmax_scalar), SCALAR(fmaxnm_scalar),
		SCALAR(fmin_scalar), SCALAR(fminnm_scalar)};
	static const std::array vector_binary{
		VECTOR(fabd_vector), std::array<std::uintptr_t, 5>{},
		VECTOR(fmax_vector), VECTOR(fmaxnm_vector),
		VECTOR(fmin_vector), VECTOR(fminnm_vector)};
	static const std::array scalar_pair{
		std::array<std::uintptr_t, 3>{}, SCALAR(faddp_scalar),
		SCALAR(fmaxp_scalar), SCALAR(fmaxnmp_scalar),
		SCALAR(fminp_scalar), SCALAR(fminnmp_scalar)};
	static const std::array vector_pair{
		std::array<std::uintptr_t, 5>{}, VECTOR(faddp_vector),
		VECTOR(fmaxp_vector), VECTOR(fmaxnmp_vector),
		VECTOR(fminp_vector), VECTOR(fminnmp_vector)};
	static const std::array reduction{
		std::array<std::uintptr_t, 3>{},
		std::array<std::uintptr_t, 3>{},
		REDUCE(fmaxv), REDUCE(fmaxnmv),
			REDUCE(fminv), REDUCE(fminnmv)};
	const unsigned operation =
		static_cast<unsigned>(form.operation);
	if (form.kind == test::Kind::scalar_binary)
		return scalar_binary[operation][scalar_shape];
	if (form.kind == test::Kind::vector_binary)
		return vector_binary[operation][vector_shape];
	if (form.kind == test::Kind::scalar_pairwise)
		return scalar_pair[operation][scalar_shape];
	if (form.kind == test::Kind::vector_pairwise)
		return vector_pair[operation][vector_shape];
	const unsigned shape = form.width == 32U ? 2U :
		form.vector_bytes == 16U ? 1U : 0U;
	return reduction[operation][shape];
#undef REDUCE
#undef VECTOR
#undef SCALAR
}

using uml::emulator::tests::vector_offset;

void direct_operands_live_symbols_and_hot_shapes()
{
	std::set<std::uintptr_t> selected;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases) {
			const auto built = gadgets::Builder{}.build(
				test::engine_tests::engine_test_code_base,
				std::array{test::instruction(form, registers)});
			test::common::require(
				built.complete && built.program.words().size() >= 2U);
			const auto entry = entry_for(form);
			test::common::require(built.program.words()[0] == entry);
			const bool uses_second =
				form.kind == test::Kind::scalar_binary ||
				form.kind == test::Kind::vector_binary ||
				form.kind == test::Kind::vector_pairwise;
			test::common::require(built.program.words().size() == 4U);
			test::common::require(built.program.words()[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.first),
					uses_second ?
						vector_offset(registers.second) : 0U));
			selected.insert(entry);
		}
	});
	test::common::require(selected.size() == 92U);
	for (const auto entry : selected) {
		const auto shape = a64::body_shape(entry, {.window = 40U});
		test::common::require(shape.reached_dispatch);
		test::common::require(shape.conditional_branches <= 1U);
		test::common::require(shape.instructions <= 12U);
	}
}

} // namespace

int main()
{
	direct_operands_live_symbols_and_hot_shapes();
}
