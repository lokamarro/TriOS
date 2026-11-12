/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FMA3 132/213/231 packed and scalar fused operations. */

.macro vector_fma_scalar_value operation, element, output, a, b, c
	.if \element == 32
	.if \operation == 9
	fmadd s\output, s\a, s\b, s\c
	.elseif \operation == 0xb
	fnmsub s\output, s\a, s\b, s\c
	.elseif \operation == 0xd
	fmsub s\output, s\a, s\b, s\c
	.else
	fnmadd s\output, s\a, s\b, s\c
	.endif
	.else
	.if \operation == 9
	fmadd d\output, d\a, d\b, d\c
	.elseif \operation == 0xb
	fnmsub d\output, d\a, d\b, d\c
	.elseif \operation == 0xd
	fmsub d\output, d\a, d\b, d\c
	.else
	fnmadd d\output, d\a, d\b, d\c
	.endif
	.endif
.endm

/* v`output` = a * b + c, with the signs and the alternation `operation`
 * names. */
.macro vector_fma_packed_value operation, element, output, a, b, c
	.if \element == 32
	.set Lx86_vector_fma_vector\@, 4
	.else
	.set Lx86_vector_fma_vector\@, 2
	.endif
	mov v\output\().16b, v\c\().16b
	.if \operation == 0xa || \operation == 0xb || \
		\operation == 0xe || \operation == 0xf
	.if \element == 32
	fneg v\output\().4s, v\output\().4s
	.else
	fneg v\output\().2d, v\output\().2d
	.endif
	.elseif \operation == 6 || \operation == 7
	.if \element == 32
	movi v16.4s, #0
	mov w14, #0x80000000
	.if \operation == 6
	mov v16.s[0], w14
	mov v16.s[2], w14
	.else
	mov v16.s[1], w14
	mov v16.s[3], w14
	.endif
	.else
	movi v16.2d, #0
	mov x14, #0x8000000000000000
	.if \operation == 6
	mov v16.d[0], x14
	.else
	mov v16.d[1], x14
	.endif
	.endif
	eor v\output\().16b, v\output\().16b, v16.16b
	.endif
	.if \element == 32
	.if \operation >= 0xc
	fmls v\output\().4s, v\a\().4s, v\b\().4s
	.else
	fmla v\output\().4s, v\a\().4s, v\b\().4s
	.endif
	.else
	.if \operation >= 0xc
	fmls v\output\().2d, v\a\().2d, v\b\().2d
	.else
	fmla v\output\().2d, v\a\().2d, v\b\().2d
	.endif
	.endif
.endm

/* Invoke `name` with the extra arguments and then x86's a, b and c of
 * a * b + c for `form`, from v`destination`, v`vvvv` and v`rm`: 132 is
 * destination * rm + vvvv, 213 vvvv * destination + rm, and 231
 * vvvv * rm + destination. */
.macro vector_fma_by_form form, name, first, second, third, destination, vvvv, rm
	.if \form == 0x90
	\name \first, \second, \third, \destination, \rm, \vvvv
	.elseif \form == 0xa0
	\name \first, \second, \third, \vvvv, \destination, \rm
	.else
	\name \first, \second, \third, \vvvv, \rm, \destination
	.endif
.endm

/* x86 returns the first of a, b and c that is a NaN, quieted, with its own
 * sign whatever the operation negates, and the negative indefinite for a NaN
 * the operation generates -- also for an infinity times zero plus a quiet
 * NaN, where the host generates one. */
.macro vector_fma_x86_nan_scalar operation, element, output, a, b, c
	.if \element == 32
	fcmp s\output, s\output
	b.vc Lx86_vector_fma_nan_done\@
	fcmp s\a, s\a
	b.vs Lx86_vector_fma_nan_a\@
	fcmp s\b, s\b
	b.vs Lx86_vector_fma_nan_b\@
	fcmp s\c, s\c
	b.vs Lx86_vector_fma_nan_c\@
	mov w14, #0xffc00000
	b Lx86_vector_fma_nan_store\@
