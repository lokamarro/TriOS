// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_encodings.hpp"

#include <cstdint>

namespace uml::cpu::arm64::aarch32 {

/* The operations that group the decoder's forms (one per XML encoding) by
 * mnemonic. The semantic library executes them and the JIT's compiler lowers
 * them. The register or immediate shape of a form and its operands come from
 * the decoded instruction. */
enum class Op : std::uint8_t {
	none,
	/* Data processing. */
	adc, add, addw, adr, and_, bic, cmn, cmp, eor, mov, movw, movt, mvn, orn,
	orr, rsb, rsc, sbc, sub, subw, teq, tst,
	/* Bit fields, counts, reversals, extensions and packing. */
	bfc, bfi, sbfx, ubfx, clz, rbit, rev, rev16, revsh, sxtb, sxth, uxtb,
	uxth, sxtab, sxtah, uxtab, uxtah, sxtb16, uxtb16, sxtab16, uxtab16, pkh,
	sel,
	/* Multiplies and divides. */
	mul, mla, mls, umull, umlal, smull, smlal, umaal, sdiv, udiv, smla_xy,
	smul_xy, smlal_xy, smlaw, smulw, smlad, smlsd, smuad, smusd, smlald,
	smlsld, smmla, smmls, smmul, usad8, usada8,
	/* Saturation and parallel arithmetic. */
	qadd, qsub, qdadd, qdsub, ssat, usat, ssat16, usat16, parallel,
	/* Branches. */
	b, bl, blx, bx, cbz, cbnz, tb,
	/* Single loads and stores. */
	ldr, ldrb, ldrh, ldrsb, ldrsh, ldrd, str, strb, strh, strd, ldrt, ldrbt,
	ldrht, ldrsbt, ldrsht, strt, strbt, strht,
	/* Multiple loads and stores, pushes and pops. */
	ldm, stm,
	/* Exclusives and the acquire/release group. */
	ldrex, ldrexb, ldrexh, ldrexd, strex, strexb, strexh, strexd, ldaex,
	ldaexb, ldaexh, ldaexd, stlex, stlexb, stlexh, stlexd, lda, ldab, ldah,
	stl, stlb, stlh, clrex,
	/* System. */
	prefetch, svc, bkpt, udf, mrs, msr, coprocessor, nop, dmb, dsb, isb, it,
	/* VFP and Advanced SIMD memory: VLDR, VSTR, VLDM, VSTM (FLDMX and FSTMX
	 * included) and the structure loads and stores. */
	vldr, vstr, vldm, vstm, vld, vst,
	/* VFP and Advanced SIMD data processing and moves. The mnemonic does not
	 * name them: the library's table row names the handler that executes one
	 * (semantics_internal.hpp) and the JIT classes them by the form's level.
	 * form_operation() never returns it. */
	vector,
	/* The JIT's class for the Armv8 additions, which it lowers by extension
	 * or leaves to the library (jit/a32/a32_operations.hpp).
	 * form_operation() never returns it. */
	library,
};

/* The suffix of a parallel add or subtract, with its signedness and mode. */
enum class ParallelKind : std::uint8_t { add16, add8, asx, sax, sub16, sub8 };
enum class ParallelMode : std::uint8_t {
	signed_wrap, saturating, signed_halving, unsigned_wrap,
	unsigned_saturating, unsigned_halving,
};

/* How a form names its second operand: an immediate, a register shifted by
 * an immediate, a register shifted by a register, or the literal pool. */
enum class Shape : std::uint8_t { other, immediate, reg, reg_shifted, literal };

/* What a form's mnemonic and encoding name say about its operation. */
struct FormOperation {
	Op op{Op::none};
	ParallelKind kind{};
	ParallelMode mode{};
	Shape shape{Shape::other};
	/* The form's Rn is SP, which its operands leave out. */
	bool sp_base{};
};

/* The operation of a form, from its mnemonic, with the shape and SP base
 * from its name. BLX (immediate) shares BL's encoding file but is Op::blx. A
 * form whose mnemonic names no operation, such as a VFP or Advanced SIMD
 * data-processing form, has Op::none. The table is built on first use. */
[[nodiscard]] const FormOperation &form_operation(Form form) noexcept;

} // namespace uml::cpu::arm64::aarch32
