/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4.1/AVX dot products and multiple packed byte sums of differences. */

.macro vector_map3_dpps_lane output, left, right
	vector_map3_mask_s 7, 4
	and v6.16b, v\left\().16b, v7.16b
	and v7.16b, v\right\().16b, v7.16b
	fmul v6.4s, v6.4s, v7.4s
	faddp v6.4s, v6.4s, v6.4s
	dup v7.4s, v6.s[1]
	fadd s6, s6, s7
	fmov w14, s6
	dup v\output\().4s, w14
	vector_map3_mask_s 7, 0
	and v\output\().16b, v\output\().16b, v7.16b
.endm

.macro vector_map3_dppd_lane output, left, right
	vector_map3_mask_d 7, 4
	and v6.16b, v\left\().16b, v7.16b
	and v7.16b, v\right\().16b, v7.16b
	fmul v6.2d, v6.2d, v7.2d
	dup v7.2d, v6.d[1]
	fadd d6, d6, d7
	fmov x14, d6
	dup v\output\().2d, x14
	vector_map3_mask_d 7, 0
	and v\output\().16b, v\output\().16b, v7.16b
.endm

/* MXCSR.DAZ and FTZ apply to every step of a dot product: FTZ to each
 * product and sum that is tiny after rounding, raising underflow and
 * precision, and DAZ to each that is an input of the next addition. */
.macro vector_map3_dot_flush element, value, product, input
	tbz w12, #15, Lx86_vector_map3_dot_ftz_ready\@
	.if \product
	simd_flush_tiny_packed mul, \element, \value, 20, 22
	.else
	simd_flush_denormal_results \element, \value
	.endif
Lx86_vector_map3_dot_ftz_ready\@:
	.if \input
	tbz w12, #6, Lx86_vector_map3_dot_daz_ready\@
	simd_flush_denormals \value, \element
Lx86_vector_map3_dot_daz_ready\@:
	.endif
.endm

/* x86's DPPS and DPPD step by step, for MXCSR.DAZ or FTZ, or a special
 * result: the selected products and their sums, with x86's NaN rules at
 * every step. Each result lane sums in its own order, which picks among NaNs
 * as x86 hardware does -- the SDM ranks only distinct source operands: for
 * DPPS lane i adds p[i ^ 1] + p[i] and then that sum + the other pair's, for
 * DPPD p[i] + p[i ^ 1]. Products of unselected lanes are zero, computed from
 * zeros so that they raise nothing. Uses v16 to v24. */
.macro vector_map3_dot_exact element, output, left, right
	.if \element == 32
	vector_map3_mask_s 7, 4
	.else
	vector_map3_mask_d 7, 4
	.endif
	and v20.16b, v\left\().16b, v7.16b
	and v22.16b, v\right\().16b, v7.16b
	tbz w12, #6, Lx86_vector_map3_dot_inputs_ready\@
	simd_flush_denormals_each \element, 20, 22
Lx86_vector_map3_dot_inputs_ready\@:
	.if \element == 32
	fmul v21.4s, v20.4s, v22.4s
	.else
	fmul v21.2d, v20.2d, v22.2d
	.endif
	simd_x86_nan \element, 21, 20, 22, 1, 0
	vector_map3_dot_flush \element, 21, 1, 1
	.if \element == 32
	rev64 v23.4s, v21.4s
	mov v24.16b, v21.16b
	fadd v21.4s, v23.4s, v24.4s
	simd_x86_nan 32, 21, 23, 24, 1, 0
	vector_map3_dot_flush 32, 21, 0, 1
	mov v23.16b, v21.16b
	ext v24.16b, v21.16b, v21.16b, #8
	fadd v21.4s, v23.4s, v24.4s
	simd_x86_nan 32, 21, 23, 24, 1, 0
	vector_map3_dot_flush 32, 21, 0, 0
	vector_map3_mask_s 7, 0
	.else
	mov v23.16b, v21.16b
	ext v24.16b, v21.16b, v21.16b, #8
	fadd v21.2d, v23.2d, v24.2d
	simd_x86_nan 64, 21, 23, 24, 1, 0
	vector_map3_dot_flush 64, 21, 0, 0
	vector_map3_mask_d 7, 0
	.endif
	and v\output\().16b, v21.16b, v7.16b
.endm

