// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <set>
#include <string_view>

namespace arm64_simd_aes_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	encrypt_round = 4,
	decrypt_round = 5,
	mix_columns = 6,
	inverse_mix_columns = 7,
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Variant final {
	Operation operation;
	Registers registers;
};

using uml::testing::fail;
using uml::testing::require;

/*
 * Direct transcription of the four encoding diagrams. The operation field
 * occupies bits 14:12; values 0..3 are reserved by this allocation.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	return 0x4e28'0800U |
		((static_cast<unsigned>(variant.operation) & 7U) << 12) |
		((variant.registers.source & 31U) << 5) |
		(variant.registers.destination & 31U);
}

} // namespace arm64_simd_aes_test

namespace a64 = uml::testing::a64;

extern "C" {
void uml_arm64_gadget_simd_aese_native();
void uml_arm64_gadget_simd_aese_soft();
void uml_arm64_gadget_simd_aesd_native();
void uml_arm64_gadget_simd_aesd_soft();
void uml_arm64_gadget_simd_aesmc_native();
void uml_arm64_gadget_simd_aesmc_soft();
void uml_arm64_gadget_simd_aesimc_native();
void uml_arm64_gadget_simd_aesimc_soft();
}

namespace test = arm64_simd_aes_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array operations{
	test::Operation::encrypt_round,
	test::Operation::decrypt_round,
	test::Operation::mix_columns,
	test::Operation::inverse_mix_columns,
};

using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] Gadget expected_gadget(
	test::Operation operation, bool native)
{
	switch (operation) {
	case test::Operation::encrypt_round:
		return native ? uml_arm64_gadget_simd_aese_native :
			uml_arm64_gadget_simd_aese_soft;
	case test::Operation::decrypt_round:
		return native ? uml_arm64_gadget_simd_aesd_native :
			uml_arm64_gadget_simd_aesd_soft;
	case test::Operation::mix_columns:
		return native ? uml_arm64_gadget_simd_aesmc_native :
			uml_arm64_gadget_simd_aesmc_soft;
	case test::Operation::inverse_mix_columns:
		return native ? uml_arm64_gadget_simd_aesimc_native :
			uml_arm64_gadget_simd_aesimc_soft;
	}
	return nullptr;
}

[[nodiscard]] unsigned instruction_count(Gadget gadget)
{
	const auto shape = a64::body_shape(reinterpret_cast<std::uintptr_t>(gadget), {.window = 32U});
	if (!shape.reached_dispatch)
		test::fail("block-transform gadget did not reach threaded dispatch");
	return shape.instructions + 1U;
}

[[nodiscard]] unsigned conditional_branches(Gadget gadget)
{
	const auto shape = a64::body_shape(reinterpret_cast<std::uintptr_t>(gadget), {.window = 32U});
	if (!shape.reached_dispatch)
		test::fail("block-transform gadget did not reach threaded dispatch");
	return shape.conditional_branches;
}

void translation_specializes_operation_host_path_and_registers()
{
	const bool native =
		::uml::platform::apple::probed_host_capabilities().has(
			::uml::cpu::ArmFeature::advanced_simd) &&
		::uml::platform::apple::probed_host_capabilities().has(
			::uml::cpu::ArmFeature::aes);
	constexpr test::Registers registers{31, 30};
	std::set<std::uintptr_t> selected;
	for (const auto operation : operations) {
		const test::Variant variant{operation, registers};
		const auto built = gadgets::Builder{}.build(
			test::engine_tests::engine_test_code_base,
			std::array{test::instruction(variant)});
		const auto words = built.program.words();
		test::require(built.complete && words.size() == 4U,
			"block-transform gadget stream is truncated");
		test::require(words[0] ==
				address(expected_gadget(operation, native)),
			"operation or host path remained in the runtime stream");
		test::require(words[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.source)),
			"register offsets were not packed during translation");
		selected.insert(words[0]);

		gadgets::BuildOptions options{};
		options.force_software_aes = true;
		const auto portable = gadgets::Builder{}.build(
			test::engine_tests::engine_test_code_base,
			std::array{test::instruction(variant)}, {},
			std::pmr::get_default_resource(), {}, options);
		const auto portable_words = portable.program.words();
		test::require(portable.complete &&
				portable_words.size() == 4U,
			"portable block-transform stream is truncated");
		test::require(portable_words[0] ==
				address(expected_gadget(operation, false)),
			"portable operation remained selected at runtime");
		test::require(portable_words[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.source)),
			"portable register offsets were not packed");
	}
	test::require(selected.size() == operations.size(),
		"translation did not select every current-host gadget");
}

void every_definition_is_reachable_and_branch_free()
{
	std::set<std::uintptr_t> definitions;
	unsigned maximum_native = 0;
	unsigned maximum_software = 0;
	for (const auto operation : operations)
		for (const bool native : {false, true}) {
			const auto gadget = expected_gadget(operation, native);
			definitions.insert(address(gadget));
			test::require(conditional_branches(gadget) == 0U,
				"valid gadget contains a conditional branch");
			auto &maximum =
				native ? maximum_native : maximum_software;
			maximum = std::max(maximum,
				instruction_count(gadget));
		}
	test::require(definitions.size() == 8U,
		"block-transform gadget table has a duplicate entry");
	test::require(maximum_native <= 9U,
		"native block-transform hot path exceeds its instruction budget");
	test::require(maximum_software <= 11U,
		"portable block-transform path exceeds its instruction budget");
}

} // namespace

int main()
{
	translation_specializes_operation_host_path_and_registers();
	every_definition_is_reachable_and_branch_free();
	return 0;
}
