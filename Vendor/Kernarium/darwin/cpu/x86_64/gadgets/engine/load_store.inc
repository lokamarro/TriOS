/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar transfers, effective addresses, stack operations, and memory control transfers. */

.macro scalar_move_rr name, load, store, load_register, store_register
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \load_register\()11, [_vcpu, x10]
	\store \store_register\()11, [_vcpu, x9]
	retire_and_next
.endm

.macro scalar_move_ri name, store, store_register
gadget \name
	ldp x8, x11, [_stream], #16
	and x9, x8, #0xffff
	\store \store_register\()11, [_vcpu, x9]
	retire_and_next
.endm

scalar_move_rr move_8_rr, ldrb, strb, w, w
scalar_move_rr move_16_rr, ldrh, strh, w, w
scalar_move_rr move_32_rr, ldr, str, w, x
scalar_move_rr move_64_rr, ldr, str, x, x
scalar_move_ri move_8_ri, strb, w
scalar_move_ri move_16_ri, strh, w
scalar_move_ri move_32_ri, str, x
scalar_move_ri move_64_ri, str, x

.purgem scalar_move_ri
.purgem scalar_move_rr

.macro memory_move_load width, load, host_register, store, store_register
gadget move_\width\()_rm
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), Lx86_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()11, [x7]
Lx86_load_write\@:
	and x8, x23, #0xffff
	\store \store_register\()11, [_vcpu, x8]
	retire_and_next
Lx86_load_cross\@:
	cross_page_load (\width / 8)
	mov x11, x0
	b Lx86_load_write\@
.endm

.macro memory_move_store form, width, load, host_register, store
gadget move_\width\()_m\form
	effective_address
	mov x25, x7
	.ifc \form,r
	and x8, x23, #0xffff
	\load \host_register\()24, [_vcpu, x8]
	.endif
	.ifc \form,i
	ldr x24, [_stream], #8
	.endif
	page_cross_check (\width / 8), Lx86_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
	finish_store (\width / 8)
Lx86_store_cross\@:
	cross_page_store x24, (\width / 8)
	finish_store (\width / 8)
.endm

memory_move_load 8, ldrb, w, strb, w
memory_move_load 16, ldrh, w, strh, w
memory_move_load 32, ldr, w, str, x
memory_move_load 64, ldr, x, str, x
memory_move_store r, 8, ldrb, w, strb
memory_move_store r, 16, ldrh, w, strh
memory_move_store r, 32, ldr, w, str
memory_move_store r, 64, ldr, x, str
memory_move_store i, 8, ldrb, w, strb
memory_move_store i, 16, ldrh, w, strh
memory_move_store i, 32, ldr, w, str
memory_move_store i, 64, ldr, x, str

.purgem memory_move_store
.purgem memory_move_load

gadget lea_16
	effective_address
	and x8, x23, #0xffff
	strh w7, [_vcpu, x8]
	retire_and_next

gadget lea_32
	effective_address
	and x8, x23, #0xffff
	mov w7, w7
	str x7, [_vcpu, x8]
	retire_and_next

gadget lea_64
	effective_address
	and x8, x23, #0xffff
	str x7, [_vcpu, x8]
	retire_and_next

/* Push the value in `host_register`24 as a `width`-bit stack element, ending
 * the gadget. */
.macro push_value width, store, host_register
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 8)
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_push_value_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
Lx86_push_value_done\@:
	str x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store (\width / 8)
Lx86_push_value_cross\@:
	cross_page_store x24, (\width / 8)
	b Lx86_push_value_done\@
.endm

.macro push_register width, load, host_register, store
gadget push_\width\()_r
	ldr x23, [_stream], #8
	and x8, x23, #0xffff
	\load \host_register\()24, [_vcpu, x8]
	push_value \width, \store, \host_register
.endm

.macro push_immediate width, host_register, store
gadget push_\width\()_i
	ldp x23, x24, [_stream], #16
	push_value \width, \store, \host_register
.endm

.macro pop_register width, load, host_register, store, store_register
gadget pop_\width\()_r
	ldr x23, [_stream], #8
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_pop_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
Lx86_pop_done\@:
	add x9, x25, #(\width / 8)
	.if \width == 32
	mov w9, w9
	.endif
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	and x8, x23, #0xffff
	\store \store_register\()24, [_vcpu, x8]
	retire_and_next
