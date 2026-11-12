/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AVX/AVX2 variable permutes, shifts, broadcasts, and F16C expansion. */

.macro vector_map2_misc_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	movi v6.16b, #0
	str q6, [x9, #16]
	.endif
.endm

.macro vector_map2_permute_dword_indices output, control, mask
	movi v\mask\().4s, #7
	and v\output\().16b, v\control\().16b, v\mask\().16b
	shl v\output\().4s, v\output\().4s, #2
	mov w14, #0x0101
	movk w14, #0x0101, lsl #16
	dup v10.4s, w14
	mul v\output\().4s, v\output\().4s, v10.4s
	mov w14, #0x0100
	movk w14, #0x0302, lsl #16
	dup v\mask\().4s, w14
	add v\output\().4s, v\output\().4s, v\mask\().4s
.endm

.macro vector_map2_permute_body opcode, width
	.if \opcode == 0x0c
	movi v7.4s, #3
	and v6.16b, v1.16b, v7.16b
	shl v6.4s, v6.4s, #2
	mov w14, #0x0101
	movk w14, #0x0101, lsl #16
	dup v10.4s, w14
	mul v6.4s, v6.4s, v10.4s
	mov w14, #0x0100
	movk w14, #0x0302, lsl #16
	dup v7.4s, w14
	add v6.4s, v6.4s, v7.4s
	tbl v4.16b, {v0.16b}, v6.16b
	.if \width == 256
	movi v7.4s, #3
	and v6.16b, v3.16b, v7.16b
	shl v6.4s, v6.4s, #2
	mov w14, #0x0101
	movk w14, #0x0101, lsl #16
	dup v10.4s, w14
	mul v6.4s, v6.4s, v10.4s
	mov w14, #0x0100
	movk w14, #0x0302, lsl #16
	dup v7.4s, w14
	add v6.4s, v6.4s, v7.4s
	tbl v5.16b, {v2.16b}, v6.16b
	.endif
	.elseif \opcode == 0x0d
	mov x16, #0x0101
	movk x16, #0x0101, lsl #16
	movk x16, #0x0101, lsl #32
	movk x16, #0x0101, lsl #48
	mov x14, #0x0100
	movk x14, #0x0302, lsl #16
	movk x14, #0x0504, lsl #32
	movk x14, #0x0706, lsl #48
	.irp lane, 0, 1
	umov x15, v1.d[\lane]
	ubfx x15, x15, #1, #1
	lsl x15, x15, #3
	mul x15, x15, x16
	add x15, x15, x14
	mov v6.d[\lane], x15
	.endr
	tbl v4.16b, {v0.16b}, v6.16b
	.if \width == 256
	.irp lane, 0, 1
	umov x15, v3.d[\lane]
	ubfx x15, x15, #1, #1
	lsl x15, x15, #3
	mul x15, x15, x16
	add x15, x15, x14
	mov v6.d[\lane], x15
	.endr
	tbl v5.16b, {v2.16b}, v6.16b
	.endif
	.else
	mov v8.16b, v1.16b
	mov v9.16b, v3.16b
	vector_map2_permute_dword_indices 6, 0, 7
	tbl v4.16b, {v8.16b, v9.16b}, v6.16b
	vector_map2_permute_dword_indices 6, 2, 7
	tbl v5.16b, {v8.16b, v9.16b}, v6.16b
	.endif
.endm

.macro vector_map2_permute_register symbol, opcode, width
gadget vector_map2_permute_\symbol\()_\width\()_rr
	ldr x8, [_stream], #8
	vector_read_pair \width
	vector_map2_permute_body \opcode, \width
	vector_map2_misc_write \width, 8
	retire_and_next
.endm

.macro vector_map2_permute_memory symbol, opcode, width
gadget vector_map2_permute_\symbol\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_map2_permute_cross_fault\@
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_map2_permute_body \opcode, \width
	vector_map2_misc_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map2_permute_cross_fault\@
.endm

.macro vector_map2_permute_family symbol, opcode, width
	vector_map2_permute_register \symbol, \opcode, \width
	vector_map2_permute_memory \symbol, \opcode, \width
.endm

vector_map2_permute_family 0c, 0x0c, 128
vector_map2_permute_family 0c, 0x0c, 256
vector_map2_permute_family 0d, 0x0d, 128
vector_map2_permute_family 0d, 0x0d, 256
vector_map2_permute_family 16, 0x16, 256
vector_map2_permute_family 36, 0x36, 256

.purgem vector_map2_permute_family
.purgem vector_map2_permute_memory
.purgem vector_map2_permute_register
.purgem vector_map2_permute_body
.purgem vector_map2_permute_dword_indices

.macro vector_map2_shift_lane opcode, element, output, value, counts
	.if \element == 32
	mov w14, #32
	dup v7.4s, w14
	cmhi v8.4s, v7.4s, v\counts\().4s
	bsl v8.16b, v\counts\().16b, v7.16b
	.if \opcode == 0x45
	neg v8.4s, v8.4s
	ushl v\output\().4s, v\value\().4s, v8.4s
	.elseif \opcode == 0x46
	neg v8.4s, v8.4s
	sshl v\output\().4s, v\value\().4s, v8.4s
	.else
	ushl v\output\().4s, v\value\().4s, v8.4s
	.endif
	.else
	mov x14, #64
	dup v7.2d, x14
	cmhi v8.2d, v7.2d, v\counts\().2d
	bsl v8.16b, v\counts\().16b, v7.16b
	.if \opcode == 0x45
	neg v8.2d, v8.2d
	ushl v\output\().2d, v\value\().2d, v8.2d
	.elseif \opcode == 0x46
	neg v8.2d, v8.2d
	sshl v\output\().2d, v\value\().2d, v8.2d
	.else
	ushl v\output\().2d, v\value\().2d, v8.2d
	.endif
	.endif
.endm

.macro vector_map2_shift_body opcode, element, width
	vector_map2_shift_lane \opcode, \element, 4, 0, 1
	.if \width == 256
	vector_map2_shift_lane \opcode, \element, 5, 2, 3
	.endif
.endm

.macro vector_map2_shift_register symbol, opcode, element, width
gadget vector_map2_shift_\symbol\()_\element\()_\width\()_rr
	ldr x8, [_stream], #8
	vector_read_pair \width
	vector_map2_shift_body \opcode, \element, \width
	vector_map2_misc_write \width, 8
	retire_and_next
.endm

.macro vector_map2_shift_memory symbol, opcode, element, width
gadget vector_map2_shift_\symbol\()_\element\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_map2_shift_cross_fault\@
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_map2_shift_body \opcode, \element, \width
	vector_map2_misc_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map2_shift_cross_fault\@
.endm

.macro vector_map2_shift_family symbol, opcode, element, width
	vector_map2_shift_register \symbol, \opcode, \element, \width
	vector_map2_shift_memory \symbol, \opcode, \element, \width
.endm

.irp operation, 45, 46, 47
vector_map2_shift_family \operation, 0x\operation, 32, 128
vector_map2_shift_family \operation, 0x\operation, 32, 256
vector_map2_shift_family \operation, 0x\operation, 64, 128
vector_map2_shift_family \operation, 0x\operation, 64, 256
.endr

.purgem vector_map2_shift_family
.purgem vector_map2_shift_memory
.purgem vector_map2_shift_register
.purgem vector_map2_shift_body
.purgem vector_map2_shift_lane

.macro vector_map2_broadcast_body element, width
	.if \element == 8
	dup v4.16b, v1.b[0]
	.elseif \element == 16
	dup v4.8h, v1.h[0]
	.elseif \element == 32
	dup v4.4s, v1.s[0]
	.elseif \element == 64
	dup v4.2d, v1.d[0]
	.else
	mov v4.16b, v1.16b
	.endif
	.if \width == 256
	mov v5.16b, v4.16b
	.endif
.endm

.macro vector_map2_broadcast_register element, width
gadget vector_map2_broadcast_\element\()_\width\()_r
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ldr q1, [_vcpu, x10]
	vector_map2_broadcast_body \element, \width
	vector_map2_misc_write \width, 8
	retire_and_next
.endm

.macro vector_map2_broadcast_memory element, width
gadget vector_map2_broadcast_\element\()_\width\()_m
	effective_address
	mov x25, x7
	vector_memory_source (\element / 8), Lx86_vector_map2_broadcast_cross_fault\@, frame=16, scalar=1
	vector_map2_broadcast_body \element, \width
	vector_map2_misc_write \width, 23
	retire_and_next
	.if \element == 128
	vector_memory_source_fault Lx86_vector_map2_broadcast_cross_fault\@, 16
	.endif
.endm

.macro vector_map2_broadcast_family element, width
	vector_map2_broadcast_register \element, \width
	vector_map2_broadcast_memory \element, \width
.endm

.irp element, 8, 16, 32, 64
vector_map2_broadcast_family \element, 128
vector_map2_broadcast_family \element, 256
.endr
vector_map2_broadcast_family 128, 256

.purgem vector_map2_broadcast_family
.purgem vector_map2_broadcast_memory
.purgem vector_map2_broadcast_register
.purgem vector_map2_broadcast_body

/* Widening raises invalid operation only for a signalling NaN, which it
 * leaves a NaN; the host raises it for the same lanes, so the leave keeps
 * FPSR. */
.macro vector_map2_half_body width
	fcvtl v4.4s, v1.4h
	.if \width == 256
	fcvtl2 v5.4s, v1.8h
	.endif
	simd_check_result 32, \width, 0
.endm

.macro vector_map2_half_register width
gadget vector_map2_half_\width\()_r
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ldr q1, [_vcpu, x10]
	vector_map2_half_body \width
	vector_map2_misc_write \width, 8
	retire_and_next
.endm

.macro vector_map2_half_memory width, input_size
gadget vector_map2_half_\width\()_m
	effective_address
	mov x25, x7
	vector_memory_source \input_size, Lx86_vector_map2_half_cross_fault\@, frame=16, scalar=1
	vector_map2_half_body \width
	vector_map2_misc_write \width, 23
	retire_and_next
	.if \input_size == 16
	vector_memory_source_fault Lx86_vector_map2_half_cross_fault\@, 16
	.endif
.endm

vector_map2_half_register 128
vector_map2_half_memory 128, 8
vector_map2_half_register 256
vector_map2_half_memory 256, 16

.purgem vector_map2_half_memory
.purgem vector_map2_half_register
.purgem vector_map2_half_body
.purgem vector_map2_misc_write
