/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Immediate shuffle, blend, align, and 128-bit lane-control operations. */

.macro vector_map3_qword_indices output, first
	mov x16, #0x0101
	movk x16, #0x0101, lsl #16
	movk x16, #0x0101, lsl #32
	movk x16, #0x0101, lsl #48
	mov x17, #0x0100
	movk x17, #0x0302, lsl #16
	movk x17, #0x0504, lsl #32
	movk x17, #0x0706, lsl #48
	.irp lane, 0, 1
	lsr w14, w13, #((\first + \lane) * 2)
	and w14, w14, #3
	lsl w14, w14, #3
	mul x14, x14, x16
	add x14, x14, x17
	mov v\output\().d[\lane], x14
	.endr
.endm

.macro vector_map3_mask_h output
	movi v\output\().16b, #0
	.irp lane, 0, 1, 2, 3, 4, 5, 6, 7
	ubfx w14, w13, #\lane, #1
	neg w14, w14
	mov v\output\().h[\lane], w14
	.endr
.endm

.macro vector_map3_select_128 output, shift
	ubfx w14, w13, #\shift, #4
	tbnz w14, #3, Lx86_vector_map3_select_zero\@
	and w14, w14, #3
	cmp w14, #1
	b.eq Lx86_vector_map3_select_left_high\@
	cmp w14, #2
	b.eq Lx86_vector_map3_select_right_low\@
	cmp w14, #3
	b.eq Lx86_vector_map3_select_right_high\@
	mov v\output\().16b, v0.16b
	b Lx86_vector_map3_select_done\@
Lx86_vector_map3_select_left_high\@:
	mov v\output\().16b, v2.16b
	b Lx86_vector_map3_select_done\@
Lx86_vector_map3_select_right_low\@:
	mov v\output\().16b, v1.16b
	b Lx86_vector_map3_select_done\@
Lx86_vector_map3_select_right_high\@:
	mov v\output\().16b, v3.16b
	b Lx86_vector_map3_select_done\@
Lx86_vector_map3_select_zero\@:
	movi v\output\().16b, #0
Lx86_vector_map3_select_done\@:
.endm

.macro vector_map3_variable_blend element, output, left, right, mask
	.if \element == 8
	cmlt v\mask\().16b, v\mask\().16b, #0
	.elseif \element == 32
	cmlt v\mask\().4s, v\mask\().4s, #0
	.else
	cmlt v\mask\().2d, v\mask\().2d, #0
	.endif
	bsl v\mask\().16b, v\right\().16b, v\left\().16b
	mov v\output\().16b, v\mask\().16b
.endm

.macro vector_map3_shuffle_lane opcode, width
	.if \opcode == 0x00 || \opcode == 0x01
	vector_map3_qword_indices 6, 0
	mov v8.16b, v1.16b
	mov v9.16b, v3.16b
	tbl v4.16b, {v8.16b, v9.16b}, v6.16b
	.if \width == 256
	vector_map3_qword_indices 7, 2
	tbl v5.16b, {v8.16b, v9.16b}, v7.16b
	.endif
	.elseif \opcode == 0x02 || \opcode == 0x0c
	vector_map3_mask_s 6, 0
	bsl v6.16b, v1.16b, v0.16b
	mov v4.16b, v6.16b
	.if \width == 256
	vector_map3_mask_s 7, 4
	bsl v7.16b, v3.16b, v2.16b
	mov v5.16b, v7.16b
	.endif
	.elseif \opcode == 0x04
	vector_shuffle_dword_indices
	tbl v4.16b, {v1.16b}, v6.16b
	.if \width == 256
	tbl v5.16b, {v3.16b}, v6.16b
	.endif
	.elseif \opcode == 0x05
	tbz w13, #0, Lx86_vector_map3_pd_0_low\@
	mov v4.d[0], v1.d[1]
	b Lx86_vector_map3_pd_0_done\@
Lx86_vector_map3_pd_0_low\@:
	mov v4.d[0], v1.d[0]
Lx86_vector_map3_pd_0_done\@:
	tbz w13, #1, Lx86_vector_map3_pd_1_low\@
	mov v4.d[1], v1.d[1]
	b Lx86_vector_map3_pd_1_done\@
Lx86_vector_map3_pd_1_low\@:
	mov v4.d[1], v1.d[0]
Lx86_vector_map3_pd_1_done\@:
	.if \width == 256
	tbz w13, #2, Lx86_vector_map3_pd_2_low\@
	mov v5.d[0], v3.d[1]
	b Lx86_vector_map3_pd_2_done\@
Lx86_vector_map3_pd_2_low\@:
	mov v5.d[0], v3.d[0]
Lx86_vector_map3_pd_2_done\@:
	tbz w13, #3, Lx86_vector_map3_pd_3_low\@
	mov v5.d[1], v3.d[1]
	b Lx86_vector_map3_pd_3_done\@
Lx86_vector_map3_pd_3_low\@:
	mov v5.d[1], v3.d[0]
