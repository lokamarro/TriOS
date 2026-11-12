/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux sanity check for the advertised HWCAP families.
 *
 * G125 advertises a family only where an engine has committed to executing it,
 * but binary analysis of Geekbench 5, 6 and 7 found that several of those
 * families never appear in any of those binaries: zero LDAPR, LDAPUR, STLUR,
 * FCADD, FCMLA, CFINV, RMIF, SETF, AXFLAG, XAFLAG, SQRDMLAH, FMLAL, FJCVTZS and FRINT32/64 across all three. A
 * benchmark therefore cannot tell us whether those advertisements are honest -
 * a flat score is what both "correct" and "broken" look like when nothing
 * executes the instruction.
 *
 * This executes one instance of each advertised family with inputs whose
 * results are checked against constants derived by hand from DDI0487, and
 * exits with a distinct status per family so a failure names itself. The
 * SME families of the advertised profile (FEAT_SME and its F32F32, B16F32,
 * F16F32, I8I32, F64F64 and I16I64 outer products) run in streaming mode at
 * whatever vector length RDSVL reports; each stores its results and leaves
 * streaming mode before anything is compared. It reads
 * AT_HWCAP and AT_HWCAP2 from its auxiliary vector and skips each family the
 * kernel does not advertise, so an engine is held to exactly what it
 * advertises. It is a correctness check, not a benchmark.
 *
 * Exit status: 0 all families correct, otherwise the failing family's number.
 */

/* Armv8.5 already implies FCMA, LRCPC2, JSCVT and FRINTTS, so only the
 * genuinely optional extensions are named. The spellings that are not implied
 * differ between toolchains - Apple clang rejects `jscvt`, `fcma` and
 * `rcpc-immo`, which the Homebrew LLVM accepts - so relying on the
 * architecture level rather than naming them keeps this assembling under both.
 * An assembler directive rather than a -march flag, so the file builds the
 * same way from any entry point. */
	.arch armv8.5-a+fp16+dotprod+rdm+fp16fml+rcpc
	.arch_extension sme
	.arch_extension sme-f64f64
	.arch_extension sme-i16i64
	.arch_extension bf16

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94

	.equ E_LDAPR,		1
	.equ E_LDAPUR,		2
	.equ E_STLUR,		3
	.equ E_FCADD,		4
	.equ E_FCMLA,		5
	.equ E_SDOT,		6
	.equ E_UDOT,		7
	.equ E_SQRDMLAH,	8
	.equ E_FJCVTZS,		9
	.equ E_FRINT32Z,	10
	.equ E_FP16,		11
	.equ E_FMLAL,		12
	.equ E_CFINV,		13
	.equ E_RMIF,		14
	.equ E_SETF,		15
	.equ E_AXFLAG,		16
	.equ E_XAFLAG,		17
	.equ E_USCAT,		18
	.equ E_SB,		19
	.equ E_DIT,		20
	.equ E_AUXV,		21
	.equ E_JSCVT_VALUE,	22
	.equ E_JSCVT_NZCV,	23
	.equ E_JSCVT_FPSR,	24
	.equ JSCVT_CASES,	16
	.equ E_ECV,		25
	.equ E_CSSC,		26
	.equ E_MOPS,		27
	.equ E_AFP,		28
	.equ E_RPRES,		29
	.equ E_SME,		30
	.equ E_SME_F32F32,	31
	.equ E_SME_B16F32,	32
	.equ E_SME_F16F32,	33
	.equ E_SME_I8I32,	34
	.equ E_SME_F64F64,	35
	.equ E_SME_I16I64,	36

	.equ AT_HWCAP,		16
	.equ AT_HWCAP2,		26

	/* Bit numbers in AT_HWCAP and AT_HWCAP2, from arm64 Linux's uapi hwcap.h. */
	.equ HWCAP_FP,		0
	.equ HWCAP_ASIMD,	1
	.equ HWCAP_ATOMICS,	8
	.equ HWCAP_FPHP,	9
	.equ HWCAP_ASIMDRDM,	12
	.equ HWCAP_JSCVT,	13
	.equ HWCAP_FCMA,	14
	.equ HWCAP_LRCPC,	15
	.equ HWCAP_ASIMDDP,	20
	.equ HWCAP_ASIMDFHM,	23
	.equ HWCAP_DIT,		24
	.equ HWCAP_USCAT,	25
	.equ HWCAP_ILRCPC,	26
	.equ HWCAP_FLAGM,	27
	.equ HWCAP_SB,		29
	.equ HWCAP2_FLAGM2,	7
	.equ HWCAP2_FRINT,	8
	.equ HWCAP2_ECV,	19
	.equ HWCAP2_AFP,	20
	.equ HWCAP2_RPRES,	21
	.equ HWCAP2_SME,	23
	.equ HWCAP2_SME_I16I64,	24
	.equ HWCAP2_SME_F64F64,	25
	.equ HWCAP2_SME_I8I32,	26
	.equ HWCAP2_SME_F16F32,	27
	.equ HWCAP2_SME_B16F32,	28
	.equ HWCAP2_SME_F32F32,	29
	.equ HWCAP2_CSSC,	34
	.equ HWCAP2_MOPS,	43

	/* FPCR.FIZ, AH and NEP, the FEAT_AFP controls, are bits 0, 1 and 2. */
	.equ FPCR_AH,		1 << 1
	.equ FPCR_AFP_CONTROLS,	7

	/* The engine smoke accepts the payload only when it prints exactly "OK"
	 * and exits zero, so a failure reports the family by name on stdout
	 * where the runner will echo it, rather than by exit status alone. */
	.macro fail code, label
	adrp	x1, \label
	add	x1, x1, :lo12:\label
	mov	x2, #16
	mov	x0, #1
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #\code
	b	.Lexit
	.endm

	/* Every element of the first slice of ZA tile 0, stored to sme_row, has
	 * the value in \expected, for the RDSVL #1 bytes in x23. */
	.macro check_row width, expected, label
	mov	x9, #0
