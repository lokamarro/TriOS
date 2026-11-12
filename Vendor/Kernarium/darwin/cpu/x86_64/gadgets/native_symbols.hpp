// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::cpu::x86_64::gadgets::native {

extern "C" {
void uml_x86_64_gadget_block_exit();
void uml_x86_64_gadget_block_exit_linked();
void uml_x86_64_gadget_nop();
void uml_x86_64_gadget_extended_state_fxsave();
void uml_x86_64_gadget_extended_state_fxrstor();
void uml_x86_64_gadget_extended_state_xsave();
void uml_x86_64_gadget_extended_state_xrstor();
void uml_x86_64_gadget_x87_emms();
void uml_x86_64_gadget_x87_nop();
void uml_x86_64_gadget_x87_load_register();
void uml_x86_64_gadget_x87_exchange_register();
void uml_x86_64_gadget_x87_change_sign();
void uml_x86_64_gadget_x87_absolute();
void uml_x86_64_gadget_x87_examine();
void uml_x86_64_gadget_x87_constant_one();
void uml_x86_64_gadget_x87_constant_log2_10();
void uml_x86_64_gadget_x87_constant_log2_e();
void uml_x86_64_gadget_x87_constant_pi();
void uml_x86_64_gadget_x87_constant_log10_2();
void uml_x86_64_gadget_x87_constant_ln_2();
void uml_x86_64_gadget_x87_constant_zero();
void uml_x86_64_gadget_x87_decrement_top();
void uml_x86_64_gadget_x87_increment_top();
void uml_x86_64_gadget_x87_free_register();
void uml_x86_64_gadget_x87_free_register_pop();
void uml_x86_64_gadget_x87_store_register_keep();
void uml_x86_64_gadget_x87_store_register_pop();
void uml_x86_64_gadget_x87_store_status_ax();
void uml_x86_64_gadget_x87_clear_exceptions();
void uml_x86_64_gadget_x87_initialize();
void uml_x86_64_gadget_x87_load_f32();
void uml_x86_64_gadget_x87_load_f64();
void uml_x86_64_gadget_x87_load_i16();
void uml_x86_64_gadget_x87_load_i32();
void uml_x86_64_gadget_x87_load_i64();
void uml_x86_64_gadget_x87_load_f80();
void uml_x86_64_gadget_x87_load_bcd();
void uml_x86_64_gadget_x87_store_f32_keep();
void uml_x86_64_gadget_x87_store_f32_pop();
void uml_x86_64_gadget_x87_store_f64_keep();
void uml_x86_64_gadget_x87_store_f64_pop();
void uml_x86_64_gadget_x87_store_i16_truncate_pop();
void uml_x86_64_gadget_x87_store_i16_keep();
void uml_x86_64_gadget_x87_store_i16_pop();
void uml_x86_64_gadget_x87_store_i32_truncate_pop();
void uml_x86_64_gadget_x87_store_i32_keep();
void uml_x86_64_gadget_x87_store_i32_pop();
void uml_x86_64_gadget_x87_store_i64_truncate_pop();
void uml_x86_64_gadget_x87_store_i64_pop();
void uml_x86_64_gadget_x87_store_f80_pop();
void uml_x86_64_gadget_x87_store_bcd_pop();
void uml_x86_64_gadget_x87_load_control();
void uml_x86_64_gadget_x87_store_control();
void uml_x86_64_gadget_x87_store_status();
void uml_x86_64_gadget_x87_environment_restore_env14();
void uml_x86_64_gadget_x87_environment_restore_env28();
void uml_x86_64_gadget_x87_environment_restore_state94();
void uml_x86_64_gadget_x87_environment_restore_state108();
void uml_x86_64_gadget_x87_environment_save_env14();
void uml_x86_64_gadget_x87_environment_save_env28();
void uml_x86_64_gadget_x87_environment_save_state94();
void uml_x86_64_gadget_x87_environment_save_state108();
#define UML_X86_DECLARE_X87_REGISTER_ARITHMETIC(opcode) \
	void uml_x86_64_gadget_x87_register_##opcode##_0(); \
	void uml_x86_64_gadget_x87_register_##opcode##_1(); \
	void uml_x86_64_gadget_x87_register_##opcode##_4(); \
	void uml_x86_64_gadget_x87_register_##opcode##_5(); \
	void uml_x86_64_gadget_x87_register_##opcode##_6(); \
	void uml_x86_64_gadget_x87_register_##opcode##_7();
UML_X86_DECLARE_X87_REGISTER_ARITHMETIC(d8)
UML_X86_DECLARE_X87_REGISTER_ARITHMETIC(dc)
UML_X86_DECLARE_X87_REGISTER_ARITHMETIC(de)
#undef UML_X86_DECLARE_X87_REGISTER_ARITHMETIC
void uml_x86_64_gadget_x87_compare_status_keep();
void uml_x86_64_gadget_x87_compare_status_pop();
void uml_x86_64_gadget_x87_compare_status_pop2();
void uml_x86_64_gadget_x87_compare_integer_keep();
void uml_x86_64_gadget_x87_compare_integer_pop();
void uml_x86_64_gadget_x87_test_zero();
#define UML_X86_DECLARE_X87_CONDITIONAL_MOVE(condition) \
	void uml_x86_64_gadget_x87_conditional_move_##condition();
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(0)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(1)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(2)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(3)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(4)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(5)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(6)
UML_X86_DECLARE_X87_CONDITIONAL_MOVE(7)
#undef UML_X86_DECLARE_X87_CONDITIONAL_MOVE
#define UML_X86_DECLARE_X87_MEMORY_ARITHMETIC(type) \
	void uml_x86_64_gadget_x87_memory_##type##_add(); \
	void uml_x86_64_gadget_x87_memory_##type##_multiply(); \
	void uml_x86_64_gadget_x87_memory_##type##_subtract(); \
	void uml_x86_64_gadget_x87_memory_##type##_subtract_reverse(); \
	void uml_x86_64_gadget_x87_memory_##type##_divide(); \
	void uml_x86_64_gadget_x87_memory_##type##_divide_reverse(); \
	void uml_x86_64_gadget_x87_memory_##type##_compare_0(); \
	void uml_x86_64_gadget_x87_memory_##type##_compare_1();
UML_X86_DECLARE_X87_MEMORY_ARITHMETIC(f32)
UML_X86_DECLARE_X87_MEMORY_ARITHMETIC(i32)
UML_X86_DECLARE_X87_MEMORY_ARITHMETIC(f64)
UML_X86_DECLARE_X87_MEMORY_ARITHMETIC(i16)
#undef UML_X86_DECLARE_X87_MEMORY_ARITHMETIC
void uml_x86_64_gadget_x87_f2xm1();
void uml_x86_64_gadget_x87_fyl2x();
void uml_x86_64_gadget_x87_fyl2xp1();
void uml_x86_64_gadget_x87_tangent();
void uml_x86_64_gadget_x87_atan();
void uml_x86_64_gadget_x87_extract();
void uml_x86_64_gadget_x87_fprem_with_flags();
void uml_x86_64_gadget_x87_fprem1_with_flags();
void uml_x86_64_gadget_x87_sqrt();
void uml_x86_64_gadget_x87_sincos();
void uml_x86_64_gadget_x87_round();
void uml_x86_64_gadget_x87_scale();
void uml_x86_64_gadget_x87_sine();
void uml_x86_64_gadget_x87_cosine();

#define UML_X86_DECLARE_STRING_WIDTH(operation, width) \
	void uml_x86_64_gadget_string_##operation##_##width(); \
	void uml_x86_64_gadget_string_##operation##_##width##_step();
#define UML_X86_DECLARE_STRING(operation) \
	UML_X86_DECLARE_STRING_WIDTH(operation, 8) \
	UML_X86_DECLARE_STRING_WIDTH(operation, 16) \
	UML_X86_DECLARE_STRING_WIDTH(operation, 32) \
	UML_X86_DECLARE_STRING_WIDTH(operation, 64)
UML_X86_DECLARE_STRING(move)
UML_X86_DECLARE_STRING(compare)
UML_X86_DECLARE_STRING(store)
UML_X86_DECLARE_STRING(load)
UML_X86_DECLARE_STRING(scan)
#undef UML_X86_DECLARE_STRING
#undef UML_X86_DECLARE_STRING_WIDTH

#define UML_X86_DECLARE_WIDTH_FORMS(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_ri();
#define UML_X86_DECLARE_WIDTHS(operation) \
	UML_X86_DECLARE_WIDTH_FORMS(operation, 8) \
	UML_X86_DECLARE_WIDTH_FORMS(operation, 16) \
	UML_X86_DECLARE_WIDTH_FORMS(operation, 32) \
	UML_X86_DECLARE_WIDTH_FORMS(operation, 64)

UML_X86_DECLARE_WIDTHS(move)
UML_X86_DECLARE_WIDTHS(add)
UML_X86_DECLARE_WIDTHS(sub)
UML_X86_DECLARE_WIDTHS(and)
UML_X86_DECLARE_WIDTHS(or)
UML_X86_DECLARE_WIDTHS(xor)
UML_X86_DECLARE_WIDTHS(cmp)
UML_X86_DECLARE_WIDTHS(test)
UML_X86_DECLARE_WIDTHS(adc)
UML_X86_DECLARE_WIDTHS(sbb)

#define UML_X86_DECLARE_SHIFT_FORM(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_ri(); \
	void uml_x86_64_gadget_##operation##_##width##_mr(); \
	void uml_x86_64_gadget_##operation##_##width##_mi();
#define UML_X86_DECLARE_SHIFT(operation) \
	UML_X86_DECLARE_SHIFT_FORM(operation, 8) \
	UML_X86_DECLARE_SHIFT_FORM(operation, 16) \
	UML_X86_DECLARE_SHIFT_FORM(operation, 32) \
	UML_X86_DECLARE_SHIFT_FORM(operation, 64)
UML_X86_DECLARE_SHIFT(shl)
UML_X86_DECLARE_SHIFT(shr)
UML_X86_DECLARE_SHIFT(sar)
UML_X86_DECLARE_SHIFT(rol)
UML_X86_DECLARE_SHIFT(ror)
UML_X86_DECLARE_SHIFT(rcl)
UML_X86_DECLARE_SHIFT(rcr)
#undef UML_X86_DECLARE_SHIFT
#undef UML_X86_DECLARE_SHIFT_FORM

#define UML_X86_DECLARE_DOUBLE_SHIFT_FORM(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rri(); \
	void uml_x86_64_gadget_##operation##_##width##_rrr(); \
	void uml_x86_64_gadget_##operation##_##width##_mri(); \
	void uml_x86_64_gadget_##operation##_##width##_mrr();
#define UML_X86_DECLARE_DOUBLE_SHIFT(operation) \
	UML_X86_DECLARE_DOUBLE_SHIFT_FORM(operation, 16) \
	UML_X86_DECLARE_DOUBLE_SHIFT_FORM(operation, 32) \
	UML_X86_DECLARE_DOUBLE_SHIFT_FORM(operation, 64)
UML_X86_DECLARE_DOUBLE_SHIFT(shld)
UML_X86_DECLARE_DOUBLE_SHIFT(shrd)
#undef UML_X86_DECLARE_DOUBLE_SHIFT
#undef UML_X86_DECLARE_DOUBLE_SHIFT_FORM

#define UML_X86_DECLARE_IMPLICIT_MULTIPLY(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_r(); \
	void uml_x86_64_gadget_##operation##_##width##_m();
UML_X86_DECLARE_IMPLICIT_MULTIPLY(mul, 8)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(mul, 16)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(mul, 32)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(mul, 64)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(imul, 8)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(imul, 16)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(imul, 32)
UML_X86_DECLARE_IMPLICIT_MULTIPLY(imul, 64)
#undef UML_X86_DECLARE_IMPLICIT_MULTIPLY

#define UML_X86_DECLARE_EXPLICIT_IMUL(width) \
	void uml_x86_64_gadget_imul_##width##_rr(); \
	void uml_x86_64_gadget_imul_##width##_rri(); \
	void uml_x86_64_gadget_imul_##width##_rmr(); \
	void uml_x86_64_gadget_imul_##width##_rmi();
UML_X86_DECLARE_EXPLICIT_IMUL(16)
UML_X86_DECLARE_EXPLICIT_IMUL(32)
UML_X86_DECLARE_EXPLICIT_IMUL(64)
#undef UML_X86_DECLARE_EXPLICIT_IMUL

#define UML_X86_DECLARE_DIVIDE(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_r(); \
	void uml_x86_64_gadget_##operation##_##width##_m();
UML_X86_DECLARE_DIVIDE(div, 8)
UML_X86_DECLARE_DIVIDE(div, 16)
UML_X86_DECLARE_DIVIDE(div, 32)
UML_X86_DECLARE_DIVIDE(div, 64)
UML_X86_DECLARE_DIVIDE(idiv, 8)
UML_X86_DECLARE_DIVIDE(idiv, 16)
UML_X86_DECLARE_DIVIDE(idiv, 32)
UML_X86_DECLARE_DIVIDE(idiv, 64)
#undef UML_X86_DECLARE_DIVIDE

#define UML_X86_DECLARE_BIT_OPERATION_FORM(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_ri(); \
	void uml_x86_64_gadget_##operation##_##width##_mr(); \
	void uml_x86_64_gadget_##operation##_##width##_mi();
#define UML_X86_DECLARE_BIT_OPERATION(operation) \
	UML_X86_DECLARE_BIT_OPERATION_FORM(operation, 16) \
	UML_X86_DECLARE_BIT_OPERATION_FORM(operation, 32) \
	UML_X86_DECLARE_BIT_OPERATION_FORM(operation, 64)
UML_X86_DECLARE_BIT_OPERATION(bt)
UML_X86_DECLARE_BIT_OPERATION(bts)
UML_X86_DECLARE_BIT_OPERATION(btr)
UML_X86_DECLARE_BIT_OPERATION(btc)
#undef UML_X86_DECLARE_BIT_OPERATION
#undef UML_X86_DECLARE_BIT_OPERATION_FORM

#define UML_X86_DECLARE_BIT_COUNT_FORM(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_rm();
#define UML_X86_DECLARE_BIT_COUNT(operation) \
	UML_X86_DECLARE_BIT_COUNT_FORM(operation, 16) \
	UML_X86_DECLARE_BIT_COUNT_FORM(operation, 32) \
	UML_X86_DECLARE_BIT_COUNT_FORM(operation, 64)
UML_X86_DECLARE_BIT_COUNT(bsf)
UML_X86_DECLARE_BIT_COUNT(bsr)
UML_X86_DECLARE_BIT_COUNT(tzcnt)
UML_X86_DECLARE_BIT_COUNT(lzcnt)
UML_X86_DECLARE_BIT_COUNT(popcnt)
#undef UML_X86_DECLARE_BIT_COUNT
#undef UML_X86_DECLARE_BIT_COUNT_FORM

#define UML_X86_DECLARE_ATOMIC_BINARY(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_mr();
UML_X86_DECLARE_ATOMIC_BINARY(cmpxchg, 8)
UML_X86_DECLARE_ATOMIC_BINARY(cmpxchg, 16)
UML_X86_DECLARE_ATOMIC_BINARY(cmpxchg, 32)
UML_X86_DECLARE_ATOMIC_BINARY(cmpxchg, 64)
UML_X86_DECLARE_ATOMIC_BINARY(xadd, 8)
UML_X86_DECLARE_ATOMIC_BINARY(xadd, 16)
UML_X86_DECLARE_ATOMIC_BINARY(xadd, 32)
UML_X86_DECLARE_ATOMIC_BINARY(xadd, 64)
#undef UML_X86_DECLARE_ATOMIC_BINARY
void uml_x86_64_gadget_cmpxchg_pair_8();
void uml_x86_64_gadget_cmpxchg_pair_16();

#define UML_X86_DECLARE_MOVBE(width) \
	void uml_x86_64_gadget_movbe_##width##_rm(); \
	void uml_x86_64_gadget_movbe_##width##_mr();
UML_X86_DECLARE_MOVBE(16)
UML_X86_DECLARE_MOVBE(32)
UML_X86_DECLARE_MOVBE(64)
#undef UML_X86_DECLARE_MOVBE

#define UML_X86_DECLARE_CRC32(width) \
	void uml_x86_64_gadget_crc32_##width##_rr(); \
	void uml_x86_64_gadget_crc32_##width##_rm();
UML_X86_DECLARE_CRC32(8)
UML_X86_DECLARE_CRC32(16)
UML_X86_DECLARE_CRC32(32)
UML_X86_DECLARE_CRC32(64)
#undef UML_X86_DECLARE_CRC32

#define UML_X86_DECLARE_ADX(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_rm();
UML_X86_DECLARE_ADX(adcx, 32)
UML_X86_DECLARE_ADX(adcx, 64)
UML_X86_DECLARE_ADX(adox, 32)
UML_X86_DECLARE_ADX(adox, 64)
#undef UML_X86_DECLARE_ADX

#define UML_X86_DECLARE_BMI_UNARY(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rr(); \
	void uml_x86_64_gadget_##operation##_##width##_rm();
UML_X86_DECLARE_BMI_UNARY(blsr, 32)
UML_X86_DECLARE_BMI_UNARY(blsr, 64)
UML_X86_DECLARE_BMI_UNARY(blsmsk, 32)
UML_X86_DECLARE_BMI_UNARY(blsmsk, 64)
UML_X86_DECLARE_BMI_UNARY(blsi, 32)
UML_X86_DECLARE_BMI_UNARY(blsi, 64)
#undef UML_X86_DECLARE_BMI_UNARY

#define UML_X86_DECLARE_BMI_TERNARY(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rrr(); \
	void uml_x86_64_gadget_##operation##_##width##_rrm();
#define UML_X86_DECLARE_BMI_TERNARY_WIDTHS(operation) \
	UML_X86_DECLARE_BMI_TERNARY(operation, 32) \
	UML_X86_DECLARE_BMI_TERNARY(operation, 64)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(andn)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(bzhi)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(bextr)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(shlx)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(sarx)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(shrx)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(pdep)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(pext)
UML_X86_DECLARE_BMI_TERNARY_WIDTHS(mulx)
#undef UML_X86_DECLARE_BMI_TERNARY_WIDTHS
#undef UML_X86_DECLARE_BMI_TERNARY

#define UML_X86_DECLARE_RORX(width) \
	void uml_x86_64_gadget_rorx_##width##_rri(); \
	void uml_x86_64_gadget_rorx_##width##_rmi();
UML_X86_DECLARE_RORX(32)
UML_X86_DECLARE_RORX(64)
#undef UML_X86_DECLARE_RORX

#define UML_X86_DECLARE_EXCHANGE(width) \
	void uml_x86_64_gadget_exchange_##width##_rr();
UML_X86_DECLARE_EXCHANGE(8)
UML_X86_DECLARE_EXCHANGE(16)
UML_X86_DECLARE_EXCHANGE(32)
UML_X86_DECLARE_EXCHANGE(64)
#undef UML_X86_DECLARE_EXCHANGE

#define UML_X86_DECLARE_MEMORY_ARITHMETIC_FORM(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_rm(); \
	void uml_x86_64_gadget_##operation##_##width##_mr(); \
	void uml_x86_64_gadget_##operation##_##width##_mi();
#define UML_X86_DECLARE_MEMORY_ARITHMETIC(operation) \
	UML_X86_DECLARE_MEMORY_ARITHMETIC_FORM(operation, 8) \
	UML_X86_DECLARE_MEMORY_ARITHMETIC_FORM(operation, 16) \
	UML_X86_DECLARE_MEMORY_ARITHMETIC_FORM(operation, 32) \
	UML_X86_DECLARE_MEMORY_ARITHMETIC_FORM(operation, 64)
UML_X86_DECLARE_MEMORY_ARITHMETIC(add)
UML_X86_DECLARE_MEMORY_ARITHMETIC(sub)
UML_X86_DECLARE_MEMORY_ARITHMETIC(and)
UML_X86_DECLARE_MEMORY_ARITHMETIC(or)
UML_X86_DECLARE_MEMORY_ARITHMETIC(xor)
UML_X86_DECLARE_MEMORY_ARITHMETIC(cmp)
UML_X86_DECLARE_MEMORY_ARITHMETIC(test)
UML_X86_DECLARE_MEMORY_ARITHMETIC(adc)
UML_X86_DECLARE_MEMORY_ARITHMETIC(sbb)
#undef UML_X86_DECLARE_MEMORY_ARITHMETIC
#undef UML_X86_DECLARE_MEMORY_ARITHMETIC_FORM

#define UML_X86_DECLARE_MEMORY_EXCHANGE(width) \
	void uml_x86_64_gadget_exchange_##width##_mr();
UML_X86_DECLARE_MEMORY_EXCHANGE(8)
UML_X86_DECLARE_MEMORY_EXCHANGE(16)
UML_X86_DECLARE_MEMORY_EXCHANGE(32)
UML_X86_DECLARE_MEMORY_EXCHANGE(64)
#undef UML_X86_DECLARE_MEMORY_EXCHANGE

#define UML_X86_DECLARE_MEMORY_MOVE(width) \
	void uml_x86_64_gadget_move_##width##_rm(); \
	void uml_x86_64_gadget_move_##width##_mr(); \
	void uml_x86_64_gadget_move_##width##_mi();
UML_X86_DECLARE_MEMORY_MOVE(8)
UML_X86_DECLARE_MEMORY_MOVE(16)
UML_X86_DECLARE_MEMORY_MOVE(32)
UML_X86_DECLARE_MEMORY_MOVE(64)
#undef UML_X86_DECLARE_MEMORY_MOVE

#define UML_X86_DECLARE_UNARY(operation) \
	void uml_x86_64_gadget_##operation##_8_r(); \
	void uml_x86_64_gadget_##operation##_16_r(); \
	void uml_x86_64_gadget_##operation##_32_r(); \
	void uml_x86_64_gadget_##operation##_64_r(); \
	void uml_x86_64_gadget_##operation##_8_m(); \
	void uml_x86_64_gadget_##operation##_16_m(); \
	void uml_x86_64_gadget_##operation##_32_m(); \
	void uml_x86_64_gadget_##operation##_64_m();
UML_X86_DECLARE_UNARY(increment)
UML_X86_DECLARE_UNARY(decrement)
UML_X86_DECLARE_UNARY(negate)
UML_X86_DECLARE_UNARY(not)
#undef UML_X86_DECLARE_UNARY

void uml_x86_64_gadget_zext_8_16_rr();
void uml_x86_64_gadget_zext_8_32_rr();
void uml_x86_64_gadget_zext_8_64_rr();
void uml_x86_64_gadget_zext_16_32_rr();
void uml_x86_64_gadget_zext_16_64_rr();
void uml_x86_64_gadget_zext_32_64_rr();
void uml_x86_64_gadget_sext_8_16_rr();
void uml_x86_64_gadget_sext_8_32_rr();
void uml_x86_64_gadget_sext_8_64_rr();
void uml_x86_64_gadget_sext_16_32_rr();
void uml_x86_64_gadget_sext_16_64_rr();
void uml_x86_64_gadget_sext_32_16_rr();
void uml_x86_64_gadget_sext_32_32_rr();
void uml_x86_64_gadget_sext_32_64_rr();
void uml_x86_64_gadget_zext_8_16_rm();
void uml_x86_64_gadget_zext_8_32_rm();
void uml_x86_64_gadget_zext_8_64_rm();
void uml_x86_64_gadget_zext_16_32_rm();
void uml_x86_64_gadget_zext_16_64_rm();
void uml_x86_64_gadget_zext_32_64_rm();
void uml_x86_64_gadget_sext_8_16_rm();
void uml_x86_64_gadget_sext_8_32_rm();
void uml_x86_64_gadget_sext_8_64_rm();
void uml_x86_64_gadget_sext_16_32_rm();
void uml_x86_64_gadget_sext_16_64_rm();
void uml_x86_64_gadget_sext_32_16_rm();
void uml_x86_64_gadget_sext_32_32_rm();
void uml_x86_64_gadget_sext_32_64_rm();
void uml_x86_64_gadget_byte_swap_32();
void uml_x86_64_gadget_byte_swap_64();
void uml_x86_64_gadget_sign_extend_accumulator_16();
void uml_x86_64_gadget_sign_extend_accumulator_32();
void uml_x86_64_gadget_sign_extend_accumulator_64();
void uml_x86_64_gadget_sign_extend_data_16();
void uml_x86_64_gadget_sign_extend_data_32();
void uml_x86_64_gadget_sign_extend_data_64();
void uml_x86_64_gadget_carry_clear();
void uml_x86_64_gadget_carry_set();
void uml_x86_64_gadget_carry_complement();
void uml_x86_64_gadget_direction_clear();
void uml_x86_64_gadget_direction_set();
void uml_x86_64_gadget_lahf();
void uml_x86_64_gadget_sahf();
void uml_x86_64_gadget_daa();
void uml_x86_64_gadget_das();
void uml_x86_64_gadget_aaa();
void uml_x86_64_gadget_aas();
void uml_x86_64_gadget_aam();
void uml_x86_64_gadget_aad();
void uml_x86_64_gadget_salc();
void uml_x86_64_gadget_into();
void uml_x86_64_gadget_int4();
void uml_x86_64_gadget_arpl_16_rr();
void uml_x86_64_gadget_arpl_16_mr();
void uml_x86_64_gadget_bound_16();
void uml_x86_64_gadget_bound_32();
void uml_x86_64_gadget_jump();
void uml_x86_64_gadget_jump_linked();
void uml_x86_64_gadget_link_exit();
#define UML_X86_DECLARE_LOOP(opcode) \
	void uml_x86_64_gadget_loop_##opcode##_16(); \
	void uml_x86_64_gadget_loop_##opcode##_32(); \
	void uml_x86_64_gadget_loop_##opcode##_64();
UML_X86_DECLARE_LOOP(e0)
UML_X86_DECLARE_LOOP(e1)
UML_X86_DECLARE_LOOP(e2)
UML_X86_DECLARE_LOOP(e3)
#undef UML_X86_DECLARE_LOOP
void uml_x86_64_gadget_lea_16();
void uml_x86_64_gadget_lea_32();
void uml_x86_64_gadget_lea_64();
void uml_x86_64_gadget_push_16_r();
void uml_x86_64_gadget_push_16_i();
void uml_x86_64_gadget_push_16_m();
void uml_x86_64_gadget_push_32_r();
void uml_x86_64_gadget_push_32_i();
void uml_x86_64_gadget_push_32_m();
void uml_x86_64_gadget_push_64_r();
void uml_x86_64_gadget_push_64_i();
void uml_x86_64_gadget_push_64_m();
void uml_x86_64_gadget_pop_16_r();
void uml_x86_64_gadget_pop_16_m();
void uml_x86_64_gadget_pop_32_r();
void uml_x86_64_gadget_pop_32_m();
void uml_x86_64_gadget_pop_64_r();
void uml_x86_64_gadget_pop_64_m();
void uml_x86_64_gadget_push_all_16();
void uml_x86_64_gadget_push_all_32();
void uml_x86_64_gadget_pop_all_16();
void uml_x86_64_gadget_pop_all_32();
void uml_x86_64_gadget_push_flags_16();
void uml_x86_64_gadget_push_flags_32();
void uml_x86_64_gadget_push_flags_64();
void uml_x86_64_gadget_pop_flags_16();
void uml_x86_64_gadget_pop_flags_32();
void uml_x86_64_gadget_pop_flags_64();
void uml_x86_64_gadget_leave_16();
void uml_x86_64_gadget_leave_32();
void uml_x86_64_gadget_leave_64();
void uml_x86_64_gadget_enter_16();
void uml_x86_64_gadget_enter_32();
void uml_x86_64_gadget_enter_64();
void uml_x86_64_gadget_push_segment_32();
void uml_x86_64_gadget_push_segment_64();
void uml_x86_64_gadget_call();
void uml_x86_64_gadget_call_linked();
void uml_x86_64_gadget_call_32();
void uml_x86_64_gadget_call_32_linked();
void uml_x86_64_gadget_call_positioned();
void uml_x86_64_gadget_call_positioned_linked();
void uml_x86_64_gadget_call_32_positioned();
void uml_x86_64_gadget_call_32_positioned_linked();
void uml_x86_64_gadget_call_indirect_r();
void uml_x86_64_gadget_call_indirect_m();
void uml_x86_64_gadget_call_indirect_r_cached();
void uml_x86_64_gadget_call_indirect_m_cached();
void uml_x86_64_gadget_call_indirect_32_r();
void uml_x86_64_gadget_call_indirect_32_m();
void uml_x86_64_gadget_call_indirect_32_r_cached();
void uml_x86_64_gadget_call_indirect_32_m_cached();
void uml_x86_64_gadget_return();
void uml_x86_64_gadget_return_32();
void uml_x86_64_gadget_return_cached();
void uml_x86_64_gadget_return_32_cached();
void uml_x86_64_gadget_jump_indirect_r();
void uml_x86_64_gadget_jump_indirect_m();
void uml_x86_64_gadget_jump_indirect_r_cached();
void uml_x86_64_gadget_jump_indirect_m_cached();
void uml_x86_64_gadget_jump_indirect_32_r();
void uml_x86_64_gadget_jump_indirect_32_m();
void uml_x86_64_gadget_jump_indirect_32_r_cached();
void uml_x86_64_gadget_jump_indirect_32_m_cached();
void uml_x86_64_gadget_segment_instruction();
void uml_x86_64_gadget_segment_guard();
void uml_x86_64_gadget_interrupt_return_16();
void uml_x86_64_gadget_interrupt_return_32();
void uml_x86_64_gadget_interrupt_return_64();
void uml_x86_64_gadget_interrupt_return_long_16();
void uml_x86_64_gadget_interrupt_return_long_32();
void uml_x86_64_gadget_syscall();
void uml_x86_64_gadget_int80();
void uml_x86_64_gadget_breakpoint();
void uml_x86_64_gadget_icebp();
void uml_x86_64_gadget_privileged();
void uml_x86_64_gadget_cpuid();
void uml_x86_64_gadget_xgetbv();
void uml_x86_64_gadget_rdtsc();
void uml_x86_64_gadget_rdtscp();
void uml_x86_64_gadget_rdpid_32_r();
void uml_x86_64_gadget_rdpid_64_r();
void uml_x86_64_gadget_xlat();
void uml_x86_64_gadget_random_16();
void uml_x86_64_gadget_random_32();
void uml_x86_64_gadget_random_64();
void uml_x86_64_gadget_read_fs_base_32_r();
void uml_x86_64_gadget_read_fs_base_64_r();
void uml_x86_64_gadget_read_gs_base_32_r();
void uml_x86_64_gadget_read_gs_base_64_r();
void uml_x86_64_gadget_write_fs_base_32_r();
void uml_x86_64_gadget_write_fs_base_64_r();
void uml_x86_64_gadget_write_gs_base_32_r();
void uml_x86_64_gadget_write_gs_base_64_r();
void uml_x86_64_gadget_memory_fence();
void uml_x86_64_gadget_cache_flush();
void uml_x86_64_gadget_store_machine_status_16_r();
void uml_x86_64_gadget_store_machine_status_32_r();
void uml_x86_64_gadget_store_machine_status_64_r();
void uml_x86_64_gadget_store_machine_status_16_m();
void uml_x86_64_gadget_store_gdtr();
void uml_x86_64_gadget_store_gdtr_32();
void uml_x86_64_gadget_store_idtr();
void uml_x86_64_gadget_store_idtr_32();
void uml_x86_64_gadget_load_segment_limit_r();
void uml_x86_64_gadget_load_segment_limit_m();
void uml_x86_64_gadget_mxcsr_load();
void uml_x86_64_gadget_mxcsr_store();
void uml_x86_64_gadget_vector_move_128_rr();
void uml_x86_64_gadget_vector_move_128_rr_vex();
void uml_x86_64_gadget_vector_move_64_128_rr();
void uml_x86_64_gadget_vector_move_64_128_rr_vex();
void uml_x86_64_gadget_vector_move_64_128_rm();
void uml_x86_64_gadget_vector_move_64_128_rm_vex();
void uml_x86_64_gadget_vector_move_32_mr();
void uml_x86_64_gadget_vector_move_64_mr();
void uml_x86_64_gadget_vector_move_64_mmx_rr();
void uml_x86_64_gadget_vector_move_64_mmx_rm();
void uml_x86_64_gadget_vector_gpr_to_xmm_32();
void uml_x86_64_gadget_vector_gpr_to_xmm_64();
void uml_x86_64_gadget_vector_gpr_to_xmm_32_vex();
void uml_x86_64_gadget_vector_gpr_to_xmm_64_vex();
void uml_x86_64_gadget_vector_gpr_to_mmx_32();
void uml_x86_64_gadget_vector_gpr_to_mmx_64();
void uml_x86_64_gadget_vector_memory_32_to_mmx();
void uml_x86_64_gadget_vector_scalar_move_32_rr();
void uml_x86_64_gadget_vector_scalar_move_64_rr();
void uml_x86_64_gadget_vector_scalar_move_32_rr_vex();
void uml_x86_64_gadget_vector_scalar_move_64_rr_vex();
void uml_x86_64_gadget_vector_scalar_move_32_rm();
void uml_x86_64_gadget_vector_scalar_move_64_rm();
void uml_x86_64_gadget_vector_scalar_move_32_rm_vex();
void uml_x86_64_gadget_vector_scalar_move_64_rm_vex();
void uml_x86_64_gadget_vector_movmsk_32_128();
void uml_x86_64_gadget_vector_movmsk_64_128();
void uml_x86_64_gadget_vector_movmsk_32_256();
void uml_x86_64_gadget_vector_movmsk_64_256();
void uml_x86_64_gadget_vector_pmovmskb_64();
void uml_x86_64_gadget_vector_pmovmskb_128();
void uml_x86_64_gadget_vector_pmovmskb_256();
void uml_x86_64_gadget_vector_pextrw_64();
void uml_x86_64_gadget_vector_pextrw_128();
void uml_x86_64_gadget_vector_pextrw_256();
void uml_x86_64_gadget_vector_insert_64_rr();
void uml_x86_64_gadget_vector_insert_128_rr();
void uml_x86_64_gadget_vector_insert_128_rr_vex();
void uml_x86_64_gadget_vector_insert_64_rm();
void uml_x86_64_gadget_vector_insert_128_rm();
void uml_x86_64_gadget_vector_insert_128_rm_vex();
#define UML_X86_DECLARE_VECTOR_TEST(width, stride) \
	void uml_x86_64_gadget_vector_test_##width##_##stride##_rr(); \
	void uml_x86_64_gadget_vector_test_##width##_##stride##_rm();
UML_X86_DECLARE_VECTOR_TEST(128, 1)
UML_X86_DECLARE_VECTOR_TEST(256, 1)
UML_X86_DECLARE_VECTOR_TEST(128, 4)
UML_X86_DECLARE_VECTOR_TEST(256, 4)
UML_X86_DECLARE_VECTOR_TEST(128, 8)
UML_X86_DECLARE_VECTOR_TEST(256, 8)
#undef UML_X86_DECLARE_VECTOR_TEST
void uml_x86_64_gadget_vector_compare_32_rr();
void uml_x86_64_gadget_vector_compare_64_rr();
void uml_x86_64_gadget_vector_compare_32_rm();
void uml_x86_64_gadget_vector_compare_64_rm();
void uml_x86_64_gadget_vector_compare_signalling_32_rr();
void uml_x86_64_gadget_vector_compare_signalling_64_rr();
void uml_x86_64_gadget_vector_compare_signalling_32_rm();
void uml_x86_64_gadget_vector_compare_signalling_64_rm();
void uml_x86_64_gadget_vector_move_256_rr();
void uml_x86_64_gadget_vector_move_128_rm();
void uml_x86_64_gadget_vector_move_128_rm_vex();
void uml_x86_64_gadget_vector_move_256_rm();
void uml_x86_64_gadget_vector_move_128_mr();
void uml_x86_64_gadget_vector_move_256_mr();
void uml_x86_64_gadget_vector_zero_upper();
void uml_x86_64_gadget_vector_zero_all();
void uml_x86_64_gadget_vector_mask_store_64();
void uml_x86_64_gadget_vector_mask_store_128();
void uml_x86_64_gadget_vector_lane_merge_rr();
void uml_x86_64_gadget_vector_lane_merge_rm();
#define UML_X86_DECLARE_VECTOR_DUPLICATE(kind, width) \
	void uml_x86_64_gadget_vector_duplicate_##kind##_##width##_rr(); \
	void uml_x86_64_gadget_vector_duplicate_##kind##_##width##_rm();
UML_X86_DECLARE_VECTOR_DUPLICATE(sldup, 128)
UML_X86_DECLARE_VECTOR_DUPLICATE(sldup, 256)
UML_X86_DECLARE_VECTOR_DUPLICATE(shdup, 128)
UML_X86_DECLARE_VECTOR_DUPLICATE(shdup, 256)
UML_X86_DECLARE_VECTOR_DUPLICATE(ddup, 128)
UML_X86_DECLARE_VECTOR_DUPLICATE(ddup, 256)
#undef UML_X86_DECLARE_VECTOR_DUPLICATE

#define UML_X86_DECLARE_VECTOR_SHUFFLE(operation, element, width) \
	void uml_x86_64_gadget_vector_##operation##_##element##_##width##_rr(); \
	void uml_x86_64_gadget_vector_##operation##_##element##_##width##_rm();
#define UML_X86_DECLARE_VECTOR_SHUFFLE_WIDTHS(operation, element) \
	UML_X86_DECLARE_VECTOR_SHUFFLE(operation, element, 128) \
	UML_X86_DECLARE_VECTOR_SHUFFLE(operation, element, 256)
#define UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(operation) \
	UML_X86_DECLARE_VECTOR_SHUFFLE_WIDTHS(operation, 32) \
	UML_X86_DECLARE_VECTOR_SHUFFLE_WIDTHS(operation, 64)
UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(unpack_low)
UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(unpack_high)
UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(shuffle)
UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(hadd)
UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(hsub)
UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY(addsub)
#undef UML_X86_DECLARE_VECTOR_SHUFFLE_FAMILY
#undef UML_X86_DECLARE_VECTOR_SHUFFLE_WIDTHS
#undef UML_X86_DECLARE_VECTOR_SHUFFLE

#define UML_X86_DECLARE_VECTOR_FP_SHAPE(operation, element, shape) \
	void uml_x86_64_gadget_vector_fp_##operation##_##element##_##shape##_rr(); \
	void uml_x86_64_gadget_vector_fp_##operation##_##element##_##shape##_rm();
#define UML_X86_DECLARE_VECTOR_FP_ELEMENT(operation, element) \
	UML_X86_DECLARE_VECTOR_FP_SHAPE(operation, element, 128) \
	UML_X86_DECLARE_VECTOR_FP_SHAPE(operation, element, 256) \
	UML_X86_DECLARE_VECTOR_FP_SHAPE(operation, element, scalar)
#define UML_X86_DECLARE_VECTOR_FP_OPERATION(operation) \
	UML_X86_DECLARE_VECTOR_FP_ELEMENT(operation, 32) \
	UML_X86_DECLARE_VECTOR_FP_ELEMENT(operation, 64)
UML_X86_DECLARE_VECTOR_FP_OPERATION(sqrt)
UML_X86_DECLARE_VECTOR_FP_ELEMENT(rsqrt, 32)
UML_X86_DECLARE_VECTOR_FP_ELEMENT(rcp, 32)
UML_X86_DECLARE_VECTOR_FP_OPERATION(add)
UML_X86_DECLARE_VECTOR_FP_OPERATION(mul)
UML_X86_DECLARE_VECTOR_FP_OPERATION(sub)
UML_X86_DECLARE_VECTOR_FP_OPERATION(min)
UML_X86_DECLARE_VECTOR_FP_OPERATION(div)
UML_X86_DECLARE_VECTOR_FP_OPERATION(max)
#define UML_X86_DECLARE_VECTOR_FP_PREDICATE(predicate) \
	UML_X86_DECLARE_VECTOR_FP_OPERATION(cmp_##predicate)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(0)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(1)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(2)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(3)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(4)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(5)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(6)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(7)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(8)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(9)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(10)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(11)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(12)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(13)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(14)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(15)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(16)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(17)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(18)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(19)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(20)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(21)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(22)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(23)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(24)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(25)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(26)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(27)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(28)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(29)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(30)
UML_X86_DECLARE_VECTOR_FP_PREDICATE(31)
#undef UML_X86_DECLARE_VECTOR_FP_PREDICATE
#undef UML_X86_DECLARE_VECTOR_FP_OPERATION
#undef UML_X86_DECLARE_VECTOR_FP_ELEMENT
#undef UML_X86_DECLARE_VECTOR_FP_SHAPE

#define UML_X86_DECLARE_VECTOR_CONVERT(name) \
	void uml_x86_64_gadget_vector_convert_##name##_rr(); \
	void uml_x86_64_gadget_vector_convert_##name##_rm();
UML_X86_DECLARE_VECTOR_CONVERT(ps_pd_128)
UML_X86_DECLARE_VECTOR_CONVERT(ps_pd_256)
UML_X86_DECLARE_VECTOR_CONVERT(pd_ps_128)
UML_X86_DECLARE_VECTOR_CONVERT(pd_ps_256)
UML_X86_DECLARE_VECTOR_CONVERT(ss_sd)
UML_X86_DECLARE_VECTOR_CONVERT(sd_ss)
UML_X86_DECLARE_VECTOR_CONVERT(dq_ps_128)
UML_X86_DECLARE_VECTOR_CONVERT(dq_ps_256)
UML_X86_DECLARE_VECTOR_CONVERT(ps_dq_128)
UML_X86_DECLARE_VECTOR_CONVERT(ps_dq_256)
UML_X86_DECLARE_VECTOR_CONVERT(ttps_dq_128)
UML_X86_DECLARE_VECTOR_CONVERT(ttps_dq_256)
UML_X86_DECLARE_VECTOR_CONVERT(dq_pd_128)
UML_X86_DECLARE_VECTOR_CONVERT(dq_pd_256)
UML_X86_DECLARE_VECTOR_CONVERT(pd_dq_128)
UML_X86_DECLARE_VECTOR_CONVERT(pd_dq_256)
UML_X86_DECLARE_VECTOR_CONVERT(ttpd_dq_128)
UML_X86_DECLARE_VECTOR_CONVERT(ttpd_dq_256)
#undef UML_X86_DECLARE_VECTOR_CONVERT

#define UML_X86_DECLARE_INTEGER_SCALAR(integer, element) \
	void uml_x86_64_gadget_vector_scalar_i##integer##_f##element##_rr(); \
	void uml_x86_64_gadget_vector_scalar_i##integer##_f##element##_rm();
UML_X86_DECLARE_INTEGER_SCALAR(32, 32)
UML_X86_DECLARE_INTEGER_SCALAR(64, 32)
UML_X86_DECLARE_INTEGER_SCALAR(32, 64)
UML_X86_DECLARE_INTEGER_SCALAR(64, 64)
#undef UML_X86_DECLARE_INTEGER_SCALAR
void uml_x86_64_gadget_vector_pi_f32_rr();
void uml_x86_64_gadget_vector_pi_f32_rm();
void uml_x86_64_gadget_vector_pi_f64_rr();
void uml_x86_64_gadget_vector_pi_f64_rm();
#define UML_X86_DECLARE_SCALAR_INTEGER(element, integer, truncate) \
	void uml_x86_64_gadget_vector_scalar_f##element##_i##integer##_##truncate##_rr(); \
	void uml_x86_64_gadget_vector_scalar_f##element##_i##integer##_##truncate##_rm();
UML_X86_DECLARE_SCALAR_INTEGER(32, 32, 0)
UML_X86_DECLARE_SCALAR_INTEGER(32, 32, 1)
UML_X86_DECLARE_SCALAR_INTEGER(32, 64, 0)
UML_X86_DECLARE_SCALAR_INTEGER(32, 64, 1)
UML_X86_DECLARE_SCALAR_INTEGER(64, 32, 0)
UML_X86_DECLARE_SCALAR_INTEGER(64, 32, 1)
UML_X86_DECLARE_SCALAR_INTEGER(64, 64, 0)
UML_X86_DECLARE_SCALAR_INTEGER(64, 64, 1)
#undef UML_X86_DECLARE_SCALAR_INTEGER
#define UML_X86_DECLARE_FP_PI(element, truncate) \
	void uml_x86_64_gadget_vector_f##element##_pi_##truncate##_rr(); \
	void uml_x86_64_gadget_vector_f##element##_pi_##truncate##_rm();
UML_X86_DECLARE_FP_PI(32, 0)
UML_X86_DECLARE_FP_PI(32, 1)
UML_X86_DECLARE_FP_PI(64, 0)
UML_X86_DECLARE_FP_PI(64, 1)
#undef UML_X86_DECLARE_FP_PI


#define UML_X86_DECLARE_FMA_SHAPE(opcode, element, shape) \
	void uml_x86_64_gadget_vector_fma_##opcode##_##element##_##shape##_rr(); \
	void uml_x86_64_gadget_vector_fma_##opcode##_##element##_##shape##_rm();
#define UML_X86_DECLARE_FMA_ELEMENT(opcode, element) \
	UML_X86_DECLARE_FMA_SHAPE(opcode, element, 128) \
	UML_X86_DECLARE_FMA_SHAPE(opcode, element, 256) \
	UML_X86_DECLARE_FMA_SHAPE(opcode, element, scalar)
#define UML_X86_DECLARE_FMA_OPCODE(opcode) \
	UML_X86_DECLARE_FMA_ELEMENT(opcode, 32) \
	UML_X86_DECLARE_FMA_ELEMENT(opcode, 64)
UML_X86_DECLARE_FMA_OPCODE(96)
UML_X86_DECLARE_FMA_OPCODE(97)
UML_X86_DECLARE_FMA_OPCODE(98)
UML_X86_DECLARE_FMA_OPCODE(99)
UML_X86_DECLARE_FMA_OPCODE(9a)
UML_X86_DECLARE_FMA_OPCODE(9b)
UML_X86_DECLARE_FMA_OPCODE(9c)
UML_X86_DECLARE_FMA_OPCODE(9d)
UML_X86_DECLARE_FMA_OPCODE(9e)
UML_X86_DECLARE_FMA_OPCODE(9f)
UML_X86_DECLARE_FMA_OPCODE(a6)
UML_X86_DECLARE_FMA_OPCODE(a7)
UML_X86_DECLARE_FMA_OPCODE(a8)
UML_X86_DECLARE_FMA_OPCODE(a9)
UML_X86_DECLARE_FMA_OPCODE(aa)
UML_X86_DECLARE_FMA_OPCODE(ab)
UML_X86_DECLARE_FMA_OPCODE(ac)
UML_X86_DECLARE_FMA_OPCODE(ad)
UML_X86_DECLARE_FMA_OPCODE(ae)
UML_X86_DECLARE_FMA_OPCODE(af)
UML_X86_DECLARE_FMA_OPCODE(b6)
UML_X86_DECLARE_FMA_OPCODE(b7)
UML_X86_DECLARE_FMA_OPCODE(b8)
UML_X86_DECLARE_FMA_OPCODE(b9)
UML_X86_DECLARE_FMA_OPCODE(ba)
UML_X86_DECLARE_FMA_OPCODE(bb)
UML_X86_DECLARE_FMA_OPCODE(bc)
UML_X86_DECLARE_FMA_OPCODE(bd)
UML_X86_DECLARE_FMA_OPCODE(be)
UML_X86_DECLARE_FMA_OPCODE(bf)
#undef UML_X86_DECLARE_FMA_OPCODE
#undef UML_X86_DECLARE_FMA_ELEMENT
#undef UML_X86_DECLARE_FMA_SHAPE

#define UML_X86_DECLARE_AES(opcode, width) \
	void uml_x86_64_gadget_vector_aes_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_aes_##opcode##_##width##_rm();
UML_X86_DECLARE_AES(db, 128)
UML_X86_DECLARE_AES(dc, 128)
UML_X86_DECLARE_AES(dc, 256)
UML_X86_DECLARE_AES(dd, 128)
UML_X86_DECLARE_AES(dd, 256)
UML_X86_DECLARE_AES(de, 128)
UML_X86_DECLARE_AES(de, 256)
UML_X86_DECLARE_AES(df, 128)
UML_X86_DECLARE_AES(df, 256)
#undef UML_X86_DECLARE_AES

#define UML_X86_DECLARE_DOT(opcode, width) \
	void uml_x86_64_gadget_vector_dot_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_dot_##opcode##_##width##_rm();
UML_X86_DECLARE_DOT(50, 128)
UML_X86_DECLARE_DOT(50, 256)
UML_X86_DECLARE_DOT(51, 128)
UML_X86_DECLARE_DOT(51, 256)
UML_X86_DECLARE_DOT(52, 128)
UML_X86_DECLARE_DOT(52, 256)
UML_X86_DECLARE_DOT(53, 128)
UML_X86_DECLARE_DOT(53, 256)
#undef UML_X86_DECLARE_DOT

#define UML_X86_DECLARE_SHA(opcode) \
	void uml_x86_64_gadget_vector_sha_##opcode##_rr(); \
	void uml_x86_64_gadget_vector_sha_##opcode##_rm();
UML_X86_DECLARE_SHA(c8)
UML_X86_DECLARE_SHA(c9)
UML_X86_DECLARE_SHA(ca)
UML_X86_DECLARE_SHA(cb)
UML_X86_DECLARE_SHA(cc)
UML_X86_DECLARE_SHA(cd)
#undef UML_X86_DECLARE_SHA

#define UML_X86_DECLARE_VECTOR_INTEGER_WIDTH(opcode, width) \
	void uml_x86_64_gadget_vector_integer_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_integer_##opcode##_##width##_rm();
#define UML_X86_DECLARE_VECTOR_INTEGER(opcode) \
	UML_X86_DECLARE_VECTOR_INTEGER_WIDTH(opcode, 64) \
	UML_X86_DECLARE_VECTOR_INTEGER_WIDTH(opcode, 128) \
	UML_X86_DECLARE_VECTOR_INTEGER_WIDTH(opcode, 256)
UML_X86_DECLARE_VECTOR_INTEGER(60)
UML_X86_DECLARE_VECTOR_INTEGER(61)
UML_X86_DECLARE_VECTOR_INTEGER(62)
UML_X86_DECLARE_VECTOR_INTEGER(63)
UML_X86_DECLARE_VECTOR_INTEGER(64)
UML_X86_DECLARE_VECTOR_INTEGER(65)
UML_X86_DECLARE_VECTOR_INTEGER(66)
UML_X86_DECLARE_VECTOR_INTEGER(67)
UML_X86_DECLARE_VECTOR_INTEGER(68)
UML_X86_DECLARE_VECTOR_INTEGER(69)
UML_X86_DECLARE_VECTOR_INTEGER(6a)
UML_X86_DECLARE_VECTOR_INTEGER(6b)
UML_X86_DECLARE_VECTOR_INTEGER(6c)
UML_X86_DECLARE_VECTOR_INTEGER(6d)
UML_X86_DECLARE_VECTOR_INTEGER(70)
UML_X86_DECLARE_VECTOR_INTEGER(74)
UML_X86_DECLARE_VECTOR_INTEGER(75)
UML_X86_DECLARE_VECTOR_INTEGER(76)
UML_X86_DECLARE_VECTOR_INTEGER(d1)
UML_X86_DECLARE_VECTOR_INTEGER(d2)
UML_X86_DECLARE_VECTOR_INTEGER(d3)
UML_X86_DECLARE_VECTOR_INTEGER(d4)
UML_X86_DECLARE_VECTOR_INTEGER(d5)
UML_X86_DECLARE_VECTOR_INTEGER(d8)
UML_X86_DECLARE_VECTOR_INTEGER(d9)
UML_X86_DECLARE_VECTOR_INTEGER(da)
UML_X86_DECLARE_VECTOR_INTEGER(db)
UML_X86_DECLARE_VECTOR_INTEGER(dc)
UML_X86_DECLARE_VECTOR_INTEGER(dd)
UML_X86_DECLARE_VECTOR_INTEGER(de)
UML_X86_DECLARE_VECTOR_INTEGER(df)
UML_X86_DECLARE_VECTOR_INTEGER(e0)
UML_X86_DECLARE_VECTOR_INTEGER(e1)
UML_X86_DECLARE_VECTOR_INTEGER(e2)
UML_X86_DECLARE_VECTOR_INTEGER(e3)
UML_X86_DECLARE_VECTOR_INTEGER(e4)
UML_X86_DECLARE_VECTOR_INTEGER(e5)
UML_X86_DECLARE_VECTOR_INTEGER(e8)
UML_X86_DECLARE_VECTOR_INTEGER(e9)
UML_X86_DECLARE_VECTOR_INTEGER(ea)
UML_X86_DECLARE_VECTOR_INTEGER(eb)
UML_X86_DECLARE_VECTOR_INTEGER(ec)
UML_X86_DECLARE_VECTOR_INTEGER(ed)
UML_X86_DECLARE_VECTOR_INTEGER(ee)
UML_X86_DECLARE_VECTOR_INTEGER(ef)
UML_X86_DECLARE_VECTOR_INTEGER(f1)
UML_X86_DECLARE_VECTOR_INTEGER(f2)
UML_X86_DECLARE_VECTOR_INTEGER(f3)
UML_X86_DECLARE_VECTOR_INTEGER(f4)
UML_X86_DECLARE_VECTOR_INTEGER(f5)
UML_X86_DECLARE_VECTOR_INTEGER(f6)
UML_X86_DECLARE_VECTOR_INTEGER(f8)
UML_X86_DECLARE_VECTOR_INTEGER(f9)
UML_X86_DECLARE_VECTOR_INTEGER(fa)
UML_X86_DECLARE_VECTOR_INTEGER(fb)
UML_X86_DECLARE_VECTOR_INTEGER(fc)
UML_X86_DECLARE_VECTOR_INTEGER(fd)
UML_X86_DECLARE_VECTOR_INTEGER(fe)
#undef UML_X86_DECLARE_VECTOR_INTEGER
#undef UML_X86_DECLARE_VECTOR_INTEGER_WIDTH

#define UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(opcode, group, width) \
	void uml_x86_64_gadget_vector_integer_##opcode##_g##group##_##width##_ri();
#define UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(opcode, group) \
	UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(opcode, group, 64) \
	UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(opcode, group, 128) \
	UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(opcode, group, 256)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(71, 2)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(71, 4)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(71, 6)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(72, 2)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(72, 4)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(72, 6)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(73, 2)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE(73, 6)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 3, 128)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 3, 256)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 7, 128)
UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH(73, 7, 256)
#undef UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE
#undef UML_X86_DECLARE_VECTOR_INTEGER_IMMEDIATE_WIDTH

#define UML_X86_DECLARE_VECTOR_MAP2_WIDTH(opcode, width) \
	void uml_x86_64_gadget_vector_integer_map2_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_integer_map2_##opcode##_##width##_rm();
#define UML_X86_DECLARE_VECTOR_MAP2(opcode) \
	UML_X86_DECLARE_VECTOR_MAP2_WIDTH(opcode, 64) \
	UML_X86_DECLARE_VECTOR_MAP2_WIDTH(opcode, 128) \
	UML_X86_DECLARE_VECTOR_MAP2_WIDTH(opcode, 256)
UML_X86_DECLARE_VECTOR_MAP2(00)
UML_X86_DECLARE_VECTOR_MAP2(01)
UML_X86_DECLARE_VECTOR_MAP2(02)
UML_X86_DECLARE_VECTOR_MAP2(03)
UML_X86_DECLARE_VECTOR_MAP2(04)
UML_X86_DECLARE_VECTOR_MAP2(05)
UML_X86_DECLARE_VECTOR_MAP2(06)
UML_X86_DECLARE_VECTOR_MAP2(07)
UML_X86_DECLARE_VECTOR_MAP2(08)
UML_X86_DECLARE_VECTOR_MAP2(09)
UML_X86_DECLARE_VECTOR_MAP2(0a)
UML_X86_DECLARE_VECTOR_MAP2(0b)
UML_X86_DECLARE_VECTOR_MAP2(10)
UML_X86_DECLARE_VECTOR_MAP2(14)
UML_X86_DECLARE_VECTOR_MAP2(15)
UML_X86_DECLARE_VECTOR_MAP2(1c)
UML_X86_DECLARE_VECTOR_MAP2(1d)
UML_X86_DECLARE_VECTOR_MAP2(1e)
UML_X86_DECLARE_VECTOR_MAP2(20)
UML_X86_DECLARE_VECTOR_MAP2(21)
UML_X86_DECLARE_VECTOR_MAP2(22)
UML_X86_DECLARE_VECTOR_MAP2(23)
UML_X86_DECLARE_VECTOR_MAP2(24)
UML_X86_DECLARE_VECTOR_MAP2(25)
UML_X86_DECLARE_VECTOR_MAP2(28)
UML_X86_DECLARE_VECTOR_MAP2(29)
UML_X86_DECLARE_VECTOR_MAP2(2b)
UML_X86_DECLARE_VECTOR_MAP2(30)
UML_X86_DECLARE_VECTOR_MAP2(31)
UML_X86_DECLARE_VECTOR_MAP2(32)
UML_X86_DECLARE_VECTOR_MAP2(33)
UML_X86_DECLARE_VECTOR_MAP2(34)
UML_X86_DECLARE_VECTOR_MAP2(35)
UML_X86_DECLARE_VECTOR_MAP2(37)
UML_X86_DECLARE_VECTOR_MAP2(38)
UML_X86_DECLARE_VECTOR_MAP2(39)
UML_X86_DECLARE_VECTOR_MAP2(3a)
UML_X86_DECLARE_VECTOR_MAP2(3b)
UML_X86_DECLARE_VECTOR_MAP2(3c)
UML_X86_DECLARE_VECTOR_MAP2(3d)
UML_X86_DECLARE_VECTOR_MAP2(3e)
UML_X86_DECLARE_VECTOR_MAP2(3f)
UML_X86_DECLARE_VECTOR_MAP2(40)
UML_X86_DECLARE_VECTOR_MAP2(41)
#undef UML_X86_DECLARE_VECTOR_MAP2
#undef UML_X86_DECLARE_VECTOR_MAP2_WIDTH

