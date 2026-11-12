// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace arm64_simd_fcvt_precision_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct Pair final {
	unsigned source_width;
	unsigned destination_width;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array pairs{
	Pair{16U, 32U}, Pair{16U, 64U}, Pair{32U, 16U},
	Pair{32U, 64U}, Pair{64U, 16U}, Pair{64U, 32U},
};

[[nodiscard]] constexpr unsigned type_for_width(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Pair pair,
	Registers registers = {28U, 29U}) noexcept
{
	return 0x1e22'4000U |
		(type_for_width(pair.source_width) << 22) |
		(type_for_width(pair.destination_width) << 15) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction_from_types(
	unsigned source_type, unsigned destination_type,
	Registers registers = {28U, 29U}) noexcept
{
	return 0x1e22'4000U | ((source_type & 3U) << 22) |
		((destination_type & 3U) << 15) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_type_pair(unsigned source_type,
	unsigned destination_type) noexcept
{
	return source_type != 2U && destination_type != 2U &&
		source_type != destination_type;
}

[[nodiscard]] constexpr bool belongs_to_bfloat_conversion(
	unsigned source_type, unsigned destination_type) noexcept
{
	return source_type == 1U && destination_type == 2U;
}

} // namespace arm64_simd_fcvt_precision_test

extern "C" {
void uml_arm64_gadget_floating_convert_h_to_s();
void uml_arm64_gadget_floating_convert_h_to_d();
void uml_arm64_gadget_floating_convert_s_to_h();
void uml_arm64_gadget_floating_convert_s_to_d();
void uml_arm64_gadget_floating_convert_d_to_h();
void uml_arm64_gadget_floating_convert_d_to_s();
void uml_arm64_gadget_illegal();
}

namespace test = arm64_simd_fcvt_precision_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array<Gadget, 6> declared_gadgets{
	uml_arm64_gadget_floating_convert_h_to_s,
	uml_arm64_gadget_floating_convert_h_to_d,
	uml_arm64_gadget_floating_convert_s_to_h,
	uml_arm64_gadget_floating_convert_s_to_d,
	uml_arm64_gadget_floating_convert_d_to_h,
	uml_arm64_gadget_floating_convert_d_to_s,
};

using uml::testing::require;
using uml::emulator::tests::address;
using uml::emulator::tests::emitted_entry;
using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Registers registers)
{
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.source) << 16U);
}

void translation_selects_each_precision_pair_and_direct_offsets()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U};
	constexpr std::array tail{std::uint32_t{0xd400'0001U}};
	std::set<std::uintptr_t> selected;
	for (unsigned index = 0U; index < test::pairs.size(); ++index) {
		const std::array code{
			test::instruction(test::pairs[index], registers), tail[0]};
		const auto built = gadgets::Builder{}.build(base, code);
		require(built.complete,
			"valid scalar precision conversion did not build");
		require(built.program.instruction_count() == 2U,
			"scalar precision conversion changed block length");
		require(built.program.accesses_fp_state(),
			"scalar precision conversion lost FP-state tracking");
		const auto words = built.program.words();
		require(words.size() >= 2U,
			"scalar precision gadget stream is truncated");
		require(words[0] == emitted_entry(address(declared_gadgets[index])),
			"precision pair remained undecoded in the runtime stream");
		require(words[1] == translated_operand(registers),
			"register fields were not translated to direct offsets");
		selected.insert(words[0]);
	}
	std::set<std::uintptr_t> declared;
	for (const auto gadget : declared_gadgets)
		declared.insert(emitted_entry(address(gadget)));
	require(declared.size() == declared_gadgets.size(),
		"scalar precision gadget table contains duplicate entries");
	require(selected == declared,
		"scalar precision gadget table contains an unused entry");
}

void reserved_pairs_translate_directly_to_illegal()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr std::array tail{std::uint32_t{0xd400'0001U}};
	for (unsigned source_type = 0U; source_type < 4U; ++source_type)
		for (unsigned destination_type = 0U;
		     destination_type < 4U; ++destination_type) {
			if (test::valid_type_pair(source_type, destination_type) ||
			    test::belongs_to_bfloat_conversion(source_type,
				    destination_type))
				continue;
			const std::array code{
				test::instruction_from_types(source_type,
					destination_type, {31U, 30U}),
				tail[0]};
			const auto built = gadgets::Builder{}.build(base, code);
			require(built.complete,
				"reserved scalar precision conversion did not build");
			const auto words = built.program.words();
			require(!words.empty() &&
					words[0] ==
						address(uml_arm64_gadget_illegal),
				"reserved precision pair reached a runtime decoder");
		}
}

} // namespace

int main()
{
	translation_selects_each_precision_pair_and_direct_offsets();
	reserved_pairs_translate_directly_to_illegal();
	return 0;
}