Lx86_vector_fma_nan_a\@:
	fmov w14, s\a
	b Lx86_vector_fma_nan_quiet\@
Lx86_vector_fma_nan_b\@:
	fmov w14, s\b
	b Lx86_vector_fma_nan_quiet\@
Lx86_vector_fma_nan_c\@:
	fmov w14, s\c
Lx86_vector_fma_nan_quiet\@:
	orr w14, w14, #0x00400000
Lx86_vector_fma_nan_store\@:
	fmov s\output, w14
	.else
	fcmp d\output, d\output
	b.vc Lx86_vector_fma_nan_done\@
	fcmp d\a, d\a
	b.vs Lx86_vector_fma_nan_a\@
	fcmp d\b, d\b
	b.vs Lx86_vector_fma_nan_b\@
	fcmp d\c, d\c
	b.vs Lx86_vector_fma_nan_c\@
	mov x14, #0xfff8000000000000
	b Lx86_vector_fma_nan_store\@
Lx86_vector_fma_nan_a\@:
	fmov x14, d\a
	b Lx86_vector_fma_nan_quiet\@
Lx86_vector_fma_nan_b\@:
	fmov x14, d\b
	b Lx86_vector_fma_nan_quiet\@
Lx86_vector_fma_nan_c\@:
	fmov x14, d\c
Lx86_vector_fma_nan_quiet\@:
	orr x14, x14, #0x0008000000000000
Lx86_vector_fma_nan_store\@:
	fmov d\output, x14
	.endif
Lx86_vector_fma_nan_done\@:
.endm

/* The same for each lane of v`output`. */
.macro vector_fma_x86_nan_packed operation, element, output, a, b, c
	.if \element == 32
	fcmeq v17.4s, v\output\().4s, v\output\().4s
	uminv s18, v17.4s
	fmov w14, s18
	cbnz w14, Lx86_vector_fma_nan_done\@
	mov w14, #0xffc00000
	dup v16.4s, w14
	movi v20.4s, #0x40, lsl #16
	fcmeq v18.4s, v\c\().4s, v\c\().4s
	orr v19.16b, v\c\().16b, v20.16b
	bsl v18.16b, v16.16b, v19.16b
	fcmeq v16.4s, v\b\().4s, v\b\().4s
	orr v19.16b, v\b\().16b, v20.16b
	bsl v16.16b, v18.16b, v19.16b
	fcmeq v18.4s, v\a\().4s, v\a\().4s
	.else
	fcmeq v17.2d, v\output\().2d, v\output\().2d
	uminv s18, v17.4s
	fmov w14, s18
	cbnz w14, Lx86_vector_fma_nan_done\@
	mov x14, #0xfff8000000000000
	dup v16.2d, x14
	mov x14, #0x0008000000000000
	dup v20.2d, x14
	fcmeq v18.2d, v\c\().2d, v\c\().2d
	orr v19.16b, v\c\().16b, v20.16b
	bsl v18.16b, v16.16b, v19.16b
	fcmeq v16.2d, v\b\().2d, v\b\().2d
	orr v19.16b, v\b\().16b, v20.16b
	bsl v16.16b, v18.16b, v19.16b
	fcmeq v18.2d, v\a\().2d, v\a\().2d
	.endif
	orr v19.16b, v\a\().16b, v20.16b
	bsl v18.16b, v16.16b, v19.16b
	bsl v17.16b, v\output\().16b, v18.16b
	mov v\output\().16b, v17.16b
Lx86_vector_fma_nan_done\@:
.endm