#define UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(opcode, width) \
	void uml_x86_64_gadget_vector_map2_permute_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_map2_permute_##opcode##_##width##_rm();
UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(0c, 128)
UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(0c, 256)
UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(0d, 128)
UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(0d, 256)
UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(16, 256)
UML_X86_DECLARE_VECTOR_MAP2_PERMUTE(36, 256)
#undef UML_X86_DECLARE_VECTOR_MAP2_PERMUTE

#define UML_X86_DECLARE_VECTOR_MAP2_SHIFT(opcode, element, width) \
	void uml_x86_64_gadget_vector_map2_shift_##opcode##_##element##_##width##_rr(); \
	void uml_x86_64_gadget_vector_map2_shift_##opcode##_##element##_##width##_rm();
#define UML_X86_DECLARE_VECTOR_MAP2_SHIFT_OPCODE(opcode) \
	UML_X86_DECLARE_VECTOR_MAP2_SHIFT(opcode, 32, 128) \
	UML_X86_DECLARE_VECTOR_MAP2_SHIFT(opcode, 32, 256) \
	UML_X86_DECLARE_VECTOR_MAP2_SHIFT(opcode, 64, 128) \
	UML_X86_DECLARE_VECTOR_MAP2_SHIFT(opcode, 64, 256)
UML_X86_DECLARE_VECTOR_MAP2_SHIFT_OPCODE(45)
UML_X86_DECLARE_VECTOR_MAP2_SHIFT_OPCODE(46)
UML_X86_DECLARE_VECTOR_MAP2_SHIFT_OPCODE(47)
#undef UML_X86_DECLARE_VECTOR_MAP2_SHIFT_OPCODE
#undef UML_X86_DECLARE_VECTOR_MAP2_SHIFT

