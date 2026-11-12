/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar arithmetic, logical, unary, byte-order, and flag operations. */
/* Stream immediates arrive as a full 64-bit word, so they still need an
 * explicit narrowing to the guest operand width.
 */
.macro normalize_immediate width, destination, source
	.if \width == 8
	and \destination, \source, #0xff
	.elseif \width == 16
	uxth \destination, \source
	.elseif \width == 32
	mov \destination, \source
	.elseif \width == 64
	mov \destination, \source
	.endif
.endm


.macro scalar_arithmetic_rr name, instruction, flag_operation, writes, load, store, host_register, store_register, width
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_vcpu, x9]
	\load \host_register\()12, [_vcpu, x10]
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	.if \writes
	\store \store_register\()13, [_vcpu, x9]
	.endif
	record_deferred \flag_operation, \width
	retire_and_next
.endm

.macro scalar_arithmetic_ri name, instruction, flag_operation, writes, load, store, host_register, store_register, width
gadget \name
	ldp x8, x12, [_stream], #16
	and x9, x8, #0xffff
	\load \host_register\()11, [_vcpu, x9]
	normalize_immediate \width, \host_register\()12, \host_register\()12
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	.if \writes
	\store \store_register\()13, [_vcpu, x9]
	.endif
	record_deferred \flag_operation, \width
	retire_and_next
.endm

.macro arithmetic_width operation, instruction, flag_operation, writes, width, load, store, host_register, store_register
scalar_arithmetic_rr \operation\()_\width\()_rr, \instruction, \flag_operation, \writes, \load, \store, \host_register, \store_register, \width
scalar_arithmetic_ri \operation\()_\width\()_ri, \instruction, \flag_operation, \writes, \load, \store, \host_register, \store_register, \width
.endm

/* Narrow operands are zero-extended, so subtraction and logical operations
 * preserve the guest zero condition in NZCV. Subtraction also preserves
 * unsigned ordering. Sign and overflow still require the deferred record
 * because the host uses bit 31.
 */
.macro arithmetic_family operation, instruction, flag_operation, writes, host_flags=0
.if \host_flags
arithmetic_width \operation, \instruction\()s, \flag_operation, \writes, 8, ldrb, strb, w, w
arithmetic_width \operation, \instruction\()s, \flag_operation, \writes, 16, ldrh, strh, w, w
arithmetic_width \operation, \instruction\()s, \flag_operation, \writes, 32, ldr, str, w, x
arithmetic_width \operation, \instruction\()s, \flag_operation, \writes, 64, ldr, str, x, x
.else
arithmetic_width \operation, \instruction, \flag_operation, \writes, 8, ldrb, strb, w, w
arithmetic_width \operation, \instruction, \flag_operation, \writes, 16, ldrh, strh, w, w
arithmetic_width \operation, \instruction, \flag_operation, \writes, 32, ldr, str, w, x
arithmetic_width \operation, \instruction, \flag_operation, \writes, 64, ldr, str, x, x
.endif
.endm

/* DeferredFlagOperation: add=1, subtract=2, logical=3. */
arithmetic_family add, add, 1, 1, 1
arithmetic_family sub, sub, 2, 1, 1
arithmetic_family and, and, 3, 1, 1
arithmetic_family or, orr, 3, 1
arithmetic_family xor, eor, 3, 1
arithmetic_family cmp, sub, 2, 0, 1
arithmetic_family test, and, 3, 0, 1

/* ADC or SBB of the operands in x11 and x12 with the carry flag, stored to
 * the register at x9, and the flags it defines recorded. */
.macro scalar_carry_finish instruction, flag_operation, width, host_register, store, store_register
	ldr x16, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	and w16, w16, #1
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	\instruction \host_register\()13, \host_register\()13, \host_register\()16
	normalize_result \width, \host_register\()13
	\store \store_register\()13, [_vcpu, x9]
	record_deferred \flag_operation, \width
	strb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	retire_and_next