/* MXCSR.FTZ, which x86 applies to a result tiny after rounding (see
 * vector_float.inc). A denormal result is tiny. One the host rounded up to
 * the smallest normal is when a * b + c, with a and c scaled up into the
 * normal range, rounds below the threshold scaled alike. A scaled operand
 * that overflows comes only with an exact result, which the overflow marks
 * as not tiny. The scaled operation's flags are discarded, and a flush
 * raises underflow and precision. */
.macro vector_fma_flush_tiny_scalar operation, element, output, a, b, c
	.if \element == 32
	fmov w14, s\output
	and w14, w14, #0x7fffffff
	cmp w14, #0x800, lsl #12
	b.ne Lx86_vector_fma_flush\@
	mrs x15, fpsr
	mov w14, #0x5f800000
	fmov s16, w14
	fmul s19, s\a, s16
	fmul s20, s\c, s16
	vector_fma_scalar_value \operation, 32, 21, 19, \b, 20
	msr fpsr, x15
	fmov w14, s21
	and w14, w14, #0x7fffffff
	mov w15, #0x20800000
	cmp w14, w15
	b.hs Lx86_vector_fma_tiny_done\@
Lx86_vector_fma_flush\@:
	mxcsr_raise 0x30, 14
	fmov w14, s\output
	and w14, w14, #0x80000000
	fmov s\output, w14
	.else
	fmov x14, d\output
	and x14, x14, #0x7fffffffffffffff
	mov x15, #0x0010000000000000
	cmp x14, x15
	b.ne Lx86_vector_fma_flush\@
	mrs x15, fpsr
	mov x14, #0x5ff0000000000000
	fmov d16, x14
	fmul d19, d\a, d16
	fmul d20, d\c, d16
	vector_fma_scalar_value \operation, 64, 21, 19, \b, 20
	msr fpsr, x15
	fmov x14, d21
	and x14, x14, #0x7fffffffffffffff
	mov x15, #0x2010000000000000
	cmp x14, x15
	b.hs Lx86_vector_fma_tiny_done\@
Lx86_vector_fma_flush\@:
	mxcsr_raise 0x30, 14
	fmov x14, d\output
	and x14, x14, #0x8000000000000000
	fmov d\output, x14
	.endif
Lx86_vector_fma_tiny_done\@:
.endm

.macro vector_fma_flush_tiny_packed operation, element, output, a, b, c
	simd_small_lanes \element, \output, 17, 18
	mrs x15, fpsr
	.if \element == 32
	shl v18.4s, v\output\().4s, #1
	movi v16.4s, #0x01, lsl #24
	cmeq v18.4s, v18.4s, v16.4s
	mov w14, #0x5f800000
	dup v16.4s, w14
	fmul v19.4s, v\a\().4s, v16.4s
	fmul v20.4s, v\c\().4s, v16.4s
	vector_fma_packed_value \operation, 32, 21, 19, \b, 20
	shl v21.4s, v21.4s, #1
	movi v16.4s, #0x41, lsl #24
	cmhi v21.4s, v16.4s, v21.4s
	.else
	shl v18.2d, v\output\().2d, #1
	mov x14, #0x0020000000000000
	dup v16.2d, x14
	cmeq v18.2d, v18.2d, v16.2d
	mov x14, #0x5ff0000000000000
	dup v16.2d, x14
	fmul v19.2d, v\a\().2d, v16.2d
	fmul v20.2d, v\c\().2d, v16.2d
	vector_fma_packed_value \operation, 64, 21, 19, \b, 20
	shl v21.2d, v21.2d, #1
	mov x14, #0x4020000000000000
	dup v16.2d, x14
	cmhi v21.2d, v16.2d, v21.2d
	.endif
	msr fpsr, x15
	orn v21.16b, v21.16b, v18.16b
	and v17.16b, v17.16b, v21.16b
	mxcsr_raise_flushed 17, 21
	.if \element == 32
	movi v16.4s, #0x80, lsl #24
	.else
	mov x14, #0x8000000000000000
	dup v16.2d, x14
	.endif
	and v16.16b, v\output\().16b, v16.16b
	bsl v17.16b, v16.16b, v\output\().16b
	mov v\output\().16b, v17.16b
