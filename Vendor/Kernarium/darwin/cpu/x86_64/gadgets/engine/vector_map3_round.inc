/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4.1/AVX packed and scalar immediate floating-point rounding. */

.macro vector_map3_round_apply element, output, input, scalar
	and w14, w13, #3
	tbz w13, #2, Lx86_vector_map3_round_mode_ready\@
	ldr w14, [_vcpu, #UML_X86_64_CPU_MXCSR]
	ubfx w14, w14, #13, #2
Lx86_vector_map3_round_mode_ready\@:
	cmp w14, #1
	b.eq Lx86_vector_map3_round_down\@
	cmp w14, #2
	b.eq Lx86_vector_map3_round_up\@
	cmp w14, #3
	b.eq Lx86_vector_map3_round_zero\@
	.if \scalar
	.if \element == 32
	frintn s6, s\input
	.else
	frintn d6, d\input
	.endif
	.else
	.if \element == 32
	frintn v6.4s, v\input\().4s
	.else
	frintn v6.2d, v\input\().2d
	.endif
	.endif
	b Lx86_vector_map3_round_done\@
Lx86_vector_map3_round_down\@:
	.if \scalar
	.if \element == 32
	frintm s6, s\input
	.else
	frintm d6, d\input
	.endif
	.else
	.if \element == 32
	frintm v6.4s, v\input\().4s
	.else
	frintm v6.2d, v\input\().2d
	.endif
	.endif
	b Lx86_vector_map3_round_done\@
Lx86_vector_map3_round_up\@:
	.if \scalar
	.if \element == 32
	frintp s6, s\input
	.else
	frintp d6, d\input
	.endif
	.else
	.if \element == 32
	frintp v6.4s, v\input\().4s
	.else
	frintp v6.2d, v\input\().2d
	.endif
	.endif
	b Lx86_vector_map3_round_done\@
Lx86_vector_map3_round_zero\@:
	.if \scalar
	.if \element == 32
	frintz s6, s\input
	.else
	frintz d6, d\input
	.endif
	.else
	.if \element == 32
	frintz v6.4s, v\input\().4s
	.else
	frintz v6.2d, v\input\().2d
	.endif
	.endif
Lx86_vector_map3_round_done\@:
	.if \scalar
	.if \element == 32
	mov v\output\().s[0], v6.s[0]
	.else
	mov v\output\().d[0], v6.d[0]
	.endif
	.else
	mov v\output\().16b, v6.16b
	.endif
.endm

/* Rounding and F16C's narrowing have an exact path (`simd_exact_branch`),
 * where MXCSR.DAZ makes a denormal input zero and the SIMD checks run;
 * neither result can be tiny, and F16C ignores FTZ. */
.macro vector_map3_round_inputs element, width
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w12, #6, Lx86_vector_map3_round_inputs_ready\@
	.if \width == 256
	simd_flush_denormals_each \element, 1, 3
	.else
	simd_flush_denormals 1, \element
	.endif
Lx86_vector_map3_round_inputs_ready\@:
.endm

/* ROUND raises precision for a lane whose rounding changed it, unless bit 3
 * of its immediate (w13) suppresses that; FRINTN, FRINTM, FRINTP and FRINTZ
 * raise none. Precision is sticky, so nothing is tested while MXCSR holds
 * it. The result is in v4 (and v5) from v1 (and v3), or lane 0 of v4 from
 * v1 for a scalar one. A NaN raised its invalid operation as it rounded and
 * raises no precision. Uses v16 to v18 and w14. */
.macro vector_map3_round_precision element, width, scalar
	tbnz w13, #3, Lx86_vector_map3_round_precise\@
	ldr w14, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbnz w14, #5, Lx86_vector_map3_round_precise\@
	.if \scalar
	.if \element == 32
	fcmp s4, s1
	.else
	fcmp d4, d1
	.endif
	b.eq Lx86_vector_map3_round_precise\@
	b.vs Lx86_vector_map3_round_precise\@
	.else
	.if \element == 32
	fcmeq v16.4s, v4.4s, v1.4s
	fcmeq v17.4s, v1.4s, v1.4s
	.else
	fcmeq v16.2d, v4.2d, v1.2d
	fcmeq v17.2d, v1.2d, v1.2d
	.endif
	bic v17.16b, v17.16b, v16.16b
	.if \width == 256
	.if \element == 32
	fcmeq v16.4s, v5.4s, v3.4s
	fcmeq v18.4s, v3.4s, v3.4s
	.else
	fcmeq v16.2d, v5.2d, v3.2d
	fcmeq v18.2d, v3.2d, v3.2d
	.endif
	bic v18.16b, v18.16b, v16.16b
	orr v17.16b, v17.16b, v18.16b
	.endif
	umaxv s16, v17.4s
	fmov w14, s16
	cbz w14, Lx86_vector_map3_round_precise\@
	.endif
	mxcsr_raise 0x20, 14
Lx86_vector_map3_round_precise\@:
.endm

/* Rounding raises invalid operation only for a signalling NaN, which it
 * leaves a NaN, and precision after any leave. `exact` emits the exact
 * path. */
.macro vector_map3_round_compute opcode, width, exact
	.if \exact
	.if \opcode == 0x08 || \opcode == 0x0a
	vector_map3_round_inputs 32, \width
	.else
	vector_map3_round_inputs 64, \width
	.endif
	.endif
	.if \opcode == 0x08
	vector_map3_round_apply 32, 4, 1, 0
	.if \width == 256
	vector_map3_round_apply 32, 5, 3, 0
	.endif
	.if \exact
	simd_check_result 32, \width
	.endif
	vector_map3_round_precision 32, \width, 0
	.elseif \opcode == 0x09
	vector_map3_round_apply 64, 4, 1, 0
	.if \width == 256
	vector_map3_round_apply 64, 5, 3, 0
	.endif
	.if \exact
	simd_check_result 64, \width
	.endif
	vector_map3_round_precision 64, \width, 0
	.elseif \opcode == 0x0a
	mov v4.16b, v0.16b
	vector_map3_round_apply 32, 4, 1, 1
	.if \exact
	simd_check_scalar 32, 4
	.endif
	vector_map3_round_precision 32, 128, 1
	.else
	mov v4.16b, v0.16b
	vector_map3_round_apply 64, 4, 1, 1
	.if \exact
	simd_check_scalar 64, 4
	.endif
	vector_map3_round_precision 64, 128, 1
	.endif
.endm

.macro vector_map3_round_body opcode, width
	simd_exact_branch Lx86_vector_map3_round_common\@
	vector_map3_round_compute \opcode, \width, 1
	simd_exact_end Lx86_vector_map3_round_common\@, Lx86_vector_map3_round_done\@
Lx86_vector_map3_round_common\@:
	vector_map3_round_compute \opcode, \width, 0
Lx86_vector_map3_round_done\@:
.endm

.macro vector_map3_round_register symbol, opcode, width
gadget vector_map3_round_\symbol\()_\width\()_rr
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q3, [x11, #16]
	.endif
	vector_map3_round_body \opcode, \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_map3_round_memory symbol, opcode, width, input_size
gadget vector_map3_round_\symbol\()_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	vector_memory_source \input_size, Lx86_vector_map3_round_cross_fault\@, scalar=1, single=1
	mov w13, w27
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	vector_map3_round_body \opcode, \width
	vector_write_xmm \width, 23
	retire_and_next
	.if \input_size > 8
	vector_memory_source_fault Lx86_vector_map3_round_cross_fault\@
	.endif
.endm

vector_map3_round_register 08, 0x08, 128
vector_map3_round_register 08, 0x08, 256
vector_map3_round_memory 08, 0x08, 128, 16
vector_map3_round_memory 08, 0x08, 256, 32
vector_map3_round_register 09, 0x09, 128
vector_map3_round_register 09, 0x09, 256
vector_map3_round_memory 09, 0x09, 128, 16
vector_map3_round_memory 09, 0x09, 256, 32
vector_map3_round_register 0a, 0x0a, 128
vector_map3_round_memory 0a, 0x0a, 128, 4
vector_map3_round_register 0b, 0x0b, 128
vector_map3_round_memory 0b, 0x0b, 128, 8

.purgem vector_map3_round_memory
.purgem vector_map3_round_register
.purgem vector_map3_round_body
.purgem vector_map3_round_compute
.purgem vector_map3_round_precision
.purgem vector_map3_round_apply

/* F16C uses its immediate (or MXCSR) rounding mode rather than the host
 * thread's FPCR. Save and restore FPCR around the native narrowing operation.
 * `exact` emits the exact path.
 */
.macro vector_map3_half_compute width, exact
	.if \exact
	vector_map3_round_inputs 32, \width
	.endif
	and w14, w13, #3
	tbz w13, #2, Lx86_vector_map3_half_mode_ready\@
	ldr w14, [_vcpu, #UML_X86_64_CPU_MXCSR]
	ubfx w14, w14, #13, #2
Lx86_vector_map3_half_mode_ready\@:
	and w15, w14, #1
	lsl w15, w15, #1
	ubfx w17, w14, #1, #1
	orr w15, w15, w17
	mrs x16, fpcr
	bic x17, x16, #(3 << 22)
	orr x17, x17, x15, lsl #22
	msr fpcr, x17
	fcvtn v4.4h, v1.4s
	.if \width == 256
	fcvtn v5.4h, v3.4s
	mov v4.d[1], v5.d[0]
	.else
	movi v5.16b, #0
	mov v4.d[1], v5.d[0]
	.endif
	msr fpcr, x16
	.if \exact
	simd_check_packed 16, 4
	.endif
.endm

.macro vector_map3_half_body width
	simd_exact_branch Lx86_vector_map3_half_common\@
	vector_map3_half_compute \width, 1
	simd_exact_end Lx86_vector_map3_half_common\@, Lx86_vector_map3_half_done\@
Lx86_vector_map3_half_common\@:
	vector_map3_half_compute \width, 0
Lx86_vector_map3_half_done\@:
.endm

.macro vector_map3_half_register width
gadget vector_map3_half_\width\()_r
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	add x10, _vcpu, x10
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x10, #16]
	.endif
	vector_map3_half_body \width
	and x9, x8, #0xffff
	add x9, _vcpu, x9
	str q4, [x9]
	movi v5.16b, #0
	str q5, [x9, #16]
	retire_and_next
.endm

.macro vector_map3_half_memory width, output_size
gadget vector_map3_half_\width\()_m
	effective_address
	ldr x13, [_stream], #8
	mov x25, x7
	and x10, x23, #0xffff
	add x10, _vcpu, x10
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x10, #16]
	.endif
	vector_map3_half_body \width
	add x9, x25, #(\output_size - 1)
	eor x9, x9, x25
	tbnz x9, #12, Lx86_vector_map3_half_cross\@
	mov x7, x25
	translate 1, UML_X86_64_TLB_WRITE_PAGE, \output_size
	.if \output_size == 8
	str d4, [x7]
	.else
	str q4, [x7]
	.endif
	finish_store \output_size
Lx86_vector_map3_half_cross\@:
	.if \output_size == 8
	mov x0, _context
	mov x1, x25
	fmov x2, d4
	mov w3, #8
	helper_call uml_x86_64_gadget_cross_page_store
	cbz w0, Lx86_memory_fault
	.else
	sub sp, sp, #16
	str q4, [sp]
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #16
	helper_call uml_x86_64_gadget_cross_page_vector_store
	add sp, sp, #16
	cbz w0, Lx86_memory_fault
	.endif
	finish_store \output_size
.endm

vector_map3_half_register 128
vector_map3_half_register 256
vector_map3_half_memory 128, 8
vector_map3_half_memory 256, 16

.purgem vector_map3_half_memory
.purgem vector_map3_half_register
.purgem vector_map3_half_body
.purgem vector_map3_half_compute
.purgem vector_map3_round_inputs