9:
	.if \width == 4
	ldr	w10, [x20, x9]
	cmp	w10, \expected
	.else
	ldr	x10, [x20, x9]
	cmp	x10, \expected
	.endif
	b.ne	\label
	add	x9, x9, #\width
	cmp	x9, x23
	b.lo	9b
	.endm

	/* Starts one outer-product family: SM and ZA on, ZA zeroed, every
	 * element of P0 true. */
	.macro sme_enter skip, bit
	tbz	x29, #\bit, \skip
	rdsvl	x23, #1
	adrp	x20, sme_row
	add	x20, x20, :lo12:sme_row
	smstart
	zero	{za}
	ptrue	p0.b
	.endm

	/* Stores the first slice of ZA tile 0, which is ZA's first row at
	 * every element size, and leaves streaming mode. */
	.macro sme_leave
	mov	w12, #0
	str	za[w12, 0], [x20]
	smstop
	.endm

	.section .text
	.global _start
	.type _start, %function
_start:
	/* A static program starts with sp at argc, then argv[] and its NULL,
	 * envp[] and its NULL, and the auxiliary vector's (type, value) pairs up
	 * to AT_NULL. x28 and x29 keep AT_HWCAP and AT_HWCAP2. */
	ldr	x0, [sp]
	mov	x1, sp
	add	x1, x1, x0, lsl #3
	add	x1, x1, #16
.Lskip_environment:
	ldr	x2, [x1], #8
	cbnz	x2, .Lskip_environment
	mov	x28, #0
	mov	x29, #0
.Lread_auxv:
	ldp	x2, x3, [x1], #16
	cbz	x2, .Lread_auxv_done
	cmp	x2, #AT_HWCAP
	csel	x28, x3, x28, eq
	cmp	x2, #AT_HWCAP2
	csel	x29, x3, x29, eq
	b	.Lread_auxv
.Lread_auxv_done:
	/* Every arm64 kernel advertises FP and ASIMD, so without them the walk
	 * did not find AT_HWCAP and every family would be skipped. */
	tbz	x28, #HWCAP_FP, .Lfail_auxv
	tbz	x28, #HWCAP_ASIMD, .Lfail_auxv

	adrp	x19, scratch
	add	x19, x19, :lo12:scratch

	tbz	x28, #HWCAP_LRCPC, .Lskip_lrcpc
	/* FEAT_LRCPC: LDAPR returns the value most recently stored. */
	movz	x1, #0x7788
	movk	x1, #0x5566, lsl #16
	movk	x1, #0x3344, lsl #32
	movk	x1, #0x1122, lsl #48
	str	x1, [x19]
	ldapr	x2, [x19]
	cmp	x2, x1
	b.ne	.Lfail_ldapr
