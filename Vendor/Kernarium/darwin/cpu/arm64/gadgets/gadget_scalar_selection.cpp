// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_selection_api.hpp"

namespace uml::cpu::arm64::gadgets::selection {

[[nodiscard]] bool pointer_auth_lr_unavailable(
	std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'c000U) == 0xdac1'8000U ||
		(instruction & 0xffc0'0000U) == 0xf380'0000U ||
		(instruction & 0xffc0'0000U) == 0x5500'0000U ||
		((instruction & 0xffff'fbe0U) == 0xd65f'0be0U &&
		 (instruction & 31U) != 31U);
}

/* The cases of a switch over the conditions for one family of gadgets, as
 * UML_ARM64_CONDITION_GADGETS lists its entries. */
#define UML_CONDITION_CASE(index, name, prefix, suffix) \
	case index: return prefix##name##suffix;
#define UML_CONDITION_CASES(prefix, suffix) \
	UML_ARM64_CONDITIONS(UML_CONDITION_CASE, prefix, suffix)

/* Fused CMP of two registers + B.cond (G116); the builder says why. */
[[nodiscard]] Gadget compare_register_branch_gadget(
	bool wide, unsigned condition, bool linked) noexcept
{
	static constexpr std::array wide_direct{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_register_branch_x_, )};
	static constexpr std::array narrow_direct{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_register_branch_w_, )};
	static constexpr std::array wide_linked{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_register_branch_x_, _linked)};
	static constexpr std::array narrow_linked{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_register_branch_w_, _linked)};
	if (condition >= wide_direct.size())
		return nullptr;
	if (linked)
		return wide ? wide_linked[condition] : narrow_linked[condition];
	return wide ? wide_direct[condition] : narrow_direct[condition];
}

/* Fused CMP-immediate + B.cond. Conditions 14 and 15 are the always forms,
 * which the branch decoder folds before it ever reaches a fusion, so they have
 * no fused entry and select nothing here. */
[[nodiscard]] Gadget compare_immediate_branch_gadget(
	bool wide, unsigned condition, bool linked) noexcept
{
	static constexpr std::array wide_direct{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_immediate_branch_x_, )};
	static constexpr std::array narrow_direct{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_immediate_branch_w_, )};
	static constexpr std::array wide_linked{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_immediate_branch_x_, _linked)};
	static constexpr std::array narrow_linked{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_immediate_branch_w_, _linked)};
	if (condition >= wide_direct.size())
		return nullptr;
	if (linked)
		return wide ? wide_linked[condition] : narrow_linked[condition];
	return wide ? wide_direct[condition] : narrow_direct[condition];
}

/* The fall-through translated inline behind the fused pair. */
[[nodiscard]] Gadget compare_immediate_branch_inline_gadget(
	bool wide, unsigned condition) noexcept
{
	static constexpr std::array wide_inline{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_immediate_branch_x_, _inline)};
	static constexpr std::array narrow_inline{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_immediate_branch_w_, _inline)};
	if (condition >= wide_inline.size())
		return nullptr;
	return wide ? wide_inline[condition] : narrow_inline[condition];
}

/* The fall-through translated inline behind the fused pair. */
[[nodiscard]] Gadget compare_register_branch_inline_gadget(
	bool wide, unsigned condition) noexcept
{
	static constexpr std::array wide_inline{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_register_branch_x_, _inline)};
	static constexpr std::array narrow_inline{UML_ARM64_CONDITION_GADGETS(
		uml_arm64_gadget_compare_register_branch_w_, _inline)};
	if (condition >= wide_inline.size())
		return nullptr;
	return wide ? wide_inline[condition] : narrow_inline[condition];
}

[[nodiscard]] Gadget conditional_branch_gadget(unsigned condition) noexcept
{
	switch (condition) {
	UML_CONDITION_CASES(uml_arm64_gadget_branch_, )
	default: return uml_arm64_gadget_branch_always;
	}
}

[[nodiscard]] Gadget conditional_branch_linked_gadget(
	unsigned condition) noexcept
{
	switch (condition) {
	UML_CONDITION_CASES(uml_arm64_gadget_branch_, _linked)
	default: return uml_arm64_gadget_branch_always_linked;
	}
}

/* The fall-through translated inline behind the branch. The always forms have
 * no fall-through to translate, so they keep ending the block. */
[[nodiscard]] Gadget conditional_branch_inline_gadget(
	unsigned condition) noexcept
{
	switch (condition) {
	UML_CONDITION_CASES(uml_arm64_gadget_branch_, _inline)
	default: return nullptr;
	}
}

#undef UML_CONDITION_CASES
#undef UML_CONDITION_CASE

/* The fall-through translated inline behind the branch. */
[[nodiscard]] Gadget compare_branch_inline_gadget(
	bool wide, bool nonzero) noexcept
{
	if (wide)
		return nonzero ? uml_arm64_gadget_compare_branch_x_nz_inline :
			uml_arm64_gadget_compare_branch_x_z_inline;
	return nonzero ? uml_arm64_gadget_compare_branch_w_nz_inline :
		uml_arm64_gadget_compare_branch_w_z_inline;
}

