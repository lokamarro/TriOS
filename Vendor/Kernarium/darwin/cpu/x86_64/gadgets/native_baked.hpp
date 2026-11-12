// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/* Entry tables of the register-baked scalar bank that
 * engine/generate_baked.py emits. Each table lists its family's entries in
 * the generator's enumeration order: destination-major register pairs,
 * registers in offset order with the four high-byte registers after the
 * sixteen low ones, and the sixteen bases followed by "none". The index
 * helpers in native_builder.cpp are the only readers; keep them and the
 * generator in step.
 */
namespace uml::cpu::x86_64::gadgets::baked {

inline constexpr unsigned gpr_slots = 16;
inline constexpr unsigned byte_slots = 20;
inline constexpr unsigned base_slots = 17;
inline constexpr unsigned no_base_slot = 16;
inline constexpr unsigned scale_slots = 4;

extern "C" {

#define UML_X86_BAKED_ARITH_WIDTH(op, width) \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_rr[]; \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_ri[]; \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_rm[]; \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_xrm[]; \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_mr[]; \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_xmr[]; \
	extern const std::uintptr_t uml_x86_64_baked_##op##_##width##_mi[];
#define UML_X86_BAKED_ARITH(op) \
	UML_X86_BAKED_ARITH_WIDTH(op, 8) \
	UML_X86_BAKED_ARITH_WIDTH(op, 16) \
	UML_X86_BAKED_ARITH_WIDTH(op, 32) \
	UML_X86_BAKED_ARITH_WIDTH(op, 64) \
	extern const std::uintptr_t uml_x86_64_baked_##op##_xmi[];
UML_X86_BAKED_ARITH(add)
UML_X86_BAKED_ARITH(sub)
UML_X86_BAKED_ARITH(and)
UML_X86_BAKED_ARITH(or)
UML_X86_BAKED_ARITH(xor)
UML_X86_BAKED_ARITH(cmp)
UML_X86_BAKED_ARITH(test)
#undef UML_X86_BAKED_ARITH
#undef UML_X86_BAKED_ARITH_WIDTH

#define UML_X86_BAKED_MOVE(width) \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_rr[]; \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_ri[]; \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_rm[]; \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_xm[]; \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_mr[]; \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_xmr[]; \
	extern const std::uintptr_t uml_x86_64_baked_move_##width##_mi[]; \
	extern const std::uintptr_t uml_x86_64_baked_increment_##width##_r[]; \
	extern const std::uintptr_t uml_x86_64_baked_decrement_##width##_r[]; \
	extern const std::uintptr_t uml_x86_64_baked_negate_##width##_r[]; \
	extern const std::uintptr_t uml_x86_64_baked_not_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_increment_settled_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_increment_borrow_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_increment_carry_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_increment_clear_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_decrement_settled_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_decrement_borrow_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_decrement_carry_##width##_r[]; \
	extern const std::uintptr_t \
		uml_x86_64_baked_decrement_clear_##width##_r[];
UML_X86_BAKED_MOVE(8)
UML_X86_BAKED_MOVE(16)
UML_X86_BAKED_MOVE(32)
UML_X86_BAKED_MOVE(64)
#undef UML_X86_BAKED_MOVE
extern const std::uintptr_t uml_x86_64_baked_move_xmi[];

#define UML_X86_BAKED_EXTEND(name) \
	extern const std::uintptr_t uml_x86_64_baked_##name##_rr[]; \
	extern const std::uintptr_t uml_x86_64_baked_##name##_rm[]; \
	extern const std::uintptr_t uml_x86_64_baked_##name##_xm[];
UML_X86_BAKED_EXTEND(zext_8_32)
UML_X86_BAKED_EXTEND(zext_8_64)
UML_X86_BAKED_EXTEND(zext_16_32)
UML_X86_BAKED_EXTEND(zext_16_64)
UML_X86_BAKED_EXTEND(sext_8_32)
UML_X86_BAKED_EXTEND(sext_8_64)
UML_X86_BAKED_EXTEND(sext_16_32)
UML_X86_BAKED_EXTEND(sext_16_64)
UML_X86_BAKED_EXTEND(sext_32_64)
#undef UML_X86_BAKED_EXTEND
extern const std::uintptr_t uml_x86_64_baked_zext_8_16_rr[];
extern const std::uintptr_t uml_x86_64_baked_sext_8_16_rr[];

extern const std::uintptr_t uml_x86_64_baked_ea[];
extern const std::uintptr_t uml_x86_64_baked_lea_32[];
extern const std::uintptr_t uml_x86_64_baked_lea_32_x[];
extern const std::uintptr_t uml_x86_64_baked_lea_64[];
extern const std::uintptr_t uml_x86_64_baked_lea_64_x[];
extern const std::uintptr_t uml_x86_64_baked_push_64_r[];
extern const std::uintptr_t uml_x86_64_baked_pop_64_r[];

} // extern "C"

} // namespace uml::cpu::x86_64::gadgets::baked