.endm

.macro scalar_carry_rr name, instruction, flag_operation, width, load, store, host_register, store_register
gadget \name\()_\width\()_rr
	bl uml_x86_64_gadget_materialize_carry
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_vcpu, x9]
	\load \host_register\()12, [_vcpu, x10]
	scalar_carry_finish \instruction, \flag_operation, \width, \host_register, \store, \store_register
.endm

.macro scalar_carry_ri name, instruction, flag_operation, width, load, store, host_register, store_register
gadget \name\()_\width\()_ri
	bl uml_x86_64_gadget_materialize_carry
	ldp x8, x12, [_stream], #16
	and x9, x8, #0xffff
	\load \host_register\()11, [_vcpu, x9]
	normalize_immediate \width, \host_register\()12, \host_register\()12
	scalar_carry_finish \instruction, \flag_operation, \width, \host_register, \store, \store_register
.endm

.macro scalar_carry_width name, instruction, flag_operation, width, load, store, host_register, store_register
	scalar_carry_rr \name, \instruction, \flag_operation, \width, \load, \store, \host_register, \store_register
	scalar_carry_ri \name, \instruction, \flag_operation, \width, \load, \store, \host_register, \store_register
.endm

.macro scalar_carry_family name, instruction, flag_operation
	scalar_carry_width \name, \instruction, \flag_operation, 8, ldrb, strb, w, w
	scalar_carry_width \name, \instruction, \flag_operation, 16, ldrh, strh, w, w
	scalar_carry_width \name, \instruction, \flag_operation, 32, ldr, str, w, x
	scalar_carry_width \name, \instruction, \flag_operation, 64, ldr, str, x, x
.endm

scalar_carry_family adc, add, 6
scalar_carry_family sbb, sub, 7

.purgem scalar_carry_family
.purgem scalar_carry_width
.purgem scalar_carry_ri
.purgem scalar_carry_rr
.purgem scalar_carry_finish

.purgem arithmetic_family
.purgem arithmetic_width
.purgem scalar_arithmetic_ri
.purgem scalar_arithmetic_rr

.macro scalar_exchange width, load, store, load_register, store_register
gadget exchange_\width\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \load_register\()11, [_vcpu, x9]
	\load \load_register\()12, [_vcpu, x10]
	\store \store_register\()12, [_vcpu, x9]
	\store \store_register\()11, [_vcpu, x10]
	retire_and_next
.endm

scalar_exchange 8, ldrb, strb, w, w
scalar_exchange 16, ldrh, strh, w, w
scalar_exchange 32, ldr, str, w, x
scalar_exchange 64, ldr, str, x, x

.purgem scalar_exchange

.macro scalar_unary name, instruction, flag_operation, materialize, width, load, store, host_register, store_register
gadget \name\()_\width\()_r
	.if \materialize
	lazy_carry_prologue
	.endif
	ldr x8, [_stream], #8
	\load \host_register\()11, [_vcpu, x8]
	/* At 32 and 64 bits the flag-setting host form leaves N, Z and V
	 * describing the guest operation; only C differs (INC and DEC preserve
	 * it, NEG inverts it), and the builder keeps the carry conditions off
	 * these producers. */
	.ifc \instruction,add
	.if \width >= 32
	adds \host_register\()13, \host_register\()11, #1
	.else
	add \host_register\()13, \host_register\()11, #1
	.endif
	mov x12, #1
	.endif
	.ifc \instruction,sub
	.if \width >= 32
	subs \host_register\()13, \host_register\()11, #1
	.else
	sub \host_register\()13, \host_register\()11, #1
	.endif
	mov x12, #1
	.endif
	.ifc \instruction,neg
	.if \width >= 32
	negs \host_register\()13, \host_register\()11
	.else
	neg \host_register\()13, \host_register\()11
	.endif
	mov x12, x11
	mov x11, xzr
	.endif
	normalize_result \width, \host_register\()13
	\store \store_register\()13, [_vcpu, x8]
	.if \materialize
	record_deferred_pending \flag_operation, \width
	.else
	record_deferred \flag_operation, \width
	.endif
	retire_and_next