[[nodiscard]] Gadget test_bit_branch_inline_gadget(bool nonzero) noexcept
{
	return nonzero ? uml_arm64_gadget_test_bit_branch_nz_inline :
		uml_arm64_gadget_test_bit_branch_z_inline;
}

[[nodiscard]] Gadget compare_branch_gadget(bool wide, bool nonzero,
	bool linked) noexcept
{
	static constexpr std::array table{
		std::array{
			std::array{uml_arm64_gadget_compare_branch_w_z,
				uml_arm64_gadget_compare_branch_w_z_linked},
			std::array{uml_arm64_gadget_compare_branch_w_nz,
				uml_arm64_gadget_compare_branch_w_nz_linked}},
		std::array{
			std::array{uml_arm64_gadget_compare_branch_x_z,
				uml_arm64_gadget_compare_branch_x_z_linked},
			std::array{uml_arm64_gadget_compare_branch_x_nz,
				uml_arm64_gadget_compare_branch_x_nz_linked}},
	};
	return table[wide][nonzero][linked];
}

[[nodiscard]] Gadget test_bit_branch_gadget(bool nonzero, bool linked) noexcept
{
	static constexpr std::array table{
		std::array{uml_arm64_gadget_test_bit_branch_z,
			uml_arm64_gadget_test_bit_branch_z_linked},
		std::array{uml_arm64_gadget_test_bit_branch_nz,
			uml_arm64_gadget_test_bit_branch_nz_linked},
	};
	return table[nonzero][linked];
}

[[nodiscard]] Gadget mops_gadget(
	unsigned family, unsigned stage, bool source_zero) noexcept
{
	static constexpr std::array prologue{
		uml_arm64_gadget_mops_prologue_copy_forward,
		uml_arm64_gadget_mops_prologue_copy_general,
		uml_arm64_gadget_mops_prologue_set};
	static constexpr std::array epilogue{
		uml_arm64_gadget_mops_epilogue_copy_forward,
		uml_arm64_gadget_mops_epilogue_copy_general};
	if (stage == 0)
		return prologue[family];
	if (stage == 1)
		return uml_arm64_gadget_nop;
	if (family == 2)
		return source_zero ? uml_arm64_gadget_mops_epilogue_set_z :
			uml_arm64_gadget_mops_epilogue_set_r;
	return epilogue[family];
}

[[nodiscard]] PstateImmediateGadget pstate_boolean_gadget(
	Gadget zero, Gadget one, unsigned immediate) noexcept
{
	return {(immediate & 1U) != 0 ? one : zero, true, false};
}

