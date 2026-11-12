// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/sme_layout.h"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace uml::emulator::tests::base_pstate {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

enum class Field : std::uint8_t {
	flag_alias,
	reserved,
	uao,
	pan,
	stack_pointer_select,
	all_interrupt_mask,
	profiling_mask,
	ssbs,
	dit,
	streaming_mode,
	array_state,
	streaming_and_array_state,
	tag_check_override,
	interrupt_mask_set,
	interrupt_mask_clear,
};

struct Decoded final {
	Field field{Field::reserved};
	bool allocated{};
	bool el0_accessible{};
};

[[nodiscard]] constexpr std::uint32_t encode(
	unsigned op1, unsigned immediate, unsigned op2) noexcept
{
	return 0xd500'401fU | ((op1 & 7U) << 16U) |
		((immediate & 15U) << 8U) | ((op2 & 7U) << 5U);
}

[[nodiscard]] constexpr bool is_flag_alias(
	unsigned op1, unsigned immediate, unsigned op2) noexcept
{
	return op1 == 0U && immediate == 0U && op2 <= 2U;
}

/*
 * Independent transcription of the MSR (immediate) allocation tables.  The
 * emulator runs an EL0 guest process, so architecturally allocated fields with
 * a higher minimum exception level are distinguished from reserved encodings.
 */
[[nodiscard]] inline Decoded decode(
	unsigned op1, unsigned immediate, unsigned op2) noexcept
{
	if (is_flag_alias(op1, immediate, op2))
		return {
			.field = Field::flag_alias,
			.allocated = true,
			.el0_accessible = true,
		};

	Field field = Field::reserved;
	bool allocated = false;
	bool el0_accessible = false;
	if (op1 == 0U) {
		switch (op2) {
		case 3U:
			field = Field::uao;
			allocated = true;
			break;
		case 4U:
			field = Field::pan;
			allocated = true;
			break;
		case 5U:
			field = Field::stack_pointer_select;
			allocated = true;
			break;
		default:
			break;
		}
	} else if (op1 == 1U && op2 == 0U) {
		if (immediate <= 1U) {
			field = Field::all_interrupt_mask;
			allocated = true;
		} else if (immediate <= 3U) {
			field = Field::profiling_mask;
			allocated = true;
		}
	} else if (op1 == 3U) {
		el0_accessible = true;
		switch (op2) {
		case 1U:
			field = Field::ssbs;
			allocated = true;
			break;
		case 2U:
			field = Field::dit;
			allocated = true;
			break;
		case 3U:
			if (immediate < 8U) {
				switch ((immediate >> 1U) & 3U) {
				case 1U:
					field = Field::streaming_mode;
					allocated = true;
					break;
				case 2U:
					field = Field::array_state;
					allocated = true;
					break;
				case 3U:
					field = Field::streaming_and_array_state;
					allocated = true;
					break;
				default:
					break;
				}
			}
			break;
		case 4U:
			field = Field::tag_check_override;
			allocated = true;
			break;
		case 6U:
			field = Field::interrupt_mask_set;
			allocated = true;
			break;
		case 7U:
			field = Field::interrupt_mask_clear;
			allocated = true;
			break;
		default:
			break;
		}
	}
	if (!allocated)
		el0_accessible = false;
	return {
		.field = field,
		.allocated = allocated,
		.el0_accessible = el0_accessible,
	};
}

} // namespace uml::emulator::tests::base_pstate

namespace a64 = uml::testing::a64;

