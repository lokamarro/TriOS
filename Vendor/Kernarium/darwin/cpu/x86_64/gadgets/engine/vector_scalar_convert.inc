/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy/MMX and scalar GPR floating-point conversions. */

/* `precise` rounds with FRINTX, which raises the host's precision flag for
 * an inexact result; otherwise FRINTI, which raises nothing. */
.macro vector_scalar_round input, output, element, truncate, precise
	.if \truncate
	.if \element == 32
	frintz s\output, s\input
	.else
	frintz d\output, d\input
	.endif
	.else
	/* The dispatch already holds the guest's rounding mode. */
	.if \element == 32
	.if \precise
	frintx s\output, s\input
	.else
	frinti s\output, s\input
	.endif
	.else
	.if \precise
	frintx d\output, d\input
	.else
	frinti d\output, d\input
	.endif
	.endif
	.endif
.endm

/* Result is returned in x24 (or zero-extended w24 for 32-bit results). A NaN
 * or out-of-range input raises invalid operation: its path leaves while the
 * gadgets check SIMD exceptions, and converts the value at the result's width
 * anyway, so that a checked run finds the host's invalid-operation flag set
 * as x86 sets MXCSR's. An input in range raises precision when it converts
 * inexactly, which FRINTX or, truncated, FCVTZS of the source raises; a
 * double out of the int32 range may have a fraction, so for that conversion
 * FRINTX runs only once the input is known to be in range (see
 * vector_convert.inc). `computed` makes the leave a computed one, for a
 * gadget that converted an earlier lane. */
.macro vector_scalar_fp_to_int input, element, width, truncate, computed=0
	/* MXCSR.DAZ makes a denormal input zero, which converts exactly. */
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w12, #6, Lx86_vector_scalar_input_ready\@
	simd_flush_denormals \input, \element
Lx86_vector_scalar_input_ready\@:
	.set Lx86_vector_scalar_precise\@, (\element == 32 || \width == 64)
	vector_scalar_round \input, 6, \element, \truncate, Lx86_vector_scalar_precise\@
	.if \element == 32
	fcmp s6, s6
	b.vs Lx86_vector_scalar_invalid\@
	.if \width == 32
	mov w14, #0xcf000000
	.else
	mov w14, #0xdf000000
	.endif
	fmov s7, w14
	fcmp s6, s7
	b.lt Lx86_vector_scalar_invalid\@
	.if \width == 32
	mov w14, #0x4f000000
	.else
	mov w14, #0x5f000000
	.endif
	fmov s7, w14
	fcmp s6, s7
	b.ge Lx86_vector_scalar_invalid\@
	.if \truncate
	.if \width == 32
	fcvtzs w24, s\input
	.else
	fcvtzs x24, s\input
	.endif
	.else
	.if \width == 32
	fcvtzs w24, s6
	.else
	fcvtzs x24, s6
	.endif
	.endif
	.else
	fcmp d6, d6
	b.vs Lx86_vector_scalar_invalid\@
	.if \width == 32
	mov x14, #0xc1e0000000000000
	.else
	mov x14, #0xc3e0000000000000
	.endif
	fmov d7, x14
	fcmp d6, d7
	b.lt Lx86_vector_scalar_invalid\@
	.if \width == 32
	mov x14, #0x41e0000000000000
	.else
	mov x14, #0x43e0000000000000
	.endif
	fmov d7, x14
	fcmp d6, d7
	b.ge Lx86_vector_scalar_invalid\@
	.if \truncate
	.if \width == 32
	fcvtzs w24, d\input
	.else
	fcvtzs x24, d\input
	.endif
	.else
	.if !Lx86_vector_scalar_precise\@
	frintx d6, d\input
	.endif
	.if \width == 32
	fcvtzs w24, d6
	.else
	fcvtzs x24, d6
	.endif
	.endif
	.endif
	b Lx86_vector_scalar_integer_ready\@
Lx86_vector_scalar_invalid\@:
	.if \computed
	simd_leave_computed_if_checking
	.else
	simd_leave_if_checking
	.endif
	.if \element == 32
	.if \width == 32
	fcvtzs w24, s6
	.else
	fcvtzs x24, s6
	.endif
	.else
	.if \width == 32
	fcvtzs w24, d6
	.else
	fcvtzs x24, d6
	.endif
	.endif
	mov x24, #1
	lsl x24, x24, #(\width - 1)
