// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gadget_entry.hpp"

/* The fourteen A64 conditions in encoding order; the always forms (14 and 15)
 * get entries of their own where a family has them. Expands
 * X(index, name[, extra arguments]) once per condition, so a declaration
 * list, a table or a switch over the conditions is one line. */
#define UML_ARM64_CONDITIONS(X, ...) \
	X(0, eq __VA_OPT__(,) __VA_ARGS__) \
	X(1, ne __VA_OPT__(,) __VA_ARGS__) \
	X(2, hs __VA_OPT__(,) __VA_ARGS__) \
	X(3, lo __VA_OPT__(,) __VA_ARGS__) \
	X(4, mi __VA_OPT__(,) __VA_ARGS__) \
	X(5, pl __VA_OPT__(,) __VA_ARGS__) \
	X(6, vs __VA_OPT__(,) __VA_ARGS__) \
	X(7, vc __VA_OPT__(,) __VA_ARGS__) \
	X(8, hi __VA_OPT__(,) __VA_ARGS__) \
	X(9, ls __VA_OPT__(,) __VA_ARGS__) \
	X(10, ge __VA_OPT__(,) __VA_ARGS__) \
	X(11, lt __VA_OPT__(,) __VA_ARGS__) \
	X(12, gt __VA_OPT__(,) __VA_ARGS__) \
	X(13, le __VA_OPT__(,) __VA_ARGS__)

/* The gadgets of one family, one per condition in encoding order, as the
 * elements of an initializer list:
 * UML_ARM64_CONDITION_GADGETS(uml_arm64_gadget_branch_, _inline) lists
 * uml_arm64_gadget_branch_eq_inline, uml_arm64_gadget_branch_ne_inline, ... */
#define UML_ARM64_CONDITION_GADGET(index, name, prefix, suffix) \
	prefix##name##suffix,
#define UML_ARM64_CONDITION_GADGETS(prefix, suffix) \
	UML_ARM64_CONDITIONS(UML_ARM64_CONDITION_GADGET, prefix, suffix)

/* The sixteen DMB and DSB options, expanded as UML_ARM64_CONDITIONS is; and
 * UML_ARM64_BARRIER_GADGETS(uml_arm64_gadget_dmb_) lists uml_arm64_gadget_dmb_0
 * to uml_arm64_gadget_dmb_15 as the elements of an initializer list. */
#define UML_ARM64_BARRIER_OPTIONS(X, ...) \
	X(0 __VA_OPT__(,) __VA_ARGS__) \
	X(1 __VA_OPT__(,) __VA_ARGS__) \
	X(2 __VA_OPT__(,) __VA_ARGS__) \
	X(3 __VA_OPT__(,) __VA_ARGS__) \
	X(4 __VA_OPT__(,) __VA_ARGS__) \
	X(5 __VA_OPT__(,) __VA_ARGS__) \
	X(6 __VA_OPT__(,) __VA_ARGS__) \
	X(7 __VA_OPT__(,) __VA_ARGS__) \
	X(8 __VA_OPT__(,) __VA_ARGS__) \
	X(9 __VA_OPT__(,) __VA_ARGS__) \
	X(10 __VA_OPT__(,) __VA_ARGS__) \
	X(11 __VA_OPT__(,) __VA_ARGS__) \
	X(12 __VA_OPT__(,) __VA_ARGS__) \
	X(13 __VA_OPT__(,) __VA_ARGS__) \
	X(14 __VA_OPT__(,) __VA_ARGS__) \
	X(15 __VA_OPT__(,) __VA_ARGS__)
#define UML_ARM64_BARRIER_GADGET(option, prefix) prefix##option,
#define UML_ARM64_BARRIER_GADGETS(prefix) \
	UML_ARM64_BARRIER_OPTIONS(UML_ARM64_BARRIER_GADGET, prefix)

namespace uml::cpu::arm64::gadgets::selection {

extern "C" {
void uml_arm64_gadget_move_wide_constant();
void uml_arm64_gadget_move_wide_packed_zero();
void uml_arm64_gadget_move_wide_packed_ones();
#define UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(prefix) \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_rr(); \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_rs(); \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_sr(); \
	void uml_arm64_gadget_add_sub_immediate_##prefix##_ss();
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_add_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_add_s)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_sub_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(w_sub_s)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_add_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_add_s)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_sub_n)
UML_DECLARE_ADD_SUB_IMMEDIATE_MODES(x_sub_s)
#undef UML_DECLARE_ADD_SUB_IMMEDIATE_MODES
/* Base entries for the in-place shape, indexed by
 * ((bits64 * 2 + subtract) * 2 + flags) * 31 + register. */
extern void (*const uml_arm64_gadget_add_sub_immediate_inplace_table[])();
extern void (*const uml_arm64_gadget_logical_immediate_inplace_table[])();
extern void (*const uml_arm64_gadget_move_wide_keep_inplace_table[])();
extern void (*const uml_arm64_gadget_add_sub_register_inplace_table[])();
extern void (*const uml_arm64_gadget_simd_fused_inplace_table[])();
/* Unsigned-offset transfers whose transferred register is baked, indexed by
 * ((operation * 2 + bits64) * 2 + base_sp) * 31 + register. */
extern void (*const uml_arm64_gadget_scalar_unsigned_inplace_table[])();
#define UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix) \
	void uml_arm64_gadget_add_sub_register_##prefix##_r(); \
	void uml_arm64_gadget_add_sub_register_##prefix##_d();
#define UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_rr) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_rz) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_zr) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_zz)
#define UML_DECLARE_ADD_SUB_SHIFTED_TRANSFORMS(prefix) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_n) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_l) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_r) \
	UML_DECLARE_ADD_SUB_SHIFTED_SOURCES(prefix##_sh_a)
#define UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_r) \
	UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS(prefix##_z)
#define UML_DECLARE_ADD_SUB_EXTENDED_TRANSFORMS(prefix) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e0n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e0s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e1n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e1s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e2n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e2s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e3n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e3s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e4n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e4s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e5n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e5s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e6n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e6s) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e7n) \
	UML_DECLARE_ADD_SUB_EXTENDED_SOURCES(prefix##_ex_e7s)
#define UML_DECLARE_ADD_SUB_REGISTER_ALL(prefix) \
	UML_DECLARE_ADD_SUB_SHIFTED_TRANSFORMS(prefix) \
	UML_DECLARE_ADD_SUB_EXTENDED_TRANSFORMS(prefix)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_add_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_add_s)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_sub_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(w_sub_s)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_add_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_add_s)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_sub_n)
UML_DECLARE_ADD_SUB_REGISTER_ALL(x_sub_s)
#undef UML_DECLARE_ADD_SUB_REGISTER_ALL
#undef UML_DECLARE_ADD_SUB_EXTENDED_TRANSFORMS
#undef UML_DECLARE_ADD_SUB_EXTENDED_SOURCES
#undef UML_DECLARE_ADD_SUB_SHIFTED_TRANSFORMS
#undef UML_DECLARE_ADD_SUB_SHIFTED_SOURCES
#undef UML_DECLARE_ADD_SUB_REGISTER_DESTINATIONS
#define UML_DECLARE_ADD_SUB_CARRY_RESULT(prefix) \
	void uml_arm64_gadget_add_sub_carry_##prefix##_r();
#define UML_DECLARE_ADD_SUB_CARRY_FLAGS(prefix) \
	UML_DECLARE_ADD_SUB_CARRY_RESULT(prefix) \
	void uml_arm64_gadget_add_sub_carry_##prefix##_z();
