// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>

namespace arm64_simd_indexed_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Encoding {
	indexed,
	vector_long,
	vector_rdm,
};

enum class Operation {
	mla,
	mls,
	mul,
	smlal,
	smlsl,
	smull,
	umlal,
	umlsl,
	umull,
	sqdmlal,
	sqdmlsl,
	sqdmull,
	sqdmulh,
	sqrdmulh,
	sqrdmlah,
	sqrdmlsh,
};

struct Family final {
	Operation operation;
	Encoding encoding;
};

struct Shape final {
	unsigned size;
	bool scalar;
	bool q;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
	unsigned index{};
};

inline constexpr std::array all_families{
	Family{Operation::mla, Encoding::indexed},
	Family{Operation::mls, Encoding::indexed},
	Family{Operation::mul, Encoding::indexed},
	Family{Operation::smlal, Encoding::indexed},
	Family{Operation::smlsl, Encoding::indexed},
	Family{Operation::smull, Encoding::indexed},
	Family{Operation::smlal, Encoding::vector_long},
	Family{Operation::smlsl, Encoding::vector_long},
	Family{Operation::smull, Encoding::vector_long},
	Family{Operation::sqdmlal, Encoding::indexed},
	Family{Operation::sqdmlsl, Encoding::indexed},
	Family{Operation::sqdmlal, Encoding::vector_long},
	Family{Operation::sqdmlsl, Encoding::vector_long},
	Family{Operation::sqdmulh, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::indexed},
	Family{Operation::sqdmull, Encoding::vector_long},
	Family{Operation::sqrdmlah, Encoding::indexed},
	Family{Operation::sqrdmlah, Encoding::vector_rdm},
	Family{Operation::sqrdmlsh, Encoding::indexed},
	Family{Operation::sqrdmlsh, Encoding::vector_rdm},
	Family{Operation::sqrdmulh, Encoding::indexed},
	Family{Operation::umlal, Encoding::indexed},
	Family{Operation::umlal, Encoding::vector_long},
	Family{Operation::umlsl, Encoding::indexed},
	Family{Operation::umlsl, Encoding::vector_long},
	Family{Operation::umull, Encoding::indexed},
	Family{Operation::umull, Encoding::vector_long},
};

inline constexpr std::array indexed_vector_shapes{
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
};

inline constexpr std::array vector_long_shapes{
	Shape{0U, false, false}, Shape{0U, false, true},
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
};

inline constexpr std::array saturating_shapes{
	Shape{1U, false, false}, Shape{1U, false, true},
	Shape{2U, false, false}, Shape{2U, false, true},
	Shape{1U, true, false}, Shape{2U, true, false},
};

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr bool is_unsigned(Operation operation) noexcept
{
	return operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::umull;
}

[[nodiscard]] constexpr bool is_same_width(Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		operation == Operation::mul;
}

[[nodiscard]] constexpr bool is_normal_long(Operation operation) noexcept
{
	return operation == Operation::smlal ||
		operation == Operation::smlsl ||
		operation == Operation::smull ||
		operation == Operation::umlal ||
		operation == Operation::umlsl ||
		operation == Operation::umull;
}

[[nodiscard]] constexpr bool is_rdm(Operation operation) noexcept
{
	return operation == Operation::sqrdmlah ||
		operation == Operation::sqrdmlsh;
}

[[nodiscard]] constexpr bool subtracts(Operation operation) noexcept
{
	return operation == Operation::mls ||
		operation == Operation::smlsl ||
		operation == Operation::umlsl ||
		operation == Operation::sqdmlsl ||
		operation == Operation::sqrdmlsh;
}

[[nodiscard]] constexpr bool indexed(const Family &family) noexcept
{
	return family.encoding == Encoding::indexed;
}