.endm

/* The computation, in v6 (and v7 for 256 bits), from the destination in v0,
 * vvvv in v1 and r/m in v2 (and their upper halves in v3, v4 and v5). */
.macro vector_fma_compute form, operation, element_bits, width, scalar
	.if \scalar
	vector_fma_by_form \form, vector_fma_scalar_value, \operation, \element_bits, 6, 0, 1, 2
	.else
	vector_fma_by_form \form, vector_fma_packed_value, \operation, \element_bits, 6, 0, 1, 2
	.if \width == 256
	vector_fma_by_form \form, vector_fma_packed_value, \operation, \element_bits, 7, 3, 4, 5
	.endif
	.endif
.endm

/* x86's NaN result in v6 (and v7). */
.macro vector_fma_x86_nan form, operation, element_bits, width, scalar
	.if \scalar
	vector_fma_by_form \form, vector_fma_x86_nan_scalar, \operation, \element_bits, 6, 0, 1, 2
	.else
	vector_fma_by_form \form, vector_fma_x86_nan_packed, \operation, \element_bits, 6, 0, 1, 2
	.if \width == 256
	vector_fma_by_form \form, vector_fma_x86_nan_packed, \operation, \element_bits, 7, 3, 4, 5
	.endif
	.endif
.endm

/* The exact path (`simd_exact_branch`), entered with `exception_checks` in
 * w14. MXCSR.DAZ flushes the inputs -- for a scalar one lane 0 of the
 * destination alone, whose other lanes the result keeps. One test finds a
 * special result, and under FTZ a result no larger than the smallest
 * normal; the path for either leaves while the gadgets check SIMD
 * exceptions, with FPSR as the instruction found it, since the host raises
 * invalid operation for infinity times zero plus a quiet NaN, repairs a
 * NaN, and flushes a tiny result. Returns to `back`. Uses v16 to v21. */
.macro vector_fma_exact form, operation, element_bits, width, scalar, back
	simd_save_status_branch Lx86_vector_fma_save\@, 14
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w12, #6, Lx86_vector_fma_inputs_ready\@
	simd_flush_masks \element_bits, 18, 19
	.if \scalar
	mov v7.16b, v0.16b
	simd_flush_denormals_masked 7, \element_bits, 18, 19
	.if \element_bits == 32
	mov v0.s[0], v7.s[0]
	.else
	mov v0.d[0], v7.d[0]
	.endif
	simd_flush_denormals_masked 1, \element_bits, 18, 19
	simd_flush_denormals_masked 2, \element_bits, 18, 19
	.else
	simd_flush_denormals_masked 0, \element_bits, 18, 19
	simd_flush_denormals_masked 1, \element_bits, 18, 19
	simd_flush_denormals_masked 2, \element_bits, 18, 19
	.if \width == 256
	simd_flush_denormals_masked 3, \element_bits, 18, 19
	simd_flush_denormals_masked 4, \element_bits, 18, 19
	simd_flush_denormals_masked 5, \element_bits, 18, 19
	.endif
	.endif
Lx86_vector_fma_inputs_ready\@:
	vector_fma_compute \form, \operation, \element_bits, \width, \scalar
	.if \scalar
	.if \element_bits == 32
	fmov w14, s6
	ubfx w15, w14, #23, #8
	cmp w15, #0xfe
	b.hs Lx86_vector_fma_unusual_stub\@
	tbz w12, #15, Lx86_vector_fma_ready\@
	lsl w15, w14, #1
	sub w15, w15, #1
	tst w15, #0xff000000
	b.eq Lx86_vector_fma_unusual_stub\@
	.else
	fmov x14, d6
	ubfx x15, x14, #52, #11
	cmp x15, #0x7fe
	b.hs Lx86_vector_fma_unusual_stub\@
	tbz w12, #15, Lx86_vector_fma_ready\@
	lsl x15, x14, #1
	sub x15, x15, #1
	tst x15, #0xffe0000000000000
	b.eq Lx86_vector_fma_unusual_stub\@
	.endif
	.else
	vector_fma_unusual_lanes \element_bits, 6, 20
	.if \width == 256
	vector_fma_unusual_lanes \element_bits, 7, 21
	orr v20.16b, v20.16b, v21.16b
	.endif
	umaxv s20, v20.4s
	fmov w14, s20
	cbnz w14, Lx86_vector_fma_unusual_stub\@
	.endif