Lx86_vector_scalar_integer_ready\@:
.endm

.macro vector_scalar_write_xmm descriptor
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q4, [x9]
	tbz x\descriptor, #63, Lx86_vector_scalar_write_xmm_done\@
	movi v6.16b, #0
	str q6, [x9, #16]
Lx86_vector_scalar_write_xmm_done\@:
.endm

.macro vector_integer_to_scalar_body integer_width, element
	mov v4.16b, v0.16b
	.if \element == 32
	.if \integer_width == 32
	scvtf s6, w24
	.else
	scvtf s6, x24
	.endif
	mov v4.s[0], v6.s[0]
	.else
	.if \integer_width == 32
	scvtf d6, w24
	.else
	scvtf d6, x24
	.endif
	mov v4.d[0], v6.d[0]
	.endif
.endm

.macro vector_integer_to_scalar_register integer_width, element
gadget vector_scalar_i\integer_width\()_f\element\()_rr
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr q0, [_vcpu, x10]
	.if \integer_width == 32
	ldr w24, [_vcpu, x11]
	.else
	ldr x24, [_vcpu, x11]
	.endif
	vector_integer_to_scalar_body \integer_width, \element
	vector_scalar_write_xmm 8
	retire_and_next
.endm

.macro vector_integer_to_scalar_memory integer_width, element
gadget vector_scalar_i\integer_width\()_f\element\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	page_cross_check (\integer_width / 8), Lx86_vector_integer_to_scalar_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\integer_width / 8)
	.if \integer_width == 32
	ldr w24, [x7]
	.else
	ldr x24, [x7]
	.endif
	b Lx86_vector_integer_to_scalar_loaded\@
Lx86_vector_integer_to_scalar_cross\@:
	cross_page_load (\integer_width / 8)
	.if \integer_width == 32
	mov w24, w0
	.else
	mov x24, x0
	.endif
Lx86_vector_integer_to_scalar_loaded\@:
	and x10, x26, #0xffff
	ldr q0, [_vcpu, x10]
	vector_integer_to_scalar_body \integer_width, \element
	vector_scalar_write_xmm 23
	retire_and_next
.endm

.macro vector_integer_to_scalar_family integer_width, element
	vector_integer_to_scalar_register \integer_width, \element
	vector_integer_to_scalar_memory \integer_width, \element
.endm

vector_integer_to_scalar_family 32, 32
vector_integer_to_scalar_family 64, 32
vector_integer_to_scalar_family 32, 64
vector_integer_to_scalar_family 64, 64

.purgem vector_integer_to_scalar_family
.purgem vector_integer_to_scalar_memory
.purgem vector_integer_to_scalar_register
.purgem vector_integer_to_scalar_body

.macro vector_pi_body element
	mov v4.16b, v0.16b
	.if \element == 32
	scvtf v6.2s, v1.2s
	mov v4.d[0], v6.d[0]
	.else
	sshll v6.2d, v1.2s, #0
	scvtf v4.2d, v6.2d
	.endif
.endm

.macro vector_pi_register element
gadget vector_pi_f\element\()_rr
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr q0, [_vcpu, x10]
	ldr d1, [_vcpu, x11]
	vector_pi_body \element
	vector_scalar_write_xmm 8
	strh wzr, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	retire_and_next
.endm

.macro vector_pi_memory element
gadget vector_pi_f\element\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	page_cross_check 8, Lx86_vector_pi_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr d1, [x7]
	b Lx86_vector_pi_loaded\@
Lx86_vector_pi_cross\@:
	cross_page_load 8
	fmov d1, x0
Lx86_vector_pi_loaded\@:
	and x10, x26, #0xffff
	ldr q0, [_vcpu, x10]
	vector_pi_body \element
	vector_scalar_write_xmm 23
	retire_and_next
.endm

vector_pi_register 32
vector_pi_memory 32
vector_pi_register 64
vector_pi_memory 64

.purgem vector_pi_memory
.purgem vector_pi_register
.purgem vector_pi_body