#define UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(element, width) \
	void uml_x86_64_gadget_vector_map2_broadcast_##element##_##width##_r(); \
	void uml_x86_64_gadget_vector_map2_broadcast_##element##_##width##_m();
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(8, 128)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(8, 256)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(16, 128)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(16, 256)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(32, 128)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(32, 256)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(64, 128)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(64, 256)
UML_X86_DECLARE_VECTOR_MAP2_BROADCAST(128, 256)
#undef UML_X86_DECLARE_VECTOR_MAP2_BROADCAST
void uml_x86_64_gadget_vector_map2_half_128_r();
void uml_x86_64_gadget_vector_map2_half_128_m();
void uml_x86_64_gadget_vector_map2_half_256_r();
void uml_x86_64_gadget_vector_map2_half_256_m();

#define UML_X86_DECLARE_VECTOR_MAP2_MASK(element, width) \
	void uml_x86_64_gadget_vector_map2_mask_load_##element##_##width(); \
	void uml_x86_64_gadget_vector_map2_mask_store_##element##_##width();
UML_X86_DECLARE_VECTOR_MAP2_MASK(32, 128)
UML_X86_DECLARE_VECTOR_MAP2_MASK(32, 256)
UML_X86_DECLARE_VECTOR_MAP2_MASK(64, 128)
UML_X86_DECLARE_VECTOR_MAP2_MASK(64, 256)
#undef UML_X86_DECLARE_VECTOR_MAP2_MASK