Lx86_vector_map3_pd_3_done\@:
	.endif
	.elseif \opcode == 0x06 || \opcode == 0x46
	vector_map3_select_128 4, 0
	vector_map3_select_128 5, 4
	.elseif \opcode == 0x0d
	vector_map3_mask_d 6, 0
	bsl v6.16b, v1.16b, v0.16b
	mov v4.16b, v6.16b
	.if \width == 256
	vector_map3_mask_d 7, 2
	bsl v7.16b, v3.16b, v2.16b
	mov v5.16b, v7.16b
	.endif
	.elseif \opcode == 0x0e
	vector_map3_mask_h 6
	bsl v6.16b, v1.16b, v0.16b
	mov v4.16b, v6.16b
	.if \width == 256
	vector_map3_mask_h 7
	bsl v7.16b, v3.16b, v2.16b
	mov v5.16b, v7.16b
	.endif
	.elseif \opcode == 0x0f
	/* PALIGNR reads the concatenation through TBL, which answers an index
	 * past the table with zero, and that is also what the architecture
	 * produces for a byte count of 32 or more. The index addition is
	 * byte-wise, so a count of 241 or more would wrap back into the table
	 * and resurrect a byte the shift had discarded; clamp the count to the
	 * table's size, which leaves every smaller count untouched. */
	vector_map3_index_sequence 6
	mov w14, #32
	cmp w13, w14
	csel w14, w13, w14, lo
	dup v7.16b, w14
	add v6.16b, v6.16b, v7.16b
	.if \width == 64
	mov v7.d[0], v1.d[0]
	mov v7.d[1], v0.d[0]
	tbl v4.16b, {v7.16b}, v6.16b
	.else
	mov v8.16b, v1.16b
	mov v9.16b, v0.16b
	tbl v4.16b, {v8.16b, v9.16b}, v6.16b
	.if \width == 256
	mov v8.16b, v3.16b
	mov v9.16b, v2.16b
	tbl v5.16b, {v8.16b, v9.16b}, v6.16b
	.endif
	.endif
	.elseif \opcode == 0x18 || \opcode == 0x38
	mov v4.16b, v0.16b
	mov v5.16b, v2.16b
	tbz w13, #0, Lx86_vector_map3_insert_low\@
	mov v5.16b, v1.16b
	b Lx86_vector_map3_insert_done\@
Lx86_vector_map3_insert_low\@:
	mov v4.16b, v1.16b
Lx86_vector_map3_insert_done\@:
	.elseif \opcode == 0x4a || \opcode == 0x4b || \opcode == 0x4c
	lsr w14, w13, #4
	and w14, w14, #0xf
	lsl x14, x14, #5
	add x14, _vcpu, x14
	add x14, x14, #UML_X86_64_CPU_VECTORS
	ldr q6, [x14]
	.if \opcode == 0x4a
	vector_map3_variable_blend 32, 4, 0, 1, 6
	.elseif \opcode == 0x4b
	vector_map3_variable_blend 64, 4, 0, 1, 6
	.else
	vector_map3_variable_blend 8, 4, 0, 1, 6
	.endif
	.if \width == 256
	ldr q7, [x14, #16]
	.if \opcode == 0x4a
	vector_map3_variable_blend 32, 5, 2, 3, 7
	.elseif \opcode == 0x4b
	vector_map3_variable_blend 64, 5, 2, 3, 7
	.else
	vector_map3_variable_blend 8, 5, 2, 3, 7
	.endif
	.endif
	.endif
.endm

.macro vector_map3_shuffle_input_size opcode, width
	.set Lx86_vector_map3_shuffle_input_size, (\width / 8)
	.if \opcode == 0x18 || \opcode == 0x38
	.set Lx86_vector_map3_shuffle_input_size, 16
	.endif
.endm

.macro vector_map3_shuffle_register symbol, opcode, width
gadget vector_map3_shuffle_\symbol\()_\width\()_rr
	ldp x8, x13, [_stream], #16
	mov x23, x8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	.if \width == 64
	ldr d0, [_vcpu, x10]
	ldr d1, [_vcpu, x11]
	.else
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q2, [x10, #16]
	vector_map3_shuffle_input_size \opcode, \width
	.if Lx86_vector_map3_shuffle_input_size == 32
	ldr q3, [x11, #16]
	.else
	movi v3.16b, #0
	.endif
	.endif
	.endif
	vector_map3_shuffle_lane \opcode, \width
	vector_write \width, 23
	retire_and_next
.endm

.macro vector_map3_shuffle_memory symbol, opcode, width
gadget vector_map3_shuffle_\symbol\()_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	vector_map3_shuffle_input_size \opcode, \width
	vector_memory_source Lx86_vector_map3_shuffle_input_size, Lx86_vector_map3_shuffle_cross_fault\@, zero_high=(\width == 256)
	mov w13, w27
	and x10, x26, #0xffff
	vector_read \width
	vector_map3_shuffle_lane \opcode, \width
	vector_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map3_shuffle_cross_fault\@
.endm

.macro vector_map3_shuffle_family opcode
	vector_map3_shuffle_register \opcode, 0x\opcode, 64
	vector_map3_shuffle_memory \opcode, 0x\opcode, 64
	vector_map3_shuffle_register \opcode, 0x\opcode, 128
	vector_map3_shuffle_memory \opcode, 0x\opcode, 128
	vector_map3_shuffle_register \opcode, 0x\opcode, 256
	vector_map3_shuffle_memory \opcode, 0x\opcode, 256
.endm

vector_map3_shuffle_family 00
vector_map3_shuffle_family 01
vector_map3_shuffle_family 02
vector_map3_shuffle_family 04
vector_map3_shuffle_family 05
vector_map3_shuffle_family 06
vector_map3_shuffle_family 0c
vector_map3_shuffle_family 0d
vector_map3_shuffle_family 0e
vector_map3_shuffle_family 0f
vector_map3_shuffle_family 18
vector_map3_shuffle_family 38
vector_map3_shuffle_family 46
vector_map3_shuffle_family 4a
vector_map3_shuffle_family 4b
vector_map3_shuffle_family 4c

.purgem vector_map3_shuffle_family
.purgem vector_map3_shuffle_memory
.purgem vector_map3_shuffle_register
.purgem vector_map3_shuffle_input_size
.purgem vector_map3_shuffle_lane
.purgem vector_map3_variable_blend
.purgem vector_map3_select_128
.purgem vector_map3_mask_h
.purgem vector_map3_qword_indices