.endm

.macro scalar_not width, load, store, host_register, store_register
gadget not_\width\()_r
	ldr x8, [_stream], #8
	\load \host_register\()11, [_vcpu, x8]
	mvn \host_register\()11, \host_register\()11
	\store \store_register\()11, [_vcpu, x8]
	retire_and_next
.endm

.macro unary_width width, load, store, host_register, store_register
scalar_unary increment, add, 4, 1, \width, \load, \store, \host_register, \store_register
scalar_unary decrement, sub, 5, 1, \width, \load, \store, \host_register, \store_register
scalar_unary negate, neg, 2, 0, \width, \load, \store, \host_register, \store_register
scalar_not \width, \load, \store, \host_register, \store_register
.endm

unary_width 8, ldrb, strb, w, w
unary_width 16, ldrh, strh, w, w
unary_width 32, ldr, str, w, x
unary_width 64, ldr, str, x, x

.macro memory_arithmetic_rm operation, instruction, flag_operation, writes, width, load, store, host_register, store_register
gadget \operation\()_\width\()_rm
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), Lx86_arithmetic_rm_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()12, [x7]
Lx86_arithmetic_rm_loaded\@:
	and x8, x23, #0xffff
	\load \host_register\()11, [_vcpu, x8]
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	.if \writes
	\store \store_register\()13, [_vcpu, x8]
	.endif
	record_deferred \flag_operation, \width
	retire_and_next
Lx86_arithmetic_rm_cross\@:
	cross_page_load (\width / 8)
	mov x12, x0
	b Lx86_arithmetic_rm_loaded\@
.endm

/* The address of a read-modify-write memory destination in x7 and x25, and
 * its source in x26: the register or immediate that `form` (r or i) names. */
.macro memory_source_operand form, width, load, host_register
	effective_address
	mov x25, x7
	.ifc \form,r
	and x8, x23, #0xffff
	\load \host_register\()26, [_vcpu, x8]
	.endif
	.ifc \form,i
	ldr x26, [_stream], #8
	normalize_immediate \width, \host_register\()26, \host_register\()26
	.endif
.endm

.macro memory_arithmetic_msource operation, instruction, flag_operation, writes, form, width, load, store, host_register, locked
	.if \locked
gadget \operation\()_\width\()_m\form\()_locked
	.else
gadget \operation\()_\width\()_m\form
	.endif
	memory_source_operand \form, \width, \load, \host_register
	.if \locked
	locked_aligned \width, Lx86_arithmetic_msource_split\@
Lx86_arithmetic_msource_exclusive\@:
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
1:
	load_exclusive \width, 24, x7
	\instruction \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	store_exclusive \width, 9, 27, x7
	cbnz w9, 1b
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	finish_store (\width / 8)
Lx86_arithmetic_msource_split\@:
	split_operand (\width / 8), Lx86_arithmetic_msource_exclusive\@
	.endif
	page_cross_check (\width / 8), Lx86_arithmetic_msource_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
Lx86_arithmetic_msource_read\@:
	\instruction \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	.if !\writes
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	retire_and_next
	.endif
	.if \writes
	mov x7, x25
	page_cross_check (\width / 8), Lx86_arithmetic_msource_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
Lx86_arithmetic_msource_written\@:
	.if \locked
	split_lock_leave
	.endif
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	finish_store (\width / 8)
Lx86_arithmetic_msource_write_cross\@:
	cross_page_store x27, (\width / 8)
	b Lx86_arithmetic_msource_written\@
	.endif
Lx86_arithmetic_msource_read_cross\@:
	cross_page_load (\width / 8)
	mov x24, x0
	b Lx86_arithmetic_msource_read\@
.endm

