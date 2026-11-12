/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed/scalar floating-point width and signed-int32 conversions. */

/* x86 raises precision for a lane it converts inexactly, and invalid
 * operation alone for a NaN or a value out of the int32 range. A single
 * out of that range is an integer, so FRINTX, which raises the host's
 * precision flag, rounds singles; FCVTZS of a truncated single's source
 * raises it too. A double out of the range may have a fraction, so doubles
 * round with FRINTI, which raises nothing, and only the lanes in range go
 * through FRINTX, or FCVTZS of the source when truncated, for the flag. */
.macro vector_convert_round input, output, element, truncate
	.if \truncate
	.if \element == 32
	frintz v\output\().4s, v\input\().4s
	.else
	frintz v\output\().2d, v\input\().2d
	.endif
	.else
	/* The dispatch already holds the guest's rounding mode. */
	.if \element == 32
	frintx v\output\().4s, v\input\().4s
	.else
	frinti v\output\().2d, v\input\().2d
	.endif
	.endif
.endm

/* A lane whose in-range mask in v7 is clear raises invalid operation. While
 * the gadgets check SIMD exceptions its conversion leaves; the common path
 * of a conversion of singles with an exact path (`checked` clear) runs only
 * while they do not. A conversion of doubles also raises the host's
 * invalid-operation flag for it, which its conversion to 64-bit integers
 * would not, so that a checked run finds the flag as x86 sets MXCSR's. The
 * leave of a rounded conversion of singles is a computed one: FRINTX raised
 * precision for lanes x86 does not convert once one is invalid. */
.macro vector_convert_check_range element, checked, truncate
	.if \element == 64 || \checked
	.if \element == 32
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_vector_convert_in_range\@
	.endif
	uminv s18, v7.4s
	fmov w14, s18
	cbnz w14, Lx86_vector_convert_in_range\@
	.if \element == 32 && !\truncate
	simd_leave_computed_if_checking
	.else
	simd_leave_if_checking
	.endif
	.if \element == 64
	movi d18, #0xffffffffffffffff
	fcmpe d18, d18
	.endif
Lx86_vector_convert_in_range\@:
	.endif
.endm

/* Convert one q register of float/double values to signed int32. Double
 * inputs produce two packed dwords in the low d register.
 */
.macro vector_convert_fp_i32 input, output, element, truncate, checked=1
	vector_convert_round \input, 6, \element, \truncate
	.if \element == 32
	mov w14, #0x4f000000
	dup v16.4s, w14
	fcmgt v7.4s, v16.4s, v6.4s
	mov w14, #0xcf000000
	dup v17.4s, w14
	fcmge v17.4s, v6.4s, v17.4s
	and v7.16b, v7.16b, v17.16b
	vector_convert_check_range 32, \checked, \truncate
	.if \truncate
	fcvtzs v16.4s, v\input\().4s
	.else
	fcvtzs v16.4s, v6.4s
	.endif
	mov w14, #0x80000000
	dup v17.4s, w14
	bsl v7.16b, v16.16b, v17.16b
	mov v\output\().16b, v7.16b
	.else
	mov x14, #0x41e0000000000000
	dup v16.2d, x14
	fcmgt v7.2d, v16.2d, v6.2d
	mov x14, #0xc1e0000000000000
	dup v17.2d, x14
	fcmge v17.2d, v6.2d, v17.2d
	and v7.16b, v7.16b, v17.16b
	vector_convert_check_range 64, 1, \truncate
	and v16.16b, v\input\().16b, v7.16b
	.if !\truncate
	frintx v16.2d, v16.2d
	.endif
	fcvtzs v16.2d, v16.2d
	mov x14, #0x80000000
	dup v17.2d, x14
	bsl v7.16b, v16.16b, v17.16b
	xtn v\output\().2s, v7.2d
	.endif
.endm

/* MXCSR.DAZ flushes a conversion's denormal inputs, v1 and for a 32-byte
 * input v3, `element` bits wide. Sets w12 to MXCSR. */
.macro vector_convert_inputs element, input_size
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w12, #6, Lx86_vector_convert_inputs_ready\@
	.if \input_size == 32
	simd_flush_denormals_each \element, 1, 3
	.else
	simd_flush_denormals 1, \element
	.endif
Lx86_vector_convert_inputs_ready\@:
.endm

/* MXCSR.FTZ flushes a narrowed single that is tiny after rounding (see
 * vector_float.inc): every denormal one, and one the host rounded up to the
 * smallest normal when its double, scaled up, narrows below the scaled
 * threshold, raising underflow and precision. The scalar result is in lane
 * 0 of v6 from d1; the packed one in v4 from v1, and v3 for a 32-byte
 * input. It tests w12, MXCSR. */
