/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4/AVX scalar element and 128-bit lane extraction/insertion. */

.macro vector_map3_extract_register size
gadget vector_map3_extract_\size\()_r
	ldp x8, x13, [_stream], #16
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	and x13, x13, #(16 / \size - 1)
	add x10, _vcpu, x10
	.if \size == 1
	ldrb w24, [x10, x13]
	str x24, [_vcpu, x9]
	.elseif \size == 2
	ldrh w24, [x10, x13, lsl #1]
	str x24, [_vcpu, x9]
	.elseif \size == 4
	ldr w24, [x10, x13, lsl #2]
	str x24, [_vcpu, x9]
	.else
	ldr x24, [x10, x13, lsl #3]
	str x24, [_vcpu, x9]
	.endif
	retire_and_next
.endm

.macro vector_map3_extract_memory size
gadget vector_map3_extract_\size\()_m
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	and x10, x26, #0xffff
	ubfx x13, x26, #16, #8
	and x13, x13, #(16 / \size - 1)
	add x10, _vcpu, x10
	.if \size == 1
	ldrb w24, [x10, x13]
	.elseif \size == 2
	ldrh w24, [x10, x13, lsl #1]
	.elseif \size == 4
	ldr w24, [x10, x13, lsl #2]
	.else
	ldr x24, [x10, x13, lsl #3]
	.endif
	page_cross_check (\size), Lx86_vector_map3_extract_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, \size
	.if \size == 1
	strb w24, [x7]
	.elseif \size == 2
	strh w24, [x7]
	.elseif \size == 4
	str w24, [x7]
	.else
	str x24, [x7]
	.endif
	finish_store \size
Lx86_vector_map3_extract_cross\@:
	cross_page_store x24, \size
	finish_store \size
.endm

vector_map3_extract_register 1
vector_map3_extract_register 2
vector_map3_extract_register 4
vector_map3_extract_register 8
vector_map3_extract_memory 1
vector_map3_extract_memory 2
vector_map3_extract_memory 4
vector_map3_extract_memory 8

.purgem vector_map3_extract_memory
.purgem vector_map3_extract_register

gadget vector_map3_extract_128_r
	ldp x8, x13, [_stream], #16
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	and x13, x13, #1
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	add x10, x10, x13, lsl #4
	ldr q0, [x10]
	str q0, [x9]
	movi v1.16b, #0
	str q1, [x9, #16]
	retire_and_next

/* The lane to store stays in the register file until the destination has been
 * translated: a translation may miss and call out, and the host vector
 * registers do not survive a call. x24 holds the lane's address in the state,
 * which does survive one, and the page-crossing helper reads the lane from
 * there as well.
 */
gadget vector_map3_extract_128_m
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	and x10, x26, #0xffff
	ubfx x13, x26, #16, #1
	add x24, _vcpu, x10
	add x24, x24, x13, lsl #4
	page_cross_check 16, Lx86_vector_map3_extract_128_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 16
	ldr q0, [x24]
	str q0, [x7]
	finish_store 16
Lx86_vector_map3_extract_128_cross:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #16
	helper_call uml_x86_64_gadget_cross_page_vector_store
	cbz w0, Lx86_memory_fault
	finish_store 16

.macro vector_map3_insert_register size
gadget vector_map3_insert_\size\()_rr
	ldp x8, x13, [_stream], #16
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	ldr q0, [x10]
	str q0, [x9]
	and x13, x13, #(16 / \size - 1)
	.if \size == 1
	ldrb w24, [_vcpu, x11]
	strb w24, [x9, x13]
	.elseif \size == 4
	ldr w24, [_vcpu, x11]
	str w24, [x9, x13, lsl #2]
	.else
	ldr x24, [_vcpu, x11]
	str x24, [x9, x13, lsl #3]
	.endif
	tbz x8, #63, Lx86_vector_map3_insert_register_done\@
	movi v1.16b, #0
	str q1, [x9, #16]
Lx86_vector_map3_insert_register_done\@:
	retire_and_next
.endm

.macro vector_map3_insert_memory size
gadget vector_map3_insert_\size\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	page_cross_check (\size), Lx86_vector_map3_insert_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \size
	.if \size == 1
	ldrb w24, [x7]
	.elseif \size == 4
	ldr w24, [x7]
	.else
	ldr x24, [x7]
	.endif
	b Lx86_vector_map3_insert_loaded\@
Lx86_vector_map3_insert_cross\@:
	cross_page_load \size
	.if \size == 8
	mov x24, x0
	.else
	mov w24, w0
	.endif
Lx86_vector_map3_insert_loaded\@:
	and x9, x23, #0xffff
	and x10, x26, #0xffff
	ubfx x13, x26, #16, #8
	and x13, x13, #(16 / \size - 1)
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	ldr q0, [x10]
	str q0, [x9]
	.if \size == 1
	strb w24, [x9, x13]
	.elseif \size == 4
	str w24, [x9, x13, lsl #2]
	.else
	str x24, [x9, x13, lsl #3]
	.endif
	tbz x23, #63, Lx86_vector_map3_insert_memory_done\@
	movi v1.16b, #0
	str q1, [x9, #16]
Lx86_vector_map3_insert_memory_done\@:
	retire_and_next
.endm

vector_map3_insert_register 1
vector_map3_insert_register 4
vector_map3_insert_register 8
vector_map3_insert_memory 1
vector_map3_insert_memory 4
vector_map3_insert_memory 8

.purgem vector_map3_insert_memory
.purgem vector_map3_insert_register

.macro vector_map3_insertps_finish descriptor, control
	movi v6.16b, #0
	.irp lane, 0, 1, 2, 3
	ubfx w14, w\control, #\lane, #1
	neg w14, w14
	mov v6.s[\lane], w14
	.endr
	bic v4.16b, v4.16b, v6.16b
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q4, [x9]
	tbz x\descriptor, #63, Lx86_vector_map3_insertps_done\@
	movi v6.16b, #0
	str q6, [x9, #16]
Lx86_vector_map3_insertps_done\@:
	retire_and_next
.endm

/* Insert w24 into the lane of v4 that the 2-bit field in w14 selects. */
.macro vector_map3_insertps_destination
	cmp w14, #1
	b.eq Lx86_insertps_lane_1\@
	cmp w14, #2
	b.eq Lx86_insertps_lane_2\@
	cmp w14, #3
	b.eq Lx86_insertps_lane_3\@
	mov v4.s[0], w24
	b Lx86_insertps_lane_done\@
Lx86_insertps_lane_1\@:
	mov v4.s[1], w24
	b Lx86_insertps_lane_done\@
Lx86_insertps_lane_2\@:
	mov v4.s[2], w24
	b Lx86_insertps_lane_done\@
Lx86_insertps_lane_3\@:
	mov v4.s[3], w24
Lx86_insertps_lane_done\@:
.endm

gadget vector_map3_insertps_rr
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q4, [x10]
	ldr q1, [x11]
	ubfx w14, w13, #6, #2
	umov w24, v1.s[0]
	cmp w14, #1
	b.eq Lx86_vector_map3_insertps_source_1
	cmp w14, #2
	b.eq Lx86_vector_map3_insertps_source_2
	cmp w14, #3
	b.eq Lx86_vector_map3_insertps_source_3
	b Lx86_vector_map3_insertps_source_done
Lx86_vector_map3_insertps_source_1:
	umov w24, v1.s[1]
	b Lx86_vector_map3_insertps_source_done
Lx86_vector_map3_insertps_source_2:
	umov w24, v1.s[2]
	b Lx86_vector_map3_insertps_source_done
Lx86_vector_map3_insertps_source_3:
	umov w24, v1.s[3]
Lx86_vector_map3_insertps_source_done:
	ubfx w14, w13, #4, #2
	vector_map3_insertps_destination
	vector_map3_insertps_finish 8, 13

gadget vector_map3_insertps_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	read_operand 4, ldr, w, 24, w
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q4, [x10]
	ubfx w14, w26, #20, #2
	vector_map3_insertps_destination
	lsr w13, w26, #16
	vector_map3_insertps_finish 23, 13

.purgem vector_map3_insertps_destination
.purgem vector_map3_insertps_finish
