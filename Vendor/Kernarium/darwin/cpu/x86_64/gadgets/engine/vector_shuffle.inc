/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed floating-point unpack, shuffle, horizontal, and add/sub layouts. */

/* HADD, HSUB and ADDSUB follow x86's NaN, DAZ and FTZ rules (see
 * vector_float.inc). Their first operand is the lower element of each
 * pair, or the first source for ADDSUB, as x86 hardware chooses between two
 * NaNs; the SDM ranks only distinct source operands. A sum or difference
 * that is tiny is exact, so FTZ flushes denormal results alone, raising
 * underflow and precision. Each
 * computes only the lanes x86 does, whose host flags a checked run reads:
 * HSUB subtracts the odd elements from the even ones, and ADDSUB adds the
 * second source with its even lanes negated, a - b being a + -b. */
.macro vector_horizontal_inputs element, width
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w12, #6, Lx86_vector_horizontal_inputs_ready\@
	.if \width == 256
	simd_flush_denormals_each \element, 0, 1, 2, 3
	.else
	simd_flush_denormals_each \element, 0, 1
	.endif
Lx86_vector_horizontal_inputs_ready\@:
.endm

/* Repair the NaN results of v`output` from the pairs of v`left` and
 * v`right`. Uses v16 to v22 and x14. */
.macro vector_horizontal_nan operation, element, output, left, right
	.ifc \operation,addsub
	simd_x86_nan \element, \output, \left, \right, 1, 0
	/* ADDSUB's even lanes add the second source negated, which the host
	 * returns negated where it is the only NaN: x86 returns it quieted. */
	.if \element == 32
	fcmeq v16.4s, v\left\().4s, v\left\().4s
	fcmeq v17.4s, v\right\().4s, v\right\().4s
	movi v18.4s, #0x40, lsl #16
	.else
	fcmeq v16.2d, v\left\().2d, v\left\().2d
	fcmeq v17.2d, v\right\().2d, v\right\().2d
	mov x14, #0x0008000000000000
	dup v18.2d, x14
	.endif
	bic v16.16b, v16.16b, v17.16b
	orr v18.16b, v\right\().16b, v18.16b
	bit v\output\().16b, v18.16b, v16.16b
	.else
	.if \element == 32
	uzp1 v21.4s, v\left\().4s, v\right\().4s
	uzp2 v22.4s, v\left\().4s, v\right\().4s
	.else
	uzp1 v21.2d, v\left\().2d, v\right\().2d
	uzp2 v22.2d, v\left\().2d, v\right\().2d
	.endif
	simd_x86_nan \element, \output, 21, 22, 1, 0
	.endif
.endm

.macro vector_horizontal_results operation, element, width
	simd_special_lanes \element, 4
	.if \width == 256
	cbnz w14, Lx86_vector_horizontal_special\@
	simd_special_lanes \element, 5
	.endif
	cbz w14, Lx86_vector_horizontal_nan_ready\@
Lx86_vector_horizontal_special\@:
	b Lx86_vector_horizontal_repair\@
Lx86_vector_horizontal_nan_ready\@:
	tbz w12, #15, Lx86_vector_horizontal_ftz_ready\@
	.if \width == 256
	simd_flush_denormal_results \element, 4, 5
	.else
	simd_flush_denormal_results \element, 4
	.endif
Lx86_vector_horizontal_ftz_ready\@:
	.pushsection GADGET_SECTION_COLD
Lx86_vector_horizontal_repair\@:
	simd_leave_computed_if_checking
	vector_horizontal_nan \operation, \element, 4, 0, 1
	.if \width == 256
	vector_horizontal_nan \operation, \element, 5, 2, 3
	.endif
	b Lx86_vector_horizontal_nan_ready\@
	.popsection
.endm

