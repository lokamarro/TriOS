/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed and scalar SSE/AVX floating-point arithmetic and comparisons. */

/* The same for the scalar result in lane 0 of v6, from v0 and v1, which is
 * not zero and at most the smallest normal in magnitude, raising underflow
 * and precision as it flushes. */
.macro vector_fp_flush_tiny_scalar operation, element
	.set Lx86_vector_fp_rounded\@, 0
	.ifc \operation,mul
	.set Lx86_vector_fp_rounded\@, 1
	.endif
	.ifc \operation,div
	.set Lx86_vector_fp_rounded\@, 1
	.endif
	.if \element == 32
	fmov w14, s6
	and w14, w14, #0x7fffffff
	cmp w14, #0x800, lsl #12
	.if Lx86_vector_fp_rounded\@
	b.ne Lx86_vector_fp_flush\@
	mrs x15, fpsr
	mov w14, #0x5f800000
	fmov s16, w14
	fmul s16, s0, s16
	.ifc \operation,mul
	fmul s16, s16, s1
	.else
	fdiv s16, s16, s1
	.endif
	msr fpsr, x15
	fmov w14, s16
	and w14, w14, #0x7fffffff
	mov w15, #0x20800000
	cmp w14, w15
	b.hs Lx86_vector_fp_tiny_done\@
	.else
	b.eq Lx86_vector_fp_tiny_done\@
	.endif
Lx86_vector_fp_flush\@:
	mxcsr_raise 0x30, 14
	fmov w14, s6
	and w14, w14, #0x80000000
	fmov s6, w14
	.else
	fmov x14, d6
	and x14, x14, #0x7fffffffffffffff
	mov x15, #0x0010000000000000
	cmp x14, x15
	.if Lx86_vector_fp_rounded\@
	b.ne Lx86_vector_fp_flush\@
	mrs x15, fpsr
	mov x14, #0x5ff0000000000000
	fmov d16, x14
	fmul d16, d0, d16
	.ifc \operation,mul
	fmul d16, d16, d1
	.else
	fdiv d16, d16, d1
	.endif
	msr fpsr, x15
	fmov x14, d16
	and x14, x14, #0x7fffffffffffffff
	mov x15, #0x2010000000000000
	cmp x14, x15
	b.hs Lx86_vector_fp_tiny_done\@
	.else
	b.eq Lx86_vector_fp_tiny_done\@
	.endif
Lx86_vector_fp_flush\@:
	mxcsr_raise 0x30, 14
	fmov x14, d6
	and x14, x14, #0x8000000000000000
	fmov d6, x14
	.endif
Lx86_vector_fp_tiny_done\@:
.endm

/* One comparison of a whole vector, or of lane 0 alone for a scalar
 * instruction, whose other lanes x86 does not compare and so must raise no
 * exception. */
.macro vector_fp_cmp operation, element, scalar, output, left, right
	.if \scalar
	.if \element == 32
	\operation s\output, s\left, s\right
	.else
	\operation d\output, d\left, d\right
	.endif
	.elseif \element == 32
	\operation v\output\().4s, v\left\().4s, v\right\().4s
	.else
	\operation v\output\().2d, v\left\().2d, v\right\().2d
	.endif
.endm

/* x86's 32 comparison predicates: 0 to 15 and, from VEX, 16 to 31, which
 * repeat them with the other choice of whether a quiet NaN raises invalid
 * operation. A signalling NaN always does. The host's FCMEQ is quiet and its
 * FCMGE and FCMGT signal, as x86's equality and ordering relations do in 0
 * to 15. For 16 to 31, a quiet ordering compares the ordered lanes after
 * NaN lanes become zeros, and a signalling equality first runs FCMGE for
 * its flags alone; FALSE and TRUE run FCMEQ or FCMGE for their flags. Uses
 * v16 and v17. */