extern constexpr std::array<Gadget, 16> pstate_daif_set_gadgets{
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

extern constexpr std::array<Gadget, 16> pstate_daif_clear_gadgets{
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

[[nodiscard]] PstateImmediateGadget pstate_immediate_gadget(
	std::uint32_t instruction) noexcept
{
	const unsigned op1 = (instruction >> 16) & 7U;
	const unsigned immediate = (instruction >> 8) & 15U;
	const unsigned op2 = (instruction >> 5) & 7U;
	if (op1 != 3U)
		return {uml_arm64_gadget_illegal, false, false};
	/* The virtual CPU implements neither FEAT_SSBS (op2 1) nor FEAT_SME
	 * (op2 3, SMSTART and SMSTOP): their fields are unallocated. */
	switch (op2) {
	case 2:
		return pstate_boolean_gadget(
			uml_arm64_gadget_pstate_dit_0,
			uml_arm64_gadget_pstate_dit_1, immediate);
	case 6:
		return {pstate_daif_set_gadgets[immediate], true, true};
	case 7:
		return {pstate_daif_clear_gadgets[immediate], true, true};
	default: break;
	}
	return {uml_arm64_gadget_illegal, false, false};
}

[[nodiscard]] SystemRegisterGadget system_register_gadget(
	std::uint32_t instruction) noexcept
{
	const auto system = instruction & 0xffff'ffe0U;
	const bool zero = (instruction & 31U) == 31U;
	/* Discarded reads have no architectural side effects in this EL0 model. */
	if (zero && (instruction & 0x0020'0000U) != 0)
		return {uml_arm64_gadget_nop, false};
	if (system == 0xd53b'd040U)
		return {uml_arm64_gadget_sysreg_tls_read, true};
	if (system == 0xd51b'd040U)
		return {zero ? uml_arm64_gadget_sysreg_tls_write_z :
			uml_arm64_gadget_sysreg_tls_write_r, !zero};
	if (system == 0xd53b'd060U)
		return {uml_arm64_gadget_sysreg_zero_read, true};
	if (system == 0xd53b'00e0U)
		return {uml_arm64_gadget_sysreg_dczid_read, true};
	if (system == 0xd53b'0020U)
		return {uml_arm64_gadget_sysreg_ctr_read, true};
	if (system == 0xd53b'4400U)
		return {uml_arm64_gadget_sysreg_fpcr_read, true};
	if (system == 0xd51b'4400U)
		return {zero ? uml_arm64_gadget_sysreg_fpcr_write_z :
			uml_arm64_gadget_sysreg_fpcr_write_r, !zero};
	if (system == 0xd53b'4420U)
		return {uml_arm64_gadget_sysreg_fpsr_read, true};
	if (system == 0xd51b'4420U)
		return {zero ? uml_arm64_gadget_sysreg_fpsr_write_z :
			uml_arm64_gadget_sysreg_fpsr_write_r, !zero};
	if (system == 0xd53b'4200U)
		return {uml_arm64_gadget_sysreg_nzcv_read, true};
	if (system == 0xd51b'4200U)
		return {zero ? uml_arm64_gadget_sysreg_nzcv_write_z :
			uml_arm64_gadget_sysreg_nzcv_write_r, !zero};
	if (system == 0xd53b'e000U)
		return {uml_arm64_gadget_sysreg_cntfrq_read, true};
	if (system == 0xd53b'42a0U)
		return {uml_arm64_gadget_sysreg_dit_read, true};
	if (system == 0xd51b'42a0U)
		return {zero ? uml_arm64_gadget_sysreg_dit_write_z :
			uml_arm64_gadget_sysreg_dit_write_r, !zero};
	if (system == 0xd53b'e0c0U)
		return {uml_arm64_gadget_sysreg_cntvctss_read, true};
	/* CNTVCT_EL0 is the residual case.  This selector has no illegal result,
	 * so it may only be reached for encodings the program builder's decode
	 * gate already accepted; the two lists must be extended together. */
	return {uml_arm64_gadget_sysreg_cntvct_read, true};
}

#define UML_CONDITIONAL_COMPARE_FAST_ROW(index, name) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_i, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_r, \
		uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_i},

[[nodiscard]] Gadget conditional_compare_gadget(unsigned condition,
	bool subtract, bool is_64_bit, bool immediate) noexcept
{
	static constexpr std::array table{
		UML_ARM64_CONDITIONS(UML_CONDITIONAL_COMPARE_FAST_ROW)
		UML_CONDITIONAL_COMPARE_FAST_ROW(14, always)};
	return table[condition < 14U ? condition : 14U]
		[subtract * 4U + is_64_bit * 2U + immediate];
}

#undef UML_CONDITIONAL_COMPARE_FAST_ROW

#define UML_CONDITIONAL_SELECT_FAST_ROW(index, name) \
	std::array<Gadget, 8>{ \
		uml_arm64_gadget_conditional_select_fast_##name##_w_sel, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_sel, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_inc, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_inc, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_inv, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_inv, \
		uml_arm64_gadget_conditional_select_fast_##name##_w_neg, \
		uml_arm64_gadget_conditional_select_fast_##name##_x_neg},

[[nodiscard]] Gadget conditional_select_gadget(unsigned condition,
	unsigned operation, bool is_64_bit) noexcept
{
	static constexpr std::array table{
		UML_ARM64_CONDITIONS(UML_CONDITIONAL_SELECT_FAST_ROW)};
	if (condition >= 14U)
		return is_64_bit ?
			uml_arm64_gadget_conditional_select_fast_always_x :
			uml_arm64_gadget_conditional_select_fast_always_w;
	return table[condition][operation * 2U + is_64_bit];
}

#undef UML_CONDITIONAL_SELECT_FAST_ROW

[[nodiscard]] Gadget data_memory_barrier_gadget(
	bool synchronization, unsigned option) noexcept
{
	static constexpr std::array<Gadget, 16> dmb{
		UML_ARM64_BARRIER_GADGETS(uml_arm64_gadget_dmb_)};
	static constexpr std::array<Gadget, 16> dsb{
		UML_ARM64_BARRIER_GADGETS(uml_arm64_gadget_dsb_)};
	return (synchronization ? dsb : dmb)[option & 15U];
}

/* DMB, DSB and SB. ISB carries operands, so the builder emits it itself. */
[[nodiscard]] Gadget memory_barrier_gadget(std::uint32_t instruction) noexcept
{
	if (instruction == 0xd503'30ffU)
		return host_capabilities().has(ArmFeature::sb) ?
			uml_arm64_gadget_speculation_barrier :
			uml_arm64_gadget_speculation_barrier_fallback;
	return data_memory_barrier_gadget(
		(instruction & 0x60U) == 0, (instruction >> 8) & 15U);
}

[[nodiscard]] Gadget architectural_hint_gadget(
	std::uint32_t instruction) noexcept
{
	const unsigned immediate = (instruction >> 5) & 0x7fU;
	if (immediate == 1U)
		return uml_arm64_gadget_yield;
	if (immediate == 6U)
		return uml_arm64_gadget_dgh;
	if (immediate == 20U)
		return uml_arm64_gadget_csdb;
	if (immediate == 22U)
		return uml_arm64_gadget_clrbhb;
	/* The remaining allocated hints have no guest-visible effect in the
	 * currently exposed architecture: pointer authentication, guarded
	 * pages, GCS, RAS delivery, profiling, trace, and a blocking event
	 * register are not advertised.
	 * Their architectural feature-absent or unguarded behavior is NOP. Keep
	 * that policy at translation time so the valid hot path has no decoder.
	 */
	return uml_arm64_gadget_nop;
}

#define UML_INTEGER_MIN_MAX_FAST_ROW(name) \
	std::array<Gadget, 4>{ \
		uml_arm64_gadget_integer_##name##_fast_w_r, \
		uml_arm64_gadget_integer_##name##_fast_w_i, \
		uml_arm64_gadget_integer_##name##_fast_x_r, \
		uml_arm64_gadget_integer_##name##_fast_x_i}

[[nodiscard]] Gadget integer_min_max_gadget(unsigned operation,
	bool is_64_bit, bool immediate) noexcept
{
	static constexpr std::array table{
		UML_INTEGER_MIN_MAX_FAST_ROW(smax),
		UML_INTEGER_MIN_MAX_FAST_ROW(umax),
		UML_INTEGER_MIN_MAX_FAST_ROW(smin),
		UML_INTEGER_MIN_MAX_FAST_ROW(umin)};
	return table[operation][is_64_bit * 2U + immediate];
}

#undef UML_INTEGER_MIN_MAX_FAST_ROW

#define UML_SELECT_ADD_SUB_IMMEDIATE_MODES(prefix) \
	if (source_sp) \
		return destination_special ? \
			uml_arm64_gadget_add_sub_immediate_##prefix##_ss : \
			uml_arm64_gadget_add_sub_immediate_##prefix##_sr; \
	return destination_special ? \
		uml_arm64_gadget_add_sub_immediate_##prefix##_rs : \
		uml_arm64_gadget_add_sub_immediate_##prefix##_rr

[[nodiscard]] Gadget add_sub_immediate_gadget(bool is_64_bit, bool subtract,
	bool set_flags, bool source_sp, bool destination_special) noexcept
{
	if (is_64_bit) {
		if (subtract) {
			if (set_flags) {
				UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_sub_s);
			}
			UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_sub_n);
		}
		if (set_flags) {
			UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_add_s);
		}
		UML_SELECT_ADD_SUB_IMMEDIATE_MODES(x_add_n);
	}
	if (subtract) {
		if (set_flags) {
			UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_sub_s);
		}
		UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_sub_n);
	}
	if (set_flags) {
		UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_add_s);
	}
	UML_SELECT_ADD_SUB_IMMEDIATE_MODES(w_add_n);
}

#undef UML_SELECT_ADD_SUB_IMMEDIATE_MODES

/* Rd == Rn, both ordinary registers: one baked displacement serves the store
 * and the load, so the table is one row of 31 per operation. */
[[nodiscard]] Gadget add_sub_immediate_inplace_gadget(bool is_64_bit, bool subtract,
	bool set_flags, unsigned int register_index) noexcept
{
	const auto row = (static_cast<unsigned>(is_64_bit) * 2U +
		static_cast<unsigned>(subtract)) * 2U + static_cast<unsigned>(set_flags);
	return uml_arm64_gadget_add_sub_immediate_inplace_table[row * 31U + register_index];
}

/* and/orr/eor/ands Rd, Rd, #imm: row = width * 4 + operation. */
[[nodiscard]] Gadget logical_immediate_inplace_gadget(bool is_64_bit,
	unsigned int operation, unsigned int register_index) noexcept
{
	return uml_arm64_gadget_logical_immediate_inplace_table[
		(static_cast<unsigned>(is_64_bit) * 4U + operation) * 31U + register_index];
}

/* movk Rd, #imm, lsl #shift: rows are w0 w1 x0 x1 x2 x3, matching the bank. */
[[nodiscard]] Gadget move_wide_keep_inplace_gadget(bool is_64_bit,
	unsigned int halfword, unsigned int register_index) noexcept
{
	const auto row = is_64_bit ? 2U + halfword : halfword;
	return uml_arm64_gadget_move_wide_keep_inplace_table[row * 31U + register_index];
}

/* add/sub Rd, Rd, Rm with no shift. */
[[nodiscard]] Gadget add_sub_register_inplace_gadget(bool is_64_bit, bool subtract,
	bool set_flags, unsigned int register_index) noexcept
{
	const auto row = (static_cast<unsigned>(is_64_bit) * 2U +
		static_cast<unsigned>(subtract)) * 2U + static_cast<unsigned>(set_flags);
	return uml_arm64_gadget_add_sub_register_inplace_table[row * 31U + register_index];
}

#define UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix) \
	return destination_discard ? \
		uml_arm64_gadget_add_sub_register_##prefix##_d : \
		uml_arm64_gadget_add_sub_register_##prefix##_r
#define UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_zz); \
		} \
		UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_zr); \
	} \
	if (right_zero) { \
		UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_rz); \
	} \
	UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_rr)
#define UML_SELECT_ADD_SUB_SHIFTED_TRANSFORM(prefix) \
	switch (transform) { \
	case 0: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_n); \
	case 1: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_l); \
	case 2: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_r); \
	default: UML_SELECT_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_a); \
	}
#define UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix) \
	if (right_zero) { \
		UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_z); \
	} \
	UML_SELECT_ADD_SUB_REGISTER_DESTINATION(prefix##_r)
#define UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, number) \
	if (extension == number) { \
		if (shift_nonzero) { \
			UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e##number##s); \
		} \
		UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e##number##n); \
	}
#define UML_SELECT_ADD_SUB_EXTENDED_TRANSFORM(prefix) \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 0); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 1); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 2); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 3); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 4); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 5); \
	UML_SELECT_ADD_SUB_EXTENDED_ONE(prefix, 6); \
	if (shift_nonzero) { \
		UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e7s); \
	} \
	UML_SELECT_ADD_SUB_EXTENDED_SOURCE(prefix##_ex_e7n)
#define UML_SELECT_ADD_SUB_REGISTER_FORM(prefix) \
	if (extended) { \
		UML_SELECT_ADD_SUB_EXTENDED_TRANSFORM(prefix); \
	} \
	UML_SELECT_ADD_SUB_SHIFTED_TRANSFORM(prefix)

[[nodiscard]] Gadget add_sub_register_gadget(bool is_64_bit, bool subtract,
	bool set_flags, bool extended, unsigned transform, unsigned extension,
	bool shift_nonzero, bool left_zero, bool right_zero,
	bool destination_discard) noexcept
{
	if (is_64_bit) {
		if (subtract) {
			if (set_flags) {
				UML_SELECT_ADD_SUB_REGISTER_FORM(x_sub_s);
			}
			UML_SELECT_ADD_SUB_REGISTER_FORM(x_sub_n);
		}
		if (set_flags) {
			UML_SELECT_ADD_SUB_REGISTER_FORM(x_add_s);
		}
		UML_SELECT_ADD_SUB_REGISTER_FORM(x_add_n);
	}
	if (subtract) {
		if (set_flags) {
			UML_SELECT_ADD_SUB_REGISTER_FORM(w_sub_s);
		}
		UML_SELECT_ADD_SUB_REGISTER_FORM(w_sub_n);
	}
	if (set_flags) {
		UML_SELECT_ADD_SUB_REGISTER_FORM(w_add_s);
	}
	UML_SELECT_ADD_SUB_REGISTER_FORM(w_add_n);
}

#undef UML_SELECT_ADD_SUB_REGISTER_FORM
#undef UML_SELECT_ADD_SUB_EXTENDED_TRANSFORM
#undef UML_SELECT_ADD_SUB_EXTENDED_ONE
#undef UML_SELECT_ADD_SUB_EXTENDED_SOURCE
#undef UML_SELECT_ADD_SUB_SHIFTED_TRANSFORM
#undef UML_SELECT_ADD_SUB_SHIFTED_SOURCES
#undef UML_SELECT_ADD_SUB_REGISTER_DESTINATION

#define UML_SELECT_ADD_SUB_CARRY_FLAGS(prefix) \
	return destination_zero ? \
		uml_arm64_gadget_add_sub_carry_##prefix##_z : \
		uml_arm64_gadget_add_sub_carry_##prefix##_r

#define UML_SELECT_ADD_SUB_CARRY_SOURCES(prefix, result) \
	if (left_zero) { \
		if (right_zero) { \
			result(prefix##_zz); \
		} \
		result(prefix##_zr); \
	} \
	if (right_zero) { \
		result(prefix##_rz); \
	} \
	if (same_source) { \
		result(prefix##_eq); \
	} \
	result(prefix##_rr)

#define UML_SELECT_ADD_SUB_CARRY_RESULT(prefix) \
	return uml_arm64_gadget_add_sub_carry_##prefix##_r

[[nodiscard]] Gadget add_sub_carry_gadget(bool is_64_bit, bool subtract,
	bool set_flags, bool left_zero, bool right_zero,
	bool same_source, bool destination_zero) noexcept
{
	if (destination_zero && !set_flags)
		return uml_arm64_gadget_nop;
	if (is_64_bit) {
		if (subtract) {
			if (set_flags) {
				UML_SELECT_ADD_SUB_CARRY_SOURCES(x_sbc_s,
					UML_SELECT_ADD_SUB_CARRY_FLAGS);
			}
			UML_SELECT_ADD_SUB_CARRY_SOURCES(x_sbc_n,
				UML_SELECT_ADD_SUB_CARRY_RESULT);
		}
		if (set_flags) {
			UML_SELECT_ADD_SUB_CARRY_SOURCES(x_adc_s,
				UML_SELECT_ADD_SUB_CARRY_FLAGS);
		}
		UML_SELECT_ADD_SUB_CARRY_SOURCES(x_adc_n,
			UML_SELECT_ADD_SUB_CARRY_RESULT);
	}
	if (subtract) {
		if (set_flags) {
			UML_SELECT_ADD_SUB_CARRY_SOURCES(w_sbc_s,
				UML_SELECT_ADD_SUB_CARRY_FLAGS);
		}
		UML_SELECT_ADD_SUB_CARRY_SOURCES(w_sbc_n,
			UML_SELECT_ADD_SUB_CARRY_RESULT);
	}
	if (set_flags) {
		UML_SELECT_ADD_SUB_CARRY_SOURCES(w_adc_s,
			UML_SELECT_ADD_SUB_CARRY_FLAGS);
	}
	UML_SELECT_ADD_SUB_CARRY_SOURCES(w_adc_n,
		UML_SELECT_ADD_SUB_CARRY_RESULT);
}

#undef UML_SELECT_ADD_SUB_CARRY_RESULT
#undef UML_SELECT_ADD_SUB_CARRY_SOURCES
#undef UML_SELECT_ADD_SUB_CARRY_FLAGS

#define UML_SELECT_BINARY_SOURCES(kind, prefix) \
	if (left_zero) { \
		if (right_zero) { \
			return uml_arm64_gadget_##kind##_##prefix##_zz_r; \
		} \
		return uml_arm64_gadget_##kind##_##prefix##_zr_r; \
	} \
	if (right_zero) { \
		return uml_arm64_gadget_##kind##_##prefix##_rz_r; \
	} \
	if (same_source) { \
		return uml_arm64_gadget_##kind##_##prefix##_eq_r; \
	} \
	return uml_arm64_gadget_##kind##_##prefix##_rr_r

[[nodiscard]] Gadget variable_shift_gadget(bool is_64_bit,
	unsigned operation, bool left_zero, bool right_zero,
	bool same_source) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0: UML_SELECT_BINARY_SOURCES(variable_shift, x_lsl);
		case 1: UML_SELECT_BINARY_SOURCES(variable_shift, x_lsr);
		case 2: UML_SELECT_BINARY_SOURCES(variable_shift, x_asr);
		default: UML_SELECT_BINARY_SOURCES(variable_shift, x_ror);
		}
	}
	switch (operation) {
	case 0: UML_SELECT_BINARY_SOURCES(variable_shift, w_lsl);
	case 1: UML_SELECT_BINARY_SOURCES(variable_shift, w_lsr);
	case 2: UML_SELECT_BINARY_SOURCES(variable_shift, w_asr);
	default: UML_SELECT_BINARY_SOURCES(variable_shift, w_ror);
	}
}

[[nodiscard]] Gadget divide_gadget(bool is_64_bit, bool is_signed,
	bool left_zero, bool right_zero, bool same_source) noexcept
{
	if (is_64_bit) {
		if (is_signed) {
			UML_SELECT_BINARY_SOURCES(divide, x_s);
		}
		UML_SELECT_BINARY_SOURCES(divide, x_u);
	}
	if (is_signed) {
		UML_SELECT_BINARY_SOURCES(divide, w_s);
	}
	UML_SELECT_BINARY_SOURCES(divide, w_u);
}

#undef UML_SELECT_BINARY_SOURCES

#define UML_SELECT_LOGICAL_IMMEDIATE_MODES(prefix) \
	if (source_zero) \
		return destination_special ? \
			uml_arm64_gadget_logical_immediate_##prefix##_zs : \
			uml_arm64_gadget_logical_immediate_##prefix##_zr; \
	return destination_special ? \
		uml_arm64_gadget_logical_immediate_##prefix##_rs : \
		uml_arm64_gadget_logical_immediate_##prefix##_rr

[[nodiscard]] Gadget logical_immediate_gadget(bool is_64_bit,
	unsigned operation, bool source_zero, bool destination_special) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_and);
		case 1: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_orr);
		case 2: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_eor);
		default: UML_SELECT_LOGICAL_IMMEDIATE_MODES(x_ands);
		}
	}
	switch (operation) {
	case 0: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_and);
	case 1: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_orr);
	case 2: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_eor);
	default: UML_SELECT_LOGICAL_IMMEDIATE_MODES(w_ands);
	}
}