.macro vector_horizontal_operation operation, element, width
	.ifc \operation,hadd
	.if \element == 32
	faddp v4.4s, v0.4s, v1.4s
	.if \width == 256
	faddp v5.4s, v2.4s, v3.4s
	.endif
	.else
	faddp d4, v0.2d
	faddp d6, v1.2d
	mov v4.d[1], v6.d[0]
	.if \width == 256
	faddp d5, v2.2d
	faddp d6, v3.2d
	mov v5.d[1], v6.d[0]
	.endif
	.endif
	.endif
	.ifc \operation,hsub
	.if \element == 32
	uzp1 v6.4s, v0.4s, v1.4s
	uzp2 v7.4s, v0.4s, v1.4s
	fsub v4.4s, v6.4s, v7.4s
	.if \width == 256
	uzp1 v6.4s, v2.4s, v3.4s
	uzp2 v7.4s, v2.4s, v3.4s
	fsub v5.4s, v6.4s, v7.4s
	.endif
	.else
	ext v6.16b, v0.16b, v0.16b, #8
	fsub d4, d0, d6
	ext v6.16b, v1.16b, v1.16b, #8
	fsub d6, d1, d6
	mov v4.d[1], v6.d[0]
	.if \width == 256
	ext v6.16b, v2.16b, v2.16b, #8
	fsub d5, d2, d6
	ext v6.16b, v3.16b, v3.16b, #8
	fsub d6, d3, d6
	mov v5.d[1], v6.d[0]
	.endif
	.endif
	.endif
	.ifc \operation,addsub
	.if \element == 32
	movi v6.4s, #0x80, lsl #24
	ushr v6.2d, v6.2d, #32
	.if \width == 256
	eor v7.16b, v3.16b, v6.16b
	fadd v5.4s, v2.4s, v7.4s
	.endif
	eor v6.16b, v1.16b, v6.16b
	fadd v4.4s, v0.4s, v6.4s
	.else
	fneg d6, d1
	mov v6.d[1], v1.d[1]
	fadd v4.2d, v0.2d, v6.2d
	.if \width == 256
	fneg d7, d3
	mov v7.d[1], v3.d[1]
	fadd v5.2d, v2.2d, v7.2d
	.endif
	.endif
	.endif
.endm

/* The common path repairs a NaN result alone; the exact path
 * (`simd_exact_branch`) applies DAZ and FTZ and the SIMD checks too. */
.macro vector_horizontal_body operation, element, width
	simd_exact_branch Lx86_vector_horizontal_common\@
	vector_horizontal_inputs \element, \width
	vector_horizontal_operation \operation, \element, \width
	vector_horizontal_results \operation, \element, \width
	simd_exact_end Lx86_vector_horizontal_common\@, Lx86_vector_horizontal_done\@
Lx86_vector_horizontal_common\@:
	vector_horizontal_operation \operation, \element, \width
	.if \width == 256
	simd_nan_lanes \element, Lx86_vector_horizontal_nan\@, 4, 5
	.else
	simd_nan_lanes \element, Lx86_vector_horizontal_nan\@, 4
	.endif
Lx86_vector_horizontal_done\@:
	.pushsection GADGET_SECTION_COLD
Lx86_vector_horizontal_nan\@:
	vector_horizontal_nan \operation, \element, 4, 0, 1
	.if \width == 256
	vector_horizontal_nan \operation, \element, 5, 2, 3
	.endif
	b Lx86_vector_horizontal_done\@
	.popsection
.endm

.macro vector_shuffle_ps_index lane, shift, right
	lsr w16, w13, #\shift
	and w16, w16, #3
	.if \right
	add w16, w16, #4
	.endif
	lsl w16, w16, #2
	mov w17, #0x0101
	movk w17, #0x0101, lsl #16
	mul w16, w16, w17
	mov w17, #0x0100
	movk w17, #0x0302, lsl #16
	add w16, w16, w17
	mov v6.s[\lane], w16
.endm

.macro vector_shuffle_body operation, element, width
	.ifc \operation,unpack_low
	.if \element == 32
	zip1 v4.4s, v0.4s, v1.4s
	.if \width == 256
	zip1 v5.4s, v2.4s, v3.4s
	.endif
	.else
	zip1 v4.2d, v0.2d, v1.2d
	.if \width == 256
	zip1 v5.2d, v2.2d, v3.2d
	.endif
	.endif
	.endif
	.ifc \operation,unpack_high
	.if \element == 32
	zip2 v4.4s, v0.4s, v1.4s
	.if \width == 256
	zip2 v5.4s, v2.4s, v3.4s
	.endif
	.else
	zip2 v4.2d, v0.2d, v1.2d
	.if \width == 256
	zip2 v5.2d, v2.2d, v3.2d
	.endif
	.endif
	.endif
	.ifc \operation,shuffle
	.if \element == 32
	vector_shuffle_ps_index 0, 0, 0
	vector_shuffle_ps_index 1, 2, 0
	vector_shuffle_ps_index 2, 4, 1
	vector_shuffle_ps_index 3, 6, 1
	tbl v4.16b, {v0.16b, v1.16b}, v6.16b
	.if \width == 256
	tbl v5.16b, {v2.16b, v3.16b}, v6.16b
	.endif
	.else
	tbz w13, #0, Lx86_vector_shuffle_pd_left_low\@
	mov v4.d[0], v0.d[1]
	b Lx86_vector_shuffle_pd_left_done\@