.macro vector_fp_compare_mask predicate, element, scalar, output, left, right, temporary
	.set Lx86_vector_fp_base\@, (\predicate & 15)
	.set Lx86_vector_fp_signalling\@, 0
	.if Lx86_vector_fp_base\@ == 1 || Lx86_vector_fp_base\@ == 2 || \
		Lx86_vector_fp_base\@ == 5 || Lx86_vector_fp_base\@ == 6 || \
		Lx86_vector_fp_base\@ == 9 || Lx86_vector_fp_base\@ == 10 || \
		Lx86_vector_fp_base\@ == 13 || Lx86_vector_fp_base\@ == 14
	.set Lx86_vector_fp_signalling\@, 1
	.endif
	.if \predicate >= 16
	.set Lx86_vector_fp_signalling\@, !Lx86_vector_fp_signalling\@
	.endif
	.set Lx86_vector_fp_quiet_order\@, \
		(\predicate >= 16 && !Lx86_vector_fp_signalling\@)
	.if Lx86_vector_fp_signalling\@ && \predicate >= 16
	/* A signalling equality, unordered test, FALSE or TRUE. */
	vector_fp_cmp fcmge, \element, \scalar, \temporary, \left, \right
	.elseif !Lx86_vector_fp_signalling\@ && \
		(Lx86_vector_fp_base\@ == 11 || Lx86_vector_fp_base\@ == 15)
	/* A quiet FALSE or TRUE. */
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \left, \right
	.endif
	.if Lx86_vector_fp_quiet_order\@
	/* The ordered lanes in v`temporary`, and v16 and v17 the sources with
	 * NaN lanes zero; FCMEQ raises invalid operation for a signalling NaN
	 * alone, and nothing compared after it sees a NaN. */
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \left, \left
	vector_fp_cmp fcmeq, \element, \scalar, \output, \right, \right
	and v\temporary\().16b, v\temporary\().16b, v\output\().16b
	and v16.16b, v\left\().16b, v\temporary\().16b
	and v17.16b, v\right\().16b, v\temporary\().16b
	vector_fp_relation Lx86_vector_fp_base\@, \element, \scalar, \output, \
		16, 17, \temporary, 1
	.else
	vector_fp_relation Lx86_vector_fp_base\@, \element, \scalar, \output, \
		\left, \right, \temporary, 0
	.endif
.endm

/* Predicate `base` (0 to 15) of v`left` and v`right`. With `ordered`, v`left`
 * and v`right` hold no NaN and v`temporary` the ordered lanes, which the
 * relation is then limited to: an ordered predicate holds only there and an
 * unordered one everywhere else. */
.macro vector_fp_relation base, element, scalar, output, left, right, temporary, ordered
	.if \base == 0
	vector_fp_cmp fcmeq, \element, \scalar, \output, \left, \right
	.elseif \base == 1
	vector_fp_cmp fcmgt, \element, \scalar, \output, \right, \left
	.elseif \base == 2
	vector_fp_cmp fcmge, \element, \scalar, \output, \right, \left
	.elseif \base == 3
	vector_fp_cmp fcmeq, \element, \scalar, \output, \left, \left
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \right, \right
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	mvn v\output\().16b, v\output\().16b
	.elseif \base == 4
	vector_fp_cmp fcmeq, \element, \scalar, \output, \left, \right
	mvn v\output\().16b, v\output\().16b
	.elseif \base == 5
	vector_fp_cmp fcmgt, \element, \scalar, \output, \right, \left
	.if \ordered
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.endif
	mvn v\output\().16b, v\output\().16b
	.elseif \base == 6
	vector_fp_cmp fcmge, \element, \scalar, \output, \right, \left
	.if \ordered
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.endif
	mvn v\output\().16b, v\output\().16b
	.elseif \base == 7
	vector_fp_cmp fcmeq, \element, \scalar, \output, \left, \left
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \right, \right
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \base == 8
	vector_fp_cmp fcmeq, \element, \scalar, \output, \left, \left
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \right, \right
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	mvn v\output\().16b, v\output\().16b
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \left, \right
	orr v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \base == 9
	vector_fp_cmp fcmge, \element, \scalar, \output, \left, \right
	.if \ordered
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.endif
	mvn v\output\().16b, v\output\().16b
	.elseif \base == 10
	vector_fp_cmp fcmgt, \element, \scalar, \output, \left, \right
	.if \ordered
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.endif
	mvn v\output\().16b, v\output\().16b
	.elseif \base == 11
	movi v\output\().16b, #0
	.elseif \base == 12
	vector_fp_cmp fcmeq, \element, \scalar, \output, \left, \left
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \right, \right
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	vector_fp_cmp fcmeq, \element, \scalar, \temporary, \left, \right
	bic v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \base == 13
	vector_fp_cmp fcmge, \element, \scalar, \output, \left, \right
	.elseif \base == 14
	vector_fp_cmp fcmgt, \element, \scalar, \output, \left, \right
	.else
	movi v\output\().16b, #0xff
	.endif
	.if \ordered && (\base == 1 || \base == 2 || \base == 13 || \base == 14)
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.endif
.endm