#define UML_X86_DECLARE_VECTOR_GATHER(data, index, width) \
	void uml_x86_64_gadget_vector_gather_d##data##_i##index##_r##width();
#define UML_X86_DECLARE_VECTOR_GATHER_SIZES(data, index) \
	UML_X86_DECLARE_VECTOR_GATHER(data, index, 16) \
	UML_X86_DECLARE_VECTOR_GATHER(data, index, 32)
UML_X86_DECLARE_VECTOR_GATHER_SIZES(4, 4)
UML_X86_DECLARE_VECTOR_GATHER_SIZES(4, 8)
UML_X86_DECLARE_VECTOR_GATHER_SIZES(8, 4)
UML_X86_DECLARE_VECTOR_GATHER_SIZES(8, 8)
#undef UML_X86_DECLARE_VECTOR_GATHER_SIZES
#undef UML_X86_DECLARE_VECTOR_GATHER

#define UML_X86_DECLARE_VECTOR_MAP3_SCALAR(size) \
	void uml_x86_64_gadget_vector_map3_extract_##size##_r(); \
	void uml_x86_64_gadget_vector_map3_extract_##size##_m(); \
	void uml_x86_64_gadget_vector_map3_insert_##size##_rr(); \
	void uml_x86_64_gadget_vector_map3_insert_##size##_rm();