#undef UML_SELECT_LOGICAL_IMMEDIATE_MODES

#define UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) \
			return uml_arm64_gadget_logical_shifted_##prefix##_zz_r; \
		return uml_arm64_gadget_logical_shifted_##prefix##_zr_r; \
	} \
	if (right_zero) \
		return uml_arm64_gadget_logical_shifted_##prefix##_rz_r; \
	return uml_arm64_gadget_logical_shifted_##prefix##_rr_r
#define UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(prefix) \
	switch (shift_kind) { \
	case 0: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_n); \
	case 1: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_l); \
	case 2: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_r); \
	case 3: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_a); \
	default: UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_o); \
	}
#define UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix) \
	return destination_zero ? \
		uml_arm64_gadget_logical_shifted_##prefix##_z : \
		uml_arm64_gadget_logical_shifted_##prefix##_r
#define UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_zz); \
		} \
		UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_zr); \
	} \
	if (right_zero) { \
		UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_rz); \
	} \
	UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION(prefix##_rr)
#define UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(prefix) \
	switch (shift_kind) { \
	case 0: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_n); \
	case 1: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_l); \
	case 2: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_r); \
	case 3: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_a); \
	default: UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_o); \
	}

[[nodiscard]] Gadget logical_shifted_gadget(bool is_64_bit,
	unsigned operation, bool invert, unsigned shift_kind, bool left_zero,
	bool right_zero, bool destination_zero) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_and_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_and_p);
		case 1:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_orr_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_orr_p);
		case 2:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_eor_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(x_eor_p);
		default:
			if (invert) {
				UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(x_ands_n);
			}
			UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(x_ands_p);
		}
	}
	switch (operation) {
	case 0:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_and_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_and_p);
	case 1:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_orr_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_orr_p);
	case 2:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_eor_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT(w_eor_p);
	default:
		if (invert) {
			UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(w_ands_n);
		}
		UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT(w_ands_p);
	}
}