#define UML_DECLARE_ADD_SUB_CARRY_SOURCES(prefix, result) \
	result(prefix##_rr) \
	result(prefix##_eq) \
	result(prefix##_rz) \
	result(prefix##_zr) \
	result(prefix##_zz)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_adc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_adc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_sbc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(w_sbc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_adc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_adc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_sbc_n,
	UML_DECLARE_ADD_SUB_CARRY_RESULT)
UML_DECLARE_ADD_SUB_CARRY_SOURCES(x_sbc_s,
	UML_DECLARE_ADD_SUB_CARRY_FLAGS)
#undef UML_DECLARE_ADD_SUB_CARRY_SOURCES
#undef UML_DECLARE_ADD_SUB_CARRY_FLAGS
#undef UML_DECLARE_ADD_SUB_CARRY_RESULT
#define UML_DECLARE_MULTIPLY_RESULT(prefix) \
	void uml_arm64_gadget_multiply_add_##prefix##_r();
#define UML_DECLARE_MULTIPLY_ADDENDS(prefix) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_r) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_z)
#define UML_DECLARE_MULTIPLY_SOURCES(prefix) \
	UML_DECLARE_MULTIPLY_ADDENDS(prefix##_p) \
	UML_DECLARE_MULTIPLY_ADDENDS(prefix##_z)
UML_DECLARE_MULTIPLY_SOURCES(w_madd)
UML_DECLARE_MULTIPLY_SOURCES(w_msub)
UML_DECLARE_MULTIPLY_SOURCES(x_madd)
UML_DECLARE_MULTIPLY_SOURCES(x_msub)
UML_DECLARE_MULTIPLY_SOURCES(smaddl)
UML_DECLARE_MULTIPLY_SOURCES(smsubl)
UML_DECLARE_MULTIPLY_SOURCES(umaddl)
UML_DECLARE_MULTIPLY_SOURCES(umsubl)
#undef UML_DECLARE_MULTIPLY_SOURCES
#undef UML_DECLARE_MULTIPLY_ADDENDS
#define UML_DECLARE_MULTIPLY_HIGH_SOURCES(prefix) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_p_z) \
	UML_DECLARE_MULTIPLY_RESULT(prefix##_z_z)
UML_DECLARE_MULTIPLY_HIGH_SOURCES(smulh)
UML_DECLARE_MULTIPLY_HIGH_SOURCES(umulh)
#undef UML_DECLARE_MULTIPLY_HIGH_SOURCES
#undef UML_DECLARE_MULTIPLY_RESULT
#define UML_DECLARE_BINARY_SOURCES(kind, prefix) \
	void uml_arm64_gadget_##kind##_##prefix##_rr_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_eq_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_rz_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_zr_r(); \
	void uml_arm64_gadget_##kind##_##prefix##_zz_r();
UML_DECLARE_BINARY_SOURCES(variable_shift, w_lsl)
UML_DECLARE_BINARY_SOURCES(variable_shift, w_lsr)
UML_DECLARE_BINARY_SOURCES(variable_shift, w_asr)
UML_DECLARE_BINARY_SOURCES(variable_shift, w_ror)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_lsl)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_lsr)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_asr)
UML_DECLARE_BINARY_SOURCES(variable_shift, x_ror)
UML_DECLARE_BINARY_SOURCES(divide, w_u)
UML_DECLARE_BINARY_SOURCES(divide, w_s)
UML_DECLARE_BINARY_SOURCES(divide, x_u)
UML_DECLARE_BINARY_SOURCES(divide, x_s)
#undef UML_DECLARE_BINARY_SOURCES
#define UML_DECLARE_CRC_SOURCES(prefix) \
	void uml_arm64_gadget_crc32_##prefix##_rr(); \
	void uml_arm64_gadget_crc32_##prefix##_same(); \
	void uml_arm64_gadget_crc32_##prefix##_rz(); \
	void uml_arm64_gadget_crc32_##prefix##_zr(); \
	void uml_arm64_gadget_crc32_##prefix##_zz(); \
	void uml_arm64_gadget_crc32_software_##prefix##_rr(); \
	void uml_arm64_gadget_crc32_software_##prefix##_same(); \
	void uml_arm64_gadget_crc32_software_##prefix##_rz(); \
	void uml_arm64_gadget_crc32_software_##prefix##_zr(); \
	void uml_arm64_gadget_crc32_software_##prefix##_zz();
UML_DECLARE_CRC_SOURCES(b)
UML_DECLARE_CRC_SOURCES(h)
UML_DECLARE_CRC_SOURCES(w)
UML_DECLARE_CRC_SOURCES(x)
UML_DECLARE_CRC_SOURCES(cb)
UML_DECLARE_CRC_SOURCES(ch)
UML_DECLARE_CRC_SOURCES(cw)
UML_DECLARE_CRC_SOURCES(cx)
#undef UML_DECLARE_CRC_SOURCES
#define UML_DECLARE_EXTRACT_DESTINATIONS(prefix) \
	void uml_arm64_gadget_extract_register_##prefix##_r(); \
	void uml_arm64_gadget_extract_register_##prefix##_z();
#define UML_DECLARE_EXTRACT_SOURCES(prefix) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_rr) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_rz) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_zr) \
	UML_DECLARE_EXTRACT_DESTINATIONS(prefix##_zz)
UML_DECLARE_EXTRACT_SOURCES(w_n)
UML_DECLARE_EXTRACT_SOURCES(w_s)
UML_DECLARE_EXTRACT_SOURCES(x_n)
UML_DECLARE_EXTRACT_SOURCES(x_s)
#undef UML_DECLARE_EXTRACT_SOURCES
#undef UML_DECLARE_EXTRACT_DESTINATIONS
#define UML_DECLARE_CONDITIONAL_SELECT_FAST(index, name) \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_sel(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_sel(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_inc(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_inc(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_inv(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_inv(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_w_neg(); \
	void uml_arm64_gadget_conditional_select_fast_##name##_x_neg();
UML_ARM64_CONDITIONS(UML_DECLARE_CONDITIONAL_SELECT_FAST)
#undef UML_DECLARE_CONDITIONAL_SELECT_FAST
void uml_arm64_gadget_conditional_select_fast_always_w();
void uml_arm64_gadget_conditional_select_fast_always_x();
#define UML_DECLARE_CONDITIONAL_COMPARE_FAST(index, name) \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmn_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmn_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_w_cmp_i(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_r(); \
	void uml_arm64_gadget_conditional_compare_fast_##name##_x_cmp_i();
UML_ARM64_CONDITIONS(UML_DECLARE_CONDITIONAL_COMPARE_FAST)
UML_DECLARE_CONDITIONAL_COMPARE_FAST(14, always)
#undef UML_DECLARE_CONDITIONAL_COMPARE_FAST
#define UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix) \
	void uml_arm64_gadget_logical_shifted_##prefix##_rr_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_rz_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_zr_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_zz_r();
#define UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(prefix) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_n) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_l) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_r) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_a) \
	UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES(prefix##_o)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_and_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_and_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_orr_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_orr_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_eor_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(w_eor_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_and_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_and_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_orr_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_orr_n)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_eor_p)
UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS(x_eor_n)
#undef UML_DECLARE_LOGICAL_SHIFTED_RESULT_SHIFTS
#undef UML_DECLARE_LOGICAL_SHIFTED_RESULT_SOURCES
#define UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix) \
	void uml_arm64_gadget_logical_shifted_##prefix##_r(); \
	void uml_arm64_gadget_logical_shifted_##prefix##_z();
#define UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_rr) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_rz) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_zr) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS(prefix##_zz)
#define UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(prefix) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_n) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_l) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_r) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_a) \
	UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES(prefix##_o)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(w_ands_p)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(w_ands_n)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(x_ands_p)
UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS(x_ands_n)
#undef UML_DECLARE_LOGICAL_SHIFTED_FLAG_SHIFTS
#undef UML_DECLARE_LOGICAL_SHIFTED_FLAG_SOURCES
#undef UML_DECLARE_LOGICAL_SHIFTED_FLAG_DESTINATIONS
#define UML_DECLARE_LOGICAL_IMMEDIATE_MODES(prefix) \
	void uml_arm64_gadget_logical_immediate_##prefix##_rr(); \
	void uml_arm64_gadget_logical_immediate_##prefix##_rs(); \
	void uml_arm64_gadget_logical_immediate_##prefix##_zr(); \
	void uml_arm64_gadget_logical_immediate_##prefix##_zs();
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_and)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_orr)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_eor)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(w_ands)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_and)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_orr)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_eor)
UML_DECLARE_LOGICAL_IMMEDIATE_MODES(x_ands)
#undef UML_DECLARE_LOGICAL_IMMEDIATE_MODES
#define UML_DECLARE_BITFIELD_SOURCES(prefix) \
	void uml_arm64_gadget_bitfield_##prefix##_n_r(); \
	void uml_arm64_gadget_bitfield_##prefix##_n_z(); \
	void uml_arm64_gadget_bitfield_##prefix##_r_r(); \
	void uml_arm64_gadget_bitfield_##prefix##_r_z();
UML_DECLARE_BITFIELD_SOURCES(w_s)
UML_DECLARE_BITFIELD_SOURCES(w_b)
UML_DECLARE_BITFIELD_SOURCES(w_u)
UML_DECLARE_BITFIELD_SOURCES(x_s)
UML_DECLARE_BITFIELD_SOURCES(x_b)
UML_DECLARE_BITFIELD_SOURCES(x_u)
#undef UML_DECLARE_BITFIELD_SOURCES
#define UML_DECLARE_BITFIELD_SHIFT(prefix) \
	void uml_arm64_gadget_bitfield_shift_##prefix();
UML_DECLARE_BITFIELD_SHIFT(w_lsl)
UML_DECLARE_BITFIELD_SHIFT(w_lsr)
UML_DECLARE_BITFIELD_SHIFT(w_asr)
UML_DECLARE_BITFIELD_SHIFT(w_ubfm)
UML_DECLARE_BITFIELD_SHIFT(w_sbfm)
UML_DECLARE_BITFIELD_SHIFT(x_lsl)
UML_DECLARE_BITFIELD_SHIFT(x_lsr)
UML_DECLARE_BITFIELD_SHIFT(x_asr)
UML_DECLARE_BITFIELD_SHIFT(x_ubfm)
UML_DECLARE_BITFIELD_SHIFT(x_sbfm)
#undef UML_DECLARE_BITFIELD_SHIFT
#define UML_DECLARE_BIT_REVERSE_COUNT(prefix) \
	void uml_arm64_gadget_bit_reverse_count_##prefix();
UML_DECLARE_BIT_REVERSE_COUNT(w_rbit)
UML_DECLARE_BIT_REVERSE_COUNT(w_rev16)
UML_DECLARE_BIT_REVERSE_COUNT(w_rev)
UML_DECLARE_BIT_REVERSE_COUNT(w_clz)
UML_DECLARE_BIT_REVERSE_COUNT(w_cls)
UML_DECLARE_BIT_REVERSE_COUNT(w_ctz)
UML_DECLARE_BIT_REVERSE_COUNT(w_cnt)
UML_DECLARE_BIT_REVERSE_COUNT(w_abs)
UML_DECLARE_BIT_REVERSE_COUNT(x_rbit)
UML_DECLARE_BIT_REVERSE_COUNT(x_rev16)
UML_DECLARE_BIT_REVERSE_COUNT(x_rev32)
UML_DECLARE_BIT_REVERSE_COUNT(x_rev)
UML_DECLARE_BIT_REVERSE_COUNT(x_clz)
UML_DECLARE_BIT_REVERSE_COUNT(x_cls)
UML_DECLARE_BIT_REVERSE_COUNT(x_ctz)
UML_DECLARE_BIT_REVERSE_COUNT(x_cnt)
UML_DECLARE_BIT_REVERSE_COUNT(x_abs)
UML_DECLARE_BIT_REVERSE_COUNT(w_ctz_native)
UML_DECLARE_BIT_REVERSE_COUNT(w_cnt_native)
UML_DECLARE_BIT_REVERSE_COUNT(w_abs_native)
UML_DECLARE_BIT_REVERSE_COUNT(x_ctz_native)
UML_DECLARE_BIT_REVERSE_COUNT(x_cnt_native)
UML_DECLARE_BIT_REVERSE_COUNT(x_abs_native)
#undef UML_DECLARE_BIT_REVERSE_COUNT
#define UML_DECLARE_INTEGER_MIN_MAX_FAST(name) \
	void uml_arm64_gadget_integer_##name##_fast_w_r(); \
	void uml_arm64_gadget_integer_##name##_fast_w_i(); \
	void uml_arm64_gadget_integer_##name##_fast_x_r(); \
	void uml_arm64_gadget_integer_##name##_fast_x_i();
UML_DECLARE_INTEGER_MIN_MAX_FAST(smax)
UML_DECLARE_INTEGER_MIN_MAX_FAST(umax)
UML_DECLARE_INTEGER_MIN_MAX_FAST(smin)
UML_DECLARE_INTEGER_MIN_MAX_FAST(umin)
#undef UML_DECLARE_INTEGER_MIN_MAX_FAST
void uml_arm64_gadget_integer_min_max_copy_w();
void uml_arm64_gadget_integer_min_max_copy_x();
void uml_arm64_gadget_cfinv();
void uml_arm64_gadget_axflag();
void uml_arm64_gadget_axflag_native();
void uml_arm64_gadget_xaflag();
void uml_arm64_gadget_xaflag_native();
void uml_arm64_gadget_rmif_fast();
void uml_arm64_gadget_rmif_zero();
void uml_arm64_gadget_setf_fast_8();
void uml_arm64_gadget_setf_fast_16();
void uml_arm64_gadget_setf_native_8();
void uml_arm64_gadget_setf_native_16();
void uml_arm64_gadget_floating_move_immediate();
void uml_arm64_gadget_floating_move_half();
void uml_arm64_gadget_floating_move_single();
void uml_arm64_gadget_floating_move_double();
void uml_arm64_gadget_floating_absolute_half();
void uml_arm64_gadget_floating_absolute_single();
void uml_arm64_gadget_floating_absolute_double();
void uml_arm64_gadget_floating_negate_half();
void uml_arm64_gadget_floating_negate_single();
void uml_arm64_gadget_floating_negate_double();
void uml_arm64_gadget_floating_sqrt_half();
void uml_arm64_gadget_floating_sqrt_single();
void uml_arm64_gadget_floating_sqrt_double();
void uml_arm64_gadget_floating_multiply_half();
void uml_arm64_gadget_floating_multiply_single();
void uml_arm64_gadget_floating_multiply_double();
void uml_arm64_gadget_floating_divide_half();
void uml_arm64_gadget_floating_divide_single();
void uml_arm64_gadget_floating_divide_double();
void uml_arm64_gadget_floating_add_half();
void uml_arm64_gadget_floating_add_single();
void uml_arm64_gadget_floating_add_double();
void uml_arm64_gadget_floating_subtract_half();
void uml_arm64_gadget_floating_subtract_single();
void uml_arm64_gadget_floating_subtract_double();
void uml_arm64_gadget_floating_negative_multiply_half();
void uml_arm64_gadget_floating_negative_multiply_single();
void uml_arm64_gadget_floating_negative_multiply_double();
#define UML_DECLARE_SIMD_BASIC_FLOATING(operation) \
	void uml_arm64_gadget_simd_basic_##operation##_4h(); \
	void uml_arm64_gadget_simd_basic_##operation##_8h(); \
	void uml_arm64_gadget_simd_basic_##operation##_2s(); \
	void uml_arm64_gadget_simd_basic_##operation##_4s(); \
	void uml_arm64_gadget_simd_basic_##operation##_2d();
UML_DECLARE_SIMD_BASIC_FLOATING(fmul)
UML_DECLARE_SIMD_BASIC_FLOATING(fdiv)
UML_DECLARE_SIMD_BASIC_FLOATING(fadd)
UML_DECLARE_SIMD_BASIC_FLOATING(fsub)
UML_DECLARE_SIMD_BASIC_FLOATING(fsqrt)
#undef UML_DECLARE_SIMD_BASIC_FLOATING
void uml_arm64_gadget_floating_madd_half();
void uml_arm64_gadget_floating_madd_single();
void uml_arm64_gadget_floating_madd_double();
void uml_arm64_gadget_floating_msub_half();
void uml_arm64_gadget_floating_msub_single();
void uml_arm64_gadget_floating_msub_double();
void uml_arm64_gadget_floating_nmadd_half();
void uml_arm64_gadget_floating_nmadd_single();
void uml_arm64_gadget_floating_nmadd_double();
void uml_arm64_gadget_floating_nmsub_half();
void uml_arm64_gadget_floating_nmsub_single();
void uml_arm64_gadget_floating_nmsub_double();
#define UML_DECLARE_FLOATING_CONVERT(source, destination) \
	void uml_arm64_gadget_floating_convert_##source##_to_##destination();
UML_DECLARE_FLOATING_CONVERT(h, s)
UML_DECLARE_FLOATING_CONVERT(h, d)
UML_DECLARE_FLOATING_CONVERT(s, h)
UML_DECLARE_FLOATING_CONVERT(s, d)
UML_DECLARE_FLOATING_CONVERT(d, h)
UML_DECLARE_FLOATING_CONVERT(d, s)
#undef UML_DECLARE_FLOATING_CONVERT
void uml_arm64_gadget_floating_javascript_convert();
void uml_arm64_gadget_floating_javascript_convert_discard();
void uml_arm64_gadget_floating_javascript_convert_software_fallback();
void uml_arm64_gadget_floating_to_integer_software_fallback();
#define UML_DECLARE_FLOATING_ROUND_SCALAR(name) \
	void uml_arm64_gadget_floating_round_##name##_h(); \
	void uml_arm64_gadget_floating_round_##name##_s(); \
	void uml_arm64_gadget_floating_round_##name##_d();
UML_DECLARE_FLOATING_ROUND_SCALAR(n)
UML_DECLARE_FLOATING_ROUND_SCALAR(p)
UML_DECLARE_FLOATING_ROUND_SCALAR(m)
UML_DECLARE_FLOATING_ROUND_SCALAR(z)
UML_DECLARE_FLOATING_ROUND_SCALAR(a)
UML_DECLARE_FLOATING_ROUND_SCALAR(x)
UML_DECLARE_FLOATING_ROUND_SCALAR(i)
#undef UML_DECLARE_FLOATING_ROUND_SCALAR
#define UML_DECLARE_FLOATING_ROUND_VECTOR(name) \
	void uml_arm64_gadget_simd_round_##name##_4h(); \
	void uml_arm64_gadget_simd_round_##name##_8h(); \
	void uml_arm64_gadget_simd_round_##name##_2s(); \
	void uml_arm64_gadget_simd_round_##name##_4s(); \
	void uml_arm64_gadget_simd_round_##name##_2d();
UML_DECLARE_FLOATING_ROUND_VECTOR(n)
UML_DECLARE_FLOATING_ROUND_VECTOR(p)
UML_DECLARE_FLOATING_ROUND_VECTOR(m)
UML_DECLARE_FLOATING_ROUND_VECTOR(z)
UML_DECLARE_FLOATING_ROUND_VECTOR(a)
UML_DECLARE_FLOATING_ROUND_VECTOR(x)
UML_DECLARE_FLOATING_ROUND_VECTOR(i)
#undef UML_DECLARE_FLOATING_ROUND_VECTOR
#define UML_DECLARE_FLOATING_ROUND_SIZED(name) \
	void uml_arm64_gadget_floating_round_##name##_s(); \
	void uml_arm64_gadget_floating_round_##name##_d(); \
	void uml_arm64_gadget_simd_round_##name##_2s(); \
	void uml_arm64_gadget_simd_round_##name##_4s(); \
	void uml_arm64_gadget_simd_round_##name##_2d();
UML_DECLARE_FLOATING_ROUND_SIZED(32x)
UML_DECLARE_FLOATING_ROUND_SIZED(32z)
UML_DECLARE_FLOATING_ROUND_SIZED(64x)
UML_DECLARE_FLOATING_ROUND_SIZED(64z)
#undef UML_DECLARE_FLOATING_ROUND_SIZED
void uml_arm64_gadget_floating_round_sized_software();
#define UML_DECLARE_FLOATING_HORIZONTAL_SCALAR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_d();
#define UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2d();
#define UML_DECLARE_FLOATING_HORIZONTAL_BINARY(name) \
	UML_DECLARE_FLOATING_HORIZONTAL_SCALAR(name##_scalar) \
	UML_DECLARE_FLOATING_HORIZONTAL_VECTOR(name##_vector)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fabd)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmax)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmaxnm)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmin)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fminnm)
void uml_arm64_gadget_bfloat_convert_scalar_native();
void uml_arm64_gadget_bfloat_convert_lower_native();
void uml_arm64_gadget_bfloat_convert_upper_native();
void uml_arm64_gadget_bfloat_convert_software();
void uml_arm64_gadget_bfloat_dot_native_vector_2s();
void uml_arm64_gadget_bfloat_dot_native_vector_4s();
#define UML_DECLARE_BFLOAT_DOT_INDEXED(shape, index) \
	void uml_arm64_gadget_bfloat_dot_native_indexed_##shape##_##index();
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 0)
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 1)
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 2)
UML_DECLARE_BFLOAT_DOT_INDEXED(2s, 3)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 0)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 1)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 2)
UML_DECLARE_BFLOAT_DOT_INDEXED(4s, 3)
#undef UML_DECLARE_BFLOAT_DOT_INDEXED
void uml_arm64_gadget_bfloat_dot_software();
void uml_arm64_gadget_bfloat_matrix_native();
void uml_arm64_gadget_bfloat_matrix_software();
#define UML_DECLARE_HALF_FUSED_LONG_VARIANTS(name) \
	void uml_arm64_gadget_half_fused_long_native_##name();
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_lower_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_lower_4s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_upper_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_add_upper_4s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_lower_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_lower_4s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_upper_2s)
UML_DECLARE_HALF_FUSED_LONG_VARIANTS(vector_sub_upper_4s)
#define UML_DECLARE_HALF_FUSED_LONG_INDEXED(operation, half, shape) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_0) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_1) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_2) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_3) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_4) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_5) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_6) \
	UML_DECLARE_HALF_FUSED_LONG_VARIANTS(indexed_##operation##_##half##_##shape##_7)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, lower, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, lower, 4s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, upper, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(add, upper, 4s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, lower, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, lower, 4s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, upper, 2s)
UML_DECLARE_HALF_FUSED_LONG_INDEXED(sub, upper, 4s)
#undef UML_DECLARE_HALF_FUSED_LONG_INDEXED
#undef UML_DECLARE_HALF_FUSED_LONG_VARIANTS
void uml_arm64_gadget_half_fused_long_software();
void uml_arm64_gadget_bfloat_multiply_long_native_vector_b();
void uml_arm64_gadget_bfloat_multiply_long_native_vector_t();
#define UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(half, index) \
	void uml_arm64_gadget_bfloat_multiply_long_native_indexed_##half##_##index();
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 0)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 1)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 2)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 3)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 4)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 5)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 6)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(b, 7)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 0)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 1)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 2)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 3)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 4)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 5)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 6)
UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED(t, 7)
#undef UML_DECLARE_BFLOAT_MULTIPLY_LONG_INDEXED
void uml_arm64_gadget_bfloat_multiply_long_software();
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(faddp)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmaxp)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fmaxnmp)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fminp)
UML_DECLARE_FLOATING_HORIZONTAL_BINARY(fminnmp)
#undef UML_DECLARE_FLOATING_HORIZONTAL_BINARY
#define UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s();
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fmaxv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fmaxnmv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fminv)
UML_DECLARE_FLOATING_HORIZONTAL_REDUCE(fminnmv)
#undef UML_DECLARE_FLOATING_HORIZONTAL_REDUCE
#undef UML_DECLARE_FLOATING_HORIZONTAL_VECTOR
#undef UML_DECLARE_FLOATING_HORIZONTAL_SCALAR
#define UML_DECLARE_FLOATING_RECIPROCAL_SCALAR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_scalar_d();
#define UML_DECLARE_FLOATING_RECIPROCAL_VECTOR(host, name) \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_8h(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_4s(); \
	void uml_arm64_gadget_floating_reciprocal_##host##_##name##_vector_2d();