.macro vector_fp_regular_value operation, element, output, left, right
	.if \element == 32
	.ifc \operation,sqrt
	fsqrt s\output, s\right
	.endif
	.ifc \operation,rsqrt
	fsqrt s\output, s\right
	fmov s7, #1.0
	fdiv s\output, s7, s\output
	.endif
	.ifc \operation,rcp
	fmov s7, #1.0
	fdiv s\output, s7, s\right
	.endif
	.ifc \operation,add
	fadd s\output, s\left, s\right
	.endif
	.ifc \operation,mul
	fmul s\output, s\left, s\right
	.endif
	.ifc \operation,sub
	fsub s\output, s\left, s\right
	.endif
	.ifc \operation,div
	fdiv s\output, s\left, s\right
	.endif
	.ifc \operation,min
	fcmgt s\output, s\right, s\left
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt s\output, s\left, s\right
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.else
	.ifc \operation,sqrt
	fsqrt d\output, d\right
	.endif
	.ifc \operation,rsqrt
	fsqrt d\output, d\right
	fmov d7, #1.0
	fdiv d\output, d7, d\output
	.endif
	.ifc \operation,rcp
	fmov d7, #1.0
	fdiv d\output, d7, d\right
	.endif
	.ifc \operation,add
	fadd d\output, d\left, d\right
	.endif
	.ifc \operation,mul
	fmul d\output, d\left, d\right
	.endif
	.ifc \operation,sub
	fsub d\output, d\left, d\right
	.endif
	.ifc \operation,div
	fdiv d\output, d\left, d\right
	.endif
	.ifc \operation,min
	fcmgt d\output, d\right, d\left
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt d\output, d\left, d\right
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.endif
.endm

.macro vector_fp_packed_value operation, element, output, left, right
	.if \element == 32
	.ifc \operation,sqrt
	fsqrt v\output\().4s, v\right\().4s
	.endif
	.ifc \operation,rsqrt
	fsqrt v\output\().4s, v\right\().4s
	fmov v7.4s, #1.0
	fdiv v\output\().4s, v7.4s, v\output\().4s
	.endif
	.ifc \operation,rcp
	fmov v7.4s, #1.0
	fdiv v\output\().4s, v7.4s, v\right\().4s
	.endif
	.ifc \operation,add
	fadd v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,mul
	fmul v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,sub
	fsub v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,div
	fdiv v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,min
	fcmgt v\output\().4s, v\right\().4s, v\left\().4s
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt v\output\().4s, v\left\().4s, v\right\().4s
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.else
	.ifc \operation,sqrt
	fsqrt v\output\().2d, v\right\().2d
	.endif
	.ifc \operation,rsqrt
	fsqrt v\output\().2d, v\right\().2d
	fmov v7.2d, #1.0
	fdiv v\output\().2d, v7.2d, v\output\().2d
	.endif
	.ifc \operation,rcp
	fmov v7.2d, #1.0
	fdiv v\output\().2d, v7.2d, v\right\().2d
	.endif
	.ifc \operation,add
	fadd v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,mul
	fmul v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,sub
	fsub v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,div
	fdiv v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,min
	fcmgt v\output\().2d, v\right\().2d, v\left\().2d
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt v\output\().2d, v\left\().2d, v\right\().2d
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.endif
.endm


