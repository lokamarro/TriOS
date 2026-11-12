// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_base_load_store_register_test_support.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace a64 = uml::testing::a64;

namespace test = arm64_load_store_register_test;
namespace engine_tests = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define DECLARE_REGISTER_MODE(name, mode) \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp();
#define DECLARE_REGISTER_ROW(name) \
	DECLARE_REGISTER_MODE(name, uxtw_0) \
	DECLARE_REGISTER_MODE(name, x_0) \
	DECLARE_REGISTER_MODE(name, sxtw_0) \
	DECLARE_REGISTER_MODE(name, uxtw_1) \
	DECLARE_REGISTER_MODE(name, x_1) \
	DECLARE_REGISTER_MODE(name, sxtw_1)
DECLARE_REGISTER_ROW(store8)
DECLARE_REGISTER_ROW(load8u)
DECLARE_REGISTER_ROW(load8sx)
DECLARE_REGISTER_ROW(load8sw)
DECLARE_REGISTER_ROW(store16)
DECLARE_REGISTER_ROW(load16u)
DECLARE_REGISTER_ROW(load16sx)
DECLARE_REGISTER_ROW(load16sw)
DECLARE_REGISTER_ROW(store32)
DECLARE_REGISTER_ROW(load32u)
DECLARE_REGISTER_ROW(load32sx)
DECLARE_REGISTER_ROW(store64)
DECLARE_REGISTER_ROW(load64u)
#undef DECLARE_REGISTER_ROW
#undef DECLARE_REGISTER_MODE

void uml_arm64_gadget_nop();
}

namespace {

using uml::emulator::tests::Gadget;
using RegisterChoices = std::array<Gadget, 2>;
using BaseChoices = std::array<RegisterChoices, 2>;
using AddressChoices = std::array<BaseChoices, 6>;

#define REGISTER_CHOICES(name, mode) RegisterChoices{ \
	uml_arm64_gadget_scalar_other_##name##_##mode##_r, \
	uml_arm64_gadget_scalar_other_##name##_##mode##_z}
#define BASE_CHOICES(name, mode) BaseChoices{ \
	REGISTER_CHOICES(name, mode), \
	RegisterChoices{ \
		uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp, \
		uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp}}
#define ADDRESS_CHOICES(name) AddressChoices{ \
	BASE_CHOICES(name, uxtw_0), \
	BASE_CHOICES(name, x_0), \
	BASE_CHOICES(name, sxtw_0), \
	BASE_CHOICES(name, uxtw_1), \
	BASE_CHOICES(name, x_1), \
	BASE_CHOICES(name, sxtw_1)}

const std::array<AddressChoices, test::integer_variants.size()> rows{
	ADDRESS_CHOICES(store8),
	ADDRESS_CHOICES(load8u),
	ADDRESS_CHOICES(load8sx),
	ADDRESS_CHOICES(load8sw),
	ADDRESS_CHOICES(store16),
	ADDRESS_CHOICES(load16u),
	ADDRESS_CHOICES(load16sx),
	ADDRESS_CHOICES(load16sw),
	ADDRESS_CHOICES(store32),
	ADDRESS_CHOICES(load32u),
	ADDRESS_CHOICES(load32sx),
	ADDRESS_CHOICES(store64),
	ADDRESS_CHOICES(load64u),
};

#undef ADDRESS_CHOICES
#undef BASE_CHOICES
#undef REGISTER_CHOICES

using uml::emulator::tests::address;

[[nodiscard]] constexpr std::uintptr_t register_offset(unsigned reg,
							bool stack_pointer)
{
	if (reg < 31U)
		return reg * sizeof(std::uint64_t);
	return stack_pointer ? offsetof(arm64::CpuState, sp) :
		offsetof(arm64::CpuState, gadget_zero);
}

[[nodiscard]] constexpr std::uintptr_t packed_operand(
	unsigned rm, unsigned rn, unsigned rt)
{
	const auto transfer = rt < 31U ? register_offset(rt, false) : 0U;
	return transfer |
		(register_offset(rn, true) << 16U) |
		(register_offset(rm, false) << 32U);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(engine_tests::engine_test_code_base,
		std::array{instruction});
}

void require_register_stream(const gadgets::BuildResult &built,
			     Gadget expected, std::uintptr_t operand)
{
	test::require(built.complete && built.program.words().size() == 4U,
		"register-offset scalar-memory stream has an unexpected shape");
	test::require(built.program.words()[0] == address(expected),
		"register-offset scalar-memory gadget was not selected at translation");
	test::require(built.program.words()[1] == operand,
		"register-offset scalar-memory fields were not predecoded");
}