#define UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(name) \
	UML_DECLARE_FLOATING_RECIPROCAL_SCALAR(native, name)
#define UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(name) \
	UML_DECLARE_FLOATING_RECIPROCAL_VECTOR(native, name)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frecpe)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frecpe)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frecps)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frecps)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frecpx)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frsqrte)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frsqrte)
UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR(frsqrts)
UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR(frsqrts)
#undef UML_DECLARE_FLOATING_RECIPROCAL_VECTOR_PAIR
#undef UML_DECLARE_FLOATING_RECIPROCAL_SCALAR_PAIR
#undef UML_DECLARE_FLOATING_RECIPROCAL_VECTOR
#undef UML_DECLARE_FLOATING_RECIPROCAL_SCALAR
#define UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(name, shape) \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_0(); \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_90(); \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_180(); \
	void uml_arm64_gadget_floating_complex_##name##_##shape##_270();
#define UML_DECLARE_FLOATING_COMPLEX_ADD(shape) \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_90(); \
	void uml_arm64_gadget_floating_complex_fcadd_##shape##_270();
UML_DECLARE_FLOATING_COMPLEX_ADD(4h)
UML_DECLARE_FLOATING_COMPLEX_ADD(8h)
UML_DECLARE_FLOATING_COMPLEX_ADD(2s)
UML_DECLARE_FLOATING_COMPLEX_ADD(4s)
UML_DECLARE_FLOATING_COMPLEX_ADD(2d)
#undef UML_DECLARE_FLOATING_COMPLEX_ADD
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 4h)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 8h)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 2s)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 4s)
UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla, 2d)
#define UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(shape, index) \
	UML_DECLARE_FLOATING_COMPLEX_ROTATIONS(fcmla_element, shape##_##index)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4h, 0)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4h, 1)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 0)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 1)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 2)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(8h, 3)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4s, 0)
UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX(4s, 1)
#undef UML_DECLARE_FLOATING_COMPLEX_ELEMENT_INDEX
#undef UML_DECLARE_FLOATING_COMPLEX_ROTATIONS
#define UML_DECLARE_FLOATING_COMPARE(width) \
	void uml_arm64_gadget_floating_compare_quiet_##width##_register(); \
	void uml_arm64_gadget_floating_compare_quiet_##width##_zero(); \
	void uml_arm64_gadget_floating_compare_signaling_##width##_register(); \
	void uml_arm64_gadget_floating_compare_signaling_##width##_zero();
UML_DECLARE_FLOATING_COMPARE(h)
UML_DECLARE_FLOATING_COMPARE(s)
UML_DECLARE_FLOATING_COMPARE(d)
#undef UML_DECLARE_FLOATING_COMPARE
#define UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, width) \
	void uml_arm64_gadget_floating_conditional_compare_##name##_quiet_##width(); \
	void uml_arm64_gadget_floating_conditional_compare_##name##_signaling_##width();
#define UML_DECLARE_FLOATING_CONDITIONAL_COMPARE(index, name) \
	UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, h) \
	UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, s) \
	UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH(name, d)
UML_ARM64_CONDITIONS(UML_DECLARE_FLOATING_CONDITIONAL_COMPARE)
#undef UML_DECLARE_FLOATING_CONDITIONAL_COMPARE
#undef UML_DECLARE_FLOATING_CONDITIONAL_COMPARE_WIDTH
/* The eight shapes of an FP vector gadget family: scalar H, S and D, then
 * 4H, 8H, 2S, 4S and 2D. */
#define UML_DECLARE_SIMD_FLOATING_SHAPES(name) \
	void uml_arm64_gadget_simd_##name##_h(); \
	void uml_arm64_gadget_simd_##name##_s(); \
	void uml_arm64_gadget_simd_##name##_d(); \
	void uml_arm64_gadget_simd_##name##_4h(); \
	void uml_arm64_gadget_simd_##name##_8h(); \
	void uml_arm64_gadget_simd_##name##_2s(); \
	void uml_arm64_gadget_simd_##name##_4s(); \
	void uml_arm64_gadget_simd_##name##_2d();
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_eq)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_ge)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_gt)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_facge)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_facgt)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_eq0)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_ge0)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_gt0)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_le0)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcmp_lt0)
void uml_arm64_gadget_simd_fcvtl_hs();
void uml_arm64_gadget_simd_fcvtl2_hs();
void uml_arm64_gadget_simd_fcvtl_sd();
void uml_arm64_gadget_simd_fcvtl2_sd();
void uml_arm64_gadget_simd_fcvtn_sh();
void uml_arm64_gadget_simd_fcvtn2_sh();
void uml_arm64_gadget_simd_fcvtn_ds();
void uml_arm64_gadget_simd_fcvtn2_ds();
void uml_arm64_gadget_simd_fcvtxn_scalar();
void uml_arm64_gadget_simd_fcvtxn_vector();
void uml_arm64_gadget_simd_fcvtxn2_vector();
#define UML_DECLARE_FLOATING_TO_GPR(operation, floating) \
	void uml_arm64_gadget_floating_to_gpr_##operation##_w_##floating(); \
	void uml_arm64_gadget_floating_to_gpr_##operation##_x_##floating(); \
	void uml_arm64_gadget_floating_to_gpr_##operation##_w_##floating##_discard(); \
	void uml_arm64_gadget_floating_to_gpr_##operation##_x_##floating##_discard();
#define UML_DECLARE_FLOATING_TO_GPR_WIDTHS(operation) \
	UML_DECLARE_FLOATING_TO_GPR(operation, h) \
	UML_DECLARE_FLOATING_TO_GPR(operation, s) \
	UML_DECLARE_FLOATING_TO_GPR(operation, d)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtns)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtnu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtps)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtpu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtms)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtmu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtzs)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtzu)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtas)
UML_DECLARE_FLOATING_TO_GPR_WIDTHS(fcvtau)
#undef UML_DECLARE_FLOATING_TO_GPR_WIDTHS
#undef UML_DECLARE_FLOATING_TO_GPR
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtns)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtnu)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtps)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtpu)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtms)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtmu)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtzs)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtzu)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtas)
UML_DECLARE_SIMD_FLOATING_SHAPES(fcvtau)
#define UML_DECLARE_FIXED_TABLE(name) \
	void uml_arm64_gadget_##name(); \
	void uml_arm64_gadget_##name##_end();
#define UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, floating) \
	UML_DECLARE_FIXED_TABLE(floating_to_gpr_fixed_##operation##_w_##floating) \
	UML_DECLARE_FIXED_TABLE(floating_to_gpr_fixed_##operation##_x_##floating) \
	UML_DECLARE_FIXED_TABLE( \
		floating_to_gpr_fixed_##operation##_w_##floating##_discard) \
	UML_DECLARE_FIXED_TABLE( \
		floating_to_gpr_fixed_##operation##_x_##floating##_discard)
#define UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS(operation) \
	UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, h) \
	UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, s) \
	UML_DECLARE_FLOATING_TO_GPR_FIXED(operation, d)
UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS(fcvtzs)
UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS(fcvtzu)
#undef UML_DECLARE_FLOATING_TO_GPR_FIXED_WIDTHS
#undef UML_DECLARE_FLOATING_TO_GPR_FIXED
#define UML_DECLARE_SIMD_FIXED_TABLES(operation) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_h) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_s) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_d) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_4h) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_8h) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_2s) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_4s) \
	UML_DECLARE_FIXED_TABLE(simd_fixed_##operation##_2d)
UML_DECLARE_SIMD_FIXED_TABLES(fcvtzs)
UML_DECLARE_SIMD_FIXED_TABLES(fcvtzu)
#define UML_DECLARE_GPR_TO_FLOATING(operation, integer, floating, zero) \
	void uml_arm64_gadget_gpr_to_floating_##operation##_##integer##_##floating##zero();
#define UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, integer, zero) \
	UML_DECLARE_GPR_TO_FLOATING(operation, integer, h, zero) \
	UML_DECLARE_GPR_TO_FLOATING(operation, integer, s, zero) \
	UML_DECLARE_GPR_TO_FLOATING(operation, integer, d, zero)
#define UML_DECLARE_GPR_TO_FLOATING_VARIANTS(operation) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, w, 0) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, x, 0) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, w, 1) \
	UML_DECLARE_GPR_TO_FLOATING_WIDTHS(operation, x, 1)
UML_DECLARE_GPR_TO_FLOATING_VARIANTS(scvtf)
UML_DECLARE_GPR_TO_FLOATING_VARIANTS(ucvtf)
#undef UML_DECLARE_GPR_TO_FLOATING_VARIANTS
#undef UML_DECLARE_GPR_TO_FLOATING_WIDTHS
#undef UML_DECLARE_GPR_TO_FLOATING
void uml_arm64_gadget_integer_to_floating_software_fallback();
UML_DECLARE_SIMD_FLOATING_SHAPES(scvtf)
UML_DECLARE_SIMD_FLOATING_SHAPES(ucvtf)
#undef UML_DECLARE_SIMD_FLOATING_SHAPES
#define UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, floating, zero) \
	UML_DECLARE_FIXED_TABLE( \
		gpr_to_floating_fixed_##operation##_##integer##_##floating##zero)
#define UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, integer, zero) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, h, zero) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, s, zero) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED(operation, integer, d, zero)
#define UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS(operation) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, w, 0) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, x, 0) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, w, 1) \
	UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS(operation, x, 1)
UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS(scvtf)
UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS(ucvtf)
#undef UML_DECLARE_GPR_TO_FLOATING_FIXED_VARIANTS
#undef UML_DECLARE_GPR_TO_FLOATING_FIXED_WIDTHS
#undef UML_DECLARE_GPR_TO_FLOATING_FIXED
UML_DECLARE_SIMD_FIXED_TABLES(scvtf)
UML_DECLARE_SIMD_FIXED_TABLES(ucvtf)
#undef UML_DECLARE_SIMD_FIXED_TABLES
#undef UML_DECLARE_FIXED_TABLE
#define UML_DECLARE_SIMD_BITWISE(name) \
	void uml_arm64_gadget_##name##_8b(); \
	void uml_arm64_gadget_##name##_16b();
UML_DECLARE_SIMD_BITWISE(simd_and)
UML_DECLARE_SIMD_BITWISE(simd_bic)
UML_DECLARE_SIMD_BITWISE(simd_orr)
UML_DECLARE_SIMD_BITWISE(simd_orn)
UML_DECLARE_SIMD_BITWISE(simd_eor)
UML_DECLARE_SIMD_BITWISE(simd_bsl)
UML_DECLARE_SIMD_BITWISE(simd_bit)
UML_DECLARE_SIMD_BITWISE(simd_bif)
#undef UML_DECLARE_SIMD_BITWISE
void uml_arm64_gadget_simd_mov_8b();
void uml_arm64_gadget_simd_mov_16b();
#define UML_DECLARE_SIMD_IMMEDIATE(name) \
	void uml_arm64_gadget_simd_immediate_##name##_8b(); \
	void uml_arm64_gadget_simd_immediate_##name##_16b();
UML_DECLARE_SIMD_IMMEDIATE(replace)
UML_DECLARE_SIMD_IMMEDIATE(orr)
UML_DECLARE_SIMD_IMMEDIATE(bic)
#undef UML_DECLARE_SIMD_IMMEDIATE
#define UML_DECLARE_SIMD_EXTRACT_Q(index) void uml_arm64_gadget_simd_extract_q##index();
UML_DECLARE_SIMD_EXTRACT_Q(0) UML_DECLARE_SIMD_EXTRACT_Q(1)
UML_DECLARE_SIMD_EXTRACT_Q(2) UML_DECLARE_SIMD_EXTRACT_Q(3)
UML_DECLARE_SIMD_EXTRACT_Q(4) UML_DECLARE_SIMD_EXTRACT_Q(5)
UML_DECLARE_SIMD_EXTRACT_Q(6) UML_DECLARE_SIMD_EXTRACT_Q(7)
UML_DECLARE_SIMD_EXTRACT_Q(8) UML_DECLARE_SIMD_EXTRACT_Q(9)
UML_DECLARE_SIMD_EXTRACT_Q(10) UML_DECLARE_SIMD_EXTRACT_Q(11)
UML_DECLARE_SIMD_EXTRACT_Q(12) UML_DECLARE_SIMD_EXTRACT_Q(13)
UML_DECLARE_SIMD_EXTRACT_Q(14) UML_DECLARE_SIMD_EXTRACT_Q(15)
#undef UML_DECLARE_SIMD_EXTRACT_Q
#define UML_DECLARE_SIMD_EXTRACT_D(index) void uml_arm64_gadget_simd_extract_d##index();
UML_DECLARE_SIMD_EXTRACT_D(0) UML_DECLARE_SIMD_EXTRACT_D(1)
UML_DECLARE_SIMD_EXTRACT_D(2) UML_DECLARE_SIMD_EXTRACT_D(3)
UML_DECLARE_SIMD_EXTRACT_D(4) UML_DECLARE_SIMD_EXTRACT_D(5)
UML_DECLARE_SIMD_EXTRACT_D(6) UML_DECLARE_SIMD_EXTRACT_D(7)
#undef UML_DECLARE_SIMD_EXTRACT_D
#define UML_DECLARE_SIMD_SHIFT_SHAPES(name) \
	void uml_arm64_gadget_##name##_8b(); \
	void uml_arm64_gadget_##name##_4h(); \
	void uml_arm64_gadget_##name##_2s(); \
	void uml_arm64_gadget_##name##_16b(); \
	void uml_arm64_gadget_##name##_8h(); \
	void uml_arm64_gadget_##name##_4s(); \
	void uml_arm64_gadget_##name##_2d(); \
	void uml_arm64_gadget_##name##_d();
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_sshr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_ushr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_ssra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_usra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_srshr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_urshr)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_srsra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_ursra)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_right_sri)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_left_shl)
UML_DECLARE_SIMD_SHIFT_SHAPES(simd_shift_left_sli)
#undef UML_DECLARE_SIMD_SHIFT_SHAPES
#define UML_DECLARE_SIMD_SAT_SHIFT_LEFT(name) \
	void uml_arm64_gadget_simd_shift_left_##name##_8b(); \
	void uml_arm64_gadget_simd_shift_left_##name##_4h(); \
	void uml_arm64_gadget_simd_shift_left_##name##_2s(); \
	void uml_arm64_gadget_simd_shift_left_##name##_16b(); \
	void uml_arm64_gadget_simd_shift_left_##name##_8h(); \
	void uml_arm64_gadget_simd_shift_left_##name##_4s(); \
	void uml_arm64_gadget_simd_shift_left_##name##_2d(); \
	void uml_arm64_gadget_simd_shift_left_##name##_b(); \
	void uml_arm64_gadget_simd_shift_left_##name##_h(); \
	void uml_arm64_gadget_simd_shift_left_##name##_s(); \
	void uml_arm64_gadget_simd_shift_left_##name##_d();