.macro memory_arithmetic_width operation, instruction, flag_operation, writes, width, load, store, host_register, store_register
memory_arithmetic_rm \operation, \instruction, \flag_operation, \writes, \width, \load, \store, \host_register, \store_register
memory_arithmetic_msource \operation, \instruction, \flag_operation, \writes, r, \width, \load, \store, \host_register, 0
memory_arithmetic_msource \operation, \instruction, \flag_operation, \writes, i, \width, \load, \store, \host_register, 0
.if \writes
memory_arithmetic_msource \operation, \instruction, \flag_operation, \writes, r, \width, \load, \store, \host_register, 1
memory_arithmetic_msource \operation, \instruction, \flag_operation, \writes, i, \width, \load, \store, \host_register, 1
.endif
.endm

.macro memory_arithmetic_family operation, instruction, flag_operation, writes
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 8, ldrb, strb, w, w
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 16, ldrh, strh, w, w
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 32, ldr, str, w, x
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 64, ldr, str, x, x
.endm

memory_arithmetic_family add, add, 1, 1
memory_arithmetic_family sub, sub, 2, 1
memory_arithmetic_family and, and, 3, 1
memory_arithmetic_family or, orr, 3, 1
memory_arithmetic_family xor, eor, 3, 1
memory_arithmetic_family cmp, sub, 2, 0
memory_arithmetic_family test, and, 3, 0

