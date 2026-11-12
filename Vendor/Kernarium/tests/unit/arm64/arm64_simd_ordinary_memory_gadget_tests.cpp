// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_gadget_test_support.hpp"
#include "arm64_simd_ordinary_memory_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>

namespace a64 = uml::testing::a64;

namespace memory = arm64_simd_ordinary_memory_test;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define UML_DECLARE_UNSIGNED(width) \
	void uml_arm64_gadget_vector_load##width##_unsigned_fast(); \
	void uml_arm64_gadget_vector_load##width##_unsigned_fast_sp(); \
	void uml_arm64_gadget_vector_store##width##_unsigned_fast(); \
	void uml_arm64_gadget_vector_store##width##_unsigned_fast_sp();
UML_DECLARE_UNSIGNED(8)
UML_DECLARE_UNSIGNED(16)
UML_DECLARE_UNSIGNED(32)
UML_DECLARE_UNSIGNED(64)
UML_DECLARE_UNSIGNED(128)
#undef UML_DECLARE_UNSIGNED

#define UML_DECLARE_OTHER_MODE(name, mode) \
	void uml_arm64_gadget_vector_other_##name##_##mode(); \
	void uml_arm64_gadget_vector_other_##name##_##mode##_sp();
#define UML_DECLARE_OTHER(name) \
	UML_DECLARE_OTHER_MODE(name, offset) \
	UML_DECLARE_OTHER_MODE(name, pre) \
	UML_DECLARE_OTHER_MODE(name, post) \
	UML_DECLARE_OTHER_MODE(name, uxtw_0) \
	UML_DECLARE_OTHER_MODE(name, x_0) \
	UML_DECLARE_OTHER_MODE(name, sxtw_0) \
	UML_DECLARE_OTHER_MODE(name, uxtw_1) \
	UML_DECLARE_OTHER_MODE(name, x_1) \
	UML_DECLARE_OTHER_MODE(name, sxtw_1)
UML_DECLARE_OTHER(store8)
UML_DECLARE_OTHER(load8)
UML_DECLARE_OTHER(store16)
UML_DECLARE_OTHER(load16)
UML_DECLARE_OTHER(store32)
UML_DECLARE_OTHER(load32)
UML_DECLARE_OTHER(store64)
UML_DECLARE_OTHER(load64)
UML_DECLARE_OTHER(store128)
UML_DECLARE_OTHER(load128)
#undef UML_DECLARE_OTHER
#undef UML_DECLARE_OTHER_MODE

#define UML_DECLARE_PAIR(name) \
	void uml_arm64_gadget_vector_pair_##name##_0(); \
	void uml_arm64_gadget_vector_pair_##name##_1(); \
	void uml_arm64_gadget_vector_pair_##name##_2(); \
	void uml_arm64_gadget_vector_pair_##name##_0_sp(); \
	void uml_arm64_gadget_vector_pair_##name##_1_sp(); \
	void uml_arm64_gadget_vector_pair_##name##_2_sp();
UML_DECLARE_PAIR(store32)
UML_DECLARE_PAIR(load32)
UML_DECLARE_PAIR(store64)
UML_DECLARE_PAIR(load64)
UML_DECLARE_PAIR(store128)
UML_DECLARE_PAIR(load128)
#undef UML_DECLARE_PAIR
}

namespace {

using uml::emulator::tests::Gadget;
using Bases = std::array<Gadget, 2>;
using Widths = std::array<Bases, 5>;

using uml::testing::fail;
using uml::testing::require;
using uml::emulator::tests::address;

#define UML_BASES(name) Bases{ \
	uml_arm64_gadget_vector_##name##_unsigned_fast, \
	uml_arm64_gadget_vector_##name##_unsigned_fast_sp}
constexpr std::array unsigned_paths{
	Widths{
		UML_BASES(store8), UML_BASES(store16), UML_BASES(store32),
		UML_BASES(store64), UML_BASES(store128)},
	Widths{
		UML_BASES(load8), UML_BASES(load16), UML_BASES(load32),
		UML_BASES(load64), UML_BASES(load128)},
};
#undef UML_BASES

using ImmediateModes = std::array<Bases, 3>;
using RegisterModes = std::array<Bases, 6>;

struct OtherPaths final {
	ImmediateModes immediate;
	RegisterModes register_offset;
};

#define UML_OTHER_BASES(name, mode) Bases{ \
	uml_arm64_gadget_vector_other_##name##_##mode, \
	uml_arm64_gadget_vector_other_##name##_##mode##_sp}
#define UML_OTHER_PATHS(name) OtherPaths{ \
	ImmediateModes{ \
		UML_OTHER_BASES(name, offset), \
		UML_OTHER_BASES(name, pre), \
		UML_OTHER_BASES(name, post)}, \
	RegisterModes{ \
		UML_OTHER_BASES(name, uxtw_0), \
		UML_OTHER_BASES(name, x_0), \
		UML_OTHER_BASES(name, sxtw_0), \
		UML_OTHER_BASES(name, uxtw_1), \
		UML_OTHER_BASES(name, x_1), \
		UML_OTHER_BASES(name, sxtw_1)}}