UML_DECLARE_SIMD_SAT_SHIFT_LEFT(sqshl)
UML_DECLARE_SIMD_SAT_SHIFT_LEFT(uqshl)
#undef UML_DECLARE_SIMD_SAT_SHIFT_LEFT
#define UML_SIMD_FOR_EACH_SHIFT_8(M, shape) \
	M(shape, 0) M(shape, 1) M(shape, 2) M(shape, 3) \
	M(shape, 4) M(shape, 5) M(shape, 6) M(shape, 7)
#define UML_SIMD_FOR_EACH_SHIFT_16(M, shape) \
	UML_SIMD_FOR_EACH_SHIFT_8(M, shape) \
	M(shape, 8) M(shape, 9) M(shape, 10) M(shape, 11) \
	M(shape, 12) M(shape, 13) M(shape, 14) M(shape, 15)
#define UML_SIMD_FOR_EACH_SHIFT_32(M, shape) \
	UML_SIMD_FOR_EACH_SHIFT_16(M, shape) \
	M(shape, 16) M(shape, 17) M(shape, 18) M(shape, 19) \
	M(shape, 20) M(shape, 21) M(shape, 22) M(shape, 23) \
	M(shape, 24) M(shape, 25) M(shape, 26) M(shape, 27) \
	M(shape, 28) M(shape, 29) M(shape, 30) M(shape, 31)
#define UML_SIMD_FOR_EACH_SHIFT_64(M, shape) \
	UML_SIMD_FOR_EACH_SHIFT_32(M, shape) \
	M(shape, 32) M(shape, 33) M(shape, 34) M(shape, 35) \
	M(shape, 36) M(shape, 37) M(shape, 38) M(shape, 39) \
	M(shape, 40) M(shape, 41) M(shape, 42) M(shape, 43) \
	M(shape, 44) M(shape, 45) M(shape, 46) M(shape, 47) \
	M(shape, 48) M(shape, 49) M(shape, 50) M(shape, 51) \
	M(shape, 52) M(shape, 53) M(shape, 54) M(shape, 55) \
	M(shape, 56) M(shape, 57) M(shape, 58) M(shape, 59) \
	M(shape, 60) M(shape, 61) M(shape, 62) M(shape, 63)
#define UML_DECLARE_SIMD_SQSHLU(shape, shift) \
	void uml_arm64_gadget_simd_shift_left_sqshlu_##shape##_##shift();
UML_SIMD_FOR_EACH_SHIFT_8(UML_DECLARE_SIMD_SQSHLU, 8b)
UML_SIMD_FOR_EACH_SHIFT_16(UML_DECLARE_SIMD_SQSHLU, 4h)
UML_SIMD_FOR_EACH_SHIFT_32(UML_DECLARE_SIMD_SQSHLU, 2s)
UML_SIMD_FOR_EACH_SHIFT_8(UML_DECLARE_SIMD_SQSHLU, 16b)
UML_SIMD_FOR_EACH_SHIFT_16(UML_DECLARE_SIMD_SQSHLU, 8h)
UML_SIMD_FOR_EACH_SHIFT_32(UML_DECLARE_SIMD_SQSHLU, 4s)
UML_SIMD_FOR_EACH_SHIFT_64(UML_DECLARE_SIMD_SQSHLU, 2d)
UML_SIMD_FOR_EACH_SHIFT_8(UML_DECLARE_SIMD_SQSHLU, b)
UML_SIMD_FOR_EACH_SHIFT_16(UML_DECLARE_SIMD_SQSHLU, h)
UML_SIMD_FOR_EACH_SHIFT_32(UML_DECLARE_SIMD_SQSHLU, s)
UML_SIMD_FOR_EACH_SHIFT_64(UML_DECLARE_SIMD_SQSHLU, d)
#undef UML_DECLARE_SIMD_SQSHLU
#define UML_DECLARE_SIMD_SHIFT_NARROW(source, destination) \
	void uml_arm64_gadget_simd_shrn_##source##_to_##destination(); \
	void uml_arm64_gadget_simd_shrn2_##source##_to_##destination(); \
	void uml_arm64_gadget_simd_rshrn_##source##_to_##destination(); \
	void uml_arm64_gadget_simd_rshrn2_##source##_to_##destination();
UML_DECLARE_SIMD_SHIFT_NARROW(8h, 8b)
UML_DECLARE_SIMD_SHIFT_NARROW(4s, 4h)
UML_DECLARE_SIMD_SHIFT_NARROW(2d, 2s)
#undef UML_DECLARE_SIMD_SHIFT_NARROW
#define UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, source, destination) \
	void uml_arm64_gadget_simd_##operation##_##source##_##destination##_low(); \
	void uml_arm64_gadget_simd_##operation##_##source##_##destination##_high(); \
	void uml_arm64_gadget_simd_##operation##_##source##_##destination##_scalar();
#define UML_DECLARE_SIMD_SAT_NARROW(operation) \
	UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, h, b) \
	UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, s, h) \
	UML_DECLARE_SIMD_SAT_NARROW_SHAPE(operation, d, s)
UML_DECLARE_SIMD_SAT_NARROW(sqshrn)
UML_DECLARE_SIMD_SAT_NARROW(sqrshrn)
UML_DECLARE_SIMD_SAT_NARROW(sqshrun)
UML_DECLARE_SIMD_SAT_NARROW(sqrshrun)
UML_DECLARE_SIMD_SAT_NARROW(uqshrn)
UML_DECLARE_SIMD_SAT_NARROW(uqrshrn)
#undef UML_DECLARE_SIMD_SAT_NARROW
#undef UML_DECLARE_SIMD_SAT_NARROW_SHAPE
#define UML_DECLARE_SIMD_SHIFT_LEFT_LONG(name) \
	void uml_arm64_gadget_simd_##name##_8h_8b(); \
	void uml_arm64_gadget_simd_##name##_4s_4h(); \
	void uml_arm64_gadget_simd_##name##_2d_2s();
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(sshll)
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(sshll2)
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(ushll)
UML_DECLARE_SIMD_SHIFT_LEFT_LONG(ushll2)
#undef UML_DECLARE_SIMD_SHIFT_LEFT_LONG
#define UML_DECLARE_SIMD_TABLE_COUNT(operation, count) \
	void uml_arm64_gadget_simd_##operation##_##count##_8b(); \
	void uml_arm64_gadget_simd_##operation##_##count##_16b();
#define UML_DECLARE_SIMD_TABLE(operation) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 1) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 2) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 3) \
	UML_DECLARE_SIMD_TABLE_COUNT(operation, 4)
UML_DECLARE_SIMD_TABLE(tbl)
UML_DECLARE_SIMD_TABLE(tbx)
#undef UML_DECLARE_SIMD_TABLE
#undef UML_DECLARE_SIMD_TABLE_COUNT
#define UML_DECLARE_SIMD_PERMUTE(name) \
	void uml_arm64_gadget_simd_permute_##name##_8b(); \
	void uml_arm64_gadget_simd_permute_##name##_4h(); \
	void uml_arm64_gadget_simd_permute_##name##_2s(); \
	void uml_arm64_gadget_simd_permute_##name##_16b(); \
	void uml_arm64_gadget_simd_permute_##name##_8h(); \
	void uml_arm64_gadget_simd_permute_##name##_4s(); \
	void uml_arm64_gadget_simd_permute_##name##_2d();
UML_DECLARE_SIMD_PERMUTE(uzp1)
UML_DECLARE_SIMD_PERMUTE(trn1)
UML_DECLARE_SIMD_PERMUTE(zip1)
UML_DECLARE_SIMD_PERMUTE(uzp2)
UML_DECLARE_SIMD_PERMUTE(trn2)
UML_DECLARE_SIMD_PERMUTE(zip2)
#undef UML_DECLARE_SIMD_PERMUTE
#define UML_DECLARE_SIMD_DUPLICATE_ELEMENT(name) \
	void uml_arm64_gadget_simd_duplicate_element_##name();
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(8b)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(16b)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(4h)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(8h)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(2s)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(4s)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(2d)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(b)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(h)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(s)
UML_DECLARE_SIMD_DUPLICATE_ELEMENT(d)
#undef UML_DECLARE_SIMD_DUPLICATE_ELEMENT
#define UML_DECLARE_SIMD_DUPLICATE_GENERAL(name) \
	void uml_arm64_gadget_simd_duplicate_general_##name();
UML_DECLARE_SIMD_DUPLICATE_GENERAL(8b)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(16b)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(4h)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(8h)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(2s)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(4s)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(2d)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(zero_8b)
UML_DECLARE_SIMD_DUPLICATE_GENERAL(zero_16b)
#undef UML_DECLARE_SIMD_DUPLICATE_GENERAL
#define UML_DECLARE_SIMD_INSERT_GENERAL(size) \
	void uml_arm64_gadget_simd_insert_general_##size(); \
	void uml_arm64_gadget_simd_insert_general_zero_##size();
UML_DECLARE_SIMD_INSERT_GENERAL(b)
UML_DECLARE_SIMD_INSERT_GENERAL(h)
UML_DECLARE_SIMD_INSERT_GENERAL(s)
UML_DECLARE_SIMD_INSERT_GENERAL(d)
#undef UML_DECLARE_SIMD_INSERT_GENERAL
#define UML_DECLARE_SIMD_INSERT_ELEMENT(size) \
	void uml_arm64_gadget_simd_insert_element_##size();
UML_DECLARE_SIMD_INSERT_ELEMENT(b)
UML_DECLARE_SIMD_INSERT_ELEMENT(h)
UML_DECLARE_SIMD_INSERT_ELEMENT(s)
UML_DECLARE_SIMD_INSERT_ELEMENT(d)
#undef UML_DECLARE_SIMD_INSERT_ELEMENT
#define UML_DECLARE_SIMD_LANE_TO_GPR(name) \
	void uml_arm64_gadget_simd_lane_to_gpr_##name();