/* A dot product runs as above on its common path, while its result is not a
 * NaN, and otherwise on the exact path (`exception_check_simd_exact`), which
 * stays in the cold section: it repairs every step. A dot product can
 * overflow and cancel on the way to its result, so while the gadgets check
 * SIMD exceptions one always leaves there, to run checked. */
.macro vector_map3_dot element, width
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #2, Lx86_vector_map3_dot_common\@
	b Lx86_vector_map3_dot_exact\@
Lx86_vector_map3_dot_common\@:
	.if \element == 32
	vector_map3_dpps_lane 4, 0, 1
	.if \width == 256
	vector_map3_dpps_lane 5, 2, 3
	.endif
	.else
	vector_map3_dppd_lane 4, 0, 1
	.if \width == 256
	vector_map3_dppd_lane 5, 2, 3
	.endif
	.endif
	.if \width == 256
	simd_nan_lanes \element, Lx86_vector_map3_dot_exact\@, 4, 5
	.else
	simd_nan_lanes \element, Lx86_vector_map3_dot_exact\@, 4
	.endif
Lx86_vector_map3_dot_ready\@:
	.pushsection GADGET_SECTION_COLD
Lx86_vector_map3_dot_exact\@:
	simd_leave_if_checking
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	vector_map3_dot_exact \element, 4, 0, 1
	.if \width == 256
	vector_map3_dot_exact \element, 5, 2, 3
	.endif
	b Lx86_vector_map3_dot_ready\@
	.popsection
.endm

.macro vector_map3_mpsadbw_lane output, left, right, shift
	lsr w14, w13, #\shift
	and w14, w14, #7
	and w15, w14, #3
	lsl w15, w15, #2
	ubfx w16, w14, #2, #1
	lsl w16, w16, #2
	vector_map3_index_sequence 6
	dup v7.16b, w15
	add v7.16b, v6.16b, v7.16b
	tbl v9.16b, {v\right\().16b}, v7.16b
	dup v7.16b, w16
	add v7.16b, v6.16b, v7.16b
	tbl v8.16b, {v\left\().16b}, v7.16b
	dup v10.8b, v9.b[0]
	uabdl v\output\().8h, v8.8b, v10.8b
	ext v11.16b, v8.16b, v8.16b, #1
	dup v10.8b, v9.b[1]
	uabal v\output\().8h, v11.8b, v10.8b
	ext v11.16b, v8.16b, v8.16b, #2
	dup v10.8b, v9.b[2]
	uabal v\output\().8h, v11.8b, v10.8b
	ext v11.16b, v8.16b, v8.16b, #3
	dup v10.8b, v9.b[3]
	uabal v\output\().8h, v11.8b, v10.8b
.endm

.macro vector_map3_math_body opcode, width
	.if \opcode == 0x40
	vector_map3_dot 32, \width
	.elseif \opcode == 0x41
	vector_map3_dot 64, \width
	.else
	vector_map3_mpsadbw_lane 4, 0, 1, 0
	.if \width == 256
	vector_map3_mpsadbw_lane 5, 2, 3, 3
	.endif
	.endif
.endm

.macro vector_map3_math_register symbol, opcode, width
gadget vector_map3_math_\symbol\()_\width\()_rr
	ldp x8, x13, [_stream], #16
	vector_read_pair \width
	vector_map3_math_body \opcode, \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_map3_math_memory symbol, opcode, width
gadget vector_map3_math_\symbol\()_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_map3_math_cross_fault\@
	mov w13, w27
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_map3_math_body \opcode, \width
	vector_write_xmm \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map3_math_cross_fault\@
.endm

.macro vector_map3_math_family symbol, opcode
	vector_map3_math_register \symbol, \opcode, 128
	vector_map3_math_memory \symbol, \opcode, 128
	vector_map3_math_register \symbol, \opcode, 256
	vector_map3_math_memory \symbol, \opcode, 256
.endm

vector_map3_math_family 40, 0x40
vector_map3_math_family 41, 0x41
vector_map3_math_family 42, 0x42

.purgem vector_map3_math_family
.purgem vector_map3_math_memory
.purgem vector_map3_math_register
.purgem vector_map3_math_body
.purgem vector_map3_mpsadbw_lane
.purgem vector_map3_dot
.purgem vector_map3_dot_exact
.purgem vector_map3_dot_flush
.purgem vector_map3_dppd_lane
.purgem vector_map3_dpps_lane