Lx86_pop_cross\@:
	cross_page_load (\width / 8)
	mov x24, x0
	b Lx86_pop_done\@
.endm

push_register 16, ldrh, w, strh
push_register 32, ldr, w, str
push_register 64, ldr, x, str
push_immediate 16, w, strh
push_immediate 32, w, str
push_immediate 64, x, str
pop_register 16, ldrh, w, strh, w
pop_register 32, ldr, w, str, x
pop_register 64, ldr, x, str, x

.purgem pop_register
.purgem push_immediate
.purgem push_register

.macro push_flags width, store, host_register
gadget push_flags_\width
	bl uml_x86_64_gadget_materialize_flags
	ldr x24, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	/* PUSHF stores RF and VM as zero whatever RFLAGS holds; IRET may have
	 * loaded RF. */
	and x24, x24, #~0x30000
	ldr x23, [_stream], #8
	push_value \width, \store, \host_register
.endm

push_flags 16, strh, w
push_flags 32, str, w
push_flags 64, str, x

.purgem push_flags

.macro pop_flags width, load, host_register
gadget pop_flags_\width
	ldr x23, [_stream], #8
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_pop_flags_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
Lx86_pop_flags_done\@:
	bl uml_x86_64_gadget_materialize_flags
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x12, x9
	mov x10, #0x4dd5
	.if \width != 16
	movk x10, #0x24, lsl #16
	.endif
	bic x9, x9, x10
	and x24, x24, x10
	orr x9, x9, x24
	orr x9, x9, #0x2
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	bic x12, x9, x12
	add x9, x25, #(\width / 8)
	.if \width == 32
	mov w9, w9
	.endif
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	/* Setting TF arms #DB after the next instruction: leave at it so the
	 * runtime can run exactly that one. Setting AC leaves too, for the
	 * runtime checks alignment while it is set. */
	ldr x8, [_stream], #8
	tbnz x12, #8, Lx86_pop_flags_trap\@
	tbnz x12, #18, Lx86_pop_flags_alignment\@
	retire_and_next
Lx86_pop_flags_alignment\@:
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_pop_flags_trap\@:
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_TRAP_FLAG
	b uml_x86_64_gadget_leave
Lx86_pop_flags_cross\@:
	cross_page_load (\width / 8)
	mov x24, x0
	b Lx86_pop_flags_done\@
.endm

pop_flags 16, ldrh, w
pop_flags 32, ldr, w
pop_flags 64, ldr, x

.purgem pop_flags

.macro leave width, load, store, host_register
gadget leave_\width
	ldr x23, [_stream], #8
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_leave_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
Lx86_leave_done\@:
	add x9, x25, #(\width / 8)
	.if \width == 32
	mov w9, w9
	.endif
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	\store \host_register\()24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	retire_and_next
Lx86_leave_cross\@:
	cross_page_load (\width / 8)
	mov x24, x0
	b Lx86_leave_done\@
.endm

leave 16, ldrh, strh, w
leave 32, ldr, str, x
leave 64, ldr, str, x

.purgem leave

.macro push_memory width, load, load_register, store, store_register
gadget push_\width\()_m
	effective_address
	mov x26, x7
	page_cross_check (\width / 8), Lx86_push_memory_source_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \load_register\()24, [x7]
	b Lx86_push_memory_source_done\@
Lx86_push_memory_source_cross\@:
	mov x0, _context
	mov x1, x26
	mov w2, #(\width / 8)
	helper_call uml_x86_64_gadget_cross_page_load
	cbz x1, Lx86_memory_fault
	mov x24, x0
Lx86_push_memory_source_done\@:
	push_value \width, \store, \store_register
.endm

push_memory 16, ldrh, w, strh, w
push_memory 32, ldr, w, str, w
push_memory 64, ldr, x, str, x

.purgem push_memory
.purgem push_value

.macro pop_memory width, load, load_register, store, store_register
gadget pop_\width\()_m
	ldr x23, [_stream]
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	read_operand (\width / 8), \load, \load_register, 24
	add x26, x25, #(\width / 8)
	.if \width == 32
	mov w26, w26
	.endif
	str x26, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	effective_address
	str x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x27, x7
	page_cross_check (\width / 8), Lx86_pop_memory_destination_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \store_register\()24, [x7]
