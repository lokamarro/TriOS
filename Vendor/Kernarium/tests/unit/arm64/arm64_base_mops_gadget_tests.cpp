// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace mops_test {

namespace arm64 = uml::cpu::arm64;

enum class Stage : unsigned {
	prologue,
	main,
	epilogue,
};

[[nodiscard]] constexpr std::uint32_t raw_instruction(unsigned size,
	unsigned family, unsigned stage, unsigned options, unsigned destination,
	unsigned source, unsigned count) noexcept
{
	return 0x1900'0400U | ((size & 3U) << 30) |
		((family & 1U) << 26) | ((stage & 3U) << 22) |
		((source & 31U) << 16) | ((options & 15U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t copy_instruction(bool general,
	Stage stage, unsigned options, unsigned destination, unsigned source,
	unsigned count) noexcept
{
	return raw_instruction(0, general, static_cast<unsigned>(stage), options,
		destination, source, count);
}

[[nodiscard]] constexpr std::uint32_t set_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x19c0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t set_tag_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x1dc0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

using uml::testing::require;

} // namespace mops_test

namespace mops = mops_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

namespace {

constexpr unsigned destination_register = 3;
constexpr unsigned source_register = 4;
constexpr unsigned count_register = 5;

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(
		engine::engine_test_code_base, std::array{instruction});
}

[[nodiscard]] constexpr std::uintptr_t register_offset(
	unsigned index) noexcept
{
	return index == 31U ?
		offsetof(mops::arm64::CpuState, gadget_zero) :
		index * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t operand(unsigned source) noexcept
{
	return register_offset(destination_register) |
		(register_offset(count_register) << 16) |
		(register_offset(source) << 32);
}

[[nodiscard]] bool same_words(const gadgets::BuildResult &left,
	const gadgets::BuildResult &right)
{
	return left.program.words().size() == right.program.words().size() &&
		std::equal(left.program.words().begin(), left.program.words().end(),
			right.program.words().begin());
}

void require_operand(const gadgets::BuildResult &built,
	std::uintptr_t expected, bool carries_pc)
{
	mops::require(built.complete, "MOPS gadget translation completeness");
	mops::require(built.program.instruction_count() == 1,
		"MOPS gadget instruction count");
	mops::require(built.program.words().size() >= (carries_pc ? 3U : 2U),
		"MOPS gadget word count");
	mops::require(built.program.words()[1] == expected,
		"MOPS direct CpuState operand");
	if (carries_pc)
		mops::require(built.program.words()[2] ==
			engine::engine_test_code_base,
			"MOPS fault continuation PC");
}

void copy_option_aliases(std::set<std::uintptr_t> &entries)
{
	for (const bool general : {false, true}) {
		for (const auto stage :
		     {mops::Stage::prologue, mops::Stage::main,
		      mops::Stage::epilogue}) {
			const auto reference = build(mops::copy_instruction(general,
				stage, 0, destination_register, source_register,
				count_register));
			mops::require(reference.complete,
				"copy gadget translation completeness");
			entries.insert(reference.program.words().front());
			for (unsigned options = 1; options < 16; ++options) {
				const auto alias = build(mops::copy_instruction(general,
					stage, options, destination_register,
					source_register, count_register));
				mops::require(alias.complete,
					"copy option alias completeness");
				mops::require(same_words(alias, reference),
					"copy option eliminated at translation");
			}
			if (stage != mops::Stage::main)
				require_operand(reference, operand(source_register),
					stage == mops::Stage::epilogue);
		}
	}
}

void set_option_aliases(std::set<std::uintptr_t> &entries)
{
	for (const auto stage :
	     {mops::Stage::prologue, mops::Stage::main,
	      mops::Stage::epilogue}) {
		for (const unsigned source : {source_register, 31U}) {
			const auto instruction = mops::set_instruction(stage, 0,
				destination_register, count_register, source);
			const auto reference = build(instruction);
			mops::require(reference.complete,
				"set gadget translation completeness");
			entries.insert(reference.program.words().front());
			for (unsigned options = 1; options < 4; ++options) {
				const auto alias = build(mops::set_instruction(stage,
					options, destination_register, count_register,
					source));
				mops::require(alias.complete,
					"set option alias completeness");
				mops::require(same_words(alias, reference),
					"set option eliminated at translation");
			}
			if (stage != mops::Stage::main)
				require_operand(reference, operand(source),
					stage == mops::Stage::epilogue);
		}
	}
}

/* SETGP, SETGM, and SETGE need FEAT_MTE, which the virtual CPU omits. */
void set_tag_forms_are_illegal()
{
	const auto illegal = build(0U);
	for (const auto stage :
	     {mops::Stage::prologue, mops::Stage::main,
	      mops::Stage::epilogue})
		for (const unsigned source : {source_register, 31U})
			for (unsigned options = 0; options < 4; ++options) {
				const auto built = build(mops::set_tag_instruction(
					stage, options, destination_register,
					count_register, source));
				mops::require(built.complete &&
						same_words(built, illegal),
					"SETG form is not UNDEFINED");
			}
}

void translation_specialization()
{
	std::set<std::uintptr_t> entries;
	copy_option_aliases(entries);
	set_option_aliases(entries);
	/*
	 * One common no-op main, three prologues, and four epilogues. No
	 * runtime entry decodes option, family, stage, or XZR choices.
	 */
	mops::require(entries.size() == 8,
		"MOPS specialized gadget entry count");
	set_tag_forms_are_illegal();
}

} // namespace

int main()
{
	translation_specialization();
	std::puts("arm64-base-mops-gadget: specialization passed");
}