.macro vector_fp_to_gpr_register element, width, truncate
gadget vector_scalar_f\element\()_i\width\()_\truncate\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	.if \element == 32
	ldr s1, [_vcpu, x10]
	.else
	ldr d1, [_vcpu, x10]
	.endif
	vector_scalar_fp_to_int 1, \element, \width, \truncate
	.if \width == 32
	mov w24, w24
	.endif
	str x24, [_vcpu, x9]
	retire_and_next
.endm

.macro vector_fp_to_gpr_memory element, width, truncate
gadget vector_scalar_f\element\()_i\width\()_\truncate\()_rm
	effective_address
	mov x25, x7
	page_cross_check (\element / 8), Lx86_vector_fp_to_gpr_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\element / 8)
	.if \element == 32
	ldr s1, [x7]
	.else
	ldr d1, [x7]
	.endif
	b Lx86_vector_fp_to_gpr_loaded\@
Lx86_vector_fp_to_gpr_cross\@:
	cross_page_load (\element / 8)
	.if \element == 32
	fmov s1, w0
	.else
	fmov d1, x0
	.endif
Lx86_vector_fp_to_gpr_loaded\@:
	vector_scalar_fp_to_int 1, \element, \width, \truncate
	and x9, x23, #0xffff
	.if \width == 32
	mov w24, w24
	.endif
	str x24, [_vcpu, x9]
	retire_and_next
.endm

.macro vector_fp_to_gpr_family element, width, truncate
	vector_fp_to_gpr_register \element, \width, \truncate
	vector_fp_to_gpr_memory \element, \width, \truncate
.endm

vector_fp_to_gpr_family 32, 32, 0
vector_fp_to_gpr_family 32, 32, 1
vector_fp_to_gpr_family 32, 64, 0
vector_fp_to_gpr_family 32, 64, 1
vector_fp_to_gpr_family 64, 32, 0
vector_fp_to_gpr_family 64, 32, 1
vector_fp_to_gpr_family 64, 64, 0
vector_fp_to_gpr_family 64, 64, 1

.purgem vector_fp_to_gpr_family
.purgem vector_fp_to_gpr_memory
.purgem vector_fp_to_gpr_register

/* Two lanes, converted one after the other: the second's leave follows the
 * first's computation, so the gadget saves FPSR first, at `save`. */
.macro vector_fp_to_pi_body element, truncate, save
	simd_save_status_branch \save
	movi v4.16b, #0
	vector_scalar_fp_to_int 1, \element, 32, \truncate, 1
	mov v4.s[0], w24
	.if \element == 32
	dup v2.4s, v1.s[1]
	.else
	dup v2.2d, v1.d[1]
	.endif
	vector_scalar_fp_to_int 2, \element, 32, \truncate, 1
	mov v4.s[1], w24
.endm

.macro vector_fp_to_pi_register element, truncate
gadget vector_f\element\()_pi_\truncate\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ldr q1, [_vcpu, x10]
	vector_fp_to_pi_body \element, \truncate, Lx86_vector_fp_to_pi_save\@
	str d4, [_vcpu, x9]
	synchronize_mmx x9, d4
	retire_and_next
	simd_save_status_path Lx86_vector_fp_to_pi_save\@
.endm

.macro vector_fp_to_pi_memory element, truncate
gadget vector_f\element\()_pi_\truncate\()_rm
	effective_address
	mov x25, x7
	vector_memory_source (\element / 4), Lx86_vector_fp_to_pi_cross_fault\@, frame=16, scalar=1
	vector_fp_to_pi_body \element, \truncate, Lx86_vector_fp_to_pi_save\@
	vector_write 64, 23
	retire_and_next
	simd_save_status_path Lx86_vector_fp_to_pi_save\@
	.if \element == 64
	vector_memory_source_fault Lx86_vector_fp_to_pi_cross_fault\@, 16
	.endif
.endm

vector_fp_to_pi_register 32, 0
vector_fp_to_pi_memory 32, 0
vector_fp_to_pi_register 32, 1
vector_fp_to_pi_memory 32, 1
vector_fp_to_pi_register 64, 0
vector_fp_to_pi_memory 64, 0
vector_fp_to_pi_register 64, 1
vector_fp_to_pi_memory 64, 1

.purgem vector_fp_to_pi_memory
.purgem vector_fp_to_pi_register
.purgem vector_fp_to_pi_body
.purgem vector_scalar_write_xmm
.purgem vector_scalar_fp_to_int
.purgem vector_scalar_round