Lx86_vector_shuffle_pd_left_low\@:
	mov v4.d[0], v0.d[0]
Lx86_vector_shuffle_pd_left_done\@:
	tbz w13, #1, Lx86_vector_shuffle_pd_right_low\@
	mov v4.d[1], v1.d[1]
	b Lx86_vector_shuffle_pd_right_done\@
Lx86_vector_shuffle_pd_right_low\@:
	mov v4.d[1], v1.d[0]
Lx86_vector_shuffle_pd_right_done\@:
	.if \width == 256
	tbz w13, #2, Lx86_vector_shuffle_pd_upper_left_low\@
	mov v5.d[0], v2.d[1]
	b Lx86_vector_shuffle_pd_upper_left_done\@
Lx86_vector_shuffle_pd_upper_left_low\@:
	mov v5.d[0], v2.d[0]
Lx86_vector_shuffle_pd_upper_left_done\@:
	tbz w13, #3, Lx86_vector_shuffle_pd_upper_right_low\@
	mov v5.d[1], v3.d[1]
	b Lx86_vector_shuffle_pd_upper_right_done\@
Lx86_vector_shuffle_pd_upper_right_low\@:
	mov v5.d[1], v3.d[0]
Lx86_vector_shuffle_pd_upper_right_done\@:
	.endif
	.endif
	.endif
	.ifc \operation,hadd
	vector_horizontal_body \operation, \element, \width
	.endif
	.ifc \operation,hsub
	vector_horizontal_body \operation, \element, \width
	.endif
	.ifc \operation,addsub
	vector_horizontal_body \operation, \element, \width
	.endif
.endm

.macro vector_shuffle_register operation, element, width, control
gadget vector_\operation\()_\element\()_\width\()_rr
	ldr x8, [_stream], #8
	.if \control
	ldr x13, [_stream], #8
	.endif
	vector_read_pair \width
	vector_shuffle_body \operation, \element, \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_shuffle_memory operation, element, width, control
gadget vector_\operation\()_\element\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_shuffle_cross_fault\@
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	.if \control
	ubfx x13, x26, #16, #8
	.endif
	vector_shuffle_body \operation, \element, \width
	vector_write_xmm \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_shuffle_cross_fault\@
.endm

.macro vector_shuffle_family operation, element, width, control=0
	vector_shuffle_register \operation, \element, \width, \control
	vector_shuffle_memory \operation, \element, \width, \control
.endm

vector_shuffle_family unpack_low, 32, 128
vector_shuffle_family unpack_low, 32, 256
vector_shuffle_family unpack_low, 64, 128
vector_shuffle_family unpack_low, 64, 256
vector_shuffle_family unpack_high, 32, 128
vector_shuffle_family unpack_high, 32, 256
vector_shuffle_family unpack_high, 64, 128
vector_shuffle_family unpack_high, 64, 256
vector_shuffle_family shuffle, 32, 128, 1
vector_shuffle_family shuffle, 32, 256, 1
vector_shuffle_family shuffle, 64, 128, 1
vector_shuffle_family shuffle, 64, 256, 1
vector_shuffle_family hadd, 32, 128
vector_shuffle_family hadd, 32, 256
vector_shuffle_family hadd, 64, 128
vector_shuffle_family hadd, 64, 256
vector_shuffle_family hsub, 32, 128
vector_shuffle_family hsub, 32, 256
vector_shuffle_family hsub, 64, 128
vector_shuffle_family hsub, 64, 256
vector_shuffle_family addsub, 32, 128
vector_shuffle_family addsub, 32, 256
vector_shuffle_family addsub, 64, 128
vector_shuffle_family addsub, 64, 256

.purgem vector_shuffle_family
.purgem vector_shuffle_memory
.purgem vector_shuffle_register
.purgem vector_shuffle_body
.purgem vector_horizontal_body
.purgem vector_horizontal_operation
.purgem vector_horizontal_results
.purgem vector_horizontal_nan
.purgem vector_horizontal_inputs
.purgem vector_shuffle_ps_index
