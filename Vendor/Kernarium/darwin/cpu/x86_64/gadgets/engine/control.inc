/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Condition evaluation and direct conditional/unconditional control flow. */
.macro evaluate_condition condition
	ldr x10, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	.ifc \condition,o
	ubfx w11, w10, #11, #1
	.endif
	.ifc \condition,no
	ubfx w11, w10, #11, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,b
	and w11, w10, #1
	.endif
	.ifc \condition,ae
	and w11, w10, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,e
	ubfx w11, w10, #6, #1
	.endif
	.ifc \condition,ne
	ubfx w11, w10, #6, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,be
	and w11, w10, #1
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	.endif
	.ifc \condition,a
	and w11, w10, #1
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	eor w11, w11, #1
	.endif
	.ifc \condition,s
	ubfx w11, w10, #7, #1
	.endif
	.ifc \condition,ns
	ubfx w11, w10, #7, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,p
	ubfx w11, w10, #2, #1
	.endif
	.ifc \condition,np
	ubfx w11, w10, #2, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,l
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	.endif
	.ifc \condition,ge
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	eor w11, w11, #1
	.endif
	.ifc \condition,le
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	.endif
	.ifc \condition,g
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	eor w11, w11, #1
	.endif
.endm

.macro evaluate_deferred_zero condition
	ldrh w10, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	tbz w10, #6, Lx86_zero_from_deferred\@
	ldrh w10, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	ubfx w11, w10, #6, #1
	.ifc \condition,ne
	eor w11, w11, #1
	.endif
	b Lx86_zero_ready\@