Lx86_pop_memory_done\@:
	str x26, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x25, x27
	finish_store (\width / 8)
Lx86_pop_memory_destination_cross\@:
	mov x0, _context
	mov x1, x27
	mov x2, x24
	mov w3, #(\width / 8)
	helper_call uml_x86_64_gadget_cross_page_store
	cbz w0, Lx86_memory_fault
	b Lx86_pop_memory_done\@
.endm

pop_memory 16, ldrh, w, strh, w
pop_memory 32, ldr, w, str, w
pop_memory 64, ldr, x, str, x

.purgem pop_memory

/* A call's return-address push is an ordinary store, so it settles a pending
 * code write on exactly the terms `finish_store` uses for one: publish it, and
 * leave the stream only when the write landed inside the block that is running,
 * because that is the case where the instructions still ahead of us may have
 * changed. Publishing alone is not a reason to leave. The link the call is
 * about to follow tests `code_link_safe` for itself, which is the same test a
 * jump out of a publishing block already relies on, so a call that stays in the
 * stream is no less careful than a jump -- it was only less lucky.
 *
 * `w23` reports that decision to `call_push_and_exit`.
 */
.macro invalidate_stack_write size, publish=1
	mov w23, #0
	.if \publish
	publish_pending_code x25, \size
	.endif
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x25, x8
	b.hs Lx86_stack_write_ready\@
	add x9, x25, #\size
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls Lx86_stack_write_ready\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbnz w8, Lx86_stack_write_overlap\@
	mov x0, _context
	mov x1, x25
	helper_call uml_x86_64_gadget_invalidate_code
Lx86_stack_write_overlap\@:
	mov w23, #1
Lx86_stack_write_ready\@:
.endm

.macro call_push_and_exit width, store, host_register, linked=0, indirect=0, positioned=1
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 8)
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_call_cross\@
	stack_fast 1, Lx86_call_slow\@
	\store \host_register\()24, [x7]
	str x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	/* A plain page owes no publication; the write may still land in the
	 * block that is running, which is what the rest of the test decides. */
	invalidate_stack_write (\width / 8), 0
	b Lx86_call_settled\@
Lx86_call_slow\@:
	.if \positioned
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8), 1
	.else
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8), 1, Lx86_call_fault\@
	.endif
	\store \host_register\()24, [x7]
Lx86_call_done\@:
	str x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	invalidate_stack_write (\width / 8)
Lx86_call_settled\@:
	add _executed, _executed, #1
	x86_memory_order_boundary
	.if \indirect
	cbnz w23, Lx86_call_exit\@
	follow_indirect_link x26, x27
	.else
	.if \linked
	cbnz w23, Lx86_call_exit\@
	follow_block_link x27
	.endif
	.endif
Lx86_call_exit\@:
	str x26, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_call_cross\@:
	.if \positioned
	cross_page_store x24, (\width / 8)
	.else
	cross_page_store x24, (\width / 8), Lx86_call_fault\@
	.endif
	b Lx86_call_done\@
	.if !\positioned
	/* A direct CALL without a word of its own is five bytes long, and the
	 * return address it pushes is the address after it. */
Lx86_call_fault\@:
	.if \width == 64
	sub x22, x24, #5
	.else
	sub w22, w24, #5
	.endif
	b Lx86_memory_fault_at
	.endif
.endm

/* A direct CALL is `call_direct`; a form carrying a prefix, which no longer
 * ends five bytes before the return address, has the stream word that gives
 * its position instead. */