/* A result with an exponent of all ones or one below takes the cold path,
 * which leaves while the gadgets check SIMD exceptions -- the path for any
 * result that could have raised one the guest unmasks -- and otherwise
 * repairs a NaN result. */
.macro vector_fp_fix_nan element, output, left, right, binary, scalar
	.if \scalar
	simd_special_scalar \element, \output, Lx86_vector_fp_nan_ready\@, lo
	.else
	simd_special_lanes \element, \output
	cbz w14, Lx86_vector_fp_nan_ready\@
	.endif
	b Lx86_vector_fp_special\@
Lx86_vector_fp_nan_ready\@:
	.pushsection GADGET_SECTION_COLD
Lx86_vector_fp_special\@:
	simd_leave_computed_if_checking
	simd_x86_nan \element, \output, \left, \right, \binary, \scalar
	b Lx86_vector_fp_nan_ready\@
	.popsection
.endm

.macro vector_fp_fix_generated_nan operation, element, output, left, right, scalar
	.ifc \operation,sqrt
	vector_fp_fix_nan \element, \output, \left, \right, 0, \scalar
	.endif
	.ifc \operation,add
	vector_fp_fix_nan \element, \output, \left, \right, 1, \scalar
	.endif
	.ifc \operation,mul
	vector_fp_fix_nan \element, \output, \left, \right, 1, \scalar
	.endif
	.ifc \operation,sub
	vector_fp_fix_nan \element, \output, \left, \right, 1, \scalar
	.endif
	.ifc \operation,div
	vector_fp_fix_nan \element, \output, \left, \right, 1, \scalar
	.endif
.endm

/* MXCSR.FTZ flushes a tiny result to zero. Only sums, differences, products
 * and quotients can be tiny -- a square root never is, minimum and maximum
 * return a source unchanged, and the estimates ignore MXCSR -- and only
 * results no greater than the smallest normal take the cold path. */
.macro vector_fp_flush_to_zero operation, element, width, scalar
	.set Lx86_vector_fp_tiny_result\@, 0
	.ifc \operation,add
	.set Lx86_vector_fp_tiny_result\@, 1
	.endif
	.ifc \operation,sub
	.set Lx86_vector_fp_tiny_result\@, 1
	.endif
	.ifc \operation,mul
	.set Lx86_vector_fp_tiny_result\@, 1
	.endif
	.ifc \operation,div
	.set Lx86_vector_fp_tiny_result\@, 1
	.endif
	.if Lx86_vector_fp_tiny_result\@
	tbz w13, #2, Lx86_vector_fp_ftz_ready\@
	tbz w12, #15, Lx86_vector_fp_ftz_ready\@
	.if \scalar
	simd_small_scalar \element
	b.ne Lx86_vector_fp_ftz_ready\@
	.else
	simd_small_lanes \element, 4, 17, 18
	.if \width == 256
	simd_small_lanes \element, 5, 16, 18
	orr v17.16b, v17.16b, v16.16b
	.endif
	umaxv s17, v17.4s
	fmov w14, s17
	cbz w14, Lx86_vector_fp_ftz_ready\@
	.endif
	b Lx86_vector_fp_tiny\@
Lx86_vector_fp_ftz_ready\@:
	.pushsection GADGET_SECTION_COLD
Lx86_vector_fp_tiny\@:
	.if \scalar
	vector_fp_flush_tiny_scalar \operation, \element
	.else
	simd_flush_tiny_packed \operation, \element, 4, 0, 1
	.if \width == 256
	simd_flush_tiny_packed \operation, \element, 5, 2, 3
	.endif
	.endif
	b Lx86_vector_fp_ftz_ready\@
	.popsection
	.endif
.endm