constexpr std::array other_paths{
	std::array{
		UML_OTHER_PATHS(store8),
		UML_OTHER_PATHS(store16),
		UML_OTHER_PATHS(store32),
		UML_OTHER_PATHS(store64),
		UML_OTHER_PATHS(store128),
	},
	std::array{
		UML_OTHER_PATHS(load8),
		UML_OTHER_PATHS(load16),
		UML_OTHER_PATHS(load32),
		UML_OTHER_PATHS(load64),
		UML_OTHER_PATHS(load128),
	},
};
#undef UML_OTHER_PATHS
#undef UML_OTHER_BASES

using PairModes = std::array<Bases, 3>;
using PairWidths = std::array<PairModes, 3>;

#define UML_PAIR_BASES(name, mode) Bases{ \
	uml_arm64_gadget_vector_pair_##name##_##mode, \
	uml_arm64_gadget_vector_pair_##name##_##mode##_sp}
#define UML_PAIR_PATHS(name) PairModes{ \
	UML_PAIR_BASES(name, 0), \
	UML_PAIR_BASES(name, 1), \
	UML_PAIR_BASES(name, 2)}
constexpr std::array pair_paths{
	PairWidths{
		UML_PAIR_PATHS(store32),
		UML_PAIR_PATHS(store64),
		UML_PAIR_PATHS(store128),
	},
	PairWidths{
		UML_PAIR_PATHS(load32),
		UML_PAIR_PATHS(load64),
		UML_PAIR_PATHS(load128),
	},
};
#undef UML_PAIR_PATHS
#undef UML_PAIR_BASES

[[nodiscard]] constexpr unsigned width_index(std::size_t bytes)
{
	return bytes == 1U ? 0U :
		bytes == 2U ? 1U :
		bytes == 4U ? 2U :
		bytes == 8U ? 3U : 4U;
}

using uml::emulator::tests::vector_offset;

[[nodiscard]] constexpr std::uintptr_t base_offset(unsigned rn)
{
	return rn < 31U ? rn * sizeof(std::uint64_t) :
		offsetof(arm64::CpuState, sp);
}