.macro call_direct width, store, host_register, linked, positioned
	.if \linked
	ldp x27, x24, [_stream], #16
	.else
	ldp x26, x24, [_stream], #16
	.endif
	.if \positioned
	ldr x23, [_stream], #8
	.endif
	.if \linked
	ldr x26, [x27, #16]
	.endif
	call_push_and_exit \width, \store, \host_register, \linked, 0, \positioned
.endm

gadget call
	call_direct 64, str, x, 0, 0

gadget call_linked
	call_direct 64, str, x, 1, 0

gadget call_positioned
	call_direct 64, str, x, 0, 1

gadget call_positioned_linked
	call_direct 64, str, x, 1, 1

.macro call_indirect_register name, width, load, host_register, store, store_register, cached=0
gadget \name
	ldp x23, x24, [_stream], #16
	and x8, x23, #0xffff
	.if \cached
	ldr x27, [_stream], #8
	.endif
	\load \host_register\()26, [_vcpu, x8]
	call_push_and_exit \width, \store, \store_register, 0, \cached
.endm

.macro call_indirect_memory name, width, load, load_register, store, store_register, cached=0
gadget \name
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \load_register, 26
	ldr x24, [_stream], #8
	.if \cached
	ldr x27, [_stream], #8
	.endif
	call_push_and_exit \width, \store, \store_register, 0, \cached
.endm

gadget call_32
	call_direct 32, str, w, 0, 0

gadget call_32_linked
	call_direct 32, str, w, 1, 0

gadget call_32_positioned
	call_direct 32, str, w, 0, 1

gadget call_32_positioned_linked
	call_direct 32, str, w, 1, 1

call_indirect_register call_indirect_r, 64, ldr, x, str, x
call_indirect_register call_indirect_32_r, 32, ldr, w, str, w
call_indirect_register call_indirect_r_cached, 64, ldr, x, str, x, 1
call_indirect_register call_indirect_32_r_cached, 32, ldr, w, str, w, 1
call_indirect_memory call_indirect_m, 64, ldr, x, str, x
call_indirect_memory call_indirect_32_m, 32, ldr, w, str, w
call_indirect_memory call_indirect_m_cached, 64, ldr, x, str, x, 1
call_indirect_memory call_indirect_32_m_cached, 32, ldr, w, str, w, 1

.purgem call_indirect_memory
.purgem call_indirect_register
.purgem call_direct
.purgem call_push_and_exit
.purgem invalidate_stack_write

/* A RET has no site tier: the addresses a function returns to belong to its
 * callers, so the set grows with the call sites and a two-way tier keyed on
 * the RET holds none of it for long. Reach the shared guest-RIP tier straight
 * away, and leave on a miss without training anything -- the tier learns the
 * return site from the block that runs there, not from this branch.
 */
.macro return_gadget name, width, load, host_register, cached=0
gadget \name
	ldr x23, [_stream], #8
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_return_cross\@
	stack_fast 0, Lx86_return_slow\@
	\load \host_register\()24, [x7]
	b Lx86_return_done\@
Lx86_return_slow\@:
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8), 1
	\load \host_register\()24, [x7]
Lx86_return_done\@:
	add x9, x25, #(\width / 8)
	add x9, x9, w23, uxth
	.if \width == 32
	mov w9, w9
	.endif
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	add _executed, _executed, #1
	x86_memory_order_boundary
	.if \cached
	follow_target_link x24, Lx86_return_miss\@
Lx86_return_miss\@:
	.endif
	str x24, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_return_cross\@:
	cross_page_load (\width / 8)
	mov x24, x0
	b Lx86_return_done\@
.endm

.macro jump_indirect_register name, load, host_register, cached=0
gadget \name
	.if \cached
	ldp x8, x27, [_stream]
	.else
	ldr x8, [_stream]
	.endif
	\load \host_register\()8, [_vcpu, x8]
	add _executed, _executed, #1
	x86_memory_order_boundary
	.if \cached
	follow_indirect_link x8, x27
	.else
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
	.endif
.endm

.macro jump_indirect_memory name, width, load, host_register, cached=0
gadget \name
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 24
	.if \cached
	ldr x27, [_stream]
	.endif
	add _executed, _executed, #1
	x86_memory_order_boundary
	.if \cached
	follow_indirect_link x24, x27
	.else
	str x24, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
	.endif
.endm

