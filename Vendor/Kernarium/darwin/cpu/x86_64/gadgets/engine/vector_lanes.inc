/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MOVLPS/MOVHPS and packed low/high duplicate operations. */

.macro vector_duplicate_transform kind, width
	.ifc \kind,sldup
	trn1 v2.4s, v0.4s, v0.4s
	.if \width == 256
	trn1 v3.4s, v1.4s, v1.4s
	.endif
	.endif
	.ifc \kind,shdup
	trn2 v2.4s, v0.4s, v0.4s
	.if \width == 256
	trn2 v3.4s, v1.4s, v1.4s
	.endif
	.endif
	.ifc \kind,ddup
	dup v2.2d, v0.d[0]
	.if \width == 256
	dup v3.2d, v1.d[0]
	.endif
	.endif
.endm

.macro vector_duplicate_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q2, [x9]
	.if \width == 256
	str q3, [x9, #16]
	.else
	tbz x\descriptor, #63, Lx86_vector_duplicate_done\@
	movi v4.16b, #0
	str q4, [x9, #16]
Lx86_vector_duplicate_done\@:
	.endif
.endm

.macro vector_duplicate_register kind, width
gadget vector_duplicate_\kind\()_\width\()_rr
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q1, [x10, #16]
	.endif
	vector_duplicate_transform \kind, \width
	vector_duplicate_write \width, 8
	retire_and_next
.endm

.macro vector_duplicate_memory kind, width, input_size
gadget vector_duplicate_\kind\()_\width\()_rm
	effective_address
	mov x25, x7
	vector_memory_source \input_size, Lx86_vector_duplicate_cross_fault\@, 0, 1
	vector_duplicate_transform \kind, \width
	vector_duplicate_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_duplicate_cross_fault\@
.endm

vector_duplicate_register sldup, 128
vector_duplicate_register sldup, 256
vector_duplicate_register shdup, 128
vector_duplicate_register shdup, 256
vector_duplicate_register ddup, 128
vector_duplicate_register ddup, 256
vector_duplicate_memory sldup, 128, 16
vector_duplicate_memory sldup, 256, 32
vector_duplicate_memory shdup, 128, 16
vector_duplicate_memory shdup, 256, 32
vector_duplicate_memory ddup, 128, 8
vector_duplicate_memory ddup, 256, 32

.purgem vector_duplicate_memory
.purgem vector_duplicate_register
.purgem vector_duplicate_write
.purgem vector_duplicate_transform

/* Register form descriptor: destination, merge and incoming offsets in the
 * low three halfwords; bits 48/49 select the destination/source high qword,
 * and bit 50 requests VEX upper-lane clearing.
 */
gadget vector_lane_merge_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x10]
	tbz x8, #49, Lx86_vector_lane_merge_rr_source_ready
	add x11, x11, #8
Lx86_vector_lane_merge_rr_source_ready:
	/* All x86 sources are read before the destination is written.  The
	 * incoming register may alias the destination in three-operand VEX forms.
	 */
	ldr x12, [x11]
	str q0, [x9]
	tbz x8, #48, Lx86_vector_lane_merge_rr_store
	add x9, x9, #8
Lx86_vector_lane_merge_rr_store:
	str x12, [x9]
	tbz x8, #50, Lx86_vector_lane_merge_rr_done
	and x9, x8, #0xffff
	add x9, _vcpu, x9
	movi v0.16b, #0
	str q0, [x9, #16]
Lx86_vector_lane_merge_rr_done:
	retire_and_next

/* Memory form config: merge offset in 15:0, destination-high in bit 16,
 * VEX upper-lane clearing in bit 17.
 */
gadget vector_lane_merge_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	read_operand 8, ldr, x, 24
	and x9, x23, #0xffff
	and x10, x26, #0xffff
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	ldr q0, [x10]
	str q0, [x9]
	tbz x26, #16, Lx86_vector_lane_merge_rm_store
	add x9, x9, #8
Lx86_vector_lane_merge_rm_store:
	str x24, [x9]
	tbz x26, #17, Lx86_vector_lane_merge_rm_done
	and x9, x23, #0xffff
	add x9, _vcpu, x9
	movi v0.16b, #0
	str q0, [x9, #16]
Lx86_vector_lane_merge_rm_done:
	retire_and_next