Lx86_vector_fma_ready\@:
	b \back
	simd_save_status_path Lx86_vector_fma_save\@
Lx86_vector_fma_unusual_stub\@:
	b Lx86_vector_fma_unusual\@
	.pushsection GADGET_SECTION_COLD
Lx86_vector_fma_unusual\@:
	.if \scalar
	simd_special_scalar \element_bits, 6, Lx86_vector_fma_nan_ready\@, lo
	.else
	simd_special_lanes \element_bits, 6
	.if \width == 256
	cbnz w14, Lx86_vector_fma_special\@
	simd_special_lanes \element_bits, 7
	.endif
	cbz w14, Lx86_vector_fma_nan_ready\@
	.endif
Lx86_vector_fma_special\@:
	simd_leave_computed_if_checking
	vector_fma_x86_nan \form, \operation, \element_bits, \width, \scalar
Lx86_vector_fma_nan_ready\@:
	tbz w12, #15, Lx86_vector_fma_tiny_ready\@
	.if \scalar
	simd_small_scalar \element_bits
	b.ne Lx86_vector_fma_tiny_ready\@
	vector_fma_by_form \form, vector_fma_flush_tiny_scalar, \operation, \element_bits, 6, 0, 1, 2
	.else
	simd_small_lanes \element_bits, 6, 17, 18
	.if \width == 256
	simd_small_lanes \element_bits, 7, 16, 18
	orr v17.16b, v17.16b, v16.16b
	.endif
	umaxv s17, v17.4s
	fmov w14, s17
	cbz w14, Lx86_vector_fma_tiny_ready\@
	vector_fma_by_form \form, vector_fma_flush_tiny_packed, \operation, \element_bits, 6, 0, 1, 2
	.if \width == 256
	vector_fma_by_form \form, vector_fma_flush_tiny_packed, \operation, \element_bits, 7, 3, 4, 5
	.endif
	.endif
Lx86_vector_fma_tiny_ready\@:
	b Lx86_vector_fma_ready\@
	.popsection
.endm

/* v`mask` = all ones in the lanes of v`output` whose exponent is all ones or
 * one below, and under FTZ (w12 is MXCSR) also in those that are not zero
 * and at most the smallest normal. Uses v16 to v19. */
.macro vector_fma_unusual_lanes element, output, mask
	.if \element == 32
	shl v16.4s, v\output\().4s, #1
	movi v17.4s, #0xfe, lsl #24
	cmhs v\mask\().4s, v16.4s, v17.4s
	tbz w12, #15, Lx86_vector_fma_unusual_ready\@
	movi v18.16b, #0xff
	add v16.4s, v16.4s, v18.4s
	movi v19.4s, #0x01, lsl #24
	cmhi v16.4s, v19.4s, v16.4s
	.else
	shl v16.2d, v\output\().2d, #1
	mov x14, #0xffc0000000000000
	dup v17.2d, x14
	cmhs v\mask\().2d, v16.2d, v17.2d
	tbz w12, #15, Lx86_vector_fma_unusual_ready\@
	movi v18.16b, #0xff
	add v16.2d, v16.2d, v18.2d
	mov x14, #0x0020000000000000
	dup v19.2d, x14
	cmhi v16.2d, v19.2d, v16.2d
	.endif
	orr v\mask\().16b, v\mask\().16b, v16.16b