Lx86_zero_from_deferred\@:
	ldrb w10, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbz w10, Lx86_zero_from_flags\@
	ldr x10, [_vcpu, #UML_X86_64_CPU_DEFERRED_RESULT]
	cmp x10, #0
	.ifc \condition,e
	cset w11, eq
	.else
	cset w11, ne
	.endif
	b Lx86_zero_ready\@
Lx86_zero_from_flags\@:
	ldr x10, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	ubfx w11, w10, #6, #1
	.ifc \condition,ne
	eor w11, w11, #1
	.endif
Lx86_zero_ready\@:
.endm

/* E and NE read ZF from the pending record without materializing the flags;
 * every other condition materializes them first. The result is in w11. */
.macro evaluate_guest_condition condition
	.ifc \condition,e
	evaluate_deferred_zero \condition
	.else
	.ifc \condition,ne
	evaluate_deferred_zero \condition
	.else
	bl uml_x86_64_gadget_materialize_flags
	evaluate_condition \condition
	.endif
	.endif
.endm

/* The stream an unlinked BlockLink names is the link_exit word right in front
 * of that BlockLink, so the RIP is the BlockLink's own. */
gadget link_exit
	ldr x9, [_stream, #16]
	str x9, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

.macro set_condition condition
gadget set_\condition
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream], #8
	evaluate_condition \condition
	strb w11, [_vcpu, x8]
	retire_and_next
.endm

.macro set_condition_memory condition
gadget set_\condition\()_m
	bl uml_x86_64_gadget_materialize_flags
	effective_address
	mov x25, x7
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 1
	evaluate_condition \condition
	strb w11, [x7]
	finish_store 1
.endm

.macro conditional_move condition, width, load, store, load_register, store_register
gadget cmov_\condition\()_\width\()_rr
	evaluate_guest_condition \condition
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x13, x8, #16, #16
	/* A conditional move writes its destination register whether or not it
	 * moves, so a 32-bit one clears the upper half on the condition it does
	 * not take as well. Selecting the destination's own value and storing
	 * it unconditionally is that write, and costs a load in place of a
	 * branch. */
	\load \load_register\()12, [_vcpu, x13]
	\load \load_register\()14, [_vcpu, x9]
	cmp w11, #0
	csel x12, x12, x14, ne
	\store \store_register\()12, [_vcpu, x9]
	retire_and_next
.endm

.macro conditional_move_memory condition, width, load, host_register, store, store_register
gadget cmov_\condition\()_\width\()_rm
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 24
	bl uml_x86_64_gadget_materialize_flags
	evaluate_condition \condition
	and x8, x23, #0xffff
	\load \host_register\()13, [_vcpu, x8]
	cmp w11, #0
	csel x24, x24, x13, ne
	\store \store_register\()24, [_vcpu, x8]
	retire_and_next
.endm

.macro jump_condition condition
gadget jump_\condition
	evaluate_guest_condition \condition
	ldp x12, x13, [_stream], #16
	cmp w11, #0
	csel x8, x12, x13, ne
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro jump_condition_linked condition
gadget jump_\condition\()_linked
	evaluate_guest_condition \condition
	ldp x12, x13, [_stream], #16
	cmp w11, #0
	csel x8, x12, x13, ne
	add _executed, _executed, #1
	x86_memory_order_boundary
	follow_block_link x8
.endm

/* The fall-through translated into this same stream. Not taking the branch is
 * then one dispatch rather than a link follow, and the block keeps growing
 * past what is otherwise its first and usually only branch. The stream holds
 * one operand, the taken target's link; the fall-through is the next gadget.
 */
.macro jump_condition_inline condition
gadget jump_\condition\()_inline
	evaluate_guest_condition \condition
	ldr x12, [_stream], #8
	add _executed, _executed, #1
	x86_memory_order_boundary
	cbz w11, Lx86_jump_inline_next\@
	follow_block_link x12
Lx86_jump_inline_next\@:
	next_gadget
.endm

.macro condition_family condition
set_condition \condition
set_condition_memory \condition
conditional_move \condition, 16, ldrh, strh, w, w
conditional_move \condition, 32, ldr, str, w, x
conditional_move \condition, 64, ldr, str, x, x
conditional_move_memory \condition, 16, ldrh, w, strh, w
conditional_move_memory \condition, 32, ldr, w, str, x
conditional_move_memory \condition, 64, ldr, x, str, x
jump_condition \condition
jump_condition_linked \condition
jump_condition_inline \condition
.endm

condition_family o
condition_family no
condition_family b
condition_family ae
condition_family e
condition_family ne
condition_family be
condition_family a
condition_family s
condition_family ns
condition_family p
condition_family np
condition_family l
condition_family ge
condition_family le
condition_family g

/* A 32- or 64-bit CMP, SUB, TEST or AND leaves the host NZCV holding exactly
 * the x86 flags it just defined. Recording the deferred flags does not disturb
 * them -- the record is three stores and an address -- and neither does
 * threaded dispatch, so a consumer in the next stream slot can read the
 * condition straight off the host rather than materializing RFLAGS to extract
 * one bit. The builder names these variants only where it has just emitted
 * such a producer.
 *
 * ARM's carry is the inverted borrow of a subtraction, which is what makes the
 * unsigned conditions line up with x86's. Parity has no NZCV form at all, so
 * `p` and `np` have no fused variant. A logical producer clears both C and V,
 * matching x86 clearing CF and OF, so the sign and zero conditions carry over
 * unchanged; the four conditions that read CF do not, because ARM reads a
 * cleared C as "borrowed" where x86 reads a cleared CF as "no borrow", and the
 * builder withholds fusion for those after a logical.
 */
.macro set_condition_nzcv condition, host
gadget set_\condition\()_nzcv
	ldr x8, [_stream], #8
	cset w11, \host
	strb w11, [_vcpu, x8]
	retire_and_next
.endm

.macro conditional_move_nzcv condition, host, width, load, store, load_register, store_register
gadget cmov_\condition\()_\width\()_rr_nzcv
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x13, x8, #16, #16
	/* As the ordinary form: the destination is written either way, which a
	 * 32-bit move must do to clear the upper half. Selecting on the
	 * producer's own condition keeps NZCV as it found it. */
	\load \load_register\()12, [_vcpu, x13]
	\load \load_register\()14, [_vcpu, x9]
	csel x12, x12, x14, \host
	\store \store_register\()12, [_vcpu, x9]
	retire_and_next
.endm

.macro jump_condition_nzcv condition, host
gadget jump_\condition\()_nzcv
	ldp x12, x13, [_stream], #16
	csel x8, x12, x13, \host
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro jump_condition_linked_nzcv condition, host
gadget jump_\condition\()_linked_nzcv
	ldp x12, x13, [_stream], #16
	csel x8, x12, x13, \host
	add _executed, _executed, #1
	x86_memory_order_boundary
	follow_block_link x8
.endm

/* The fused fall-through. Loading the operand and retiring leave NZCV alone,
 * so the host condition the producer defined is still live at the branch. */
.macro jump_condition_inline_nzcv condition, host
gadget jump_\condition\()_inline_nzcv
	ldr x12, [_stream], #8
	add _executed, _executed, #1
	x86_memory_order_boundary
	b.\host Lx86_jump_inline_nzcv_taken\@
	next_gadget
Lx86_jump_inline_nzcv_taken\@:
	follow_block_link x12
.endm

.macro condition_family_nzcv condition, host
set_condition_nzcv \condition, \host
conditional_move_nzcv \condition, \host, 16, ldrh, strh, w, w
conditional_move_nzcv \condition, \host, 32, ldr, str, w, x
conditional_move_nzcv \condition, \host, 64, ldr, str, x, x
jump_condition_nzcv \condition, \host
jump_condition_linked_nzcv \condition, \host
jump_condition_inline_nzcv \condition, \host
.endm

condition_family_nzcv o, vs
condition_family_nzcv no, vc
condition_family_nzcv b, lo
condition_family_nzcv ae, hs
condition_family_nzcv e, eq
condition_family_nzcv ne, ne
condition_family_nzcv be, ls
condition_family_nzcv a, hi
condition_family_nzcv s, mi
condition_family_nzcv ns, pl
condition_family_nzcv l, lt
condition_family_nzcv ge, ge
condition_family_nzcv le, le
condition_family_nzcv g, gt

.purgem condition_family_nzcv
.purgem jump_condition_inline_nzcv
.purgem jump_condition_linked_nzcv
.purgem jump_condition_nzcv
.purgem conditional_move_nzcv
.purgem set_condition_nzcv

.purgem condition_family
.purgem jump_condition_inline
.purgem jump_condition_linked
.purgem jump_condition
.purgem conditional_move_memory
.purgem conditional_move
.purgem set_condition_memory
.purgem set_condition
.purgem evaluate_guest_condition
.purgem evaluate_deferred_zero
.purgem evaluate_condition

gadget jump
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

gadget jump_linked
	ldr x8, [_stream]
	add _executed, _executed, #1
	x86_memory_order_boundary
	follow_block_link x8

.purgem follow_block_link
.purgem follow_indirect_link
.purgem follow_target_link

.macro loop_gadget opcode, width, host_register
gadget loop_\opcode\()_\width
	ldp x26, x27, [_stream]
	.if \width == 16
	ldrh w24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.else
	ldr \host_register\()24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.endif
	.ifc \opcode,e3
	cbz \host_register\()24, Lx86_loop_taken\@
	b Lx86_loop_fallthrough\@
	.else
	sub \host_register\()24, \host_register\()24, #1
	.if \width == 16
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	bfi x8, x24, #0, #16
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.else
	str x24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.endif
	.ifc \opcode,e2
	cbnz \host_register\()24, Lx86_loop_taken\@
	b Lx86_loop_fallthrough\@
	.else
	bl uml_x86_64_gadget_materialize_flags
	cbz \host_register\()24, Lx86_loop_fallthrough\@
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	.ifc \opcode,e1
	tbz x8, #6, Lx86_loop_fallthrough\@
	.else
	tbnz x8, #6, Lx86_loop_fallthrough\@
	.endif
	b Lx86_loop_taken\@
	.endif
	.endif
Lx86_loop_taken\@:
	str x26, [_vcpu, #UML_X86_64_CPU_RIP]
	b Lx86_loop_leave\@
Lx86_loop_fallthrough\@:
	str x27, [_vcpu, #UML_X86_64_CPU_RIP]
Lx86_loop_leave\@:
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro loop_family opcode
	loop_gadget \opcode, 16, w
	loop_gadget \opcode, 32, w
	loop_gadget \opcode, 64, x
.endm

loop_family e0
loop_family e1
loop_family e2
loop_family e3

.purgem loop_family
.purgem loop_gadget