#undef UML_SELECT_LOGICAL_SHIFTED_FLAG_SHIFT
#undef UML_SELECT_LOGICAL_SHIFTED_FLAG_SOURCES
#undef UML_SELECT_LOGICAL_SHIFTED_FLAG_DESTINATION
#undef UML_SELECT_LOGICAL_SHIFTED_RESULT_SHIFT
#undef UML_SELECT_LOGICAL_SHIFTED_RESULT_SOURCES

#define UML_SELECT_MULTIPLY_RESULT(prefix) \
	return uml_arm64_gadget_multiply_add_##prefix##_r
#define UML_SELECT_MULTIPLY_ADDEND(prefix) \
	if (addend_zero) { \
		UML_SELECT_MULTIPLY_RESULT(prefix##_z); \
	} \
	UML_SELECT_MULTIPLY_RESULT(prefix##_r)
#define UML_SELECT_MULTIPLY_SOURCES(prefix) \
	if (left_zero || right_zero) { \
		UML_SELECT_MULTIPLY_ADDEND(prefix##_z); \
	} \
	UML_SELECT_MULTIPLY_ADDEND(prefix##_p)
#define UML_SELECT_MULTIPLY_HIGH_SOURCES(prefix) \
	if (left_zero || right_zero) { \
		UML_SELECT_MULTIPLY_RESULT(prefix##_z_z); \
	} \
	UML_SELECT_MULTIPLY_RESULT(prefix##_p_z)

[[nodiscard]] Gadget multiply_add_gadget(unsigned operation, bool subtract,
	bool is_64_bit, bool left_zero, bool right_zero,
	bool addend_zero) noexcept
{
	switch (operation) {
	case 0:
		if (is_64_bit) {
			if (subtract) {
				UML_SELECT_MULTIPLY_SOURCES(x_msub);
			}
			UML_SELECT_MULTIPLY_SOURCES(x_madd);
		}
		if (subtract) {
			UML_SELECT_MULTIPLY_SOURCES(w_msub);
		}
		UML_SELECT_MULTIPLY_SOURCES(w_madd);
	case 1:
		if (subtract) {
			UML_SELECT_MULTIPLY_SOURCES(smsubl);
		}
		UML_SELECT_MULTIPLY_SOURCES(smaddl);
	case 2:
		UML_SELECT_MULTIPLY_HIGH_SOURCES(smulh);
	case 5:
		if (subtract) {
			UML_SELECT_MULTIPLY_SOURCES(umsubl);
		}
		UML_SELECT_MULTIPLY_SOURCES(umaddl);
	default:
		UML_SELECT_MULTIPLY_HIGH_SOURCES(umulh);
	}
}

#undef UML_SELECT_MULTIPLY_HIGH_SOURCES
#undef UML_SELECT_MULTIPLY_SOURCES
#undef UML_SELECT_MULTIPLY_ADDEND
#undef UML_SELECT_MULTIPLY_RESULT

#define UML_SELECT_BITFIELD_SOURCES(prefix) \
	if (rotate_nonzero) \
		return source_zero ? uml_arm64_gadget_bitfield_##prefix##_r_z : \
			uml_arm64_gadget_bitfield_##prefix##_r_r; \
	return source_zero ? uml_arm64_gadget_bitfield_##prefix##_n_z : \
		uml_arm64_gadget_bitfield_##prefix##_n_r

[[nodiscard]] Gadget bitfield_gadget(bool is_64_bit, unsigned operation,
	bool rotate_nonzero, bool source_zero) noexcept
{
	if (is_64_bit) {
		switch (operation) {
		case 0: UML_SELECT_BITFIELD_SOURCES(x_s);
		case 1: UML_SELECT_BITFIELD_SOURCES(x_b);
		default: UML_SELECT_BITFIELD_SOURCES(x_u);
		}
	}
	switch (operation) {
	case 0: UML_SELECT_BITFIELD_SOURCES(w_s);
	case 1: UML_SELECT_BITFIELD_SOURCES(w_b);
	default: UML_SELECT_BITFIELD_SOURCES(w_u);
	}
}

#undef UML_SELECT_BITFIELD_SOURCES

/* SBFM and UBFM by their shift amounts: a form that shifts one way takes a
 * single-shift entry, anything else both. */
[[nodiscard]] Gadget bitfield_shift_gadget(bool is_64_bit, bool is_signed,
	bool left_nonzero, bool right_nonzero) noexcept
{
	if (is_64_bit) {
		if (!right_nonzero)
			return uml_arm64_gadget_bitfield_shift_x_lsl;
		if (!left_nonzero)
			return is_signed ? uml_arm64_gadget_bitfield_shift_x_asr :
				uml_arm64_gadget_bitfield_shift_x_lsr;
		return is_signed ? uml_arm64_gadget_bitfield_shift_x_sbfm :
			uml_arm64_gadget_bitfield_shift_x_ubfm;
	}
	if (!right_nonzero)
		return uml_arm64_gadget_bitfield_shift_w_lsl;
	if (!left_nonzero)
		return is_signed ? uml_arm64_gadget_bitfield_shift_w_asr :
			uml_arm64_gadget_bitfield_shift_w_lsr;
	return is_signed ? uml_arm64_gadget_bitfield_shift_w_sbfm :
		uml_arm64_gadget_bitfield_shift_w_ubfm;
}

#define UML_SELECT_EXTRACT_DESTINATION(prefix) \
	return destination_zero ? uml_arm64_gadget_extract_register_##prefix##_z : \
		uml_arm64_gadget_extract_register_##prefix##_r
#define UML_SELECT_EXTRACT_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			UML_SELECT_EXTRACT_DESTINATION(prefix##_zz); \
		} \
		UML_SELECT_EXTRACT_DESTINATION(prefix##_zr); \
	} \
	if (right_zero) { \
		UML_SELECT_EXTRACT_DESTINATION(prefix##_rz); \
	} \
	UML_SELECT_EXTRACT_DESTINATION(prefix##_rr)

[[nodiscard]] Gadget extract_register_gadget(bool is_64_bit,
	bool shift_nonzero, bool left_zero, bool right_zero,
	bool destination_zero) noexcept
{
	if (is_64_bit) {
		if (shift_nonzero) {
			UML_SELECT_EXTRACT_SOURCES(x_s);
		}
		UML_SELECT_EXTRACT_SOURCES(x_n);
	}
	if (shift_nonzero) {
		UML_SELECT_EXTRACT_SOURCES(w_s);
	}
	UML_SELECT_EXTRACT_SOURCES(w_n);
}

#undef UML_SELECT_EXTRACT_SOURCES
#undef UML_SELECT_EXTRACT_DESTINATION

#define UML_CRC_GADGET(prefix, suffix) \
	(native ? uml_arm64_gadget_crc32_##prefix##_##suffix : \
		uml_arm64_gadget_crc32_software_##prefix##_##suffix)
#define UML_SELECT_CRC_SOURCES(prefix) \
	if (left_zero) { \
		if (right_zero) { \
			return UML_CRC_GADGET(prefix, zz); \
		} \
		return UML_CRC_GADGET(prefix, zr); \
	} \
	if (right_zero) { \
		return UML_CRC_GADGET(prefix, rz); \
	} \
	return same_source ? UML_CRC_GADGET(prefix, same) : \
		UML_CRC_GADGET(prefix, rr)

[[nodiscard]] Gadget crc32_gadget(unsigned operation, bool left_zero,
	bool right_zero, bool same_source, bool native) noexcept
{
	switch (operation) {
	case 0: UML_SELECT_CRC_SOURCES(b);
	case 1: UML_SELECT_CRC_SOURCES(h);
	case 2: UML_SELECT_CRC_SOURCES(w);
	case 3: UML_SELECT_CRC_SOURCES(x);
	case 4: UML_SELECT_CRC_SOURCES(cb);
	case 5: UML_SELECT_CRC_SOURCES(ch);
	case 6: UML_SELECT_CRC_SOURCES(cw);
	default: UML_SELECT_CRC_SOURCES(cx);
	}
}

#undef UML_SELECT_CRC_SOURCES
#undef UML_CRC_GADGET

[[nodiscard]] Gadget bit_reverse_count_gadget(bool is_64_bit,
	unsigned operation) noexcept
{
	const bool native = operation >= 6U && host_capabilities().cssc;
	if (is_64_bit) {
		switch (operation) {
		case 0: return uml_arm64_gadget_bit_reverse_count_x_rbit;
		case 1: return uml_arm64_gadget_bit_reverse_count_x_rev16;
		case 2: return uml_arm64_gadget_bit_reverse_count_x_rev32;
		case 3: return uml_arm64_gadget_bit_reverse_count_x_rev;
		case 4: return uml_arm64_gadget_bit_reverse_count_x_clz;
		case 5: return uml_arm64_gadget_bit_reverse_count_x_cls;
		case 6: return native ?
			uml_arm64_gadget_bit_reverse_count_x_ctz_native :
			uml_arm64_gadget_bit_reverse_count_x_ctz;
		case 7: return native ?
			uml_arm64_gadget_bit_reverse_count_x_cnt_native :
			uml_arm64_gadget_bit_reverse_count_x_cnt;
		default: return native ?
			uml_arm64_gadget_bit_reverse_count_x_abs_native :
			uml_arm64_gadget_bit_reverse_count_x_abs;
		}
	}
	switch (operation) {
	case 0: return uml_arm64_gadget_bit_reverse_count_w_rbit;
	case 1: return uml_arm64_gadget_bit_reverse_count_w_rev16;
	case 2: return uml_arm64_gadget_bit_reverse_count_w_rev;
	case 4: return uml_arm64_gadget_bit_reverse_count_w_clz;
	case 5: return uml_arm64_gadget_bit_reverse_count_w_cls;
	case 6: return native ?
		uml_arm64_gadget_bit_reverse_count_w_ctz_native :
		uml_arm64_gadget_bit_reverse_count_w_ctz;
	case 7: return native ?
		uml_arm64_gadget_bit_reverse_count_w_cnt_native :
		uml_arm64_gadget_bit_reverse_count_w_cnt;
	default: return native ?
		uml_arm64_gadget_bit_reverse_count_w_abs_native :
		uml_arm64_gadget_bit_reverse_count_w_abs;
	}
}

} // namespace uml::cpu::arm64::gadgets::selection