UML_DECLARE_SIMD_LANE_TO_GPR(smov_w_b)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_w_h)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_x_b)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_x_h)
UML_DECLARE_SIMD_LANE_TO_GPR(smov_x_s)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_w_b)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_w_h)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_w_s)
UML_DECLARE_SIMD_LANE_TO_GPR(umov_x_d)
UML_DECLARE_SIMD_LANE_TO_GPR(discard)
#undef UML_DECLARE_SIMD_LANE_TO_GPR
#define UML_DECLARE_SIMD_FLOATING_SIGN(name) \
	void uml_arm64_gadget_simd_floating_##name();
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_4h)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_8h)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_2s)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_4s)
UML_DECLARE_SIMD_FLOATING_SIGN(fabs_2d)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_4h)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_8h)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_2s)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_4s)
UML_DECLARE_SIMD_FLOATING_SIGN(fneg_2d)
#undef UML_DECLARE_SIMD_FLOATING_SIGN
void uml_arm64_gadget_simd_addp_scalar();
void uml_arm64_gadget_simd_aese_native();
void uml_arm64_gadget_simd_aese_soft();
void uml_arm64_gadget_simd_aesd_native();
void uml_arm64_gadget_simd_aesd_soft();
void uml_arm64_gadget_simd_aesmc_native();
void uml_arm64_gadget_simd_aesmc_soft();
void uml_arm64_gadget_simd_aesimc_native();
void uml_arm64_gadget_simd_aesimc_soft();
void uml_arm64_gadget_simd_sha1c();
void uml_arm64_gadget_simd_sha1c_soft();
void uml_arm64_gadget_simd_sha1p();
void uml_arm64_gadget_simd_sha1p_soft();
void uml_arm64_gadget_simd_sha1m();
void uml_arm64_gadget_simd_sha1m_soft();
void uml_arm64_gadget_simd_sha1su0();
void uml_arm64_gadget_simd_sha1su0_soft();
void uml_arm64_gadget_simd_sha256h();
void uml_arm64_gadget_simd_sha256h_soft();
void uml_arm64_gadget_simd_sha256h2();
void uml_arm64_gadget_simd_sha256h2_soft();
void uml_arm64_gadget_simd_sha256su1();
void uml_arm64_gadget_simd_sha256su1_soft();
void uml_arm64_gadget_simd_sha1h();
void uml_arm64_gadget_simd_sha1h_soft();
void uml_arm64_gadget_simd_sha1su1();
void uml_arm64_gadget_simd_sha1su1_soft();
void uml_arm64_gadget_simd_sha256su0();
void uml_arm64_gadget_simd_sha256su0_soft();
void uml_arm64_gadget_simd_sha512h();
void uml_arm64_gadget_simd_sha512h_soft();
void uml_arm64_gadget_simd_sha512h2();
void uml_arm64_gadget_simd_sha512h2_soft();
void uml_arm64_gadget_simd_sha512su0();
void uml_arm64_gadget_simd_sha512su0_soft();
void uml_arm64_gadget_simd_sha512su1();
void uml_arm64_gadget_simd_sha512su1_soft();
#define UML_DECLARE_SIMD_INDEXED_ONE(name, shape, index) \
	void uml_arm64_gadget_simd_indexed_##name##_##shape##_##index();
#define UML_DECLARE_SIMD_INDEXED_4(name, shape) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 1) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 2) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 3)
#define UML_DECLARE_SIMD_INDEXED_2(name, shape) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 1)
#define UML_DECLARE_SIMD_INDEXED_8(name, shape) \
	UML_DECLARE_SIMD_INDEXED_4(name, shape) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 4) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 5) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 6) \
	UML_DECLARE_SIMD_INDEXED_ONE(name, shape, 7)
#define UML_DECLARE_SIMD_INDEXED_SAME(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, 4h) \
	UML_DECLARE_SIMD_INDEXED_8(name, 8h) \
	UML_DECLARE_SIMD_INDEXED_4(name, 2s) \
	UML_DECLARE_SIMD_INDEXED_4(name, 4s)
UML_DECLARE_SIMD_INDEXED_SAME(mla)
UML_DECLARE_SIMD_INDEXED_SAME(mls)
UML_DECLARE_SIMD_INDEXED_SAME(mul)
#undef UML_DECLARE_SIMD_INDEXED_SAME
#define UML_DECLARE_SIMD_INDEXED_LONG(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_low) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_high) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_low) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_high)
UML_DECLARE_SIMD_INDEXED_LONG(smlal)
UML_DECLARE_SIMD_INDEXED_LONG(smlsl)
UML_DECLARE_SIMD_INDEXED_LONG(smull)
UML_DECLARE_SIMD_INDEXED_LONG(umlal)
UML_DECLARE_SIMD_INDEXED_LONG(umlsl)
UML_DECLARE_SIMD_INDEXED_LONG(umull)
#undef UML_DECLARE_SIMD_INDEXED_LONG
#define UML_DECLARE_SIMD_INDEXED_SATURATING(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_low) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_high) \
	UML_DECLARE_SIMD_INDEXED_8(name, h_scalar) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_low) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_high) \
	UML_DECLARE_SIMD_INDEXED_4(name, s_scalar)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmlal)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmlsl)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmull)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqdmulh)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqrdmulh)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqrdmlah)
UML_DECLARE_SIMD_INDEXED_SATURATING(sqrdmlsh)
#undef UML_DECLARE_SIMD_INDEXED_SATURATING
#define UML_DECLARE_SIMD_INDEXED_FLOATING(name) \
	UML_DECLARE_SIMD_INDEXED_8(name, h) \
	UML_DECLARE_SIMD_INDEXED_8(name, 4h) \
	UML_DECLARE_SIMD_INDEXED_8(name, 8h) \
	UML_DECLARE_SIMD_INDEXED_4(name, s) \
	UML_DECLARE_SIMD_INDEXED_4(name, 2s) \
	UML_DECLARE_SIMD_INDEXED_4(name, 4s) \
	UML_DECLARE_SIMD_INDEXED_2(name, d) \
	UML_DECLARE_SIMD_INDEXED_2(name, 2d)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmla)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmls)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmul)
UML_DECLARE_SIMD_INDEXED_FLOATING(fmulx)
#undef UML_DECLARE_SIMD_INDEXED_FLOATING
void uml_arm64_gadget_simd_fmulx_h();
void uml_arm64_gadget_simd_fmulx_4h();
void uml_arm64_gadget_simd_fmulx_8h();
void uml_arm64_gadget_simd_fmulx_s();
void uml_arm64_gadget_simd_fmulx_2s();
void uml_arm64_gadget_simd_fmulx_4s();
void uml_arm64_gadget_simd_fmulx_d();
void uml_arm64_gadget_simd_fmulx_2d();
#undef UML_DECLARE_SIMD_INDEXED_8
#undef UML_DECLARE_SIMD_INDEXED_4
#undef UML_DECLARE_SIMD_INDEXED_2
#undef UML_DECLARE_SIMD_INDEXED_ONE
void uml_arm64_gadget_simd_eor3_native();
void uml_arm64_gadget_simd_eor3_soft();
void uml_arm64_gadget_simd_bcax_native();
void uml_arm64_gadget_simd_bcax_soft();
void uml_arm64_gadget_simd_rax1_native();
void uml_arm64_gadget_simd_rax1_soft();
void uml_arm64_gadget_simd_pmull_8b_direct();
void uml_arm64_gadget_simd_pmull_8b2_direct();
void uml_arm64_gadget_simd_pmull_1d_native_direct();
void uml_arm64_gadget_simd_pmull_1d2_native_direct();
void uml_arm64_gadget_simd_pmull_1d_soft_direct();
void uml_arm64_gadget_simd_pmull_1d2_soft_direct();
#define UML_DECLARE_SIMD_XAR(rotate) \
	void uml_arm64_gadget_simd_xar_native_##rotate(); \
	void uml_arm64_gadget_simd_xar_soft_##rotate();
UML_DECLARE_SIMD_XAR(0) UML_DECLARE_SIMD_XAR(1)
UML_DECLARE_SIMD_XAR(2) UML_DECLARE_SIMD_XAR(3)
UML_DECLARE_SIMD_XAR(4) UML_DECLARE_SIMD_XAR(5)
UML_DECLARE_SIMD_XAR(6) UML_DECLARE_SIMD_XAR(7)
UML_DECLARE_SIMD_XAR(8) UML_DECLARE_SIMD_XAR(9)
UML_DECLARE_SIMD_XAR(10) UML_DECLARE_SIMD_XAR(11)
UML_DECLARE_SIMD_XAR(12) UML_DECLARE_SIMD_XAR(13)
UML_DECLARE_SIMD_XAR(14) UML_DECLARE_SIMD_XAR(15)
UML_DECLARE_SIMD_XAR(16) UML_DECLARE_SIMD_XAR(17)
UML_DECLARE_SIMD_XAR(18) UML_DECLARE_SIMD_XAR(19)
UML_DECLARE_SIMD_XAR(20) UML_DECLARE_SIMD_XAR(21)
UML_DECLARE_SIMD_XAR(22) UML_DECLARE_SIMD_XAR(23)
UML_DECLARE_SIMD_XAR(24) UML_DECLARE_SIMD_XAR(25)
UML_DECLARE_SIMD_XAR(26) UML_DECLARE_SIMD_XAR(27)
UML_DECLARE_SIMD_XAR(28) UML_DECLARE_SIMD_XAR(29)
UML_DECLARE_SIMD_XAR(30) UML_DECLARE_SIMD_XAR(31)
UML_DECLARE_SIMD_XAR(32) UML_DECLARE_SIMD_XAR(33)
UML_DECLARE_SIMD_XAR(34) UML_DECLARE_SIMD_XAR(35)
UML_DECLARE_SIMD_XAR(36) UML_DECLARE_SIMD_XAR(37)
UML_DECLARE_SIMD_XAR(38) UML_DECLARE_SIMD_XAR(39)
UML_DECLARE_SIMD_XAR(40) UML_DECLARE_SIMD_XAR(41)
UML_DECLARE_SIMD_XAR(42) UML_DECLARE_SIMD_XAR(43)
UML_DECLARE_SIMD_XAR(44) UML_DECLARE_SIMD_XAR(45)
UML_DECLARE_SIMD_XAR(46) UML_DECLARE_SIMD_XAR(47)
UML_DECLARE_SIMD_XAR(48) UML_DECLARE_SIMD_XAR(49)
UML_DECLARE_SIMD_XAR(50) UML_DECLARE_SIMD_XAR(51)
UML_DECLARE_SIMD_XAR(52) UML_DECLARE_SIMD_XAR(53)
UML_DECLARE_SIMD_XAR(54) UML_DECLARE_SIMD_XAR(55)
UML_DECLARE_SIMD_XAR(56) UML_DECLARE_SIMD_XAR(57)
UML_DECLARE_SIMD_XAR(58) UML_DECLARE_SIMD_XAR(59)
UML_DECLARE_SIMD_XAR(60) UML_DECLARE_SIMD_XAR(61)
UML_DECLARE_SIMD_XAR(62) UML_DECLARE_SIMD_XAR(63)
#undef UML_DECLARE_SIMD_XAR
#define UML_DECLARE_SIMD_THREE_SAME_V6(name) \
	void uml_arm64_gadget_simd_three_same_##name##_8b(); \
	void uml_arm64_gadget_simd_three_same_##name##_4h(); \
	void uml_arm64_gadget_simd_three_same_##name##_2s(); \
	void uml_arm64_gadget_simd_three_same_##name##_16b(); \
	void uml_arm64_gadget_simd_three_same_##name##_8h(); \
	void uml_arm64_gadget_simd_three_same_##name##_4s();
#define UML_DECLARE_SIMD_THREE_SAME_V7(name) \
	UML_DECLARE_SIMD_THREE_SAME_V6(name) \
	void uml_arm64_gadget_simd_three_same_##name##_2d();
#define UML_DECLARE_SIMD_THREE_SAME_S4(name) \
	void uml_arm64_gadget_simd_three_same_##name##_b(); \
	void uml_arm64_gadget_simd_three_same_##name##_h(); \
	void uml_arm64_gadget_simd_three_same_##name##_s(); \
	void uml_arm64_gadget_simd_three_same_##name##_d();
UML_DECLARE_SIMD_THREE_SAME_V6(shadd)
UML_DECLARE_SIMD_THREE_SAME_V6(uhadd)
UML_DECLARE_SIMD_THREE_SAME_V6(srhadd)
UML_DECLARE_SIMD_THREE_SAME_V6(urhadd)
UML_DECLARE_SIMD_THREE_SAME_V6(shsub)
UML_DECLARE_SIMD_THREE_SAME_V6(uhsub)
UML_DECLARE_SIMD_THREE_SAME_V7(sqadd)
UML_DECLARE_SIMD_THREE_SAME_S4(sqadd)
UML_DECLARE_SIMD_THREE_SAME_V7(uqadd)
UML_DECLARE_SIMD_THREE_SAME_S4(uqadd)
UML_DECLARE_SIMD_THREE_SAME_V7(sqsub)
UML_DECLARE_SIMD_THREE_SAME_S4(sqsub)
UML_DECLARE_SIMD_THREE_SAME_V7(uqsub)
UML_DECLARE_SIMD_THREE_SAME_S4(uqsub)
UML_DECLARE_SIMD_THREE_SAME_V7(sshl)
void uml_arm64_gadget_simd_three_same_sshl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(ushl)
void uml_arm64_gadget_simd_three_same_ushl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(sqshl)
UML_DECLARE_SIMD_THREE_SAME_S4(sqshl)
UML_DECLARE_SIMD_THREE_SAME_V7(uqshl)
UML_DECLARE_SIMD_THREE_SAME_S4(uqshl)
UML_DECLARE_SIMD_THREE_SAME_V7(srshl)
void uml_arm64_gadget_simd_three_same_srshl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(urshl)
void uml_arm64_gadget_simd_three_same_urshl_d();
UML_DECLARE_SIMD_THREE_SAME_V7(sqrshl)
UML_DECLARE_SIMD_THREE_SAME_S4(sqrshl)
UML_DECLARE_SIMD_THREE_SAME_V7(uqrshl)
UML_DECLARE_SIMD_THREE_SAME_S4(uqrshl)
UML_DECLARE_SIMD_THREE_SAME_V7(cmgt)
void uml_arm64_gadget_simd_three_same_cmgt_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmhi)
void uml_arm64_gadget_simd_three_same_cmhi_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmge)
void uml_arm64_gadget_simd_three_same_cmge_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmhs)
void uml_arm64_gadget_simd_three_same_cmhs_d();
UML_DECLARE_SIMD_THREE_SAME_V6(smax)
UML_DECLARE_SIMD_THREE_SAME_V6(umax)
UML_DECLARE_SIMD_THREE_SAME_V6(smin)
UML_DECLARE_SIMD_THREE_SAME_V6(umin)
UML_DECLARE_SIMD_THREE_SAME_V6(sabd)
UML_DECLARE_SIMD_THREE_SAME_V6(uabd)
UML_DECLARE_SIMD_THREE_SAME_V6(saba)
UML_DECLARE_SIMD_THREE_SAME_V6(uaba)
UML_DECLARE_SIMD_THREE_SAME_V7(add)
void uml_arm64_gadget_simd_three_same_add_d();
UML_DECLARE_SIMD_THREE_SAME_V7(sub)
void uml_arm64_gadget_simd_three_same_sub_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmtst)
void uml_arm64_gadget_simd_three_same_cmtst_d();
UML_DECLARE_SIMD_THREE_SAME_V7(cmeq)
void uml_arm64_gadget_simd_three_same_cmeq_d();
UML_DECLARE_SIMD_THREE_SAME_V6(mla)
UML_DECLARE_SIMD_THREE_SAME_V6(mls)
UML_DECLARE_SIMD_THREE_SAME_V6(mul)
void uml_arm64_gadget_simd_destination_resident_load();
#define UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(shape) \
	void uml_arm64_gadget_simd_destination_resident_mla_##shape();
UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(8b)
UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(4h)
UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(2s)
UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(16b)
UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(8h)
UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA(4s)
#undef UML_DECLARE_SIMD_DESTINATION_RESIDENT_MLA
void uml_arm64_gadget_simd_destination_resident_flush();
void uml_arm64_gadget_simd_three_same_pmul_8b();
void uml_arm64_gadget_simd_three_same_pmul_16b();
UML_DECLARE_SIMD_THREE_SAME_V6(smaxp)
UML_DECLARE_SIMD_THREE_SAME_V6(umaxp)
UML_DECLARE_SIMD_THREE_SAME_V6(sminp)
UML_DECLARE_SIMD_THREE_SAME_V6(uminp)
UML_DECLARE_SIMD_THREE_SAME_V7(addp)
void uml_arm64_gadget_simd_three_same_sqdmulh_4h();
void uml_arm64_gadget_simd_three_same_sqdmulh_2s();
void uml_arm64_gadget_simd_three_same_sqdmulh_8h();
void uml_arm64_gadget_simd_three_same_sqdmulh_4s();
void uml_arm64_gadget_simd_three_same_sqdmulh_h();
void uml_arm64_gadget_simd_three_same_sqdmulh_s();
void uml_arm64_gadget_simd_three_same_sqrdmulh_4h();
void uml_arm64_gadget_simd_three_same_sqrdmulh_2s();
void uml_arm64_gadget_simd_three_same_sqrdmulh_8h();
void uml_arm64_gadget_simd_three_same_sqrdmulh_4s();
void uml_arm64_gadget_simd_three_same_sqrdmulh_h();
void uml_arm64_gadget_simd_three_same_sqrdmulh_s();
#define UML_DECLARE_SIMD_RDM(name) \
	void uml_arm64_gadget_simd_three_same_##name##_4h(); \
	void uml_arm64_gadget_simd_three_same_##name##_2s(); \
	void uml_arm64_gadget_simd_three_same_##name##_8h(); \
	void uml_arm64_gadget_simd_three_same_##name##_4s(); \
	void uml_arm64_gadget_simd_three_same_##name##_h(); \
	void uml_arm64_gadget_simd_three_same_##name##_s();
UML_DECLARE_SIMD_RDM(sqrdmlah)
UML_DECLARE_SIMD_RDM(sqrdmlsh)
#undef UML_DECLARE_SIMD_RDM
void uml_arm64_gadget_simd_rdm_software();
void uml_arm64_gadget_simd_dot_vector_sdot_2s();
void uml_arm64_gadget_simd_dot_vector_sdot_4s();
void uml_arm64_gadget_simd_dot_vector_udot_2s();
void uml_arm64_gadget_simd_dot_vector_udot_4s();
void uml_arm64_gadget_simd_dot_vector_usdot_2s();
void uml_arm64_gadget_simd_dot_vector_usdot_4s();
void uml_arm64_gadget_simd_dot_vector_sdot_2s_soft();
void uml_arm64_gadget_simd_dot_vector_sdot_4s_soft();
void uml_arm64_gadget_simd_dot_vector_udot_2s_soft();
void uml_arm64_gadget_simd_dot_vector_udot_4s_soft();
void uml_arm64_gadget_simd_dot_vector_usdot_2s_soft();
void uml_arm64_gadget_simd_dot_vector_usdot_4s_soft();
void uml_arm64_gadget_simd_matrix_smmla();
void uml_arm64_gadget_simd_matrix_ummla();
void uml_arm64_gadget_simd_matrix_usmmla();
void uml_arm64_gadget_simd_matrix_smmla_soft();
void uml_arm64_gadget_simd_matrix_ummla_soft();
void uml_arm64_gadget_simd_matrix_usmmla_soft();
#define UML_DECLARE_SIMD_DOT_INDEXED_SHAPE(name, shape) \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_0(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_1(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_2(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_3();
#define UML_DECLARE_SIMD_DOT_INDEXED(name) \
	UML_DECLARE_SIMD_DOT_INDEXED_SHAPE(name, 2s) \
	UML_DECLARE_SIMD_DOT_INDEXED_SHAPE(name, 4s)
UML_DECLARE_SIMD_DOT_INDEXED(sdot)
UML_DECLARE_SIMD_DOT_INDEXED(udot)
UML_DECLARE_SIMD_DOT_INDEXED(sudot)
UML_DECLARE_SIMD_DOT_INDEXED(usdot)
#define UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(name, shape) \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_0_soft(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_1_soft(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_2_soft(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_3_soft();
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sdot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sdot, 4s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(udot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(udot, 4s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sudot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(sudot, 4s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(usdot, 2s)
UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE(usdot, 4s)
#undef UML_DECLARE_SIMD_DOT_INDEXED_SOFT_SHAPE
#undef UML_DECLARE_SIMD_DOT_INDEXED
#undef UML_DECLARE_SIMD_DOT_INDEXED_SHAPE
#undef UML_DECLARE_SIMD_THREE_SAME_S4
#undef UML_DECLARE_SIMD_THREE_SAME_V7
#undef UML_DECLARE_SIMD_THREE_SAME_V6
#define UML_DECLARE_SIMD_WIDENING_V6(name) \
	void uml_arm64_gadget_simd_widening_##name##_8b(); \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_16b(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s();
#define UML_DECLARE_SIMD_HIGH_NARROW(name) \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_2d(); \
	void uml_arm64_gadget_simd_widening_##name##_8h2(); \
	void uml_arm64_gadget_simd_widening_##name##_4s2(); \
	void uml_arm64_gadget_simd_widening_##name##_2d2();
#define UML_DECLARE_SIMD_SATURATING_LONG(name) \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_h(); \
	void uml_arm64_gadget_simd_widening_##name##_s();
UML_DECLARE_SIMD_WIDENING_V6(saddl)
UML_DECLARE_SIMD_WIDENING_V6(uaddl)
UML_DECLARE_SIMD_WIDENING_V6(saddw)
UML_DECLARE_SIMD_WIDENING_V6(uaddw)
UML_DECLARE_SIMD_WIDENING_V6(ssubl)
UML_DECLARE_SIMD_WIDENING_V6(usubl)
UML_DECLARE_SIMD_WIDENING_V6(ssubw)
UML_DECLARE_SIMD_WIDENING_V6(usubw)
UML_DECLARE_SIMD_HIGH_NARROW(addhn)
UML_DECLARE_SIMD_HIGH_NARROW(raddhn)
UML_DECLARE_SIMD_HIGH_NARROW(subhn)
UML_DECLARE_SIMD_HIGH_NARROW(rsubhn)
UML_DECLARE_SIMD_WIDENING_V6(sabdl)
UML_DECLARE_SIMD_WIDENING_V6(uabdl)
UML_DECLARE_SIMD_WIDENING_V6(sabal)
UML_DECLARE_SIMD_WIDENING_V6(uabal)
UML_DECLARE_SIMD_WIDENING_V6(smlal)
UML_DECLARE_SIMD_WIDENING_V6(umlal)
UML_DECLARE_SIMD_WIDENING_V6(smlsl)
UML_DECLARE_SIMD_WIDENING_V6(umlsl)
UML_DECLARE_SIMD_WIDENING_V6(smull)
UML_DECLARE_SIMD_WIDENING_V6(umull)
UML_DECLARE_SIMD_SATURATING_LONG(sqdmlal)
UML_DECLARE_SIMD_SATURATING_LONG(sqdmlsl)
UML_DECLARE_SIMD_SATURATING_LONG(sqdmull)
#undef UML_DECLARE_SIMD_SATURATING_LONG
#undef UML_DECLARE_SIMD_HIGH_NARROW
#undef UML_DECLARE_SIMD_WIDENING_V6
#define UML_DECLARE_SIMD_UNARY_V6(name) \
	void uml_arm64_gadget_simd_unary_##name##_8b(); \
	void uml_arm64_gadget_simd_unary_##name##_4h(); \
	void uml_arm64_gadget_simd_unary_##name##_2s(); \
	void uml_arm64_gadget_simd_unary_##name##_16b(); \
	void uml_arm64_gadget_simd_unary_##name##_8h(); \
	void uml_arm64_gadget_simd_unary_##name##_4s();
#define UML_DECLARE_SIMD_UNARY_V7(name) \
	UML_DECLARE_SIMD_UNARY_V6(name) \
	void uml_arm64_gadget_simd_unary_##name##_2d();
#define UML_DECLARE_SIMD_UNARY_S4(name) \
	void uml_arm64_gadget_simd_unary_##name##_b(); \
	void uml_arm64_gadget_simd_unary_##name##_h(); \
	void uml_arm64_gadget_simd_unary_##name##_s(); \
	void uml_arm64_gadget_simd_unary_##name##_d();
#define UML_DECLARE_SIMD_NARROW(name) \
	void uml_arm64_gadget_simd_unary_##name##_8h(); \
	void uml_arm64_gadget_simd_unary_##name##_4s(); \
	void uml_arm64_gadget_simd_unary_##name##_2d(); \
	void uml_arm64_gadget_simd_unary_##name##_8h2(); \
	void uml_arm64_gadget_simd_unary_##name##_4s2(); \
	void uml_arm64_gadget_simd_unary_##name##_2d2();
UML_DECLARE_SIMD_UNARY_V6(saddlp)
UML_DECLARE_SIMD_UNARY_V6(uaddlp)
UML_DECLARE_SIMD_UNARY_V6(sadalp)
UML_DECLARE_SIMD_UNARY_V6(uadalp)
UML_DECLARE_SIMD_UNARY_V7(suqadd)
UML_DECLARE_SIMD_UNARY_S4(suqadd)
UML_DECLARE_SIMD_UNARY_V7(usqadd)
UML_DECLARE_SIMD_UNARY_S4(usqadd)
UML_DECLARE_SIMD_UNARY_V6(cls)
UML_DECLARE_SIMD_UNARY_V6(clz)
UML_DECLARE_SIMD_UNARY_V7(sqabs)
UML_DECLARE_SIMD_UNARY_S4(sqabs)
UML_DECLARE_SIMD_UNARY_V7(sqneg)
UML_DECLARE_SIMD_UNARY_S4(sqneg)
UML_DECLARE_SIMD_UNARY_V7(abs)
void uml_arm64_gadget_simd_unary_abs_d();
UML_DECLARE_SIMD_UNARY_V7(neg)
void uml_arm64_gadget_simd_unary_neg_d();
void uml_arm64_gadget_simd_unary_cnt_8b();
void uml_arm64_gadget_simd_unary_cnt_16b();
void uml_arm64_gadget_simd_unary_not_8b();
void uml_arm64_gadget_simd_unary_not_16b();
void uml_arm64_gadget_simd_unary_rbit_8b();
void uml_arm64_gadget_simd_unary_rbit_16b();
UML_DECLARE_SIMD_UNARY_V7(cmgt0)
void uml_arm64_gadget_simd_unary_cmgt0_d();
UML_DECLARE_SIMD_UNARY_V7(cmge0)
void uml_arm64_gadget_simd_unary_cmge0_d();
UML_DECLARE_SIMD_UNARY_V7(cmeq0)
void uml_arm64_gadget_simd_unary_cmeq0_d();
UML_DECLARE_SIMD_UNARY_V7(cmle0)
void uml_arm64_gadget_simd_unary_cmle0_d();
UML_DECLARE_SIMD_UNARY_V7(cmlt0)
void uml_arm64_gadget_simd_unary_cmlt0_d();
UML_DECLARE_SIMD_NARROW(xtn)
UML_DECLARE_SIMD_NARROW(sqxtn)
UML_DECLARE_SIMD_NARROW(uqxtn)
UML_DECLARE_SIMD_NARROW(sqxtun)
void uml_arm64_gadget_simd_unary_sqxtn_b();
void uml_arm64_gadget_simd_unary_sqxtn_h();
void uml_arm64_gadget_simd_unary_sqxtn_s();
void uml_arm64_gadget_simd_unary_uqxtn_b();
void uml_arm64_gadget_simd_unary_uqxtn_h();
void uml_arm64_gadget_simd_unary_uqxtn_s();
void uml_arm64_gadget_simd_unary_sqxtun_b();
void uml_arm64_gadget_simd_unary_sqxtun_h();
void uml_arm64_gadget_simd_unary_sqxtun_s();
void uml_arm64_gadget_simd_unary_rev64_8b();
void uml_arm64_gadget_simd_unary_rev64_4h();
void uml_arm64_gadget_simd_unary_rev64_2s();
void uml_arm64_gadget_simd_unary_rev64_16b();
void uml_arm64_gadget_simd_unary_rev64_8h();
void uml_arm64_gadget_simd_unary_rev64_4s();
void uml_arm64_gadget_simd_unary_rev32_8b();
void uml_arm64_gadget_simd_unary_rev32_4h();
void uml_arm64_gadget_simd_unary_rev32_16b();
void uml_arm64_gadget_simd_unary_rev32_8h();
void uml_arm64_gadget_simd_unary_rev16_8b();
void uml_arm64_gadget_simd_unary_rev16_16b();
void uml_arm64_gadget_simd_unary_shll_8b();
void uml_arm64_gadget_simd_unary_shll_4h();
void uml_arm64_gadget_simd_unary_shll_2s();
void uml_arm64_gadget_simd_unary_shll_16b();
void uml_arm64_gadget_simd_unary_shll_8h();
void uml_arm64_gadget_simd_unary_shll_4s();
#define UML_DECLARE_SIMD_REDUCE_5(name) \
	void uml_arm64_gadget_simd_reduce_##name##_8b(); \
	void uml_arm64_gadget_simd_reduce_##name##_4h(); \
	void uml_arm64_gadget_simd_reduce_##name##_16b(); \
	void uml_arm64_gadget_simd_reduce_##name##_8h(); \
	void uml_arm64_gadget_simd_reduce_##name##_4s();
UML_DECLARE_SIMD_REDUCE_5(saddlv)
UML_DECLARE_SIMD_REDUCE_5(uaddlv)
UML_DECLARE_SIMD_REDUCE_5(smaxv)
UML_DECLARE_SIMD_REDUCE_5(umaxv)
UML_DECLARE_SIMD_REDUCE_5(sminv)
UML_DECLARE_SIMD_REDUCE_5(uminv)
UML_DECLARE_SIMD_REDUCE_5(addv)
#undef UML_DECLARE_SIMD_REDUCE_5
void uml_arm64_gadget_simd_unary_urecpe_2s();
void uml_arm64_gadget_simd_unary_urecpe_4s();
void uml_arm64_gadget_simd_unary_ursqrte_2s();
void uml_arm64_gadget_simd_unary_ursqrte_4s();
#undef UML_DECLARE_SIMD_NARROW
#undef UML_DECLARE_SIMD_UNARY_S4
#undef UML_DECLARE_SIMD_UNARY_V7
#undef UML_DECLARE_SIMD_UNARY_V6
void uml_arm64_gadget_vector_to_gpr_16_low();
void uml_arm64_gadget_gpr_to_vector_16_low();
void uml_arm64_gadget_gpr_to_vector_16_low_zero();
void uml_arm64_gadget_vector_to_gpr_32_low();
void uml_arm64_gadget_gpr_to_vector_32_low();
void uml_arm64_gadget_gpr_to_vector_32_low_zero();
void uml_arm64_gadget_vector_to_gpr_64_low();
void uml_arm64_gadget_gpr_to_vector_64_low();
void uml_arm64_gadget_gpr_to_vector_64_low_zero();
void uml_arm64_gadget_vector_to_gpr_64_high();
void uml_arm64_gadget_gpr_to_vector_64_high();
void uml_arm64_gadget_gpr_to_vector_64_high_zero();
#define UML_DECLARE_FLOATING_SELECT(index, name) \
	void uml_arm64_gadget_floating_select_##name##_h(); \
	void uml_arm64_gadget_floating_select_##name##_s(); \
	void uml_arm64_gadget_floating_select_##name##_d();
UML_ARM64_CONDITIONS(UML_DECLARE_FLOATING_SELECT)
UML_DECLARE_FLOATING_SELECT(14, always)
#undef UML_DECLARE_FLOATING_SELECT
void uml_arm64_gadget_pc_relative();
void uml_arm64_gadget_sysreg_tls_read();
void uml_arm64_gadget_sysreg_tls_write_r();
void uml_arm64_gadget_sysreg_tls_write_z();
void uml_arm64_gadget_sysreg_zero_read();
void uml_arm64_gadget_sysreg_dczid_read();
void uml_arm64_gadget_sysreg_ctr_read();
void uml_arm64_gadget_sysreg_fpcr_read();
void uml_arm64_gadget_sysreg_fpcr_write_r();
void uml_arm64_gadget_sysreg_fpcr_write_z();
void uml_arm64_gadget_sysreg_fpsr_read();
void uml_arm64_gadget_sysreg_fpsr_write_r();
void uml_arm64_gadget_sysreg_fpsr_write_z();
void uml_arm64_gadget_sysreg_nzcv_read();
void uml_arm64_gadget_sysreg_nzcv_write_r();
void uml_arm64_gadget_sysreg_nzcv_write_z();
void uml_arm64_gadget_sysreg_cntfrq_read();
void uml_arm64_gadget_sysreg_cntvct_read();
void uml_arm64_gadget_sysreg_cntvctss_read();
void uml_arm64_gadget_sysreg_dit_read();
void uml_arm64_gadget_sysreg_dit_write_r();
void uml_arm64_gadget_sysreg_dit_write_z();
void uml_arm64_gadget_cache_zero_r();
void uml_arm64_gadget_cache_zero_z();
void uml_arm64_gadget_code_synchronize_r();
void uml_arm64_gadget_code_synchronize_z();
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
#define DECLARE_MEMORY_BARRIER(kind, option) \
	void uml_arm64_gadget_##kind##_##option();
#define DECLARE_MEMORY_BARRIER_OPTION(option) \
	DECLARE_MEMORY_BARRIER(dmb, option) \
	DECLARE_MEMORY_BARRIER(dsb, option)
UML_ARM64_BARRIER_OPTIONS(DECLARE_MEMORY_BARRIER_OPTION)
#undef DECLARE_MEMORY_BARRIER_OPTION
#undef DECLARE_MEMORY_BARRIER
void uml_arm64_gadget_isb();
void uml_arm64_gadget_speculation_barrier();
void uml_arm64_gadget_speculation_barrier_fallback();
void uml_arm64_gadget_clear_exclusive();
void uml_arm64_gadget_atomic_sp_alignment_read();
void uml_arm64_gadget_atomic_sp_alignment_write();
void uml_arm64_gadget_yield();
void uml_arm64_gadget_csdb();
void uml_arm64_gadget_dgh();
void uml_arm64_gadget_clrbhb();
void uml_arm64_gadget_nop();
/* A word or doubleword register transfer takes the in-place family; only
 * XZR keeps a shared entry. */
#define UML_DECLARE_SCALAR_UNSIGNED_ZERO(name) \
	void uml_arm64_gadget_##name##_z(); \
	void uml_arm64_gadget_##name##_z_sp();
UML_DECLARE_SCALAR_UNSIGNED_ZERO(load32_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_ZERO(load64_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_ZERO(store32_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_ZERO(store64_unsigned_fast)
#undef UML_DECLARE_SCALAR_UNSIGNED_ZERO
#define UML_DECLARE_SCALAR_UNSIGNED_NARROW(name) \
	void uml_arm64_gadget_##name##_r(); \
	void uml_arm64_gadget_##name##_z(); \
	void uml_arm64_gadget_##name##_r_sp(); \
	void uml_arm64_gadget_##name##_z_sp();
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load8_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load16_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load8_signed_w_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load8_signed_x_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load16_signed_w_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load16_signed_x_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(load32_signed_x_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(store8_unsigned_fast)
UML_DECLARE_SCALAR_UNSIGNED_NARROW(store16_unsigned_fast)
#undef UML_DECLARE_SCALAR_UNSIGNED_NARROW
#define UML_DECLARE_VECTOR_UNSIGNED(width) \
	void uml_arm64_gadget_vector_load##width##_unsigned_fast(); \
	void uml_arm64_gadget_vector_load##width##_unsigned_fast_sp(); \
	void uml_arm64_gadget_vector_store##width##_unsigned_fast(); \
	void uml_arm64_gadget_vector_store##width##_unsigned_fast_sp();
UML_DECLARE_VECTOR_UNSIGNED(8)
UML_DECLARE_VECTOR_UNSIGNED(16)
UML_DECLARE_VECTOR_UNSIGNED(32)
UML_DECLARE_VECTOR_UNSIGNED(64)
UML_DECLARE_VECTOR_UNSIGNED(128)
#undef UML_DECLARE_VECTOR_UNSIGNED
#define UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, mode) \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_r_sp(); \
	void uml_arm64_gadget_scalar_other_##name##_##mode##_z_sp();
#define UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, mode) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, mode)
#define UML_DECLARE_SCALAR_OTHER_COMMON(name) \
	UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, offset) \
	UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, pre) \
	UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE(name, post) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, uxtw_0) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, x_0) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, sxtw_0) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, uxtw_1) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, x_1) \
	UML_DECLARE_SCALAR_OTHER_REGISTER_MODE(name, sxtw_1)
