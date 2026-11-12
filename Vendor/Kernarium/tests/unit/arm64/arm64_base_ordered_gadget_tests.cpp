// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_test_support.hpp"
#include "arm64_ordered_memory_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>

extern "C" {
#define UML_DECLARE_ORDERED(kind, ordering, size) \
	void uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_r(); \
	void uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_z();
#define UML_DECLARE_ORDERED_SIZES(kind, ordering) \
	UML_DECLARE_ORDERED(kind, ordering, b) \
	UML_DECLARE_ORDERED(kind, ordering, h) \
	UML_DECLARE_ORDERED(kind, ordering, w) \
	UML_DECLARE_ORDERED(kind, ordering, x)
UML_DECLARE_ORDERED_SIZES(load, acquire)
UML_DECLARE_ORDERED_SIZES(store, release)
#undef UML_DECLARE_ORDERED_SIZES
#undef UML_DECLARE_ORDERED

#define UML_DECLARE_RCPC_LOAD(name) \
	void uml_arm64_gadget_rcpc_load_##name##_r(); \
	void uml_arm64_gadget_rcpc_load_##name##_z();
UML_DECLARE_RCPC_LOAD(zero_b)
UML_DECLARE_RCPC_LOAD(zero_h)
UML_DECLARE_RCPC_LOAD(zero_w)
UML_DECLARE_RCPC_LOAD(zero_x)
UML_DECLARE_RCPC_LOAD(sb_w)
UML_DECLARE_RCPC_LOAD(sb_x)
UML_DECLARE_RCPC_LOAD(sh_w)
UML_DECLARE_RCPC_LOAD(sh_x)
UML_DECLARE_RCPC_LOAD(sw_x)
#undef UML_DECLARE_RCPC_LOAD

#define UML_DECLARE_RCPC_STORE(size) \
	void uml_arm64_gadget_rcpc_store_##size##_r(); \
	void uml_arm64_gadget_rcpc_store_##size##_z();
UML_DECLARE_RCPC_STORE(b)
UML_DECLARE_RCPC_STORE(h)
UML_DECLARE_RCPC_STORE(w)
UML_DECLARE_RCPC_STORE(x)
#undef UML_DECLARE_RCPC_STORE
}

namespace ordered = arm64_ordered_memory_test;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;
using RegisterModes = std::array<Gadget, 2>;
using SizeModes = std::array<RegisterModes, 4>;

#define UML_REGISTER_MODES(kind, ordering, size) RegisterModes{ \
	uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_r, \
	uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_z}
#define UML_SIZE_MODES(kind, ordering) SizeModes{ \
	UML_REGISTER_MODES(kind, ordering, b), \
	UML_REGISTER_MODES(kind, ordering, h), \
	UML_REGISTER_MODES(kind, ordering, w), \
	UML_REGISTER_MODES(kind, ordering, x)}
constexpr SizeModes ordered_load = UML_SIZE_MODES(load, acquire);
constexpr SizeModes ordered_store = UML_SIZE_MODES(store, release);
#undef UML_SIZE_MODES
#undef UML_REGISTER_MODES

#define UML_RCPC_MODES(kind, name) RegisterModes{ \
	uml_arm64_gadget_rcpc_##kind##_##name##_r, \
	uml_arm64_gadget_rcpc_##kind##_##name##_z}
constexpr std::array rcpc_load{
	UML_RCPC_MODES(load, zero_b),
	UML_RCPC_MODES(load, zero_h),
	UML_RCPC_MODES(load, zero_w),
	UML_RCPC_MODES(load, zero_x),
	UML_RCPC_MODES(load, sb_w),
	UML_RCPC_MODES(load, sb_x),
	UML_RCPC_MODES(load, sh_w),
	UML_RCPC_MODES(load, sh_x),
	UML_RCPC_MODES(load, sw_x),
};
constexpr std::array rcpc_store{
	UML_RCPC_MODES(store, b),
	UML_RCPC_MODES(store, h),
	UML_RCPC_MODES(store, w),
	UML_RCPC_MODES(store, x),
};
#undef UML_RCPC_MODES

using uml::emulator::tests::address;

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array stream{
		instruction,
		ordered::terminal_system_call,
	};
	return gadgets::Builder{}.build(
		ordered::engine::engine_test_code_base, stream);
}

void require_selected(std::uint32_t instruction, Gadget expected,
	std::uintptr_t operand, std::set<std::uintptr_t> &selected)
{
	const auto built = build(instruction);
	const auto words = built.program.words();
	if (!built.complete || words.size() < 3U ||
	    words[0] != address(expected) || words[1] != operand)
		std::abort();
	selected.insert(words[0]);
}

[[nodiscard]] constexpr std::uintptr_t register_offset(unsigned reg)
{
	return reg == 31U ? offsetof(arm64::CpuState, sp) :
		reg * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t scalar_operand(
	unsigned rn, unsigned rt)
{
	return (rt == 31U ? 0U : register_offset(rt)) |
		(register_offset(rn) << 16);
}

[[nodiscard]] constexpr unsigned rcpc_load_variant(
	const ordered::ScalarVariant &variant)
{
	if (variant.operation == 1U)
		return variant.size_code;
	if (variant.operation == 2U)
		return variant.size_code == 0U ? 5U :
			variant.size_code == 1U ? 7U : 8U;
	return variant.size_code == 0U ? 4U : 6U;
}

void exact_translation_selection()
{
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &sizes : {ordered_load, ordered_store})
		for (const auto &modes : sizes)
			for (const auto gadget : modes)
				expected.insert(address(gadget));
	for (const auto &modes : rcpc_load)
		for (const auto gadget : modes)
			expected.insert(address(gadget));
	for (const auto &modes : rcpc_store)
		for (const auto gadget : modes)
			expected.insert(address(gadget));

	for (const auto &variant : ordered::scalar_variants) {
		if (variant.instruction_class == ordered::ScalarClass::ordered) {
			for (const unsigned rn : {2U, 31U})
				for (const unsigned rt : {3U, 31U}) {
					const auto gadget =
						(variant.load ? ordered_load : ordered_store)
							[variant.size_code][rt == 31U];
					require_selected(
						ordered::scalar_instruction(
							variant, 0, rn, rt),
						gadget, scalar_operand(rn, rt), selected);
				}
			continue;
		}
		const int offset =
			variant.instruction_class ==
					ordered::ScalarClass::rcpc_unscaled ?
				-17 : 0;
		for (const unsigned rn : {2U, 31U})
			for (const unsigned rt : {3U, 31U}) {
				const auto gadget =
					variant.load ?
						rcpc_load[rcpc_load_variant(variant)]
							[rt == 31U] :
						rcpc_store[variant.size_code]
							[rt == 31U];
				const auto operand =
					rn | (static_cast<std::uintptr_t>(rt) << 5) |
					(static_cast<std::uintptr_t>(
						static_cast<std::uint64_t>(
							static_cast<std::int64_t>(
								offset)) & 0x1ffU) << 10);
				require_selected(
					ordered::scalar_instruction(
						variant, offset, rn, rt),
					gadget, operand, selected);
			}
	}
	if (selected != expected || selected.size() != 42U)
		std::abort();
}

} // namespace

int main()
{
	exact_translation_selection();
	return 0;
}