UML_X86_DECLARE_VECTOR_MAP3_SCALAR(1)
UML_X86_DECLARE_VECTOR_MAP3_SCALAR(4)
UML_X86_DECLARE_VECTOR_MAP3_SCALAR(8)
#undef UML_X86_DECLARE_VECTOR_MAP3_SCALAR
void uml_x86_64_gadget_vector_map3_extract_2_r();
void uml_x86_64_gadget_vector_map3_extract_2_m();
void uml_x86_64_gadget_vector_map3_extract_128_r();
void uml_x86_64_gadget_vector_map3_extract_128_m();
void uml_x86_64_gadget_vector_map3_insertps_rr();
void uml_x86_64_gadget_vector_map3_insertps_rm();

#define UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE_WIDTH(opcode, width) \
	void uml_x86_64_gadget_vector_map3_shuffle_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_map3_shuffle_##opcode##_##width##_rm();
#define UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(opcode) \
	UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE_WIDTH(opcode, 64) \
	UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE_WIDTH(opcode, 128) \
	UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE_WIDTH(opcode, 256)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(00)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(01)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(02)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(04)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(05)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(06)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(0c)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(0d)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(0e)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(0f)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(18)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(38)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(46)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(4a)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(4b)
UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE(4c)
#undef UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE
#undef UML_X86_DECLARE_VECTOR_MAP3_SHUFFLE_WIDTH