Lx86_vector_fma_unusual_ready\@:
.endm

/* The common path, while `exception_check_simd_exact` is clear: a NaN result
 * branches to `special`, and the exact path starts at `exact`. Their paths,
 * after the gadget, return to `special_back` and `exact_back`. */
.macro vector_fma_body form, operation, element_bits, width, scalar, special, special_back, exact, exact_back
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbnz w14, #2, \exact
	vector_fma_compute \form, \operation, \element_bits, \width, \scalar
	.if \scalar
	.if \element_bits == 32
	fcmp s6, s6
	.else
	fcmp d6, d6
	.endif
	.else
	.if \element_bits == 32
	.if \width == 256
	fmax v17.4s, v6.4s, v7.4s
	fmaxv s17, v17.4s
	.else
	fmaxv s17, v6.4s
	.endif
	fcmp s17, s17
	.else
	.if \width == 256
	fmax v17.2d, v6.2d, v7.2d
	fmaxp d17, v17.2d
	.else
	fmaxp d17, v6.2d
	.endif
	fcmp d17, d17
	.endif
	.endif
	b.vs \special
\special_back:
\exact_back:
	.if \scalar
	.if \element_bits == 32
	mov v6.s[1], v0.s[1]
	mov v6.s[2], v0.s[2]
	mov v6.s[3], v0.s[3]
	.else
	mov v6.d[1], v0.d[1]
	.endif
	.endif
.endm

/* The paths `vector_fma_body` branches to, placed after the gadget's tail so
 * that the common path takes no branch. A NaN result on the common path needs
 * only x86's NaN, in the cold section. The exact path runs here, as
 * applications commonly set DAZ and FTZ for good, and keeps its rare repairs
 * in the cold section. */
.macro vector_fma_paths form, operation, element_bits, width, scalar, special, special_back, exact, exact_back
\special:
	b \special\()_cold
\exact:
	vector_fma_exact \form, \operation, \element_bits, \width, \scalar, \exact_back
	.pushsection GADGET_SECTION_COLD
\special\()_cold:
	vector_fma_x86_nan \form, \operation, \element_bits, \width, \scalar
	b \special_back
	.popsection
.endm