/* RCPPS, RCPSS, RSQRTPS and RSQRTSS ignore MXCSR and raise nothing. They
 * treat a denormal input as zero, and RCP returns a zero of the input's sign
 * for a finite input of 2^126 or more. The host's exact quotient lies within
 * x86's estimate error everywhere else. v`mask` = all ones in the lanes of
 * v`input` that need more than that quotient: for RCP an exponent of zero or
 * 0xfd and above, for RSQRT an exponent of zero or a negative sign, whose
 * x86 result is the indefinite unless it is a zero or a NaN. Uses
 * v`temporary`. */
.macro vector_fp_estimate_lanes operation, input, mask, temporary
	shl v\mask\().4s, v\input\().4s, #1
	.ifc \operation,rcp
	movi v\temporary\().4s, #0x03, lsl #24
	add v\mask\().4s, v\mask\().4s, v\temporary\().4s
	movi v\temporary\().4s, #0x04, lsl #24
	cmhi v\mask\().4s, v\temporary\().4s, v\mask\().4s
	.else
	movi v\temporary\().4s, #0x01, lsl #24
	cmhi v\mask\().4s, v\temporary\().4s, v\mask\().4s
	cmlt v\temporary\().4s, v\input\().4s, #0
	orr v\mask\().16b, v\mask\().16b, v\temporary\().16b
	.endif
.endm

.macro vector_fp_estimate_special_packed operation, output, input
	simd_flush_denormals \input, 32
	vector_fp_packed_value \operation, 32, \output, \input, \input
	.ifc \operation,rcp
	shl v17.4s, v\input\().4s, #1
	movi v18.4s, #0xfd, lsl #24
	cmhs v18.4s, v17.4s, v18.4s
	movi v16.4s, #0xff, lsl #24
	cmhi v17.4s, v16.4s, v17.4s
	and v17.16b, v17.16b, v18.16b
	movi v16.4s, #0x80, lsl #24
	and v16.16b, v\input\().16b, v16.16b
	.else
	fcmeq v17.4s, v\output\().4s, v\output\().4s
	fcmeq v18.4s, v\input\().4s, v\input\().4s
	bic v17.16b, v18.16b, v17.16b
	mov w14, #0xffc00000
	dup v16.4s, w14
	.endif
	bsl v17.16b, v16.16b, v\output\().16b
	mov v\output\().16b, v17.16b
.endm

.macro vector_fp_estimate_special_scalar operation
	simd_flush_denormals 1, 32
	vector_fp_regular_value \operation, 32, 6, 1, 1
	.ifc \operation,rcp
	fmov w14, s1
	ubfx w15, w14, #23, #8
	cmp w15, #0xfd
	b.lo Lx86_vector_fp_estimate_kept\@
	cmp w15, #0xff
	b.eq Lx86_vector_fp_estimate_kept\@
	and w14, w14, #0x80000000
	fmov s6, w14
	.else
	fcmp s6, s6
	b.vc Lx86_vector_fp_estimate_kept\@
	fcmp s1, s1
	b.vs Lx86_vector_fp_estimate_kept\@
	mov w14, #0xffc00000
	fmov s6, w14
	.endif
Lx86_vector_fp_estimate_kept\@:
.endm

/* The host's quotient for an estimate's regular inputs raises precision
 * alone, and x86 raises nothing. Precision is sticky: while MXCSR holds it,
 * or FPSR does, one more changes nothing, and the quotient runs as it is;
 * otherwise `unsticky`, in the cold section, keeps FPSR around it. When
 * FPSR holds it, `unsticky` adds it to MXCSR, as the run would at its end,
 * so that the next estimate need not read FPSR. The special inputs' path
 * keeps FPSR in any case. Uses w14, MXCSR from here. */
.macro vector_fp_estimate_sticky unsticky
	ldr w14, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w14, #5, \unsticky
.endm

.macro vector_fp_estimate_regular operation, width, scalar
	.if \scalar
	vector_fp_regular_value \operation, 32, 6, 1, 1
	.else
	vector_fp_packed_value \operation, 32, 4, 1, 1
	.if \width == 256
	vector_fp_packed_value \operation, 32, 5, 3, 3
	.endif
	.endif
.endm