#define UML_X86_DECLARE_VECTOR_MAP3_ROUND(opcode, width) \
	void uml_x86_64_gadget_vector_map3_round_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_map3_round_##opcode##_##width##_rm();
UML_X86_DECLARE_VECTOR_MAP3_ROUND(08, 128)
UML_X86_DECLARE_VECTOR_MAP3_ROUND(08, 256)
UML_X86_DECLARE_VECTOR_MAP3_ROUND(09, 128)
UML_X86_DECLARE_VECTOR_MAP3_ROUND(09, 256)
UML_X86_DECLARE_VECTOR_MAP3_ROUND(0a, 128)
UML_X86_DECLARE_VECTOR_MAP3_ROUND(0b, 128)
#undef UML_X86_DECLARE_VECTOR_MAP3_ROUND
void uml_x86_64_gadget_vector_map3_half_128_r();
void uml_x86_64_gadget_vector_map3_half_128_m();
void uml_x86_64_gadget_vector_map3_half_256_r();
void uml_x86_64_gadget_vector_map3_half_256_m();

#define UML_X86_DECLARE_VECTOR_MAP3_MATH(opcode, width) \
	void uml_x86_64_gadget_vector_map3_math_##opcode##_##width##_rr(); \
	void uml_x86_64_gadget_vector_map3_math_##opcode##_##width##_rm();