#define UML_DECLARE_SCALAR_OTHER_STORE(name) \
	UML_DECLARE_SCALAR_OTHER_COMMON(name)
#define UML_DECLARE_SCALAR_OTHER_LOAD(name) \
	UML_DECLARE_SCALAR_OTHER_COMMON(name) \
	void uml_arm64_gadget_scalar_other_##name##_post_suppress_r();
UML_DECLARE_SCALAR_OTHER_STORE(store8)
UML_DECLARE_SCALAR_OTHER_LOAD(load8u)
UML_DECLARE_SCALAR_OTHER_LOAD(load8sx)
UML_DECLARE_SCALAR_OTHER_LOAD(load8sw)
UML_DECLARE_SCALAR_OTHER_STORE(store16)
UML_DECLARE_SCALAR_OTHER_LOAD(load16u)
UML_DECLARE_SCALAR_OTHER_LOAD(load16sx)
UML_DECLARE_SCALAR_OTHER_LOAD(load16sw)
UML_DECLARE_SCALAR_OTHER_STORE(store32)
UML_DECLARE_SCALAR_OTHER_LOAD(load32u)
UML_DECLARE_SCALAR_OTHER_LOAD(load32sx)
UML_DECLARE_SCALAR_OTHER_STORE(store64)
UML_DECLARE_SCALAR_OTHER_LOAD(load64u)
#undef UML_DECLARE_SCALAR_OTHER_LOAD
#undef UML_DECLARE_SCALAR_OTHER_STORE
#undef UML_DECLARE_SCALAR_OTHER_COMMON
#undef UML_DECLARE_SCALAR_OTHER_IMMEDIATE_MODE
#undef UML_DECLARE_SCALAR_OTHER_REGISTER_MODE
#define UML_DECLARE_VECTOR_OTHER_MODE(name, mode) \
	void uml_arm64_gadget_vector_other_##name##_##mode(); \
	void uml_arm64_gadget_vector_other_##name##_##mode##_sp();
#define UML_DECLARE_VECTOR_OTHER(name) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, offset) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, pre) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, post) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, uxtw_0) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, x_0) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, sxtw_0) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, uxtw_1) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, x_1) \
	UML_DECLARE_VECTOR_OTHER_MODE(name, sxtw_1)
UML_DECLARE_VECTOR_OTHER(store8)
UML_DECLARE_VECTOR_OTHER(load8)
UML_DECLARE_VECTOR_OTHER(store16)
UML_DECLARE_VECTOR_OTHER(load16)
UML_DECLARE_VECTOR_OTHER(store32)
UML_DECLARE_VECTOR_OTHER(load32)
UML_DECLARE_VECTOR_OTHER(store64)
UML_DECLARE_VECTOR_OTHER(load64)
UML_DECLARE_VECTOR_OTHER(store128)
UML_DECLARE_VECTOR_OTHER(load128)
#undef UML_DECLARE_VECTOR_OTHER
#undef UML_DECLARE_VECTOR_OTHER_MODE
#define UML_DECLARE_INTEGER_PAIR_MODE(name, mode) \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rr(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rz(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zr(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zz(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rr_sp(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_rz_sp(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zr_sp(); \
	void uml_arm64_gadget_integer_pair_##name##_##mode##_zz_sp();
#define UML_DECLARE_INTEGER_PAIR_STORE(name) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 0) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 1) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 2)
#define UML_DECLARE_INTEGER_PAIR_LOAD(name) \
	UML_DECLARE_INTEGER_PAIR_STORE(name) \
	UML_DECLARE_INTEGER_PAIR_MODE(name, 3)
UML_DECLARE_INTEGER_PAIR_STORE(store32)
UML_DECLARE_INTEGER_PAIR_LOAD(load32)
UML_DECLARE_INTEGER_PAIR_LOAD(load32s)
UML_DECLARE_INTEGER_PAIR_STORE(store64)
UML_DECLARE_INTEGER_PAIR_LOAD(load64)
#undef UML_DECLARE_INTEGER_PAIR_LOAD
#undef UML_DECLARE_INTEGER_PAIR_STORE
#undef UML_DECLARE_INTEGER_PAIR_MODE
#define UML_DECLARE_VECTOR_PAIR(name) \
	void uml_arm64_gadget_vector_pair_##name##_0(); \
	void uml_arm64_gadget_vector_pair_##name##_1(); \
	void uml_arm64_gadget_vector_pair_##name##_2(); \
	void uml_arm64_gadget_vector_pair_##name##_0_sp(); \
	void uml_arm64_gadget_vector_pair_##name##_1_sp(); \
	void uml_arm64_gadget_vector_pair_##name##_2_sp();
UML_DECLARE_VECTOR_PAIR(store32)
UML_DECLARE_VECTOR_PAIR(load32)
UML_DECLARE_VECTOR_PAIR(store64)
UML_DECLARE_VECTOR_PAIR(load64)
UML_DECLARE_VECTOR_PAIR(store128)
UML_DECLARE_VECTOR_PAIR(load128)
#undef UML_DECLARE_VECTOR_PAIR
#define UML_DECLARE_SIMD_STRUCTURE_DIRECTION(base, direction) \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_0(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_1(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_2(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_s0(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_s1(); \
	void uml_arm64_gadget_simd_structure_##base##_##direction##_s2();
#define UML_DECLARE_SIMD_STRUCTURE(base) \
	UML_DECLARE_SIMD_STRUCTURE_DIRECTION(base, store) \
	UML_DECLARE_SIMD_STRUCTURE_DIRECTION(base, load)