[[nodiscard]] constexpr std::uintptr_t offset_register_offset(unsigned rm)
{
	return rm < 31U ? rm * sizeof(std::uint64_t) :
		offsetof(arm64::CpuState, gadget_zero);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, std::uint32_t{0xd400'0001U}};
	return gadgets::Builder{}.build(
		memory::engine::engine_test_code_base, code);
}

[[nodiscard]] std::span<const std::uintptr_t> stream(
	const gadgets::BuildResult &built)
{
	require(built.complete, "ordinary-memory translation was incomplete");
	const auto words = built.program.words();
	require(words.size() >= 3U,
		"ordinary-memory translation stream was truncated");
	return words;
}

void account(std::map<std::uintptr_t, unsigned> &references,
	const gadgets::BuildResult &built, Gadget expected,
	std::uintptr_t operand)
{
	const auto words = stream(built);
	const auto selected = address(expected);
	require(words[0] == selected,
		"ordinary-memory translation selected the wrong fixed gadget");
	require(words[1] == operand,
		"ordinary-memory translation retained runtime decode fields");
	const auto found = references.find(selected);
	require(found != references.end(),
		"ordinary-memory translation selected an unlisted gadget");
	++found->second;
}

[[nodiscard]] std::set<std::uintptr_t> definitions()
{
	std::set<std::uintptr_t> result;
	for (const auto &directions : unsigned_paths)
		for (const auto &width : directions)
			for (const auto gadget : width)
				result.insert(address(gadget));
	for (const auto &directions : other_paths)
		for (const auto &width : directions) {
			for (const auto &mode : width.immediate)
				for (const auto gadget : mode)
					result.insert(address(gadget));
			for (const auto &mode : width.register_offset)
				for (const auto gadget : mode)
					result.insert(address(gadget));
		}
	for (const auto &directions : pair_paths)
		for (const auto &width : directions)
			for (const auto &mode : width)
				for (const auto gadget : mode)
					result.insert(address(gadget));
	require(result.size() == 236U,
		"ordinary-memory gadget definition inventory is not exactly 236");
	return result;
}

void unsigned_translation(std::map<std::uintptr_t, unsigned> &references)
{
	for (const auto &variant : memory::single_variants)
		for (const bool sp_base : {false, true}) {
			const unsigned rn = sp_base ? 31U : 20U;
			constexpr unsigned rt = 29U;
			constexpr unsigned imm12 = 7U;
			const auto operand =
				vector_offset(rt) | (base_offset(rn) << 16U) |
				(static_cast<std::uintptr_t>(
					imm12 * variant.size) << 32U);
			account(references,
				build(memory::unsigned_instruction(
					variant.size_code, variant.operation,
					imm12, rn, rt)),
				unsigned_paths[variant.load]
					[width_index(variant.size)][sp_base],
				operand);
		}
}

void immediate_translation(std::map<std::uintptr_t, unsigned> &references)
{
	constexpr std::array modes{
		memory::ImmediateMode::unscaled,
		memory::ImmediateMode::post_index,
		memory::ImmediateMode::pre_index,
	};
	for (const auto &variant : memory::single_variants)
		for (unsigned mode_index = 0; mode_index < modes.size();
		     ++mode_index)
			for (const bool sp_base : {false, true}) {
				const unsigned rn = sp_base ? 31U : 20U;
				constexpr unsigned rt = 29U;
				constexpr unsigned imm9 = 7U;
				const unsigned path = mode_index == 0U ? 0U :
					mode_index == 1U ? 2U : 1U;
				const auto operand =
					vector_offset(rt) |
					(base_offset(rn) << 16U) |
					(static_cast<std::uintptr_t>(imm9) << 32U);
				account(references,
					build(memory::immediate_instruction(
						variant.size_code,
						variant.operation, imm9,
						modes[mode_index], rn, rt)),
					other_paths[variant.load]
						[width_index(variant.size)]
						.immediate[path][sp_base],
					operand);
			}
}

void register_translation(std::map<std::uintptr_t, unsigned> &references)
{
	constexpr std::array options{2U, 3U, 6U, 7U};
	for (const auto &variant : memory::single_variants)
		for (const unsigned option : options)
			for (const bool scaled : {false, true})
				for (const bool sp_base : {false, true}) {
					const unsigned rn = sp_base ? 31U : 20U;
					constexpr unsigned rt = 29U;
					constexpr unsigned rm = 5U;
					const unsigned extension =
						option == 2U ? 0U :
						option == 6U ? 2U : 1U;
					const unsigned path =
						extension + (scaled ? 3U : 0U);
					const auto operand =
						vector_offset(rt) |
						(base_offset(rn) << 16U) |
						(offset_register_offset(rm) << 32U);
					account(references,
						build(memory::register_instruction(
							variant.size_code,
							variant.operation, rm,
							option, scaled, rn, rt)),
						other_paths[variant.load]
							[width_index(variant.size)]
							.register_offset[path][sp_base],
						operand);
				}
}

[[nodiscard]] constexpr std::uint32_t pair_instruction(
	unsigned operation, bool load, unsigned mode, unsigned imm7,
	unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x2c00'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(load) << 22) |
		((mode & 3U) << 23) | ((imm7 & 0x7fU) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

void pair_translation(std::map<std::uintptr_t, unsigned> &references)
{
	/* opc 11 holds the FEAT_LSUI LDTP, STTP, LDTNP, and STTNP. */
	for (unsigned operation = 0; operation < 3U; ++operation)
		for (const bool load : {false, true})
			for (unsigned mode = 0; mode < 4U; ++mode)
				for (const bool sp_base : {false, true}) {
					const unsigned rn = sp_base ? 31U : 20U;
					constexpr unsigned rt = 3U;
					constexpr unsigned rt2 = 17U;
					constexpr unsigned imm7 = 3U;
					const unsigned width = operation;
					const unsigned path =
						mode == 1U ? 2U :
						mode == 3U ? 1U : 0U;
					const unsigned scale = operation + 2U;
					const auto displacement =
						static_cast<std::uint64_t>(
							memory::signed_imm7(imm7))
						<< scale;
					const auto operand =
						vector_offset(rt) |
						(vector_offset(rt2) << 12U) |
						(base_offset(rn) << 24U) |
						((displacement & 0xfffU) << 36U);
					account(references,
						build(pair_instruction(
							operation, load, mode,
							imm7, rn, rt, rt2)),
						pair_paths[load][width]
							[path][sp_base],
						operand);
				}
}

void exact_translation_references(
	const std::set<std::uintptr_t> &all_definitions)
{
	std::map<std::uintptr_t, unsigned> references;
	for (const auto definition : all_definitions)
		references.emplace(definition, 0U);
	unsigned_translation(references);
	immediate_translation(references);
	register_translation(references);
	pair_translation(references);
	for (const auto &directions : unsigned_paths)
		for (const auto &width : directions)
			for (const auto gadget : width)
				require(references.at(address(gadget)) == 1U,
					"unsigned gadget reference count is not exact");
	for (const auto &directions : other_paths)
		for (const auto &width : directions) {
			for (const auto &mode : width.immediate)
				for (const auto gadget : mode)
					require(references.at(address(gadget)) == 1U,
						"immediate gadget reference count is not exact");
			for (unsigned mode = 0; mode < width.register_offset.size();
			     ++mode)
				for (const auto gadget : width.register_offset[mode])
					require(references.at(address(gadget)) ==
							(mode == 1U || mode == 4U ?
								2U : 1U),
						"register gadget reference count is not exact");
		}
	for (const auto &directions : pair_paths)
		for (unsigned width = 0; width < directions.size(); ++width)
			for (unsigned mode = 0; mode < directions[width].size();
			     ++mode)
				for (const auto gadget : directions[width][mode]) {
					const unsigned expected = mode == 0U ? 2U : 1U;
					require(references.at(address(gadget)) == expected,
						"pair gadget reference count is not exact");
				}
	const auto total = std::accumulate(
		references.begin(), references.end(), 0U,
		[](unsigned sum, const auto &entry) {
			return sum + entry.second;
		});
	require(total == 288U,
		"ordinary-memory translation reference count is not exactly 288");
	for (const auto &[definition, count] : references) {
		(void)definition;
		require(count != 0U,
			"ordinary-memory gadget definition is dead");
	}
}

[[nodiscard]] constexpr bool direct_call(std::uint32_t instruction)
{
	return (instruction & 0xfc00'0000U) == 0x9400'0000U;
}

struct Shape final {
	unsigned instructions{};
	unsigned conditional_branches{};
	unsigned calls{};
};

[[nodiscard]] Shape shape(Gadget gadget)
{
	Shape result;
	const auto body = a64::body_shape(
		reinterpret_cast<std::uintptr_t>(gadget), {.window = 96U},
		[&](std::uint32_t instruction) {
			if (a64::conditional_branch(instruction)) {
				const bool condition_form =
					(instruction & 0xff00'0010U) == 0x5400'0000U;
				if (condition_form) {
					const unsigned condition = instruction & 15U;
					require(condition <= 1U,
						"ordinary-memory hot path has a non-fault condition");
				} else {
					const unsigned tested_register = instruction & 31U;
					require(tested_register == 0U ||
							tested_register == 9U,
						"ordinary-memory hot path branches on a decode field");
				}
			}
			if (direct_call(instruction))
				++result.calls;
		});
	if (!body.reached_dispatch)
		fail("ordinary-memory gadget did not reach threaded dispatch");
	result.instructions = body.instructions + 1U;
	result.conditional_branches = body.conditional_branches;
	return result;
}

void bounded_decode_free_hot_paths(
	const std::set<std::uintptr_t> &all_definitions)
{
	unsigned maximum_instructions = 0;
	unsigned maximum_branches = 0;
	unsigned maximum_calls = 0;
	for (const auto definition : all_definitions) {
		const auto properties =
			shape(reinterpret_cast<Gadget>(definition));
		maximum_instructions =
			std::max(maximum_instructions, properties.instructions);
		maximum_branches =
			std::max(maximum_branches,
				properties.conditional_branches);
		maximum_calls = std::max(maximum_calls, properties.calls);
		require(properties.instructions <= 47U,
			"ordinary-memory hot path exceeds instruction budget");
		require(properties.conditional_branches <= 6U,
			"ordinary-memory hot path has an unexpected conditional branch");
		require(properties.calls <= 1U,
			"ordinary-memory hot path has an unexpected call");
	}
	std::printf(
		"arm64-simd-ordinary-memory-gadget: definitions=%zu "
		"translation-cases=304 max-instructions=%u "
		"max-conditional-branches=%u max-calls=%u\n",
		all_definitions.size(), maximum_instructions,
		maximum_branches, maximum_calls);
}

} // namespace

int main()
{
	const auto all_definitions = definitions();
	exact_translation_references(all_definitions);
	bounded_decode_free_hot_paths(all_definitions);
}