[[nodiscard]] constexpr unsigned indexed_operation(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::mla: return 0U;
	case Operation::mls: return 4U;
	case Operation::mul: return 8U;
	case Operation::smlal:
	case Operation::umlal: return 2U;
	case Operation::smlsl:
	case Operation::umlsl: return 6U;
	case Operation::smull:
	case Operation::umull: return 10U;
	case Operation::sqdmlal: return 3U;
	case Operation::sqdmlsl: return 7U;
	case Operation::sqdmull: return 11U;
	case Operation::sqdmulh: return 12U;
	case Operation::sqrdmulh:
	case Operation::sqrdmlah: return 13U;
	case Operation::sqrdmlsh: return 15U;
	}
	return 0U;
}

[[nodiscard]] constexpr bool indexed_unsigned_group(
	Operation operation) noexcept
{
	return operation == Operation::mla ||
		operation == Operation::mls ||
		is_unsigned(operation) ||
		is_rdm(operation);
}

[[nodiscard]] constexpr unsigned vector_long_operation(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::smlal:
	case Operation::umlal: return 8U;
	case Operation::sqdmlal: return 9U;
	case Operation::smlsl:
	case Operation::umlsl: return 10U;
	case Operation::sqdmlsl: return 11U;
	case Operation::smull:
	case Operation::umull: return 12U;
	case Operation::sqdmull: return 13U;
	default: return 0U;
	}
}