.macro vector_convert_flush_narrowed scalar, input_size
	tbz w12, #15, Lx86_vector_convert_ftz_ready\@
	.if \scalar
	simd_small_scalar 32
	b.ne Lx86_vector_convert_ftz_ready\@
	.else
	simd_small_lanes 32, 4, 17, 18
	umaxv s17, v17.4s
	fmov w14, s17
	cbz w14, Lx86_vector_convert_ftz_ready\@
	.endif
	b Lx86_vector_convert_tiny\@
Lx86_vector_convert_ftz_ready\@:
	.pushsection GADGET_SECTION_COLD
Lx86_vector_convert_tiny\@:
	mrs x15, fpsr
	mov x14, #0x43f0000000000000
	.if \scalar
	fmov d16, x14
	fmul d16, d1, d16
	fcvt s16, d16
	msr fpsr, x15
	fmov w14, s6
	and w14, w14, #0x7fffffff
	cmp w14, #0x800, lsl #12
	b.ne Lx86_vector_convert_flush\@
	fmov w14, s16
	and w14, w14, #0x7fffffff
	mov w15, #0x20800000
	cmp w14, w15
	b.hs Lx86_vector_convert_tiny_done\@
Lx86_vector_convert_flush\@:
	mxcsr_raise 0x30, 14
	fmov w14, s6
	and w14, w14, #0x80000000
	fmov s6, w14
Lx86_vector_convert_tiny_done\@:
	.else
	dup v16.2d, x14
	fmul v18.2d, v1.2d, v16.2d
	fcvtn v18.2s, v18.2d
	.if \input_size == 32
	fmul v19.2d, v3.2d, v16.2d
	fcvtn2 v18.4s, v19.2d
	.endif
	msr fpsr, x15
	simd_small_lanes 32, 4, 17, 16
	shl v18.4s, v18.4s, #1
	movi v16.4s, #0x41, lsl #24
	cmhi v18.4s, v16.4s, v18.4s
	and v17.16b, v17.16b, v18.16b
	mxcsr_raise_flushed 17, 18
	movi v16.4s, #0x80, lsl #24
	and v16.16b, v4.16b, v16.16b
	bsl v17.16b, v16.16b, v4.16b
	mov v4.16b, v17.16b
	.endif
	b Lx86_vector_convert_ftz_ready\@
	.popsection
.endm

/* A conversion from floating point but to integers with truncation has an
 * exact path (`simd_exact_branch`): there MXCSR.DAZ flushes its inputs and
 * FTZ its narrowed results, and the SIMD checks run. `exact` emits that
 * path. A truncation, which has none, flushes its inputs under DAZ before
 * it converts them, so that a denormal one converts exactly. */
.macro vector_convert_floating operation, output_width, input_size, truncate, exact
	.ifc \operation,ps_pd
	.if \exact
	vector_convert_inputs 32, \input_size
	.endif
	fcvtl v4.2d, v1.2s
	.if \output_width == 256
	fcvtl2 v5.2d, v1.4s
	.endif
	.if \exact
	simd_check_result 64, \output_width
	.endif
	.endif
	.ifc \operation,pd_ps
	.if \exact
	vector_convert_inputs 64, \input_size
	.endif
	movi v4.16b, #0
	fcvtn v4.2s, v1.2d
	.if \input_size == 32
	fcvtn2 v4.4s, v3.2d
	.endif
	.if \exact
	simd_check_packed 32, 4
	vector_convert_flush_narrowed 0, \input_size
	.endif
	.endif
	.ifc \operation,ss_sd
	.if \exact
	vector_convert_inputs 32, \input_size
	.endif
	mov v4.16b, v0.16b
	fcvt d6, s1
	.if \exact
	simd_check_scalar 64, 6
	.endif
	mov v4.d[0], v6.d[0]
	.endif
	.ifc \operation,sd_ss
	.if \exact
	vector_convert_inputs 64, \input_size
	.endif
	mov v4.16b, v0.16b
	fcvt s6, d1
	.if \exact
	simd_check_scalar 32, 6
	vector_convert_flush_narrowed 1, \input_size
	.endif
	mov v4.s[0], v6.s[0]
	.endif
	.ifc \operation,ps_dq
	.if \exact
	vector_convert_inputs 32, \input_size
	.endif
	vector_convert_fp_i32 1, 4, 32, \truncate, \exact
	.if \output_width == 256
	vector_convert_fp_i32 3, 5, 32, \truncate, \exact
	.endif
	.endif
	.ifc \operation,pd_dq
	.if \exact
	vector_convert_inputs 64, \input_size
	.endif
	vector_convert_fp_i32 1, 4, 64, \truncate, \exact
	.if \input_size == 32
	vector_convert_fp_i32 3, 5, 64, \truncate, \exact
	mov v4.d[1], v5.d[0]
	.endif
	.endif
.endm

/* A conversion from integers raises at most precision, which the gadgets
 * never check. One to a wider format raises invalid operation only for a
 * signalling NaN, which its result shows as a NaN. */
