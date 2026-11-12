// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_base_immediate_memory_test_support.hpp"
#include "arm64_gadget_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace a64 = uml::testing::a64;

namespace test = uml::emulator::tests::base_immediate_memory;
namespace engine_tests = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define DECLARE_ZERO_UNSIGNED(name) \
	void uml_arm64_gadget_##name##_z(); \
	void uml_arm64_gadget_##name##_z_sp();
#define DECLARE_UNSIGNED(name) \
	void uml_arm64_gadget_##name##_r(); \
	void uml_arm64_gadget_##name##_r_sp(); \
	DECLARE_ZERO_UNSIGNED(name)
DECLARE_UNSIGNED(store8_unsigned_fast)
DECLARE_UNSIGNED(load8_unsigned_fast)
DECLARE_UNSIGNED(load8_signed_x_fast)
DECLARE_UNSIGNED(load8_signed_w_fast)
DECLARE_UNSIGNED(store16_unsigned_fast)
DECLARE_UNSIGNED(load16_unsigned_fast)
DECLARE_UNSIGNED(load16_signed_x_fast)
DECLARE_UNSIGNED(load16_signed_w_fast)
DECLARE_ZERO_UNSIGNED(store32_unsigned_fast)
DECLARE_ZERO_UNSIGNED(load32_unsigned_fast)
DECLARE_UNSIGNED(load32_signed_x_fast)
DECLARE_ZERO_UNSIGNED(store64_unsigned_fast)
DECLARE_ZERO_UNSIGNED(load64_unsigned_fast)
#undef DECLARE_UNSIGNED
#undef DECLARE_ZERO_UNSIGNED

extern void (*const uml_arm64_gadget_scalar_unsigned_inplace_table[])();

#define DECLARE_OTHER_REGISTERS(name, mode) \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp();
#define DECLARE_OTHER_COMMON(name) \
	DECLARE_OTHER_REGISTERS(name, offset) \
	DECLARE_OTHER_REGISTERS(name, pre) \
	DECLARE_OTHER_REGISTERS(name, post)
#define DECLARE_OTHER_STORE(name) DECLARE_OTHER_COMMON(name)
#define DECLARE_OTHER_LOAD(name) \
	DECLARE_OTHER_COMMON(name) \
	void uml_arm64_gadget_scalar_other_##name##_post_suppress_r();
DECLARE_OTHER_STORE(store8)
DECLARE_OTHER_LOAD(load8u)
DECLARE_OTHER_LOAD(load8sx)
DECLARE_OTHER_LOAD(load8sw)
DECLARE_OTHER_STORE(store16)
DECLARE_OTHER_LOAD(load16u)
DECLARE_OTHER_LOAD(load16sx)
DECLARE_OTHER_LOAD(load16sw)
DECLARE_OTHER_STORE(store32)
DECLARE_OTHER_LOAD(load32u)
DECLARE_OTHER_LOAD(load32sx)
DECLARE_OTHER_STORE(store64)
DECLARE_OTHER_LOAD(load64u)
#undef DECLARE_OTHER_LOAD
#undef DECLARE_OTHER_STORE
#undef DECLARE_OTHER_COMMON
#undef DECLARE_OTHER_REGISTERS
}