.macro memory_carry_rm name, instruction, flag_operation, width, load, store, host_register, store_register
gadget \name\()_\width\()_rm
	bl uml_x86_64_gadget_materialize_flags
	ldr x19, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	and w19, w19, #1
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 12
	and x8, x23, #0xffff
	\load \host_register\()11, [_vcpu, x8]
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	\instruction \host_register\()13, \host_register\()13, \host_register\()19
	normalize_result \width, \host_register\()13
	\store \store_register\()13, [_vcpu, x8]
	record_deferred \flag_operation, \width
	strb w19, [_vcpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	retire_and_next
.endm

.macro memory_carry_msource name, instruction, flag_operation, form, width, load, store, host_register, locked
	.if \locked
gadget \name\()_\width\()_m\form\()_locked
	.else
gadget \name\()_\width\()_m\form
	.endif
	bl uml_x86_64_gadget_materialize_flags
	ldr x19, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	and w19, w19, #1
	memory_source_operand \form, \width, \load, \host_register
	.if \locked
	locked_aligned \width, Lx86_carry_msource_split\@
Lx86_carry_msource_exclusive\@:
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
1:
	load_exclusive \width, 24, x7
	\instruction \host_register\()27, \host_register\()24, \host_register\()26
	\instruction \host_register\()27, \host_register\()27, \host_register\()19
	normalize_result \width, \host_register\()27
	store_exclusive \width, 9, 27, x7
	cbnz w9, 1b
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	strb w19, [_vcpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	finish_store (\width / 8)
Lx86_carry_msource_split\@:
	split_operand (\width / 8), Lx86_carry_msource_exclusive\@
	.endif
	read_operand (\width / 8), \load, \host_register, 24
	\instruction \host_register\()27, \host_register\()24, \host_register\()26
	\instruction \host_register\()27, \host_register\()27, \host_register\()19
	normalize_result \width, \host_register\()27
	mov x7, x25
	page_cross_check (\width / 8), Lx86_carry_msource_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_carry_msource_written\@
Lx86_carry_msource_write_cross\@:
	cross_page_store x27, (\width / 8)
Lx86_carry_msource_written\@:
	.if \locked
	split_lock_leave
	.endif
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	strb w19, [_vcpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	finish_store (\width / 8)
.endm

.macro memory_carry_width name, instruction, flag_operation, width, load, store, host_register, store_register
	memory_carry_rm \name, \instruction, \flag_operation, \width, \load, \store, \host_register, \store_register
	memory_carry_msource \name, \instruction, \flag_operation, r, \width, \load, \store, \host_register, 0
	memory_carry_msource \name, \instruction, \flag_operation, i, \width, \load, \store, \host_register, 0
	memory_carry_msource \name, \instruction, \flag_operation, r, \width, \load, \store, \host_register, 1
	memory_carry_msource \name, \instruction, \flag_operation, i, \width, \load, \store, \host_register, 1
.endm

.macro memory_carry_family name, instruction, flag_operation
	memory_carry_width \name, \instruction, \flag_operation, 8, ldrb, strb, w, w
	memory_carry_width \name, \instruction, \flag_operation, 16, ldrh, strh, w, w
	memory_carry_width \name, \instruction, \flag_operation, 32, ldr, str, w, x
	memory_carry_width \name, \instruction, \flag_operation, 64, ldr, str, x, x
.endm

memory_carry_family adc, add, 6
memory_carry_family sbb, sub, 7

.purgem memory_carry_family
.purgem memory_carry_width
.purgem memory_carry_msource
.purgem memory_carry_rm

.macro memory_exchange width, load, store, host_register, register_store, register_store_register
gadget exchange_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()26, [_vcpu, x8]
	/* XCHG against memory is atomic on x86 with no LOCK prefix present. */
	locked_aligned \width, Lx86_exchange_split\@
Lx86_exchange_exclusive\@:
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
1:
	load_exclusive \width, 24, x7
	store_exclusive \width, 9, 26, x7
	cbnz w9, 1b
	and x8, x23, #0xffff
	\register_store \register_store_register\()24, [_vcpu, x8]
	finish_store (\width / 8)
Lx86_exchange_split\@:
	split_operand (\width / 8), Lx86_exchange_exclusive\@
	read_operand (\width / 8), \load, \host_register, 24
	mov x7, x25
	page_cross_check (\width / 8), Lx86_exchange_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()26, [x7]
	b Lx86_exchange_written\@
Lx86_exchange_write_cross\@:
	cross_page_store x26, (\width / 8)
Lx86_exchange_written\@:
	split_lock_leave
	and x8, x23, #0xffff
	\register_store \register_store_register\()24, [_vcpu, x8]
	finish_store (\width / 8)
.endm

memory_exchange 8, ldrb, strb, w, strb, w
memory_exchange 16, ldrh, strh, w, strh, w
memory_exchange 32, ldr, str, w, str, x
memory_exchange 64, ldr, str, x, str, x

.purgem memory_exchange

/* The new value in x27 from the old one in x24. */
.macro memory_unary_compute instruction, host_register
	.ifc \instruction,add
	add \host_register\()27, \host_register\()24, #1
	.endif
	.ifc \instruction,sub
	sub \host_register\()27, \host_register\()24, #1
	.endif
	.ifc \instruction,neg
	neg \host_register\()27, \host_register\()24
	.endif
	.ifc \instruction,not
	mvn \host_register\()27, \host_register\()24
	.endif
.endm

/* The deferred record of a unary operation that defines flags, from the old
 * value in x24 and the new one in x27: NEG records a subtraction from zero,
 * and `materialize` marks INC and DEC, whose record keeps their pending
 * carry. */
.macro memory_unary_record instruction, flag_operation, materialize, width
	.if \flag_operation
	.ifc \instruction,neg
	mov x11, xzr
	mov x12, x24
	.else
	mov x11, x24
	mov x12, #1
	.endif
	mov x13, x27
	.if \materialize
	record_deferred_pending \flag_operation, \width
	.else
	record_deferred \flag_operation, \width
	.endif
	.endif
.endm

.macro memory_unary operation, instruction, flag_operation, materialize, width, load, store, host_register, locked
	.if \locked
gadget \operation\()_\width\()_m_locked
	.else
gadget \operation\()_\width\()_m
	.endif
	.if \materialize
	lazy_carry_prologue
	.endif
	effective_address
	mov x25, x7
	.if \locked
	locked_aligned \width, Lx86_unary_split\@
Lx86_unary_exclusive\@:
	mov x7, x25
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
1:
	load_exclusive \width, 24, x7
	memory_unary_compute \instruction, \host_register
	normalize_result \width, \host_register\()27
	store_exclusive \width, 9, 27, x7
	cbnz w9, 1b
	memory_unary_record \instruction, \flag_operation, \materialize, \width
	finish_store (\width / 8)
Lx86_unary_split\@:
	split_operand (\width / 8), Lx86_unary_exclusive\@
	.endif
	read_operand (\width / 8), \load, \host_register, 24
	memory_unary_compute \instruction, \host_register
	normalize_result \width, \host_register\()27
	mov x7, x25
	page_cross_check (\width / 8), Lx86_unary_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_unary_written\@
Lx86_unary_write_cross\@:
	cross_page_store x27, (\width / 8)
Lx86_unary_written\@:
	.if \locked
	split_lock_leave
	.endif
	memory_unary_record \instruction, \flag_operation, \materialize, \width
	finish_store (\width / 8)
.endm

.macro memory_unary_width width, load, store, host_register
	memory_unary increment, add, 4, 1, \width, \load, \store, \host_register, 0
	memory_unary decrement, sub, 5, 1, \width, \load, \store, \host_register, 0
	memory_unary negate, neg, 2, 0, \width, \load, \store, \host_register, 0
	memory_unary not, not, 0, 0, \width, \load, \store, \host_register, 0
	memory_unary increment, add, 4, 1, \width, \load, \store, \host_register, 1
	memory_unary decrement, sub, 5, 1, \width, \load, \store, \host_register, 1
	memory_unary negate, neg, 2, 0, \width, \load, \store, \host_register, 1
	memory_unary not, not, 0, 0, \width, \load, \store, \host_register, 1
.endm

memory_unary_width 8, ldrb, strb, w
memory_unary_width 16, ldrh, strh, w
memory_unary_width 32, ldr, str, w
memory_unary_width 64, ldr, str, x

.purgem memory_unary_width
.purgem memory_unary
.purgem memory_unary_record
.purgem memory_unary_compute

.purgem memory_arithmetic_family
.purgem memory_arithmetic_width
.purgem memory_arithmetic_msource
.purgem memory_source_operand
.purgem memory_arithmetic_rm

.purgem unary_width
.purgem scalar_not
.purgem scalar_unary
.purgem normalize_immediate

gadget byte_swap_32
	ldr x8, [_stream], #8
	ldr w9, [_vcpu, x8]
	rev w9, w9
	str x9, [_vcpu, x8]
	retire_and_next

gadget byte_swap_64
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, x8]
	rev x9, x9
	str x9, [_vcpu, x8]
	retire_and_next

gadget sign_extend_accumulator_16
	ldrsb w8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	strh w8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sign_extend_accumulator_32
	ldrsh w8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sign_extend_accumulator_64
	ldrsw x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sign_extend_data_16
	ldrsh w8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	asr w8, w8, #15
	strh w8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

gadget sign_extend_data_32
	ldr w8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	asr w8, w8, #31
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

gadget sign_extend_data_64
	ldr x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	asr x8, x8, #63
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.macro flag_control name, instruction, mask
gadget \name
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	\instruction x8, x8, #\mask
	orr x8, x8, #0x2
	str x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

flag_control carry_clear, bic, 0x1
flag_control carry_set, orr, 0x1
flag_control carry_complement, eor, 0x1
flag_control direction_clear, bic, 0x400
flag_control direction_set, orr, 0x400

.purgem flag_control

gadget lahf
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x9, #0xd5
	and x8, x8, x9
	orr x8, x8, #0x2
	ldr x10, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	bfi x10, x8, #8, #8
	str x10, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sahf
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ubfx x8, x8, #8, #8
	mov x9, #0xd5
	and x8, x8, x9
	ldr x10, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	bic x10, x10, x9
	orr x10, x10, x8
	orr x10, x10, #0x2
	str x10, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