UML_DECLARE_SIMD_STRUCTURE(contig_d_1)
UML_DECLARE_SIMD_STRUCTURE(contig_d_2)
UML_DECLARE_SIMD_STRUCTURE(contig_d_3)
UML_DECLARE_SIMD_STRUCTURE(contig_d_4)
UML_DECLARE_SIMD_STRUCTURE(contig_q_1)
UML_DECLARE_SIMD_STRUCTURE(contig_q_2)
UML_DECLARE_SIMD_STRUCTURE(contig_q_3)
UML_DECLARE_SIMD_STRUCTURE(contig_q_4)
#define UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_d_b_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_d_h_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_d_s_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_b_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_h_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_s_##count) \
	UML_DECLARE_SIMD_STRUCTURE(interleaved_q_d_##count)
UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(2)
UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(3)
UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED(4)
#undef UML_DECLARE_SIMD_STRUCTURE_INTERLEAVED
#undef UML_DECLARE_SIMD_STRUCTURE
#undef UML_DECLARE_SIMD_STRUCTURE_DIRECTION
#define UML_DECLARE_SIMD_REPLICATE_COUNT(type, count) \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_0(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_1(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_2(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_s0(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_s1(); \
	void uml_arm64_gadget_simd_replicate_##type##_##count##_s2();
#define UML_DECLARE_SIMD_REPLICATE(type) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 1) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 2) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 3) \
	UML_DECLARE_SIMD_REPLICATE_COUNT(type, 4)
UML_DECLARE_SIMD_REPLICATE(b_d)
UML_DECLARE_SIMD_REPLICATE(b_q)
UML_DECLARE_SIMD_REPLICATE(h_d)
UML_DECLARE_SIMD_REPLICATE(h_q)
UML_DECLARE_SIMD_REPLICATE(s_d)
UML_DECLARE_SIMD_REPLICATE(s_q)
UML_DECLARE_SIMD_REPLICATE(d_d)
UML_DECLARE_SIMD_REPLICATE(d_q)
#undef UML_DECLARE_SIMD_REPLICATE
#undef UML_DECLARE_SIMD_REPLICATE_COUNT
#define UML_DECLARE_SIMD_LANE_DIRECTION(type, count, direction) \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_0(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_1(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_2(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s0(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s1(); \
	void uml_arm64_gadget_simd_lane_##type##_##count##_##direction##_s2();
#define UML_DECLARE_SIMD_LANE_COUNT(type, count) \
	UML_DECLARE_SIMD_LANE_DIRECTION(type, count, store) \
	UML_DECLARE_SIMD_LANE_DIRECTION(type, count, load)
#define UML_DECLARE_SIMD_LANE(type) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 1) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 2) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 3) \
	UML_DECLARE_SIMD_LANE_COUNT(type, 4)
UML_DECLARE_SIMD_LANE(b)
UML_DECLARE_SIMD_LANE(h)
UML_DECLARE_SIMD_LANE(s)
UML_DECLARE_SIMD_LANE(d)
#undef UML_DECLARE_SIMD_LANE
#undef UML_DECLARE_SIMD_LANE_COUNT
#undef UML_DECLARE_SIMD_LANE_DIRECTION
void uml_arm64_gadget_mops_prologue_copy_forward();
void uml_arm64_gadget_mops_prologue_copy_general();
void uml_arm64_gadget_mops_prologue_set();
void uml_arm64_gadget_mops_epilogue_copy_forward();
void uml_arm64_gadget_mops_epilogue_copy_general();
void uml_arm64_gadget_mops_epilogue_set_r();
void uml_arm64_gadget_mops_epilogue_set_z();
#define UML_DECLARE_ORDERED_VARIANTS(kind, ordering, size) \
	void uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_r(); \
	void uml_arm64_gadget_ordered_##kind##_##ordering##_##size##_z();
#define UML_DECLARE_ORDERED_SIZES(kind, ordering) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, b) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, h) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, w) \
	UML_DECLARE_ORDERED_VARIANTS(kind, ordering, x)
UML_DECLARE_ORDERED_SIZES(load, acquire)
UML_DECLARE_ORDERED_SIZES(store, release)
#undef UML_DECLARE_ORDERED_SIZES
#undef UML_DECLARE_ORDERED_VARIANTS
#define UML_DECLARE_RCPC_LOAD_VARIANTS(name) \
	void uml_arm64_gadget_rcpc_load_##name##_r(); \
	void uml_arm64_gadget_rcpc_load_##name##_z();
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_b)
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_h)
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_w)
UML_DECLARE_RCPC_LOAD_VARIANTS(zero_x)
UML_DECLARE_RCPC_LOAD_VARIANTS(sb_w)
UML_DECLARE_RCPC_LOAD_VARIANTS(sb_x)
UML_DECLARE_RCPC_LOAD_VARIANTS(sh_w)
UML_DECLARE_RCPC_LOAD_VARIANTS(sh_x)
UML_DECLARE_RCPC_LOAD_VARIANTS(sw_x)
#undef UML_DECLARE_RCPC_LOAD_VARIANTS
#define UML_DECLARE_RCPC_STORE_VARIANTS(size) \
	void uml_arm64_gadget_rcpc_store_##size##_r(); \
	void uml_arm64_gadget_rcpc_store_##size##_z();
UML_DECLARE_RCPC_STORE_VARIANTS(b)
UML_DECLARE_RCPC_STORE_VARIANTS(h)
UML_DECLARE_RCPC_STORE_VARIANTS(w)
UML_DECLARE_RCPC_STORE_VARIANTS(x)
#undef UML_DECLARE_RCPC_STORE_VARIANTS
#define UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, size) \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_compare_swap_##ordering##_##size##_zz();
#define UML_DECLARE_COMPARE_SWAP_SIZES(ordering) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, b) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, h) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, w) \
	UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS(ordering, x)
UML_DECLARE_COMPARE_SWAP_SIZES(relaxed)
UML_DECLARE_COMPARE_SWAP_SIZES(release)
UML_DECLARE_COMPARE_SWAP_SIZES(acquire)
UML_DECLARE_COMPARE_SWAP_SIZES(acquire_release)
#undef UML_DECLARE_COMPARE_SWAP_SIZES
#undef UML_DECLARE_COMPARE_SWAP_REGISTER_VARIANTS
#define UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS(ordering, size) \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_compare_swap_pair_##ordering##_##size##_zz();
#define UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(ordering) \
	UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS(ordering, w) \
	UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS(ordering, x)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(relaxed)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(release)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(acquire)
UML_DECLARE_COMPARE_SWAP_PAIR_SIZES(acquire_release)
#undef UML_DECLARE_COMPARE_SWAP_PAIR_SIZES
#undef UML_DECLARE_COMPARE_SWAP_PAIR_REGISTER_VARIANTS
#define UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_load_##ordering##_##size##_r(); \
	void uml_arm64_gadget_exclusive_load_##ordering##_##size##_z();
#define UML_DECLARE_EXCLUSIVE_LOAD_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, b) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, h) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_LOAD_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_LOAD_SIZES(acquire)
#undef UML_DECLARE_EXCLUSIVE_LOAD_SIZES
#undef UML_DECLARE_EXCLUSIVE_LOAD_VARIANTS
#define UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_exclusive_store_##ordering##_##size##_zz();
#define UML_DECLARE_EXCLUSIVE_STORE_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, b) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, h) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_STORE_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_STORE_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_STORE_SIZES(release)
#undef UML_DECLARE_EXCLUSIVE_STORE_SIZES
#undef UML_DECLARE_EXCLUSIVE_STORE_VARIANTS
#define UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_exclusive_pair_load_##ordering##_##size##_zz();
#define UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES(acquire)
#undef UML_DECLARE_EXCLUSIVE_PAIR_LOAD_SIZES
#undef UML_DECLARE_EXCLUSIVE_PAIR_LOAD_VARIANTS
#define UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS(ordering, size) \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rrz(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_rzz(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zrz(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzr(); \
	void uml_arm64_gadget_exclusive_pair_store_##ordering##_##size##_zzz();
#define UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES(ordering) \
	UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS(ordering, w) \
	UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS(ordering, x)
UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES(relaxed)
UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES(release)
#undef UML_DECLARE_EXCLUSIVE_PAIR_STORE_SIZES
#undef UML_DECLARE_EXCLUSIVE_PAIR_STORE_VARIANTS
#define UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, size) \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rr(); \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_rz(); \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zr(); \
	void uml_arm64_gadget_atomic_##operation##_##ordering##_##size##_zz();
#define UML_DECLARE_ATOMIC_SIZES(operation, ordering) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, b) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, h) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, w) \
	UML_DECLARE_ATOMIC_REGISTER_VARIANTS(operation, ordering, x)
#define UML_DECLARE_ATOMIC_ORDERS(operation) \
	UML_DECLARE_ATOMIC_SIZES(operation, relaxed) \
	UML_DECLARE_ATOMIC_SIZES(operation, release) \
	UML_DECLARE_ATOMIC_SIZES(operation, acquire) \
	UML_DECLARE_ATOMIC_SIZES(operation, acquire_release)
UML_DECLARE_ATOMIC_ORDERS(ldadd)
UML_DECLARE_ATOMIC_ORDERS(ldclr)
UML_DECLARE_ATOMIC_ORDERS(ldeor)
UML_DECLARE_ATOMIC_ORDERS(ldset)
UML_DECLARE_ATOMIC_ORDERS(ldsmax)
UML_DECLARE_ATOMIC_ORDERS(ldsmin)
UML_DECLARE_ATOMIC_ORDERS(ldumax)
UML_DECLARE_ATOMIC_ORDERS(ldumin)
UML_DECLARE_ATOMIC_ORDERS(swp)
#undef UML_DECLARE_ATOMIC_ORDERS
#undef UML_DECLARE_ATOMIC_SIZES
#undef UML_DECLARE_ATOMIC_REGISTER_VARIANTS
#define UML_DECLARE_LITERAL_INTEGER(name) \
	void uml_arm64_gadget_##name##_r(); \
	void uml_arm64_gadget_##name##_z();
UML_DECLARE_LITERAL_INTEGER(literal_load32_fast)
UML_DECLARE_LITERAL_INTEGER(literal_load64_fast)
UML_DECLARE_LITERAL_INTEGER(literal_load32_signed_fast)
UML_DECLARE_LITERAL_INTEGER(literal_load32_far)
UML_DECLARE_LITERAL_INTEGER(literal_load64_far)
UML_DECLARE_LITERAL_INTEGER(literal_load32_signed_far)
#undef UML_DECLARE_LITERAL_INTEGER
void uml_arm64_gadget_literal_load_vector32_fast();
void uml_arm64_gadget_literal_load_vector64_fast();
void uml_arm64_gadget_literal_load_vector128_fast();
void uml_arm64_gadget_literal_load_vector32_far();
void uml_arm64_gadget_literal_load_vector64_far();
void uml_arm64_gadget_literal_load_vector128_far();
void uml_arm64_gadget_svc();
void uml_arm64_gadget_interrupt();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_breakpoint();
void uml_arm64_gadget_branch();
void uml_arm64_gadget_branch_linked();
void uml_arm64_gadget_branch_link();
void uml_arm64_gadget_branch_link_linked();
void uml_arm64_gadget_branch_register();
void uml_arm64_gadget_branch_link_register();
void uml_arm64_gadget_branch_register_cached();
void uml_arm64_gadget_branch_link_register_cached();
void uml_arm64_gadget_return_cached();
void uml_arm64_gadget_return_x30_cached();
void uml_arm64_gadget_return();
/* Everything a condition has: the branch, its linked and inline forms, and
 * the fused CMP and B.cond in both widths with the same three forms. */
#define UML_DECLARE_FUSED_COMPARE_BRANCH(kind, width, name) \
	void uml_arm64_gadget_##kind##_branch_##width##_##name(); \
	void uml_arm64_gadget_##kind##_branch_##width##_##name##_linked(); \
	void uml_arm64_gadget_##kind##_branch_##width##_##name##_inline();
#define UML_DECLARE_CONDITION_BRANCHES(index, name) \
	void uml_arm64_gadget_branch_##name(); \
	void uml_arm64_gadget_branch_##name##_linked(); \
	void uml_arm64_gadget_branch_##name##_inline(); \
	UML_DECLARE_FUSED_COMPARE_BRANCH(compare_register, x, name) \
	UML_DECLARE_FUSED_COMPARE_BRANCH(compare_register, w, name) \
	UML_DECLARE_FUSED_COMPARE_BRANCH(compare_immediate, x, name) \
	UML_DECLARE_FUSED_COMPARE_BRANCH(compare_immediate, w, name)
UML_ARM64_CONDITIONS(UML_DECLARE_CONDITION_BRANCHES)
#undef UML_DECLARE_CONDITION_BRANCHES
#undef UML_DECLARE_FUSED_COMPARE_BRANCH
void uml_arm64_gadget_branch_always();
void uml_arm64_gadget_branch_always_linked();
void uml_arm64_gadget_compare_branch_w_z_inline();
void uml_arm64_gadget_compare_branch_w_nz_inline();
void uml_arm64_gadget_compare_branch_x_z_inline();
void uml_arm64_gadget_compare_branch_x_nz_inline();
void uml_arm64_gadget_test_bit_branch_z_inline();
void uml_arm64_gadget_test_bit_branch_nz_inline();
#define UML_DECLARE_COMPARE_BRANCH(width, polarity) \
	void uml_arm64_gadget_compare_branch_##width##_##polarity(); \
	void uml_arm64_gadget_compare_branch_##width##_##polarity##_linked();
UML_DECLARE_COMPARE_BRANCH(w, z)
UML_DECLARE_COMPARE_BRANCH(w, nz)
UML_DECLARE_COMPARE_BRANCH(x, z)
UML_DECLARE_COMPARE_BRANCH(x, nz)
#undef UML_DECLARE_COMPARE_BRANCH
#define UML_DECLARE_TEST_BIT_BRANCH(polarity) \
	void uml_arm64_gadget_test_bit_branch_##polarity(); \
	void uml_arm64_gadget_test_bit_branch_##polarity##_linked();
UML_DECLARE_TEST_BIT_BRANCH(z)
UML_DECLARE_TEST_BIT_BRANCH(nz)
#undef UML_DECLARE_TEST_BIT_BRANCH
void uml_arm64_gadget_link_exit();
}

} // namespace uml::cpu::arm64::gadgets::selection