namespace {

using uml::emulator::tests::Gadget;
using RegisterChoices = std::array<Gadget, 2>;
using BaseChoices = std::array<RegisterChoices, 2>;
using AddressChoices = std::array<BaseChoices, 3>;

struct GadgetRow final {
	BaseChoices unsigned_offset;
	AddressChoices immediate;
	Gadget post_suppress;
};

#define REGISTER_CHOICES(prefix, mode) RegisterChoices{ \
	uml_arm64_gadget_##prefix##_##mode##_r, \
	uml_arm64_gadget_##prefix##_##mode##_z}
#define BASE_CHOICES(prefix, mode) BaseChoices{ \
	REGISTER_CHOICES(prefix, mode), \
	RegisterChoices{ \
		uml_arm64_gadget_##prefix##_##mode##_r_sp, \
		uml_arm64_gadget_##prefix##_##mode##_z_sp}}
#define UNSIGNED_CHOICES(name) BASE_CHOICES(name, fast)
/* A word or doubleword register transfer has no shared entry; the
 * translator bakes the register (unsigned_offset_entry). */
#define ZERO_UNSIGNED_CHOICES(name) BaseChoices{ \
	RegisterChoices{nullptr, uml_arm64_gadget_##name##_fast_z}, \
	RegisterChoices{nullptr, uml_arm64_gadget_##name##_fast_z_sp}}
#define OTHER_ADDRESS_CHOICES(name) AddressChoices{ \
	BASE_CHOICES(scalar_other_##name, offset), \
	BASE_CHOICES(scalar_other_##name, pre), \
	BASE_CHOICES(scalar_other_##name, post)}
#define STORE_ROW(unsigned_name, other_name) GadgetRow{ \
	UNSIGNED_CHOICES(unsigned_name), \
	OTHER_ADDRESS_CHOICES(other_name), nullptr}
#define LOAD_ROW(unsigned_name, other_name) GadgetRow{ \
	UNSIGNED_CHOICES(unsigned_name), \
	OTHER_ADDRESS_CHOICES(other_name), \
	uml_arm64_gadget_scalar_other_##other_name##_post_suppress_r}
#define ZERO_STORE_ROW(unsigned_name, other_name) GadgetRow{ \
	ZERO_UNSIGNED_CHOICES(unsigned_name), \
	OTHER_ADDRESS_CHOICES(other_name), nullptr}
#define ZERO_LOAD_ROW(unsigned_name, other_name) GadgetRow{ \
	ZERO_UNSIGNED_CHOICES(unsigned_name), \
	OTHER_ADDRESS_CHOICES(other_name), \
	uml_arm64_gadget_scalar_other_##other_name##_post_suppress_r}

const std::array<GadgetRow, test::variants.size()> rows{
	STORE_ROW(store8_unsigned, store8),
	LOAD_ROW(load8_unsigned, load8u),
	LOAD_ROW(load8_signed_x, load8sx),
	LOAD_ROW(load8_signed_w, load8sw),
	STORE_ROW(store16_unsigned, store16),
	LOAD_ROW(load16_unsigned, load16u),
	LOAD_ROW(load16_signed_x, load16sx),
	LOAD_ROW(load16_signed_w, load16sw),
	ZERO_STORE_ROW(store32_unsigned, store32),
	ZERO_LOAD_ROW(load32_unsigned, load32u),
	LOAD_ROW(load32_signed_x, load32sx),
	ZERO_STORE_ROW(store64_unsigned, store64),
	ZERO_LOAD_ROW(load64_unsigned, load64u),
};

#undef ZERO_LOAD_ROW
#undef ZERO_STORE_ROW
#undef LOAD_ROW
#undef STORE_ROW
#undef OTHER_ADDRESS_CHOICES
#undef ZERO_UNSIGNED_CHOICES
#undef UNSIGNED_CHOICES
#undef BASE_CHOICES
#undef REGISTER_CHOICES

using uml::emulator::tests::address;

[[nodiscard]] constexpr std::uintptr_t transfer_offset(unsigned rt)
{
	return rt < 31U ? rt * sizeof(std::uint64_t) : 0U;
}

[[nodiscard]] constexpr std::uintptr_t base_offset(unsigned rn)
{
	return rn < 31U ? rn * sizeof(std::uint64_t) :
		offsetof(arm64::CpuState, sp);
}

/* The scaled 32- and 64-bit unsigned-offset shapes bake their transferred
 * register, so for those the translator selects a per-register entry rather
 * than the shared one. Rows 8 and 11 are the stores, 9 and 12 the loads. */
[[nodiscard]] Gadget unsigned_offset_entry(Gadget shared,
	unsigned row_index, unsigned base_sp, unsigned transfer_zero,
	unsigned transferred)
{
	if (transfer_zero == 0U) {
		unsigned operation = 2U;
		unsigned bits64 = 0U;
		switch (row_index) {
		case 8U: operation = 0U; bits64 = 0U; break;
		case 9U: operation = 1U; bits64 = 0U; break;
		case 11U: operation = 0U; bits64 = 1U; break;
		case 12U: operation = 1U; bits64 = 1U; break;
		default: break;
		}
		if (operation < 2U) {
			const unsigned slot = ((operation * 2U + bits64) * 2U +
				base_sp) * 31U + transferred;
			return uml_arm64_gadget_scalar_unsigned_inplace_table[slot];
		}
	}
	return shared;
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(engine_tests::engine_test_code_base,
		std::array{instruction});
}

/* Every form names its instruction by position in the operand rather than by
 * a PC word, so the stream is the gadget, the operand and the terminal. */
void require_stream(
	const gadgets::BuildResult &built, Gadget expected_gadget,
	std::uintptr_t expected_operand, std::size_t expected_words = 4U)
{
	test::require(built.complete &&
			built.program.words().size() == expected_words,
		"scalar immediate-memory stream has an unexpected shape");
	test::require(built.program.words()[0] == address(expected_gadget),
		"scalar immediate-memory gadget was not selected at translation");
	test::require(built.program.words()[1] == expected_operand,
		"scalar immediate-memory operand was not predecoded");
}

[[nodiscard]] std::uintptr_t packed_operand(
	unsigned rn, unsigned rt, std::uint64_t displacement)
{
	return transfer_offset(rt) | (base_offset(rn) << 16U) |
		(static_cast<std::uintptr_t>(displacement & 0xffffU) << 32U);
}

void select_all_reachable_gadgets()
{
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (unsigned index = 0; index < test::variants.size(); ++index) {
		const auto &variant = test::variants[index];
		const auto &row = rows[index];
		for (unsigned base_sp = 0; base_sp < 2; ++base_sp)
			for (unsigned transfer_zero = 0;
			     transfer_zero < 2; ++transfer_zero) {
				const auto rn = base_sp != 0 ? 31U : 7U;
				const auto rt =
					transfer_zero != 0 ? 31U : 5U;
				const unsigned imm12 = 0xa5U;
				const auto entry = unsigned_offset_entry(
					row.unsigned_offset[base_sp]
						[transfer_zero],
					index, base_sp, transfer_zero, rt);
				const auto instruction = test::encode(
					variant, test::Form::unsigned_offset,
					imm12, rn, rt);
				require_stream(build(instruction), entry,
					packed_operand(rn, rt,
						imm12 * variant.size));
				selected.insert(address(entry));
				expected.insert(address(entry));
			}

		constexpr std::array forms{
			test::Form::unscaled,
			test::Form::pre_index,
			test::Form::post_index,
		};
		for (unsigned mode = 0; mode < forms.size(); ++mode)
			for (unsigned base_sp = 0; base_sp < 2; ++base_sp)
				for (unsigned transfer_zero = 0;
				     transfer_zero < 2; ++transfer_zero) {
					const auto rn =
						base_sp != 0 ? 31U : 7U;
					const auto rt = transfer_zero != 0 ?
						31U : 5U;
					const auto imm9 =
						test::encode_imm9(-91);
					const auto &entry =
						row.immediate[mode][base_sp]
							[transfer_zero];
					const auto instruction = test::encode(
						variant, forms[mode], imm9,
						rn, rt);
					require_stream(build(instruction), entry,
						packed_operand(rn, rt,
							static_cast<std::uint64_t>(
								test::signed_imm9(
									imm9))));
					selected.insert(address(entry));
					expected.insert(address(entry));
				}

		/* LDTR/STTR have the same straight-line address operation as LDUR/
		 * STUR and therefore intentionally reuse the offset entry.
		 */
		const auto unprivileged = build(test::encode(
			variant, test::Form::unprivileged,
			test::encode_imm9(37), 7, 5));
		require_stream(unprivileged, row.immediate[0][0][0],
			packed_operand(7, 5, 37));

		if (variant.load) {
			test::require(row.post_suppress != nullptr,
				"load row lacks overlap-suppression gadget");
			const auto post = build(test::encode(
				variant, test::Form::post_index,
				test::encode_imm9(17), 5, 5));
			require_stream(post, row.post_suppress,
				packed_operand(5, 5, 17));
			selected.insert(address(row.post_suppress));
			expected.insert(address(row.post_suppress));

			/* Suppressing pre-index writeback leaves an adjusted,
			 * non-writeback access: the ordinary offset entry is exact.
			 */
			const auto pre = build(test::encode(
				variant, test::Form::pre_index,
				test::encode_imm9(17), 5, 5));
			require_stream(pre, row.immediate[0][0][0],
				packed_operand(5, 5, 17));
		} else {
			test::require(row.post_suppress == nullptr,
				"store row unexpectedly has suppression gadget");
		}
	}
	test::require(selected == expected && selected.size() == 217U,
		"scalar immediate-memory bank has an unselected or missing gadget");
}

[[nodiscard]] unsigned conditional_branches(Gadget gadget)
{
	const auto shape = a64::body_shape(address(gadget), {.window = 160U});
	if (shape.reached_dispatch)
		return shape.conditional_branches;
	test::require(false,
		"scalar immediate-memory gadget has no threaded dispatch");
	return 0;
}

[[nodiscard]] unsigned instruction_count(Gadget gadget, std::uint32_t expected)
{
	unsigned count = 0;
	const auto shape = a64::body_shape(address(gadget), {.window = 160U},
		[&](std::uint32_t instruction) {
			count += instruction == expected;
		});
	if (shape.reached_dispatch)
		return count;
	test::require(false,
		"scalar immediate-memory gadget has no threaded dispatch");
	return 0;
}

void hot_path_branch_audit()
{
	constexpr std::uint32_t tbi_state_load = 0x3940'0000U |
		(static_cast<std::uint32_t>(
			offsetof(arm64::CpuState, tbi_enabled)) << 10U) |
		(1U << 5U) | 9U;
	for (unsigned index = 0; index < rows.size(); ++index) {
		const auto &row = rows[index];
		for (unsigned transfer_zero = 0;
		     transfer_zero < 2; ++transfer_zero) {
			/* The baked entries come from the same template, so
			 * a representative register stands for its row. */
			const auto unsigned_gpr = unsigned_offset_entry(
				row.unsigned_offset[0][transfer_zero], index,
				0U, transfer_zero, 5U);
			const auto unsigned_sp = unsigned_offset_entry(
				row.unsigned_offset[1][transfer_zero], index,
				1U, transfer_zero, 5U);
			test::require(
				instruction_count(unsigned_gpr, tbi_state_load) == 1U &&
				instruction_count(unsigned_sp, tbi_state_load) == 0U,
				"unsigned gadget has the wrong TBI-state check");
			test::require(conditional_branches(unsigned_sp) ==
				conditional_branches(unsigned_gpr),
				"unsigned SP alignment and GPR TBI branch counts differ");
			for (unsigned mode = 0; mode < 3; ++mode) {
				const auto immediate_gpr =
					row.immediate[mode][0][transfer_zero];
				const auto immediate_sp =
					row.immediate[mode][1][transfer_zero];
				test::require(
					instruction_count(immediate_gpr,
						tbi_state_load) == 1U &&
					instruction_count(immediate_sp,
						tbi_state_load) == 0U,
					"immediate gadget has the wrong TBI-state check");
				test::require(conditional_branches(immediate_sp) ==
					conditional_branches(immediate_gpr),
					"immediate SP alignment and GPR TBI branch counts differ");
			}
		}
	}
}

} // namespace

int main()
{
	select_all_reachable_gadgets();
	hot_path_branch_audit();
	return 0;
}