.macro vector_convert_body operation, output_width, input_size, scalar, truncate
	.set Lx86_vector_convert_exact\@, !\truncate
	.ifc \operation,dq_ps
	.set Lx86_vector_convert_exact\@, 0
	.endif
	.ifc \operation,dq_pd
	.set Lx86_vector_convert_exact\@, 0
	.endif
	.if Lx86_vector_convert_exact\@
	simd_exact_branch Lx86_vector_convert_common\@
	vector_convert_floating \operation, \output_width, \input_size, \truncate, 1
	simd_exact_end Lx86_vector_convert_common\@, Lx86_vector_convert_done\@
Lx86_vector_convert_common\@:
	vector_convert_floating \operation, \output_width, \input_size, \truncate, 0
Lx86_vector_convert_done\@:
	.else
	.ifc \operation,dq_ps
	scvtf v4.4s, v1.4s
	.if \output_width == 256
	scvtf v5.4s, v3.4s
	.endif
	.endif
	.ifc \operation,dq_pd
	sshll v6.2d, v1.2s, #0
	scvtf v4.2d, v6.2d
	.if \output_width == 256
	sshll2 v6.2d, v1.4s, #0
	scvtf v5.2d, v6.2d
	.endif
	.endif
	.ifc \operation,ps_dq
	vector_convert_inputs 32, \input_size
	vector_convert_fp_i32 1, 4, 32, \truncate
	.if \output_width == 256
	vector_convert_fp_i32 3, 5, 32, \truncate
	.endif
	.endif
	.ifc \operation,pd_dq
	vector_convert_inputs 64, \input_size
	vector_convert_fp_i32 1, 4, 64, \truncate
	.if \input_size == 32
	vector_convert_fp_i32 3, 5, 64, \truncate
	mov v4.d[1], v5.d[0]
	.endif
	.endif
	.endif
.endm

.macro vector_convert_register name, operation, output_width, input_size, scalar, truncate
gadget vector_convert_\name\()_rr
	ldr x8, [_stream], #8
	.if \scalar
	ubfx x10, x8, #16, #16
	add x10, _vcpu, x10
	ldr q0, [x10]
	.endif
	ubfx x11, x8, #32, #16
	add x11, _vcpu, x11
	ldr q1, [x11]
	.if \input_size == 32
	ldr q3, [x11, #16]
	.endif
	vector_convert_body \operation, \output_width, \input_size, \scalar, \truncate
	vector_write_xmm \output_width, 8
	retire_and_next
.endm

.macro vector_convert_memory name, operation, output_width, input_size, scalar, truncate
gadget vector_convert_\name\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_memory_source \input_size, Lx86_vector_convert_cross_fault\@, scalar=1, single=1
	.if \scalar
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.endif
	vector_convert_body \operation, \output_width, \input_size, \scalar, \truncate
	vector_write_xmm \output_width, 23
	retire_and_next
	.if \input_size > 8
	vector_memory_source_fault Lx86_vector_convert_cross_fault\@
	.endif
.endm

.macro vector_convert_family name, operation, output_width, input_size, scalar=0, truncate=0
	vector_convert_register \name, \operation, \output_width, \input_size, \scalar, \truncate
	vector_convert_memory \name, \operation, \output_width, \input_size, \scalar, \truncate
.endm

vector_convert_family ps_pd_128, ps_pd, 128, 8
vector_convert_family ps_pd_256, ps_pd, 256, 16
vector_convert_family pd_ps_128, pd_ps, 128, 16
vector_convert_family pd_ps_256, pd_ps, 128, 32
vector_convert_family ss_sd, ss_sd, 128, 4, 1
vector_convert_family sd_ss, sd_ss, 128, 8, 1
vector_convert_family dq_ps_128, dq_ps, 128, 16
vector_convert_family dq_ps_256, dq_ps, 256, 32
vector_convert_family ps_dq_128, ps_dq, 128, 16
vector_convert_family ps_dq_256, ps_dq, 256, 32
vector_convert_family ttps_dq_128, ps_dq, 128, 16, 0, 1
vector_convert_family ttps_dq_256, ps_dq, 256, 32, 0, 1
vector_convert_family dq_pd_128, dq_pd, 128, 8
vector_convert_family dq_pd_256, dq_pd, 256, 16
vector_convert_family pd_dq_128, pd_dq, 128, 16
vector_convert_family pd_dq_256, pd_dq, 128, 32
vector_convert_family ttpd_dq_128, pd_dq, 128, 16, 0, 1
vector_convert_family ttpd_dq_256, pd_dq, 128, 32, 0, 1

.purgem vector_convert_family
.purgem vector_convert_memory
.purgem vector_convert_register
.purgem vector_convert_body
.purgem vector_convert_flush_narrowed
.purgem vector_convert_inputs
.purgem vector_convert_fp_i32
.purgem vector_convert_check_range
.purgem vector_convert_round