[[nodiscard]] constexpr std::uint32_t instruction(const Family &family,
	Shape shape, Registers registers) noexcept
{
	if (family.encoding == Encoding::vector_rdm)
		return (shape.scalar ? 0x7e00'8400U : 0x2e00'8400U) |
			(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
			(subtracts(family.operation) ? 0x800U : 0U) |
			((shape.size & 3U) << 22) |
			((registers.right & 31U) << 16) |
			((registers.left & 31U) << 5) |
			(registers.destination & 31U);
	if (family.encoding == Encoding::vector_long)
		return (shape.scalar ? 0x5e20'0000U : 0x0e20'0000U) |
			(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
			(is_unsigned(family.operation) ? 0x2000'0000U : 0U) |
			((shape.size & 3U) << 22) |
			(vector_long_operation(family.operation) << 12) |
			((registers.right & 31U) << 16) |
			((registers.left & 31U) << 5) |
			(registers.destination & 31U);

	std::uint32_t encoded =
		(shape.scalar ? 0x5f00'0000U : 0x0f00'0000U) |
		(!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(indexed_unsigned_group(family.operation) ?
			0x2000'0000U : 0U) |
		((shape.size & 3U) << 22) |
		(indexed_operation(family.operation) << 12) |
		((registers.right & 15U) << 16) |
		((registers.left & 31U) << 5) |
		(registers.destination & 31U);
	if (shape.size == 1U) {
		encoded |= (registers.index & 1U) << 20;
		encoded |= (registers.index & 2U) << 20;
		encoded |= (registers.index & 4U) << 9;
	} else if (shape.size == 2U) {
		encoded |= ((registers.right >> 4) & 1U) << 20;
		encoded |= (registers.index & 1U) << 21;
		encoded |= (registers.index & 2U) << 10;
	}
	return encoded;
}

[[nodiscard]] constexpr std::span<const Shape>
allocated_shapes(const Family &family) noexcept
{
	if (is_same_width(family.operation) ||
	    (is_normal_long(family.operation) && indexed(family)))
		return indexed_vector_shapes;
	if (is_normal_long(family.operation))
		return vector_long_shapes;
	return saturating_shapes;
}

[[nodiscard]] constexpr unsigned lane_count(
	const Family &family, Shape shape) noexcept
{
	return indexed(family) ? (shape.size == 1U ? 8U : 4U) : 1U;
}

} // namespace arm64_simd_indexed_test

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_INDEXED_ONE(name, shape, index) \
	void uml_arm64_gadget_simd_indexed_##name##_##shape##_##index();
#define UML_DECLARE_INDEXED_4(name, shape) \
	UML_DECLARE_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_INDEXED_ONE(name, shape, 1) \
	UML_DECLARE_INDEXED_ONE(name, shape, 2) \
	UML_DECLARE_INDEXED_ONE(name, shape, 3)
#define UML_DECLARE_INDEXED_8(name, shape) \
	UML_DECLARE_INDEXED_4(name, shape) \
	UML_DECLARE_INDEXED_ONE(name, shape, 4) \
	UML_DECLARE_INDEXED_ONE(name, shape, 5) \
	UML_DECLARE_INDEXED_ONE(name, shape, 6) \
	UML_DECLARE_INDEXED_ONE(name, shape, 7)
#define UML_DECLARE_INDEXED_SAME(name) \
	UML_DECLARE_INDEXED_8(name, 4h) \
	UML_DECLARE_INDEXED_8(name, 8h) \
	UML_DECLARE_INDEXED_4(name, 2s) \
	UML_DECLARE_INDEXED_4(name, 4s)
#define UML_DECLARE_INDEXED_LONG(name) \
	UML_DECLARE_INDEXED_8(name, h_low) \
	UML_DECLARE_INDEXED_8(name, h_high) \
	UML_DECLARE_INDEXED_4(name, s_low) \
	UML_DECLARE_INDEXED_4(name, s_high)
#define UML_DECLARE_INDEXED_SATURATING(name) \
	UML_DECLARE_INDEXED_8(name, h_low) \
	UML_DECLARE_INDEXED_8(name, h_high) \
	UML_DECLARE_INDEXED_8(name, h_scalar) \
	UML_DECLARE_INDEXED_4(name, s_low) \
	UML_DECLARE_INDEXED_4(name, s_high) \
	UML_DECLARE_INDEXED_4(name, s_scalar)
UML_DECLARE_INDEXED_SAME(mla)
UML_DECLARE_INDEXED_SAME(mls)
UML_DECLARE_INDEXED_SAME(mul)
UML_DECLARE_INDEXED_LONG(smlal)
UML_DECLARE_INDEXED_LONG(smlsl)
UML_DECLARE_INDEXED_LONG(smull)
UML_DECLARE_INDEXED_LONG(umlal)
UML_DECLARE_INDEXED_LONG(umlsl)
UML_DECLARE_INDEXED_LONG(umull)
UML_DECLARE_INDEXED_SATURATING(sqdmlal)
UML_DECLARE_INDEXED_SATURATING(sqdmlsl)
UML_DECLARE_INDEXED_SATURATING(sqdmull)
UML_DECLARE_INDEXED_SATURATING(sqdmulh)
UML_DECLARE_INDEXED_SATURATING(sqrdmulh)
UML_DECLARE_INDEXED_SATURATING(sqrdmlah)
UML_DECLARE_INDEXED_SATURATING(sqrdmlsh)
#undef UML_DECLARE_INDEXED_SATURATING
#undef UML_DECLARE_INDEXED_LONG
#undef UML_DECLARE_INDEXED_SAME
#undef UML_DECLARE_INDEXED_8
#undef UML_DECLARE_INDEXED_4
#undef UML_DECLARE_INDEXED_ONE

#define UML_DECLARE_WIDENING(name) \
	void uml_arm64_gadget_simd_widening_##name##_8b(); \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_16b(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s();
#define UML_DECLARE_SATURATING_LONG(name) \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_h(); \
	void uml_arm64_gadget_simd_widening_##name##_s();
UML_DECLARE_WIDENING(smlal)
UML_DECLARE_WIDENING(smlsl)
UML_DECLARE_WIDENING(smull)
UML_DECLARE_WIDENING(umlal)
UML_DECLARE_WIDENING(umlsl)
UML_DECLARE_WIDENING(umull)
UML_DECLARE_SATURATING_LONG(sqdmlal)
UML_DECLARE_SATURATING_LONG(sqdmlsl)
UML_DECLARE_SATURATING_LONG(sqdmull)
#undef UML_DECLARE_SATURATING_LONG
#undef UML_DECLARE_WIDENING

#define UML_DECLARE_RDM(name) \
	void uml_arm64_gadget_simd_three_same_##name##_4h(); \
	void uml_arm64_gadget_simd_three_same_##name##_2s(); \
	void uml_arm64_gadget_simd_three_same_##name##_8h(); \
	void uml_arm64_gadget_simd_three_same_##name##_4s(); \
	void uml_arm64_gadget_simd_three_same_##name##_h(); \
	void uml_arm64_gadget_simd_three_same_##name##_s();
UML_DECLARE_RDM(sqrdmlah)
UML_DECLARE_RDM(sqrdmlsh)
#undef UML_DECLARE_RDM
void uml_arm64_gadget_simd_rdm_software();
std::uint64_t uml_arm64_simd_rdm_software(
	uml::cpu::arm64::CpuState *, std::uint32_t) noexcept;
}

namespace test = arm64_simd_indexed_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;
using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

#define UML_ADD_INDEXED_ONE(set, name, shape, index) \
	(set).insert(address( \
		uml_arm64_gadget_simd_indexed_##name##_##shape##_##index));
#define UML_ADD_INDEXED_4(set, name, shape) \
	UML_ADD_INDEXED_ONE(set, name, shape, 0) \
	UML_ADD_INDEXED_ONE(set, name, shape, 1) \
	UML_ADD_INDEXED_ONE(set, name, shape, 2) \
	UML_ADD_INDEXED_ONE(set, name, shape, 3)
#define UML_ADD_INDEXED_8(set, name, shape) \
	UML_ADD_INDEXED_4(set, name, shape) \
	UML_ADD_INDEXED_ONE(set, name, shape, 4) \
	UML_ADD_INDEXED_ONE(set, name, shape, 5) \
	UML_ADD_INDEXED_ONE(set, name, shape, 6) \
	UML_ADD_INDEXED_ONE(set, name, shape, 7)
#define UML_ADD_INDEXED_SAME(set, name) \
	UML_ADD_INDEXED_8(set, name, 4h) \
	UML_ADD_INDEXED_8(set, name, 8h) \
	UML_ADD_INDEXED_4(set, name, 2s) \
	UML_ADD_INDEXED_4(set, name, 4s)
#define UML_ADD_INDEXED_LONG(set, name) \
	UML_ADD_INDEXED_8(set, name, h_low) \
	UML_ADD_INDEXED_8(set, name, h_high) \
	UML_ADD_INDEXED_4(set, name, s_low) \
	UML_ADD_INDEXED_4(set, name, s_high)
#define UML_ADD_INDEXED_SATURATING(set, name) \
	UML_ADD_INDEXED_8(set, name, h_low) \
	UML_ADD_INDEXED_8(set, name, h_high) \
	UML_ADD_INDEXED_8(set, name, h_scalar) \
	UML_ADD_INDEXED_4(set, name, s_low) \
	UML_ADD_INDEXED_4(set, name, s_high) \
	UML_ADD_INDEXED_4(set, name, s_scalar)

#define UML_ADD_WIDENING(set, name) \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_8b)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_2s)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_16b)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_8h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4s));
#define UML_ADD_SATURATING_LONG(set, name) \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_2s)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_8h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_4s)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_h)); \
	(set).insert(address(uml_arm64_gadget_simd_widening_##name##_s));
#define UML_ADD_RDM(set, name) \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_4h)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_2s)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_8h)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_4s)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_h)); \
	(set).insert(address(uml_arm64_gadget_simd_three_same_##name##_s));

void add_non_rdm_entries(std::set<std::uintptr_t> &expected,
	std::set<std::uintptr_t> &saturating)
{
	UML_ADD_INDEXED_SAME(expected, mla)
	UML_ADD_INDEXED_SAME(expected, mls)
	UML_ADD_INDEXED_SAME(expected, mul)
	UML_ADD_INDEXED_LONG(expected, smlal)
	UML_ADD_INDEXED_LONG(expected, smlsl)
	UML_ADD_INDEXED_LONG(expected, smull)
	UML_ADD_INDEXED_LONG(expected, umlal)
	UML_ADD_INDEXED_LONG(expected, umlsl)
	UML_ADD_INDEXED_LONG(expected, umull)
	UML_ADD_WIDENING(expected, smlal)
	UML_ADD_WIDENING(expected, smlsl)
	UML_ADD_WIDENING(expected, smull)
	UML_ADD_WIDENING(expected, umlal)
	UML_ADD_WIDENING(expected, umlsl)
	UML_ADD_WIDENING(expected, umull)

	UML_ADD_INDEXED_SATURATING(expected, sqdmlal)
	UML_ADD_INDEXED_SATURATING(expected, sqdmlsl)
	UML_ADD_INDEXED_SATURATING(expected, sqdmull)
	UML_ADD_INDEXED_SATURATING(expected, sqdmulh)
	UML_ADD_INDEXED_SATURATING(expected, sqrdmulh)
	UML_ADD_SATURATING_LONG(expected, sqdmlal)
	UML_ADD_SATURATING_LONG(expected, sqdmlsl)
	UML_ADD_SATURATING_LONG(expected, sqdmull)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmlal)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmlsl)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmull)
	UML_ADD_INDEXED_SATURATING(saturating, sqdmulh)
	UML_ADD_INDEXED_SATURATING(saturating, sqrdmulh)
	UML_ADD_SATURATING_LONG(saturating, sqdmlal)
	UML_ADD_SATURATING_LONG(saturating, sqdmlsl)
	UML_ADD_SATURATING_LONG(saturating, sqdmull)
}

void add_rdm_entries(std::set<std::uintptr_t> &expected,
	std::set<std::uintptr_t> &saturating)
{
	UML_ADD_INDEXED_SATURATING(expected, sqrdmlah)
	UML_ADD_INDEXED_SATURATING(expected, sqrdmlsh)
	UML_ADD_RDM(expected, sqrdmlah)
	UML_ADD_RDM(expected, sqrdmlsh)
	UML_ADD_INDEXED_SATURATING(saturating, sqrdmlah)
	UML_ADD_INDEXED_SATURATING(saturating, sqrdmlsh)
	UML_ADD_RDM(saturating, sqrdmlah)
	UML_ADD_RDM(saturating, sqrdmlsh)
}

#undef UML_ADD_RDM
#undef UML_ADD_SATURATING_LONG
#undef UML_ADD_WIDENING
#undef UML_ADD_INDEXED_SATURATING
#undef UML_ADD_INDEXED_LONG
#undef UML_ADD_INDEXED_SAME
#undef UML_ADD_INDEXED_8
#undef UML_ADD_INDEXED_4
#undef UML_ADD_INDEXED_ONE

struct Body final {
	unsigned instructions{};
	unsigned conditional_branches{};
	unsigned calls{};
};

[[nodiscard]] Body body(std::uintptr_t entry)
{
	Body result{};
	const auto shape = a64::body_shape(entry, {.window = 64U},
		[&](std::uint32_t instruction) {
			result.calls += (instruction & 0xfc00'0000U) == 0x9400'0000U;
		});
	if (!shape.reached_dispatch)
		test::fail("multiply gadget did not reach indirect dispatch");
	result.instructions = shape.instructions + 1U;
	result.conditional_branches = shape.conditional_branches;
	return result;
}

void every_form_uses_direct_offsets_and_no_gadget_is_dead()
{
	const bool native_rdm = test::engine_tests::host_supports(
		test::engine_tests::NativeRequirements::features(
			{::uml::cpu::ArmFeature::advanced_simd,
			 ::uml::cpu::ArmFeature::rdm}));
	std::set<std::uintptr_t> expected;
	std::set<std::uintptr_t> saturating;
	add_non_rdm_entries(expected, saturating);
	if (native_rdm)
		add_rdm_entries(expected, saturating);
	else
		expected.insert(address(
			uml_arm64_gadget_simd_rdm_software));
	test::require(expected.size() == (native_rdm ? 534U : 451U),
		"multiply gadget inventory has unexpected size");
	test::require(saturating.size() == (native_rdm ? 282U : 198U),
		"non-RDM saturating gadget inventory has unexpected size");

	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 15U, 0U};
	std::set<std::uintptr_t> selected;
	for (const auto family : test::all_families)
		for (const auto shape : test::allocated_shapes(family))
			for (unsigned index = 0;
			     index < test::lane_count(family, shape); ++index) {
				auto current = registers;
				current.right =
					shape.size == 1U && test::indexed(family) ?
						15U : 29U;
				current.index = index;
				const auto encoded =
					test::instruction(family, shape, current);
				const auto built = gadgets::Builder{}.build(
					base, std::array{encoded});
				test::require(built.complete,
					"allocated multiply instruction did not build");
				const auto words = built.program.words();
				const bool rdm = test::is_rdm(family.operation);
				if (rdm && !native_rdm) {
					test::require(words.size() >= 2U &&
						words[0] == address(
							uml_arm64_gadget_simd_rdm_software) &&
						words[1] == encoded,
						"non-RDM host did not select exact software helper");
				} else {
					test::require(words.size() == 4U,
						"multiply gadget stream has unexpected shape");
					test::require(words[1] ==
							gadgets::selection::packed_u16_operands(
								vector_offset(current.destination),
								vector_offset(current.left),
								vector_offset(current.right)),
						"register offsets were not packed during translation");
				}
				selected.insert(words[0]);
			}
	test::require(selected == expected,
		"multiply gadget bank contains dead or unselected entries");

	for (const auto entry : selected) {
		if (entry == address(uml_arm64_gadget_simd_rdm_software))
			continue;
		const auto shape = body(entry);
		const bool updates_fpsr = saturating.contains(entry);
		if (updates_fpsr) {
			test::require(shape.conditional_branches == 1U,
				"saturating hot path needs exactly one FP activation branch");
			test::require(shape.calls == 1U,
				"saturating hot path needs exactly one cold activation call");
			test::require(shape.instructions <= 16U,
				"saturating hot path exceeds instruction budget");
		} else {
			test::require(shape.conditional_branches == 0U,
				"ordinary multiply hot path contains a conditional branch");
			test::require(shape.calls == 0U,
				"ordinary multiply hot path contains a call");
			test::require(shape.instructions <= 11U,
				"ordinary multiply hot path exceeds instruction budget");
		}
	}
}

void corrupt_software_metadata_reports_failure_without_mutation()
{
	test::arm64::CpuState cpu{};
	cpu.fpsr = 0x8800'0011U;
	cpu.vectors[3].bytes.fill(std::byte{0x5a});
	const auto vectors = cpu.vectors;
	const auto fpsr = cpu.fpsr;
	test::require(uml_arm64_simd_rdm_software(
		&cpu, 0xffff'ffffU) == 0U,
		"invalid cold-helper metadata did not report failure");
	for (std::size_t index = 0; index < cpu.vectors.size(); ++index)
		test::require(cpu.vectors[index].bytes == vectors[index].bytes,
			"invalid cold-helper metadata changed a vector register");
	test::require(cpu.fpsr == fpsr,
		"invalid cold-helper metadata changed status state");
}

} // namespace

int main()
{
	every_form_uses_direct_offsets_and_no_gadget_is_dead();
	corrupt_software_metadata_reports_failure_without_mutation();
	return 0;
}