UML_X86_DECLARE_VECTOR_MAP3_MATH(40, 128)
UML_X86_DECLARE_VECTOR_MAP3_MATH(40, 256)
UML_X86_DECLARE_VECTOR_MAP3_MATH(41, 128)
UML_X86_DECLARE_VECTOR_MAP3_MATH(41, 256)
UML_X86_DECLARE_VECTOR_MAP3_MATH(42, 128)
UML_X86_DECLARE_VECTOR_MAP3_MATH(42, 256)
#undef UML_X86_DECLARE_VECTOR_MAP3_MATH
void uml_x86_64_gadget_vector_map3_pclmul_128_rr();
void uml_x86_64_gadget_vector_map3_pclmul_128_rm();
void uml_x86_64_gadget_vector_map3_pclmul_256_rr();
void uml_x86_64_gadget_vector_map3_pclmul_256_rm();
void uml_x86_64_gadget_vector_map3_sha1rnds4_rr();
void uml_x86_64_gadget_vector_map3_sha1rnds4_rm();
void uml_x86_64_gadget_vector_map3_aeskeygen_rr();
void uml_x86_64_gadget_vector_map3_aeskeygen_rm();

#define UML_X86_DECLARE_VECTOR_MAP3_STRING(opcode) \
	void uml_x86_64_gadget_vector_map3_string_##opcode##_rr(); \
	void uml_x86_64_gadget_vector_map3_string_##opcode##_rm();
UML_X86_DECLARE_VECTOR_MAP3_STRING(60)
UML_X86_DECLARE_VECTOR_MAP3_STRING(61)
UML_X86_DECLARE_VECTOR_MAP3_STRING(62)
UML_X86_DECLARE_VECTOR_MAP3_STRING(63)
#undef UML_X86_DECLARE_VECTOR_MAP3_STRING

#define UML_X86_DECLARE_VECTOR_BITWISE(operation) \
	void uml_x86_64_gadget_vector_##operation##_128_rr(); \
	void uml_x86_64_gadget_vector_##operation##_128_rr_vex(); \
	void uml_x86_64_gadget_vector_##operation##_256_rr(); \
	void uml_x86_64_gadget_vector_##operation##_128_rm(); \
	void uml_x86_64_gadget_vector_##operation##_128_rm_vex(); \
	void uml_x86_64_gadget_vector_##operation##_256_rm();
UML_X86_DECLARE_VECTOR_BITWISE(and)
UML_X86_DECLARE_VECTOR_BITWISE(andnot)
UML_X86_DECLARE_VECTOR_BITWISE(or)
UML_X86_DECLARE_VECTOR_BITWISE(xor)
#undef UML_X86_DECLARE_VECTOR_BITWISE

#define UML_X86_DECLARE_CONDITION(condition) \
	void uml_x86_64_gadget_set_##condition(); \
	void uml_x86_64_gadget_set_##condition##_m(); \
	void uml_x86_64_gadget_cmov_##condition##_16_rr(); \
	void uml_x86_64_gadget_cmov_##condition##_32_rr(); \
	void uml_x86_64_gadget_cmov_##condition##_64_rr(); \
	void uml_x86_64_gadget_cmov_##condition##_16_rm(); \
	void uml_x86_64_gadget_cmov_##condition##_32_rm(); \
	void uml_x86_64_gadget_cmov_##condition##_64_rm(); \
	void uml_x86_64_gadget_jump_##condition(); \
	void uml_x86_64_gadget_jump_##condition##_linked(); \
	void uml_x86_64_gadget_jump_##condition##_inline();
#define UML_X86_DECLARE_FUSED_CONDITION(condition) \
	void uml_x86_64_gadget_set_##condition##_nzcv(); \
	void uml_x86_64_gadget_cmov_##condition##_16_rr_nzcv(); \
	void uml_x86_64_gadget_cmov_##condition##_32_rr_nzcv(); \
	void uml_x86_64_gadget_cmov_##condition##_64_rr_nzcv(); \
	void uml_x86_64_gadget_jump_##condition##_nzcv(); \
	void uml_x86_64_gadget_jump_##condition##_linked_nzcv(); \
	void uml_x86_64_gadget_jump_##condition##_inline_nzcv();
UML_X86_DECLARE_CONDITION(o)
UML_X86_DECLARE_CONDITION(no)
UML_X86_DECLARE_CONDITION(b)
UML_X86_DECLARE_CONDITION(ae)
UML_X86_DECLARE_CONDITION(e)
UML_X86_DECLARE_CONDITION(ne)
UML_X86_DECLARE_CONDITION(be)
UML_X86_DECLARE_CONDITION(a)
UML_X86_DECLARE_CONDITION(s)
UML_X86_DECLARE_CONDITION(ns)
UML_X86_DECLARE_CONDITION(p)
UML_X86_DECLARE_CONDITION(np)
UML_X86_DECLARE_CONDITION(l)
UML_X86_DECLARE_CONDITION(ge)
UML_X86_DECLARE_CONDITION(le)
UML_X86_DECLARE_CONDITION(g)
#undef UML_X86_DECLARE_CONDITION
UML_X86_DECLARE_FUSED_CONDITION(o)
UML_X86_DECLARE_FUSED_CONDITION(no)
UML_X86_DECLARE_FUSED_CONDITION(b)
UML_X86_DECLARE_FUSED_CONDITION(ae)
UML_X86_DECLARE_FUSED_CONDITION(e)
UML_X86_DECLARE_FUSED_CONDITION(ne)
UML_X86_DECLARE_FUSED_CONDITION(be)
UML_X86_DECLARE_FUSED_CONDITION(a)
UML_X86_DECLARE_FUSED_CONDITION(s)
UML_X86_DECLARE_FUSED_CONDITION(ns)
UML_X86_DECLARE_FUSED_CONDITION(l)
UML_X86_DECLARE_FUSED_CONDITION(ge)
UML_X86_DECLARE_FUSED_CONDITION(le)
UML_X86_DECLARE_FUSED_CONDITION(g)
#undef UML_X86_DECLARE_FUSED_CONDITION

/* LOCK-prefixed memory destinations resolve to exclusive-loop gadgets so a
 * guest atomic stays atomic against the other vCPU threads.
 */
#define UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, width) \
	void uml_x86_64_gadget_##operation##_##width##_mr_locked(); \
	void uml_x86_64_gadget_##operation##_##width##_mi_locked();
#define UML_X86_DECLARE_LOCKED_MEMORY(operation) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 8) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 16) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 32) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 64)

UML_X86_DECLARE_LOCKED_MEMORY(add)
UML_X86_DECLARE_LOCKED_MEMORY(sub)
UML_X86_DECLARE_LOCKED_MEMORY(and)
UML_X86_DECLARE_LOCKED_MEMORY(or)
UML_X86_DECLARE_LOCKED_MEMORY(xor)
UML_X86_DECLARE_LOCKED_MEMORY(adc)
UML_X86_DECLARE_LOCKED_MEMORY(sbb)

#define UML_X86_DECLARE_LOCKED_BIT(operation) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 16) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 32) \
	UML_X86_DECLARE_LOCKED_MEMORY_WIDTH(operation, 64)

UML_X86_DECLARE_LOCKED_BIT(bts)
UML_X86_DECLARE_LOCKED_BIT(btr)
UML_X86_DECLARE_LOCKED_BIT(btc)

#define UML_X86_DECLARE_LOCKED_UNARY(operation) \
	void uml_x86_64_gadget_##operation##_8_m_locked(); \
	void uml_x86_64_gadget_##operation##_16_m_locked(); \
	void uml_x86_64_gadget_##operation##_32_m_locked(); \
	void uml_x86_64_gadget_##operation##_64_m_locked();

UML_X86_DECLARE_LOCKED_UNARY(increment)
UML_X86_DECLARE_LOCKED_UNARY(decrement)
UML_X86_DECLARE_LOCKED_UNARY(negate)
UML_X86_DECLARE_LOCKED_UNARY(not)

#undef UML_X86_DECLARE_LOCKED_UNARY
#undef UML_X86_DECLARE_LOCKED_BIT
#undef UML_X86_DECLARE_LOCKED_MEMORY
#undef UML_X86_DECLARE_LOCKED_MEMORY_WIDTH
#undef UML_X86_DECLARE_WIDTHS
#undef UML_X86_DECLARE_WIDTH_FORMS
}

} // namespace uml::cpu::x86_64::gadgets::native