.Lskip_lrcpc:

	tbz	x28, #HWCAP_ILRCPC, .Lskip_ilrcpc
	/* FEAT_LRCPC2: STLUR writes at an unscaled offset, LDAPUR reads it back.
	 * Offset 12 is deliberately unaligned to the 8-byte access size, which is
	 * the case the unscaled form exists to serve. */
	movz	x3, #0xbeef
	movk	x3, #0xdead, lsl #16
	stlur	w3, [x19, #12]
	ldapur	w4, [x19, #12]
	cmp	w4, w3
	b.ne	.Lfail_stlur
	ldr	w5, [x19, #12]
	cmp	w5, w3
	b.ne	.Lfail_ldapur
.Lskip_ilrcpc:

	tbz	x28, #HWCAP_FCMA, .Lskip_fcma
	/* FEAT_FCMA: FCADD Vd.2S, Vn.2S, Vm.2S, #90 computes, per complex pair,
	 * d.re = n.re - m.im and d.im = n.im + m.re. With n = (1.0, 2.0) and
	 * m = (3.0, 4.0) that is (1 - 4, 2 + 3) = (-3.0, 5.0). */
	adrp	x6, fcadd_n
	add	x6, x6, :lo12:fcadd_n
	ldr	d0, [x6]
	ldr	d1, [x6, #8]
	fcadd	v2.2s, v0.2s, v1.2s, #90
	adrp	x7, fcadd_expected
	add	x7, x7, :lo12:fcadd_expected
	ldr	d3, [x7]
	fcmp	s2, s3
	b.ne	.Lfail_fcadd
	mov	v4.s[0], v2.s[1]
	mov	v5.s[0], v3.s[1]
	fcmp	s4, s5
	b.ne	.Lfail_fcadd

	/* FCMLA Vd.2S, Vn.2S, Vm.2S, #0 accumulates d.re += n.re * m.re and
	 * d.im += n.re * m.im. Both lanes multiply by Vn's *real* element rather
	 * than by the matching one, which is easy to get backwards; the expected
	 * pair below was confirmed by running the same instruction natively on an
	 * arm64 host. From d = (0,0), n = (1,2), m = (3,4) that is (3.0, 4.0). */
	movi	v6.2d, #0
	fcmla	v6.2s, v0.2s, v1.2s, #0
	adrp	x8, fcmla_expected
	add	x8, x8, :lo12:fcmla_expected
	ldr	d7, [x8]
	fcmp	s6, s7
	b.ne	.Lfail_fcmla
	mov	v16.s[0], v6.s[1]
	mov	v17.s[0], v7.s[1]
	fcmp	s16, s17
	b.ne	.Lfail_fcmla
.Lskip_fcma:

	tbz	x28, #HWCAP_ASIMDDP, .Lskip_asimddp
	/* FEAT_DotProd: SDOT Vd.2S, Vn.8B, Vm.8B sums four signed byte products
	 * into each 32-bit lane. Bytes 1,2,3,4 against 5,6,7,8 give
	 * 5 + 12 + 21 + 32 = 70 in lane 0. */
	adrp	x9, dot_a
	add	x9, x9, :lo12:dot_a
	ldr	d18, [x9]
	ldr	d19, [x9, #8]
	movi	v20.2d, #0
	sdot	v20.2s, v18.8b, v19.8b
	umov	w10, v20.s[0]
	cmp	w10, #70
	b.ne	.Lfail_sdot

	movi	v21.2d, #0
	udot	v21.2s, v18.8b, v19.8b
	umov	w11, v21.s[0]
	cmp	w11, #70
	b.ne	.Lfail_udot
.Lskip_asimddp:

	tbz	x28, #HWCAP_ASIMDRDM, .Lskip_asimdrdm
	/* FEAT_RDM: SQRDMLAH Vd.4H, Vn.4H, Vm.4H rounds and doubles the product
	 * before accumulating. With d = 0 and n = m = 0x4000 (0.5 in Q15) the
	 * saturating doubled rounded product is 0x2000. */
	adrp	x12, rdm_input
	add	x12, x12, :lo12:rdm_input
	ldr	d22, [x12]
	movi	v23.2d, #0
	sqrdmlah	v23.4h, v22.4h, v22.4h
	umov	w13, v23.h[0]
	mov	w14, #0x2000
	cmp	w13, w14
	b.ne	.Lfail_sqrdmlah
.Lskip_asimdrdm:

	tbz	x28, #HWCAP_JSCVT, .Lskip_jscvt
	/* FEAT_JSCVT: FJCVTZS truncates a double toward zero into a 32-bit
	 * result. -2.5 becomes -2. */
	adrp	x15, jscvt_input
	add	x15, x15, :lo12:jscvt_input
	ldr	d24, [x15]
	fjcvtzs	w16, d24
	cmn	w16, #2
	b.ne	.Lfail_fjcvtzs

	/* The special inputs, each from NZCV all set and FPSR clear. FJCVTZS
	 * sets NZCV to 0:Z:0:0, where Z says the result is exact: FPToFixedJS
	 * clears it for a NaN, an out-of-range or inexact value, and for any
	 * zero but +0.0, including a subnormal that FPCR.FZ flushes. Some
	 * cores, such as the M1, set Z for a flushed positive subnormal, and an
	 * engine that forwards that is wrong. */
	adrp	x15, jscvt_cases
	add	x15, x15, :lo12:jscvt_cases
	mov	x14, #JSCVT_CASES
.Ljscvt_case:
	ldp	x9, x10, [x15]
	msr	fpcr, x10
	msr	fpsr, xzr
	mov	x11, #0xf0000000
	msr	nzcv, x11
	fmov	d24, x9
	fjcvtzs	w16, d24
	mrs	x11, nzcv
	mrs	x12, fpsr
	msr	fpcr, xzr
	msr	fpsr, xzr
	ldp	x9, x10, [x15, #16]
	cmp	w16, w9
	b.ne	.Lfail_jscvt_value
	cmp	x11, x10
	b.ne	.Lfail_jscvt_nzcv
	ldr	x9, [x15, #32]
	cmp	x12, x9
	b.ne	.Lfail_jscvt_fpsr
	add	x15, x15, #40
	subs	x14, x14, #1
	b.ne	.Ljscvt_case
.Lskip_jscvt:

	tbz	x29, #HWCAP2_FRINT, .Lskip_frint
	/* FEAT_FRINTTS: FRINT32Z rounds toward zero to a 32-bit integral float.
	 * 2.7 becomes 2.0. */
	adrp	x17, frint_input
	add	x17, x17, :lo12:frint_input
	ldr	d25, [x17]
	frint32z	d26, d25
	adrp	x18, frint_expected
	add	x18, x18, :lo12:frint_expected
	ldr	d27, [x18]
	fcmp	d26, d27
	b.ne	.Lfail_frint32z
.Lskip_frint:

	tbz	x28, #HWCAP_FPHP, .Lskip_fphp
	/* FEAT_FP16: half-precision scalar add, 1.5 + 2.5 = 4.0. */
	adrp	x20, fp16_input
	add	x20, x20, :lo12:fp16_input
	ldr	h28, [x20]
	ldr	h29, [x20, #2]
	fadd	h30, h28, h29
	ldr	h31, [x20, #4]
	fcmp	h30, h31
	b.ne	.Lfail_fp16
.Lskip_fphp:

	tbz	x28, #HWCAP_ASIMDFHM, .Lskip_asimdfhm
	/* FEAT_FHM: FMLAL Vd.4S, Vn.4H, Vm.4H widens half-precision products into
	 * single precision and accumulates. 1.5 * 2.5 = 3.75 from zero. */
	adrp	x21, fhm_input
	add	x21, x21, :lo12:fhm_input
	ldr	d0, [x21]
	ldr	d1, [x21, #8]
	movi	v2.2d, #0
	fmlal	v2.4s, v0.4h, v1.4h
	adrp	x22, fhm_expected
	add	x22, x22, :lo12:fhm_expected
	ldr	s3, [x22]
	fcmp	s2, s3
	b.ne	.Lfail_fmlal
.Lskip_asimdfhm:

	tbz	x28, #HWCAP_FLAGM, .Lskip_flagm
	/* FEAT_FlagM: CFINV inverts PSTATE.C and leaves the rest alone, so from
	 * all-clear the result is C set. Every expected NZCV word below was taken
	 * from running the same instruction natively on an arm64 host rather than
	 * derived by reading, because these encode flag semantics that are easy to
	 * transcribe backwards. */
	msr	nzcv, xzr
	cfinv
	mrs	x23, nzcv
	movz	x24, #0x2000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_cfinv

	/* RMIF rotates its source right by the shift then inserts the low four
	 * bits into NZCV under the mask. Source 5 with shift 0 and mask 0b1111
	 * gives Z and V set. */
	msr	nzcv, xzr
	mov	x25, #5
	rmif	x25, #0, #15
	mrs	x23, nzcv
	movz	x24, #0x5000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_rmif

	/* SETF8 sets N, Z and V from an 8-bit value and leaves C. 0x80 is
	 * negative and does not fit, so N and V set. SETF16 does the same for a
	 * 16-bit value. */
	msr	nzcv, xzr
	mov	w26, #0x80
	setf8	w26
	mrs	x23, nzcv
	movz	x24, #0x9000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_setf
	msr	nzcv, xzr
	movz	w26, #0x8000
	setf16	w26
	mrs	x23, nzcv
	cmp	x23, x24
	b.ne	.Lfail_setf
.Lskip_flagm:

	tbz	x29, #HWCAP2_FLAGM2, .Lskip_flagm2
	/* FEAT_FlagM2: AXFLAG rewrites the Arm floating-point unordered encoding
	 * (C and V set) into the alternate form (Z set). */
	movz	x27, #0x3000, lsl #16
	msr	nzcv, x27
	axflag
	mrs	x23, nzcv
	movz	x24, #0x4000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_axflag

	/* XAFLAG is the inverse direction: Z back to C and V. */
	movz	x27, #0x4000, lsl #16
	msr	nzcv, x27
	xaflag
	mrs	x23, nzcv
	movz	x24, #0x3000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_xaflag
.Lskip_flagm2:

	tbz	x28, #HWCAP_USCAT, .Lskip_uscat
	tbz	x28, #HWCAP_ATOMICS, .Lskip_uscat
	/* FEAT_LSE2 (HWCAP_USCAT): an LSE atomic on an address that is not
	 * naturally aligned must still complete single-copy atomically rather than
	 * fault. Offset 4 from a 16-byte aligned base is 4-byte aligned and so is
	 * unaligned for an 8-byte access. Verified natively before use. */
	add	x23, x19, #4
	movz	x24, #0x4444
	movk	x24, #0x3333, lsl #16
	movk	x24, #0x2222, lsl #32
	movk	x24, #0x1111, lsl #48
	str	x24, [x23]
	mov	x25, #1
	ldadd	x25, x26, [x23]
	cmp	x26, x24
	b.ne	.Lfail_uscat
	ldr	x27, [x23]
	add	x24, x24, #1
	cmp	x27, x24
	b.ne	.Lfail_uscat
.Lskip_uscat:

	tbz	x28, #HWCAP_SB, .Lskip_sb
	/* FEAT_SB: the speculation barrier must execute and retire. */
	sb
.Lskip_sb:

	tbz	x28, #HWCAP_DIT, .Lskip_dit
	/* FEAT_DIT: setting PSTATE.DIT must be observable through MRS. */
	msr	dit, #1
	mrs	x23, dit
	movz	x24, #0x100, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_dit
	msr	dit, #0
.Lskip_dit:

	tbz	x29, #HWCAP2_ECV, .Lskip_ecv
	/* FEAT_ECV: CNTVCTSS_EL0 reads the same count as CNTVCT_EL0, so it
	 * lies between two reads of that, and ID_AA64MMFR0_EL1.ECV, the field
	 * Linux derives HWCAP2_ECV from, reads 1. */
	isb
	mrs	x23, cntvct_el0
	isb
	mrs	x24, s3_3_c14_c0_6	/* CNTVCTSS_EL0 */
	isb
	mrs	x25, cntvct_el0
	cmp	x24, x23
	b.lo	.Lfail_ecv
	cmp	x25, x24
	b.lo	.Lfail_ecv
	mrs	x26, id_aa64mmfr0_el1
	lsr	x26, x26, #60
	cmp	x26, #1
	b.ne	.Lfail_ecv
.Lskip_ecv:

	tbz	x29, #HWCAP2_CSSC, .Lskip_cssc
	/* FEAT_CSSC: one form of each operation, and ID_AA64ISAR2_EL1.CSSC,
	 * the field Linux derives HWCAP2_CSSC from, reads 1. The words are
	 * spelled out for assemblers that predate the extension. */
	mov	x23, #0x0100
	movk	x23, #0x00f0, lsl #48	/* 0x00f0000000000100 */
	mov	x25, #-5
	.inst	0xdac01ef8		/* cnt x24, x23 */
	cmp	x24, #5
	b.ne	.Lfail_cssc
	.inst	0xdac01af8		/* ctz x24, x23 */
	cmp	x24, #8
	b.ne	.Lfail_cssc
	.inst	0xdac02338		/* abs x24, x25 */
	cmp	x24, #5
	b.ne	.Lfail_cssc
	.inst	0x91c00f38		/* smax x24, x25, #3 */
	cmp	x24, #3
	b.ne	.Lfail_cssc
	.inst	0x9ad76f38		/* umin x24, x25, x23 */
	cmp	x24, x23
	b.ne	.Lfail_cssc
	mrs	x26, s3_0_c0_c6_2	/* ID_AA64ISAR2_EL1 */
	ubfx	x26, x26, #52, #4
	cmp	x26, #1
	b.ne	.Lfail_cssc
.Lskip_cssc:

	tbz	x29, #HWCAP2_MOPS, .Lskip_mops
	/* FEAT_MOPS: a CPY whose destination overlaps above its source, which
	 * must copy as if backward, then a SET, each as the prologue, main and
	 * epilogue a compiler emits; ID_AA64ISAR2_EL1.MOPS, the field Linux
	 * derives HWCAP2_MOPS from, reads 1. The scratch bytes start as their
	 * own offsets. */
	adrp	x22, scratch
	add	x22, x22, :lo12:scratch
	mov	x23, #0
1:	strb	w23, [x22, x23]
	add	x23, x23, #1
	cmp	x23, #64
	b.ne	1b
	add	x23, x22, #8		/* bytes 0-31 to 8-39 */
	mov	x25, x22
	mov	x24, #32
	.inst	0x1d190717		/* cpyp [x23]!, [x25]!, x24! */
	.inst	0x1d590717		/* cpym [x23]!, [x25]!, x24! */
	.inst	0x1d990717		/* cpye [x23]!, [x25]!, x24! */
	cbnz	x24, .Lfail_mops
	add	x23, x22, #48		/* bytes 48-55 to 0xa5 */
	mov	x24, #8
	mov	x25, #0xa5
	.inst	0x19d90717		/* setp [x23]!, x24!, x25 */
	.inst	0x19d94717		/* setm [x23]!, x24!, x25 */
	.inst	0x19d98717		/* sete [x23]!, x24!, x25 */
	cbnz	x24, .Lfail_mops
	add	x26, x22, #56
	cmp	x23, x26
	b.ne	.Lfail_mops
	mov	x23, #0
2:	ldrb	w24, [x22, x23]
	mov	x25, x23		/* 0-7, 40-47 and 56-63 are untouched */
	cmp	x23, #8
	b.lo	3f
	sub	x25, x23, #8		/* 8-39 hold 0-31 */
	cmp	x23, #40
	b.lo	3f
	mov	x25, x23
	cmp	x23, #48
	b.lo	3f
	mov	x26, #0xa5
	cmp	x23, #56
	csel	x25, x26, x25, lo
3:	cmp	x24, x25
	b.ne	.Lfail_mops
	add	x23, x23, #1
	cmp	x23, #64
	b.ne	2b
	mrs	x26, s3_0_c0_c6_2	/* ID_AA64ISAR2_EL1 */
	ubfx	x26, x26, #16, #4
	cmp	x26, #1
	b.ne	.Lfail_mops
.Lskip_mops:

	tbz	x29, #HWCAP2_AFP, .Lno_afp
	/* FEAT_AFP: ID_AA64MMFR1_EL1.AFP, the field Linux derives HWCAP2_AFP
	 * from, reads 1 and FPCR keeps FIZ, AH and NEP. Under NEP a scalar
	 * result keeps the upper lanes of its merge source: FADD's Vn and
	 * FSQRT's Vd. Under AH, FABS leaves a NaN's sign alone, and FIZ flushes
	 * a subnormal input without raising Input Denormal. */
	mrs	x26, id_aa64mmfr1_el1
	ubfx	x26, x26, #44, #4
	cmp	x26, #1
	b.ne	.Lfail_afp
	mov	x10, #FPCR_AFP_CONTROLS
	msr	fpcr, x10
	mrs	x11, fpcr
	and	x11, x11, #FPCR_AFP_CONTROLS
	cmp	x11, #FPCR_AFP_CONTROLS
	b.ne	.Lfail_afp
	adrp	x15, afp_vectors
	add	x15, x15, :lo12:afp_vectors
	ldp	q1, q2, [x15]
	ldr	q3, [x15, #32]
	fadd	s0, s1, s2
	fsqrt	s3, s2
	fmov	x11, d0
	umov	x12, v0.d[1]
	ldp	x9, x10, [x15, #48]
	cmp	x11, x9
	b.ne	.Lfail_afp
	cmp	x12, x10
	b.ne	.Lfail_afp
	fmov	x11, d3
	umov	x12, v3.d[1]
	ldp	x9, x10, [x15, #64]
	cmp	x11, x9
	b.ne	.Lfail_afp
	cmp	x12, x10
	b.ne	.Lfail_afp
	mov	w9, #0xffc00000		/* the default NaN, sign set */
	fmov	s4, w9
	fabs	s4, s4
	fmov	w11, s4
	cmp	w11, w9
	b.ne	.Lfail_afp
	mov	w9, #1			/* the smallest positive subnormal */
	fmov	s6, w9
	fmov	s7, wzr
	msr	fpsr, xzr
	fadd	s5, s6, s7
	mrs	x12, fpsr
	fmov	w11, s5
	cbnz	w11, .Lfail_afp
	cbnz	x12, .Lfail_afp
	msr	fpcr, xzr
	b	.Ldone_afp
.Lno_afp:
	/* Without FEAT_AFP the field reads 0 and the controls are RES0. */
	mrs	x26, id_aa64mmfr1_el1
	ubfx	x26, x26, #44, #4
	cbnz	x26, .Lfail_afp
	mov	x10, #FPCR_AFP_CONTROLS
	msr	fpcr, x10
	mrs	x11, fpcr
	msr	fpcr, xzr
	tst	x11, #FPCR_AFP_CONTROLS
	b.ne	.Lfail_afp
.Ldone_afp:

	tbz	x29, #HWCAP2_RPRES, .Lskip_rpres
	/* FEAT_RPRES: ID_AA64ISAR2_EL1.RPRES, the field Linux derives
	 * HWCAP2_RPRES from, reads 1, and a single-precision FRECPE has a
	 * 12-bit mantissa under FPCR.AH and the 8-bit one otherwise. */
	mrs	x26, s3_0_c0_c6_2	/* ID_AA64ISAR2_EL1 */
	ubfx	x26, x26, #4, #4
	cmp	x26, #1
	b.ne	.Lfail_rpres
	fmov	s1, #3.0
	frecpe	s0, s1
	fmov	w11, s0
	mov	w9, #0x8000
	movk	w9, #0x3eaa, lsl #16	/* RecipEstimate(384) */
	cmp	w11, w9
	b.ne	.Lfail_rpres
	mov	x10, #FPCR_AH
	msr	fpcr, x10
	frecpe	s0, s1
	msr	fpcr, xzr
	fmov	w11, s0
	mov	w9, #0xa000
	movk	w9, #0x3eaa, lsl #16	/* RecipEstimate(3072) at 12 bits */
	cmp	w11, w9
	b.ne	.Lfail_rpres
	msr	fpsr, xzr
.Lskip_rpres:

	/* FEAT_SME: SMSTART turns on SM and ZA, so SVCR reads 3; RDSVL and a
	 * streaming CNTB report the same vector length, one an implementation
	 * can have; TPIDR2_EL0 holds what is written to it; LDR and STR of a
	 * ZA vector round-trip; MOVA reads that slice into Z3, a streaming
	 * SVE ADD and ST1B store it plus one; SMSTOP leaves SVCR 0. */
	tbz	x29, #HWCAP2_SME, .Lskip_sme
	adrp	x19, sme_source
	add	x19, x19, :lo12:sme_source
	adrp	x20, sme_row
	add	x20, x20, :lo12:sme_row
	adrp	x21, sme_sum
	add	x21, x21, :lo12:sme_sum
	mov	x9, #0
	mov	w10, #3
.Lsme_fill:
	strb	w10, [x19, x9]
	add	w10, w10, #7
	add	x9, x9, #1
	cmp	x9, #256
	b.lo	.Lsme_fill
	mov	x13, #0x5a5a
	movk	x13, #0x1234, lsl #48
	smstart
	mrs	x22, svcr
	rdsvl	x23, #1
	cntb	x24
	msr	tpidr2_el0, x13
	mrs	x25, tpidr2_el0
	msr	tpidr2_el0, xzr
	mov	w12, #0
	ldr	za[w12, 0], [x19]
	str	za[w12, 0], [x20]
	ptrue	p0.b
	mova	z3.b, p0/m, za0h.b[w12, 0]
	add	z3.b, z3.b, #1
	st1b	{z3.b}, p0, [x21]
	smstop
	mrs	x26, svcr
	cmp	x22, #3
	b.ne	.Lfail_sme
	cbnz	x26, .Lfail_sme
	cmp	x23, x24
	b.ne	.Lfail_sme
	cmp	x23, #16
	b.lo	.Lfail_sme
	cmp	x23, #256
	b.hi	.Lfail_sme
	sub	x9, x23, #1
	tst	x23, x9
	b.ne	.Lfail_sme
	cmp	x25, x13
	b.ne	.Lfail_sme
	mov	x9, #0
.Lsme_compare:
	ldrb	w10, [x19, x9]
	ldrb	w11, [x20, x9]
	cmp	w10, w11
	b.ne	.Lfail_sme
	ldrb	w11, [x21, x9]
	add	w10, w10, #1
	and	w10, w10, #0xff
	cmp	w10, w11
	b.ne	.Lfail_sme
	add	x9, x9, #1
	cmp	x9, x23
	b.lo	.Lsme_compare
.Lskip_sme:

	/* FEAT_SME_F32F32: FMOPA of 1.5 and 2.0 in every single-precision
	 * element gives 3.0. */
	sme_enter .Lskip_sme_f32f32, HWCAP2_SME_F32F32
	fmov	z1.s, #1.5
	fmov	z2.s, #2.0
	fmopa	za0.s, p0/m, p0/m, z1.s, z2.s
	sme_leave
	mov	w11, #0x40400000		/* 3.0 */
	check_row 4, w11, .Lfail_sme_f32f32
.Lskip_sme_f32f32:

	/* FEAT_SME_B16F32: BFMOPA adds the two BFloat16 products 1.5 x 2.5
	 * into each single-precision element: 7.5. */
	sme_enter .Lskip_sme_b16f32, HWCAP2_SME_B16F32
	mov	w9, #0x3fc0			/* 1.5 */
	dup	z1.h, w9
	mov	w9, #0x4020			/* 2.5 */
	dup	z2.h, w9
	bfmopa	za0.s, p0/m, p0/m, z1.h, z2.h
	sme_leave
	mov	w11, #0x40f00000		/* 7.5 */
	check_row 4, w11, .Lfail_sme_b16f32
.Lskip_sme_b16f32:

	/* FEAT_SME_F16F32: the half-precision FMOPA does the same: 7.5. */
	sme_enter .Lskip_sme_f16f32, HWCAP2_SME_F16F32
	fmov	z1.h, #1.5
	fmov	z2.h, #2.5
	fmopa	za0.s, p0/m, p0/m, z1.h, z2.h
	sme_leave
	mov	w11, #0x40f00000		/* 7.5 */
	check_row 4, w11, .Lfail_sme_f16f32
.Lskip_sme_f16f32:

	/* FEAT_SME_I8I32: SMOPA adds four signed byte products -3 x 5 into
	 * each 32-bit element: -60. */
	sme_enter .Lskip_sme_i8i32, HWCAP2_SME_I8I32
	dup	z1.b, #-3
	dup	z2.b, #5
	smopa	za0.s, p0/m, p0/m, z1.b, z2.b
	sme_leave
	mov	w11, #-60
	check_row 4, w11, .Lfail_sme_i8i32
.Lskip_sme_i8i32:

	/* FEAT_SME_F64F64: FMOPA of 1.5 and 2.0 in every double-precision
	 * element gives 3.0. */
	sme_enter .Lskip_sme_f64f64, HWCAP2_SME_F64F64
	fmov	z1.d, #1.5
	fmov	z2.d, #2.0
	fmopa	za0.d, p0/m, p0/m, z1.d, z2.d
	sme_leave
	mov	x11, #0x4008000000000000	/* 3.0 */
	check_row 8, x11, .Lfail_sme_f64f64
.Lskip_sme_f64f64:

	/* FEAT_SME_I16I64: SMOPA adds four signed halfword products
	 * -300 x 1000 into each 64-bit element: -1,200,000. */
	sme_enter .Lskip_sme_i16i64, HWCAP2_SME_I16I64
	mov	w9, #-300
	dup	z1.h, w9
	mov	w9, #1000
	dup	z2.h, w9
	smopa	za0.d, p0/m, p0/m, z1.h, z2.h
	sme_leave
	movn	x11, #0x12, lsl #16
	movk	x11, #0xb080			/* -1,200,000 */
	check_row 8, x11, .Lfail_sme_i16i64
.Lskip_sme_i16i64:

	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	b	.Lexit

.Lfail_ldapr:		fail E_LDAPR, msg_ldapr
.Lfail_ldapur:		fail E_LDAPUR, msg_ldapur
.Lfail_stlur:		fail E_STLUR, msg_stlur
.Lfail_fcadd:		fail E_FCADD, msg_fcadd
.Lfail_fcmla:		fail E_FCMLA, msg_fcmla
.Lfail_sdot:		fail E_SDOT, msg_sdot
.Lfail_udot:		fail E_UDOT, msg_udot
.Lfail_sqrdmlah:	fail E_SQRDMLAH, msg_sqrdmlah
.Lfail_fjcvtzs:		fail E_FJCVTZS, msg_fjcvtzs
.Lfail_frint32z:	fail E_FRINT32Z, msg_frint32z
.Lfail_fp16:		fail E_FP16, msg_fp16
.Lfail_fmlal:		fail E_FMLAL, msg_fmlal
.Lfail_cfinv:		fail E_CFINV, msg_cfinv
.Lfail_rmif:		fail E_RMIF, msg_rmif
.Lfail_setf:		fail E_SETF, msg_setf
.Lfail_axflag:		fail E_AXFLAG, msg_axflag
.Lfail_xaflag:		fail E_XAFLAG, msg_xaflag
.Lfail_uscat:		fail E_USCAT, msg_uscat
.Lfail_dit:		fail E_DIT, msg_dit
.Lfail_auxv:		fail E_AUXV, msg_auxv
.Lfail_jscvt_value:	fail E_JSCVT_VALUE, msg_jscvt_value
.Lfail_jscvt_nzcv:	fail E_JSCVT_NZCV, msg_jscvt_nzcv
.Lfail_jscvt_fpsr:	fail E_JSCVT_FPSR, msg_jscvt_fpsr
.Lfail_ecv:		fail E_ECV, msg_ecv
.Lfail_cssc:		fail E_CSSC, msg_cssc
.Lfail_mops:		fail E_MOPS, msg_mops
.Lfail_afp:		fail E_AFP, msg_afp
.Lfail_rpres:		fail E_RPRES, msg_rpres
.Lfail_sme:		fail E_SME, msg_sme
.Lfail_sme_f32f32:	fail E_SME_F32F32, msg_sme_f32f32
.Lfail_sme_b16f32:	fail E_SME_B16F32, msg_sme_b16f32
.Lfail_sme_f16f32:	fail E_SME_F16F32, msg_sme_f16f32
.Lfail_sme_i8i32:	fail E_SME_I8I32, msg_sme_i8i32
.Lfail_sme_f64f64:	fail E_SME_F64F64, msg_sme_f64f64
.Lfail_sme_i16i64:	fail E_SME_I16I64, msg_sme_i16i64

.Lexit:
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
	.p2align 3
fcadd_n:
	.float 1.0, 2.0			/* n = (re 1.0, im 2.0) */
	.float 3.0, 4.0			/* m = (re 3.0, im 4.0) */
fcadd_expected:
	.float -3.0, 5.0
fcmla_expected:
	.float 3.0, 4.0
dot_a:
	.byte 1, 2, 3, 4, 0, 0, 0, 0
	.byte 5, 6, 7, 8, 0, 0, 0, 0
rdm_input:
	.hword 0x4000, 0, 0, 0
jscvt_input:
	.double -2.5
	/* input, FPCR, result, NZCV, FPSR; FZ is FPCR bit 24, and the FPSR
	 * flags are IOC 0x01, IXC 0x10 and IDC 0x80. Derived from the Arm ARM's
	 * FPToFixedJS and FPUnpack. */
	.equ FZ, 1 << 24
	.equ JSCVT_Z, 0x40000000
jscvt_cases:
	.quad 0x0000000000000001, FZ, 0, 0, 0x80		/* +subnormal, flushed */
	.quad 0x000fffffffffffff, FZ, 0, 0, 0x80		/* largest +subnormal */
	.quad 0x8000000000000001, FZ, 0, 0, 0x80		/* -subnormal, flushed */
	.quad 0x0000000000000001, 0, 0, 0, 0x10		/* +subnormal, inexact */
	.quad 0x8000000000000001, 0, 0, 0, 0x10		/* -subnormal, inexact */
	.quad 0x0000000000000000, FZ, 0, JSCVT_Z, 0		/* +0.0 is exact */
	.quad 0x8000000000000000, FZ, 0, 0, 0		/* -0.0 is not */
	.quad 0x7ff8000000000000, FZ, 0, 0, 0x01		/* quiet NaN */
	.quad 0x7ff4000000000000, FZ, 0, 0, 0x01		/* signalling NaN */
	.quad 0x7ff0000000000000, FZ, 0, 0, 0x01		/* +infinity */
	.quad 0xfff0000000000000, FZ, 0, 0, 0x01		/* -infinity */
	.quad 0x41e0000000000000, FZ, 0x80000000, 0, 0x01	/* 2^31 overflows */
	.quad 0xc1e0000000000000, FZ, 0x80000000, JSCVT_Z, 0	/* -2^31 fits */
	.quad 0xc1e0000000200000, FZ, 0x7fffffff, 0, 0x01	/* -2^31 - 1 */
	.quad 0x41f0000000500000, FZ, 5, 0, 0x01		/* 2^32 + 5 wraps */
	.quad 0x4014000000000000, FZ, 5, JSCVT_Z, 0		/* 5.0 */
	.if (. - jscvt_cases) != JSCVT_CASES * 40
	.error "JSCVT_CASES does not count jscvt_cases"
	.endif
	.p2align 4
afp_vectors:
	.word 0x3f800000, 0x11111111, 0x22222222, 0x33333333	/* Vn: 1.0 */
	.word 0x40800000, 0x55555555, 0x66666666, 0x77777777	/* Vm: 4.0 */
	.word 0x00000000, 0x44444444, 0x88888888, 0x99999999	/* FSQRT's Vd */
	.quad 0x1111111140a00000, 0x3333333322222222	/* FADD: 5.0 over Vn */
	.quad 0x4444444440000000, 0x9999999988888888	/* FSQRT: 2.0 over Vd */
frint_input:
	.double 2.7
frint_expected:
	.double 2.0
fp16_input:
	.hword 0x3e00			/* 1.5 */
	.hword 0x4100			/* 2.5 */
	.hword 0x4400			/* 4.0 */
	.hword 0
fhm_input:
	.hword 0x3e00, 0, 0, 0		/* 1.5 */
	.hword 0x4100, 0, 0, 0		/* 2.5 */
fhm_expected:
	.float 3.75
msg_ldapr:
	.ascii "FAIL ldapr      \n"
msg_ldapur:
	.ascii "FAIL ldapur     \n"
msg_stlur:
	.ascii "FAIL stlur      \n"
msg_fcadd:
	.ascii "FAIL fcadd      \n"
msg_fcmla:
	.ascii "FAIL fcmla      \n"
msg_sdot:
	.ascii "FAIL sdot       \n"
msg_udot:
	.ascii "FAIL udot       \n"
msg_sqrdmlah:
	.ascii "FAIL sqrdmlah   \n"
msg_fjcvtzs:
	.ascii "FAIL fjcvtzs    \n"
msg_frint32z:
	.ascii "FAIL frint32z   \n"
msg_fp16:
	.ascii "FAIL fp16       \n"
msg_fmlal:
	.ascii "FAIL fmlal      \n"
msg_cfinv:
	.ascii "FAIL cfinv      \n"
msg_rmif:
	.ascii "FAIL rmif       \n"
msg_setf:
	.ascii "FAIL setf       \n"
msg_axflag:
	.ascii "FAIL axflag     \n"
msg_xaflag:
	.ascii "FAIL xaflag     \n"
msg_uscat:
	.ascii "FAIL uscat      \n"
msg_dit:
	.ascii "FAIL dit        \n"
msg_auxv:
	.ascii "FAIL auxv       \n"
msg_jscvt_value:
	.ascii "FAIL jscvt value\n"
msg_jscvt_nzcv:
	.ascii "FAIL jscvt nzcv \n"
msg_jscvt_fpsr:
	.ascii "FAIL jscvt fpsr \n"
msg_ecv:
	.ascii "FAIL ecv        \n"
msg_cssc:
	.ascii "FAIL cssc       \n"
msg_mops:
	.ascii "FAIL mops       \n"
msg_afp:
	.ascii "FAIL afp        \n"
msg_rpres:
	.ascii "FAIL rpres      \n"
msg_sme:
	.ascii "FAIL sme        \n"
msg_sme_f32f32:
	.ascii "FAIL smef32f32  \n"
msg_sme_b16f32:
	.ascii "FAIL smeb16f32  \n"
msg_sme_f16f32:
	.ascii "FAIL smef16f32  \n"
msg_sme_i8i32:
	.ascii "FAIL smei8i32   \n"
msg_sme_f64f64:
	.ascii "FAIL smef64f64  \n"
msg_sme_i16i64:
	.ascii "FAIL smei16i64  \n"
success_message:
	.ascii "OK\n"

	.section .bss
	.p2align 4
scratch:
	.space 64
	/* The largest vector length is 256 bytes. */
sme_source:
	.space 256
sme_row:
	.space 256
sme_sum:
	.space 256