return_gadget return, 64, ldr, x
return_gadget return_32, 32, ldr, w
return_gadget return_cached, 64, ldr, x, 1
return_gadget return_32_cached, 32, ldr, w, 1
jump_indirect_register jump_indirect_r, ldr, x
jump_indirect_register jump_indirect_32_r, ldr, w
jump_indirect_register jump_indirect_r_cached, ldr, x, 1
jump_indirect_register jump_indirect_32_r_cached, ldr, w, 1
jump_indirect_memory jump_indirect_m, 64, ldr, x
jump_indirect_memory jump_indirect_32_m, 32, ldr, w
jump_indirect_memory jump_indirect_m_cached, 64, ldr, x, 1
jump_indirect_memory jump_indirect_32_m_cached, 32, ldr, w, 1

.purgem jump_indirect_memory
.purgem jump_indirect_register
.purgem return_gadget

.macro scalar_extend name, load, load_register, store, store_register
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \load_register\()11, [_vcpu, x10]
	\store \store_register\()11, [_vcpu, x9]
	retire_and_next
.endm

scalar_extend zext_8_16_rr, ldrb, w, strh, w
scalar_extend zext_8_32_rr, ldrb, w, str, x
scalar_extend zext_8_64_rr, ldrb, w, str, x
scalar_extend zext_16_32_rr, ldrh, w, str, x
scalar_extend zext_16_64_rr, ldrh, w, str, x
scalar_extend zext_32_64_rr, ldr, w, str, x
scalar_extend sext_8_16_rr, ldrsb, w, strh, w
scalar_extend sext_8_32_rr, ldrsb, w, str, x
scalar_extend sext_8_64_rr, ldrsb, x, str, x
scalar_extend sext_16_32_rr, ldrsh, w, str, x
scalar_extend sext_16_64_rr, ldrsh, x, str, x
scalar_extend sext_32_16_rr, ldr, w, strh, w
scalar_extend sext_32_32_rr, ldr, w, str, x
scalar_extend sext_32_64_rr, ldrsw, x, str, x

.purgem scalar_extend

.macro cross_zext
.endm
.macro cross_sxtb_w
	sxtb w11, w11
.endm
.macro cross_sxtb_x
	sxtb x11, w11
.endm
.macro cross_sxth_w
	sxth w11, w11
.endm
.macro cross_sxth_x
	sxth x11, w11
.endm
.macro cross_sxtw_x
	sxtw x11, w11
.endm

.macro memory_extend name, source_width, load, load_register, cross_extend, destination_store, destination_register
gadget \name
	effective_address
	mov x25, x7
	page_cross_check (\source_width / 8), Lx86_extend_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\source_width / 8)
	\load \load_register\()11, [x7]
	b Lx86_extend_write\@
Lx86_extend_cross\@:
	cross_page_load (\source_width / 8)
	mov x11, x0
	\cross_extend
Lx86_extend_write\@:
	and x8, x23, #0xffff
	\destination_store \destination_register\()11, [_vcpu, x8]
	retire_and_next
.endm

memory_extend zext_8_16_rm, 8, ldrb, w, cross_zext, strh, w
memory_extend zext_8_32_rm, 8, ldrb, w, cross_zext, str, x
memory_extend zext_8_64_rm, 8, ldrb, w, cross_zext, str, x
memory_extend zext_16_32_rm, 16, ldrh, w, cross_zext, str, x
memory_extend zext_16_64_rm, 16, ldrh, w, cross_zext, str, x
memory_extend zext_32_64_rm, 32, ldr, w, cross_zext, str, x
memory_extend sext_8_16_rm, 8, ldrsb, w, cross_sxtb_w, strh, w
memory_extend sext_8_32_rm, 8, ldrsb, w, cross_sxtb_w, str, x
memory_extend sext_8_64_rm, 8, ldrsb, x, cross_sxtb_x, str, x
memory_extend sext_16_32_rm, 16, ldrsh, w, cross_sxth_w, str, x
memory_extend sext_16_64_rm, 16, ldrsh, x, cross_sxth_x, str, x
memory_extend sext_32_16_rm, 32, ldr, w, cross_zext, strh, w
memory_extend sext_32_32_rm, 32, ldr, w, cross_zext, str, x
memory_extend sext_32_64_rm, 32, ldrsw, x, cross_sxtw_x, str, x

.purgem memory_extend
.purgem cross_sxtw_x
.purgem cross_sxth_x
.purgem cross_sxth_w
.purgem cross_sxtb_x
.purgem cross_sxtb_w
.purgem cross_zext