.macro vector_fp_estimate operation, width, scalar
	.if \scalar
	fmov w14, s1
	.ifc \operation,rcp
	ubfx w15, w14, #23, #8
	add w15, w15, #3
	tst w15, #0xfc
	b.ne Lx86_vector_fp_estimate_regular\@
	.else
	tst w14, #0x7f800000
	b.eq Lx86_vector_fp_estimate_cold\@
	tbz w14, #31, Lx86_vector_fp_estimate_regular\@
Lx86_vector_fp_estimate_cold\@:
	.endif
	b Lx86_vector_fp_estimate_special\@
Lx86_vector_fp_estimate_unsticky_stub\@:
	b Lx86_vector_fp_estimate_unsticky\@
Lx86_vector_fp_estimate_regular\@:
	vector_fp_estimate_sticky Lx86_vector_fp_estimate_unsticky_stub\@
Lx86_vector_fp_estimate_plain\@:
	vector_fp_estimate_regular \operation, \width, \scalar
Lx86_vector_fp_estimate_ready\@:
	mov v4.s[0], v6.s[0]
	.else
	vector_fp_estimate_lanes \operation, 1, 17, 18
	.if \width == 256
	vector_fp_estimate_lanes \operation, 3, 16, 18
	orr v17.16b, v17.16b, v16.16b
	.endif
	umaxv s17, v17.4s
	fmov w14, s17
	cbz w14, Lx86_vector_fp_estimate_regular\@
	b Lx86_vector_fp_estimate_special\@
Lx86_vector_fp_estimate_unsticky_stub\@:
	b Lx86_vector_fp_estimate_unsticky\@
Lx86_vector_fp_estimate_regular\@:
	vector_fp_estimate_sticky Lx86_vector_fp_estimate_unsticky_stub\@
Lx86_vector_fp_estimate_plain\@:
	vector_fp_estimate_regular \operation, \width, \scalar
Lx86_vector_fp_estimate_ready\@:
	.endif
	.pushsection GADGET_SECTION_COLD