[[nodiscard]] constexpr unsigned address_mode(unsigned option, bool scaled)
{
	const unsigned extension = option == 2U ? 0U :
		option == 6U ? 2U : 1U;
	return extension + (scaled ? 3U : 0U);
}

void translation_selection_and_dead_bank_audit()
{
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (unsigned variant_index = 0; variant_index < rows.size();
	     ++variant_index) {
		const auto &variant = test::integer_variants[variant_index];
		const auto &row = rows[variant_index];
		for (const auto &mode : row)
			for (const auto &base : mode)
				for (const auto entry : base)
					expected.insert(address(entry));

		for (const auto option : test::legal_options)
			for (const bool scaled : {false, true})
				for (const bool base_sp : {false, true})
					for (const bool transfer_zero :
					     {false, true})
						for (const bool offset_zero :
						     {false, true}) {
							const unsigned rm =
								offset_zero ?
									31U : 9U;
							const unsigned rn =
								base_sp ?
									31U : 7U;
							const unsigned rt =
								transfer_zero ?
									31U : 5U;
							const auto entry =
								row[address_mode(
									option,
									scaled)]
								   [base_sp]
								   [transfer_zero];
							const auto instruction =
								test::encode(
									variant
										.size_code,
									variant
										.operation,
									rm, option,
									scaled, 2,
									rn, rt);
							require_register_stream(
								build(instruction),
								entry,
								packed_operand(
									rm, rn,
									rt));
							selected.insert(
								address(
									entry));
						}
	}
	test::require(selected == expected && selected.size() == 312U,
		"register-offset scalar-memory bank has an unselected or missing gadget");
}

void prefetch_is_translation_only()
{
	for (const auto option : test::legal_options)
		for (const bool scaled : {false, true})
			for (const bool base_sp : {false, true})
				for (const bool offset_zero : {false, true})
					for (unsigned operation = 0;
					     operation < 32U; ++operation) {
						const auto instruction = test::encode(
							3, 2,
							offset_zero ? 31U : 9U,
							option, scaled, 2,
							base_sp ? 31U : 7U,
							operation);
						const auto built = build(instruction);
						test::require(
							built.complete &&
								built.program.words()
										.size() ==
									3U,
							"PRFM register stream retained runtime address work");
						test::require(
							built.program.words().front() ==
								address(
									uml_arm64_gadget_nop),
							"PRFM register stream is not the non-faulting hint gadget");
					}
}

struct BodyShape final {
	unsigned instructions{};
	unsigned conditional_branches{};
	unsigned direct_branches{};
	unsigned tbi_state_loads{};
	bool threaded_dispatch{};
};

[[nodiscard]] BodyShape body_shape(Gadget gadget)
{
	constexpr std::uint32_t tbi_state_load = 0x3940'0000U |
		(static_cast<std::uint32_t>(
			offsetof(arm64::CpuState, tbi_enabled)) << 10U) |
		(1U << 5U) | 9U;
	BodyShape result{};
	const auto shape = a64::body_shape(address(gadget),
		{.window = 192U}, [&](std::uint32_t instruction) {
			result.direct_branches +=
				(instruction & 0x7c00'0000U) == 0x1400'0000U;
			result.tbi_state_loads += instruction == tbi_state_load;
		});
	result.instructions = shape.instructions;
	result.conditional_branches = shape.conditional_branches;
	result.threaded_dispatch = shape.reached_dispatch;
	return result;
}

void hot_path_shape_audit()
{
	for (const auto &row : rows)
		for (const auto &mode : row)
			for (unsigned transfer_zero = 0;
			     transfer_zero < 2U; ++transfer_zero) {
				const auto gpr =
					body_shape(mode[0][transfer_zero]);
				const auto sp =
					body_shape(mode[1][transfer_zero]);
				test::require(
					gpr.threaded_dispatch &&
						sp.threaded_dispatch,
					"register-offset scalar-memory gadget lacks threaded dispatch");
				test::require(
					gpr.tbi_state_loads == 1U &&
						sp.tbi_state_loads == 0U,
					"register-offset gadget has the wrong TBI-state check");
				test::require(
					gpr.instructions == sp.instructions + 1U,
					"GPR TBI specialization has unexpected hot-path work");
				test::require(
					sp.conditional_branches ==
						gpr.conditional_branches,
					"SP alignment and GPR TBI branch counts differ");
				test::require(
					sp.direct_branches ==
						gpr.direct_branches,
					"SP specialization changed the ordinary direct-branch path");
			}
}

} // namespace

int main()
{
	translation_selection_and_dead_bank_audit();
	prefetch_is_translation_only();
	hot_path_shape_audit();
	return 0;
}