.macro vector_fma_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q6, [x9]
	.if \width == 256
	str q7, [x9, #16]
	.else
	movi v16.16b, #0
	str q16, [x9, #16]
	.endif
.endm

.macro vector_fma_register name, form, operation, element, shape, width, scalar
gadget vector_fma_\name\()_\element\()_\shape\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x9]
	ldr q1, [x10]
	ldr q2, [x11]
	.if \width == 256
	ldr q3, [x9, #16]
	ldr q4, [x10, #16]
	ldr q5, [x11, #16]
	.endif
	vector_fma_body \form, \operation, \element, \width, \scalar, Lx86_vector_fma_special\@, Lx86_vector_fma_special_back\@, Lx86_vector_fma_exact\@, Lx86_vector_fma_exact_back\@
	vector_fma_write \width, 8
	retire_and_next
	vector_fma_paths \form, \operation, \element, \width, \scalar, Lx86_vector_fma_special\@, Lx86_vector_fma_special_back\@, Lx86_vector_fma_exact\@, Lx86_vector_fma_exact_back\@
.endm

.macro vector_fma_memory name, form, operation, element, shape, width, scalar
gadget vector_fma_\name\()_\element\()_\shape\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	.if \scalar
	.set Lx86_vector_fma_input_size\@, (\element / 8)
	.else
	.set Lx86_vector_fma_input_size\@, (\width / 8)
	.endif
	vector_memory_source Lx86_vector_fma_input_size\@, Lx86_vector_fma_cross_fault\@, 2, 5, scalar=\scalar, single=1
	and x9, x23, #0xffff
	and x10, x26, #0xffff
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	ldr q0, [x9]
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x9, #16]
	ldr q4, [x10, #16]
	.endif
	vector_fma_body \form, \operation, \element, \width, \scalar, Lx86_vector_fma_special\@, Lx86_vector_fma_special_back\@, Lx86_vector_fma_exact\@, Lx86_vector_fma_exact_back\@
	vector_fma_write \width, 23
	retire_and_next
	.if !\scalar
	vector_memory_source_fault Lx86_vector_fma_cross_fault\@
	.endif
	vector_fma_paths \form, \operation, \element, \width, \scalar, Lx86_vector_fma_special\@, Lx86_vector_fma_special_back\@, Lx86_vector_fma_exact\@, Lx86_vector_fma_exact_back\@
.endm

.macro vector_fma_family name, form, operation, element, shape, width, scalar
	vector_fma_register \name, \form, \operation, \element, \shape, \width, \scalar
	vector_fma_memory \name, \form, \operation, \element, \shape, \width, \scalar
.endm

.macro vector_fma_opcode name, form, operation
	vector_fma_family \name, \form, \operation, 32, 128, 128, 0
	vector_fma_family \name, \form, \operation, 32, 256, 256, 0
	vector_fma_family \name, \form, \operation, 32, scalar, 128, 1
	vector_fma_family \name, \form, \operation, 64, 128, 128, 0
	vector_fma_family \name, \form, \operation, 64, 256, 256, 0
	vector_fma_family \name, \form, \operation, 64, scalar, 128, 1
.endm

vector_fma_opcode 96, 0x90, 6
vector_fma_opcode 97, 0x90, 7
vector_fma_opcode 98, 0x90, 8
vector_fma_opcode 99, 0x90, 9
vector_fma_opcode 9a, 0x90, 0xa
vector_fma_opcode 9b, 0x90, 0xb
vector_fma_opcode 9c, 0x90, 0xc
vector_fma_opcode 9d, 0x90, 0xd
vector_fma_opcode 9e, 0x90, 0xe
vector_fma_opcode 9f, 0x90, 0xf
vector_fma_opcode a6, 0xa0, 6
vector_fma_opcode a7, 0xa0, 7
vector_fma_opcode a8, 0xa0, 8
vector_fma_opcode a9, 0xa0, 9
vector_fma_opcode aa, 0xa0, 0xa
vector_fma_opcode ab, 0xa0, 0xb
vector_fma_opcode ac, 0xa0, 0xc
vector_fma_opcode ad, 0xa0, 0xd
vector_fma_opcode ae, 0xa0, 0xe
vector_fma_opcode af, 0xa0, 0xf
vector_fma_opcode b6, 0xb0, 6
vector_fma_opcode b7, 0xb0, 7
vector_fma_opcode b8, 0xb0, 8
vector_fma_opcode b9, 0xb0, 9
vector_fma_opcode ba, 0xb0, 0xa
vector_fma_opcode bb, 0xb0, 0xb
vector_fma_opcode bc, 0xb0, 0xc
vector_fma_opcode bd, 0xb0, 0xd
vector_fma_opcode be, 0xb0, 0xe
vector_fma_opcode bf, 0xb0, 0xf

.purgem vector_fma_opcode
.purgem vector_fma_family
.purgem vector_fma_memory
.purgem vector_fma_register
.purgem vector_fma_write
.purgem vector_fma_paths
.purgem vector_fma_body
.purgem vector_fma_exact
.purgem vector_fma_unusual_lanes
.purgem vector_fma_x86_nan
.purgem vector_fma_compute
.purgem vector_fma_flush_tiny_packed
.purgem vector_fma_flush_tiny_scalar
.purgem vector_fma_x86_nan_packed
.purgem vector_fma_x86_nan_scalar
.purgem vector_fma_by_form
.purgem vector_fma_packed_value
.purgem vector_fma_scalar_value