namespace uml::emulator::tests::base_pstate {
namespace {

namespace gadgets = arm64::gadgets;
using uml::emulator::tests::Gadget;

extern "C" {
#define UML_DECLARE_PSTATE_FIELD(name) \
	void uml_arm64_gadget_pstate_##name##_0(); \
	void uml_arm64_gadget_pstate_##name##_1();
UML_DECLARE_PSTATE_FIELD(dit)
#undef UML_DECLARE_PSTATE_FIELD
#define UML_DECLARE_PSTATE_DAIF(name, immediate) \
	void uml_arm64_gadget_pstate_daif_##name##_##immediate();
#define UML_DECLARE_PSTATE_DAIF_IMMEDIATE(immediate) \
	UML_DECLARE_PSTATE_DAIF(set, immediate) \
	UML_DECLARE_PSTATE_DAIF(clear, immediate)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(0)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(1)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(2)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(3)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(4)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(5)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(6)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(7)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(8)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(9)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(10)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(11)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(12)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(13)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(14)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(15)
#undef UML_DECLARE_PSTATE_DAIF_IMMEDIATE
#undef UML_DECLARE_PSTATE_DAIF
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_sme_svcr_write_immediate();
}

constexpr std::array<Gadget, 2> dit_gadgets{
	uml_arm64_gadget_pstate_dit_0,
	uml_arm64_gadget_pstate_dit_1,
};

constexpr std::array<Gadget, 16> daif_set_gadgets{
	uml_arm64_gadget_pstate_daif_set_0,
	uml_arm64_gadget_pstate_daif_set_1,
	uml_arm64_gadget_pstate_daif_set_2,
	uml_arm64_gadget_pstate_daif_set_3,
	uml_arm64_gadget_pstate_daif_set_4,
	uml_arm64_gadget_pstate_daif_set_5,
	uml_arm64_gadget_pstate_daif_set_6,
	uml_arm64_gadget_pstate_daif_set_7,
	uml_arm64_gadget_pstate_daif_set_8,
	uml_arm64_gadget_pstate_daif_set_9,
	uml_arm64_gadget_pstate_daif_set_10,
	uml_arm64_gadget_pstate_daif_set_11,
	uml_arm64_gadget_pstate_daif_set_12,
	uml_arm64_gadget_pstate_daif_set_13,
	uml_arm64_gadget_pstate_daif_set_14,
	uml_arm64_gadget_pstate_daif_set_15,
};
constexpr std::array<Gadget, 16> daif_clear_gadgets{
	uml_arm64_gadget_pstate_daif_clear_0,
	uml_arm64_gadget_pstate_daif_clear_1,
	uml_arm64_gadget_pstate_daif_clear_2,
	uml_arm64_gadget_pstate_daif_clear_3,
	uml_arm64_gadget_pstate_daif_clear_4,
	uml_arm64_gadget_pstate_daif_clear_5,
	uml_arm64_gadget_pstate_daif_clear_6,
	uml_arm64_gadget_pstate_daif_clear_7,
	uml_arm64_gadget_pstate_daif_clear_8,
	uml_arm64_gadget_pstate_daif_clear_9,
	uml_arm64_gadget_pstate_daif_clear_10,
	uml_arm64_gadget_pstate_daif_clear_11,
	uml_arm64_gadget_pstate_daif_clear_12,
	uml_arm64_gadget_pstate_daif_clear_13,
	uml_arm64_gadget_pstate_daif_clear_14,
	uml_arm64_gadget_pstate_daif_clear_15,
};

using uml::emulator::tests::address;

[[nodiscard]] bool smstart_implemented()
{
	return gadgets::sme_support != arm64::SmeSupport::none &&
		arm64::implements_sme(gadgets::BuildOptions{}.host_capabilities);
}

/* The SVCR mask and value SMSTART or SMSTOP writes. */
[[nodiscard]] std::uintptr_t svcr_write(
	const Decoded &decoded, unsigned immediate)
{
	const std::uintptr_t mask =
		decoded.field == Field::streaming_mode ? arm64::svcr_sm :
		decoded.field == Field::array_state ? arm64::svcr_za :
		arm64::svcr_mask;
	const std::uintptr_t value = (immediate & 1U) != 0U ? mask : 0U;
	return mask | (value << UML_GADGET_SME_SVCR_VALUE_SHIFT);
}

[[nodiscard]] Gadget expected_gadget(
	const Decoded &decoded, unsigned immediate)
{
	const auto value = immediate & 1U;
	if (!decoded.allocated || !decoded.el0_accessible)
		return uml_arm64_gadget_illegal;
	switch (decoded.field) {
	case Field::dit:
		return dit_gadgets[value];
	case Field::interrupt_mask_set:
		return daif_set_gadgets[immediate];
	case Field::interrupt_mask_clear:
		return daif_clear_gadgets[immediate];
	case Field::streaming_mode:
	case Field::array_state:
	case Field::streaming_and_array_state:
		/* SMSTART and SMSTOP exist when the run implements SME (the
		 * guest-SME policy on a host with the profile) and the engine
		 * implements its control core; UNDEFINED otherwise. */
		if (smstart_implemented())
			return uml_arm64_gadget_sme_svcr_write_immediate;
		break;
	case Field::ssbs:
	case Field::tag_check_override:
		/* The virtual CPU omits FEAT_SSBS and FEAT_MTE, so MSR SSBS
		 * and MSR TCO are UNDEFINED. */
	case Field::flag_alias:
	case Field::reserved:
	case Field::uao:
	case Field::pan:
	case Field::stack_pointer_select:
	case Field::all_interrupt_mask:
	case Field::profiling_mask:
		break;
	}
	return uml_arm64_gadget_illegal;
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, terminal_system_call};
	return gadgets::Builder{}.build(engine_test_code_base, code, {},
		std::pmr::get_default_resource());
}

[[nodiscard]] unsigned runtime_instruction_count(Gadget gadget)
{
	const auto shape = a64::body_shape(address(gadget), {.window = 20U});
	if (shape.reached_dispatch)
		return shape.instructions + 1U;
	arm64_system_test::require(false);
	return 0;
}

[[nodiscard]] unsigned conditional_branch_count(Gadget gadget)
{
	const auto shape = a64::body_shape(address(gadget), {.window = 20U});
	if (shape.reached_dispatch)
		return shape.conditional_branches;
	arm64_system_test::require(false);
	return 0;
}

void exhaustive_translation()
{
	for (unsigned op1 = 0; op1 < 8U; ++op1)
		for (unsigned op2 = 0; op2 < 8U; ++op2)
			for (unsigned immediate = 0; immediate < 16U; ++immediate) {
				const auto decoded = decode(op1, immediate, op2);
				if (decoded.field == Field::flag_alias)
					continue;
				const auto instruction = encode(
					op1, immediate, op2);
				const auto result = build(instruction);
				const auto expected =
					expected_gadget(decoded, immediate);
				arm64_system_test::require(result.complete);
				arm64_system_test::require(
					result.program.words().front() ==
					address(expected));
				if (expected == uml_arm64_gadget_illegal) {
					arm64_system_test::require(
						result.program.instruction_count() ==
							1U);
					arm64_system_test::require(
						result.program.words().size() == 2U);
					arm64_system_test::require(
						result.program.words()[1] ==
							engine_test_code_base);
					continue;
				}
				if (expected ==
				    uml_arm64_gadget_sme_svcr_write_immediate) {
					/* An SVCR write ends its block. */
					const auto &words = result.program.words();
					arm64_system_test::require(
						result.program.instruction_count() ==
							1U);
					arm64_system_test::require(
						words.size() == 3U);
					arm64_system_test::require(
						words[1] == svcr_write(
							decoded, immediate));
					arm64_system_test::require(
						words[2] == engine_test_code_base);
					continue;
				}
				const bool access_check =
					decoded.field ==
						Field::interrupt_mask_set ||
					decoded.field ==
						Field::interrupt_mask_clear;
				arm64_system_test::require(
					result.program.instruction_count() == 2U);
				arm64_system_test::require(
					result.program.words().size() ==
						(access_check ? 4U : 3U));
				std::size_t svc = 1U;
				if (access_check) {
					arm64_system_test::require(
						result.program.words()[1] ==
							engine_test_code_base);
					svc = 2U;
				}
				arm64_system_test::require(
					result.program.words()[svc] ==
						address(uml_arm64_gadget_svc));
				arm64_system_test::require(
					result.program.words()[svc + 1U] ==
						engine_test_code_base + 8U);
			}
}

void hot_path_shape()
{
	arm64_system_test::require(
		runtime_instruction_count(dit_gadgets[0]) == 3U);
	arm64_system_test::require(
		runtime_instruction_count(dit_gadgets[1]) == 4U);
	arm64_system_test::require(
		conditional_branch_count(dit_gadgets[0]) == 0U &&
		conditional_branch_count(dit_gadgets[1]) == 0U);

	for (unsigned immediate = 0; immediate < 16U; ++immediate)
		for (const auto gadget :
		     {daif_set_gadgets[immediate],
		      daif_clear_gadgets[immediate]}) {
			arm64_system_test::require(
				conditional_branch_count(gadget) == 1U);
			arm64_system_test::require(
				runtime_instruction_count(gadget) ==
					(immediate == 0U ? 5U : 9U));
		}
}

} // namespace
} // namespace uml::emulator::tests::base_pstate

int main()
{
	uml::emulator::tests::base_pstate::exhaustive_translation();
	uml::emulator::tests::base_pstate::hot_path_shape();
	return 0;
}