Lx86_vector_fp_estimate_unsticky\@:
	mrs x15, fpsr
	tbz x15, #4, Lx86_vector_fp_estimate_guarded\@
	orr w14, w14, #0x20
	str w14, [_vcpu, #UML_X86_64_CPU_MXCSR]
	b Lx86_vector_fp_estimate_plain\@
Lx86_vector_fp_estimate_guarded\@:
	vector_fp_estimate_regular \operation, \width, \scalar
	msr fpsr, x15
	b Lx86_vector_fp_estimate_ready\@
Lx86_vector_fp_estimate_special\@:
	mrs x16, fpsr
	.if \scalar
	vector_fp_estimate_special_scalar \operation
	.else
	vector_fp_estimate_special_packed \operation, 4, 1
	.if \width == 256
	vector_fp_estimate_special_packed \operation, 5, 3
	.endif
	.endif
	msr fpsr, x16
	b Lx86_vector_fp_estimate_ready\@
	.popsection
.endm

/* Comparisons, minimum and maximum raise invalid operation for a NaN input,
 * whatever their result. */
.macro vector_fp_nan_inputs element, width, scalar
	.if \scalar
	simd_check_scalar_nan \element, 0, 1
	.elseif \width == 256
	simd_check_packed_nan \element, 0, 1, 2, 3
	.else
	simd_check_packed_nan \element, 0, 1
	.endif
.endm

.macro vector_fp_check_inputs operation, element, width, scalar
	.ifc \operation,compare
	vector_fp_nan_inputs \element, \width, \scalar
	.endif
	.ifc \operation,min
	vector_fp_nan_inputs \element, \width, \scalar
	.endif
	.ifc \operation,max
	vector_fp_nan_inputs \element, \width, \scalar
	.endif
.endm

/* Lx86_vector_fp_saving = whether the gadget saves FPSR for its leave: a
 * sum, difference, product, quotient or square root, whose special result
 * leaves after it computed -- every lane of a packed one, and a result the
 * host found inexact which x86, raising an unmasked overflow, may not. */
.macro vector_fp_saving operation
	.set Lx86_vector_fp_saving, 0
	.irp generating, sqrt, add, mul, sub, div
	.ifc \operation,\generating
	.set Lx86_vector_fp_saving, 1
	.endif
	.endr
.endm

/* The gadget's FPSR save, after its tail. */
.macro vector_fp_save_path operation, save
	vector_fp_saving \operation
	.if Lx86_vector_fp_saving
	simd_save_status_path \save
	.endif
.endm

.macro vector_fp_compute operation, element, width, scalar, predicate, save
	.if \scalar
	mov v4.16b, v0.16b
	.endif
	.set Lx86_vector_fp_estimate\@, 0
	.ifc \operation,rcp
	.set Lx86_vector_fp_estimate\@, 1
	.endif
	.ifc \operation,rsqrt
	.set Lx86_vector_fp_estimate\@, 1
	.endif
	.if Lx86_vector_fp_estimate\@
	vector_fp_estimate \operation, \width, \scalar
	.else
	/* `exception_check_simd_exact` (w13) gates MXCSR.DAZ and FTZ and the
	 * SIMD checks, and so FPSR's save; w12 is MXCSR only while it is set. */
	ldrb w13, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w13, #2, Lx86_vector_fp_inputs_ready\@
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	vector_fp_saving \operation
	.if Lx86_vector_fp_saving
	simd_save_status_branch \save, 13
	.endif
	tbz w12, #6, Lx86_vector_fp_inputs_ready\@
	.if \width == 256
	simd_flush_denormals_each \element, 0, 1, 2, 3
	.else
	simd_flush_denormals_each \element, 0, 1
	.endif
Lx86_vector_fp_inputs_ready\@:
	vector_fp_check_inputs \operation, \element, \width, \scalar
	.ifc \operation,compare
	vector_fp_compare_mask \predicate, \element, \scalar, 6, 0, 1, 7
	.if \scalar
	.if \element == 32
	mov v4.s[0], v6.s[0]
	.else
	mov v4.d[0], v6.d[0]
	.endif
	.else
	mov v4.16b, v6.16b
	.if \width == 256
	vector_fp_compare_mask \predicate, \element, 0, 6, 2, 3, 7
	mov v5.16b, v6.16b
	.endif
	.endif
	.else
	.if \scalar
	vector_fp_regular_value \operation, \element, 6, 0, 1
	vector_fp_fix_generated_nan \operation, \element, 6, 0, 1, 1
	vector_fp_flush_to_zero \operation, \element, \width, 1
	.if \element == 32
	mov v4.s[0], v6.s[0]
	.else
	mov v4.d[0], v6.d[0]
	.endif
	.else
	vector_fp_packed_value \operation, \element, 4, 0, 1
	vector_fp_fix_generated_nan \operation, \element, 4, 0, 1, 0
	.if \width == 256
	vector_fp_packed_value \operation, \element, 5, 2, 3
	vector_fp_fix_generated_nan \operation, \element, 5, 2, 3, 0
	.endif
	vector_fp_flush_to_zero \operation, \element, \width, 0
	.endif
	.endif
	.endif
.endm

.macro vector_fp_register name, operation, element, shape, width, scalar, predicate
gadget vector_fp_\name\()_\element\()_\shape\()_rr
	ldr x8, [_stream], #8
	vector_read_pair \width
	vector_fp_compute \operation, \element, \width, \scalar, \predicate, Lx86_vector_fp_save\@
	vector_write_xmm \width, 8
	retire_and_next
	vector_fp_save_path \operation, Lx86_vector_fp_save\@
.endm

.macro vector_fp_memory name, operation, element, shape, width, scalar, predicate
gadget vector_fp_\name\()_\element\()_\shape\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	.if \scalar
	.set Lx86_vector_fp_input_size\@, (\element / 8)
	.else
	.set Lx86_vector_fp_input_size\@, (\width / 8)
	.endif
	vector_memory_source Lx86_vector_fp_input_size\@, Lx86_vector_fp_cross_fault\@, scalar=\scalar, single=1
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_fp_compute \operation, \element, \width, \scalar, \predicate, Lx86_vector_fp_save\@
	vector_write_xmm \width, 23
	retire_and_next
	vector_fp_save_path \operation, Lx86_vector_fp_save\@
	.if !\scalar
	vector_memory_source_fault Lx86_vector_fp_cross_fault\@
	.endif
.endm

.macro vector_fp_family name, operation, element, shape, width, scalar, predicate=-1
	vector_fp_register \name, \operation, \element, \shape, \width, \scalar, \predicate
	vector_fp_memory \name, \operation, \element, \shape, \width, \scalar, \predicate
.endm

.macro vector_fp_operation operation
	vector_fp_family \operation, \operation, 32, 128, 128, 0
	vector_fp_family \operation, \operation, 32, 256, 256, 0
	vector_fp_family \operation, \operation, 32, scalar, 128, 1
	vector_fp_family \operation, \operation, 64, 128, 128, 0
	vector_fp_family \operation, \operation, 64, 256, 256, 0
	vector_fp_family \operation, \operation, 64, scalar, 128, 1
.endm

/* RCP and RSQRT have single-precision forms only. */
.macro vector_fp_single_operation operation
	vector_fp_family \operation, \operation, 32, 128, 128, 0
	vector_fp_family \operation, \operation, 32, 256, 256, 0
	vector_fp_family \operation, \operation, 32, scalar, 128, 1
.endm

vector_fp_operation sqrt
vector_fp_single_operation rsqrt
vector_fp_single_operation rcp
vector_fp_operation add
vector_fp_operation mul
vector_fp_operation sub
vector_fp_operation min
vector_fp_operation div
vector_fp_operation max

.macro vector_fp_predicate predicate
	vector_fp_family cmp_\predicate, compare, 32, 128, 128, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 32, 256, 256, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 32, scalar, 128, 1, \predicate
	vector_fp_family cmp_\predicate, compare, 64, 128, 128, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 64, 256, 256, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 64, scalar, 128, 1, \predicate
.endm

vector_fp_predicate 0
vector_fp_predicate 1
vector_fp_predicate 2
vector_fp_predicate 3
vector_fp_predicate 4
vector_fp_predicate 5
vector_fp_predicate 6
vector_fp_predicate 7
vector_fp_predicate 8
vector_fp_predicate 9
vector_fp_predicate 10
vector_fp_predicate 11
vector_fp_predicate 12
vector_fp_predicate 13
vector_fp_predicate 14
vector_fp_predicate 15
vector_fp_predicate 16
vector_fp_predicate 17
vector_fp_predicate 18
vector_fp_predicate 19
vector_fp_predicate 20
vector_fp_predicate 21
vector_fp_predicate 22
vector_fp_predicate 23
vector_fp_predicate 24
vector_fp_predicate 25
vector_fp_predicate 26
vector_fp_predicate 27
vector_fp_predicate 28
vector_fp_predicate 29
vector_fp_predicate 30
vector_fp_predicate 31

.purgem vector_fp_predicate
.purgem vector_fp_single_operation
.purgem vector_fp_operation
.purgem vector_fp_family
.purgem vector_fp_memory
.purgem vector_fp_register
.purgem vector_fp_compute
.purgem vector_fp_save_path
.purgem vector_fp_saving
.purgem vector_fp_check_inputs
.purgem vector_fp_nan_inputs
.purgem vector_fp_estimate
.purgem vector_fp_estimate_regular
.purgem vector_fp_estimate_sticky
.purgem vector_fp_estimate_special_scalar
.purgem vector_fp_estimate_special_packed
.purgem vector_fp_estimate_lanes
.purgem vector_fp_flush_to_zero
.purgem vector_fp_fix_generated_nan
.purgem vector_fp_fix_nan
.purgem vector_fp_packed_value
.purgem vector_fp_regular_value
.purgem vector_fp_compare_mask
.purgem vector_fp_relation
.purgem vector_fp_cmp
.purgem vector_fp_flush_tiny_scalar
