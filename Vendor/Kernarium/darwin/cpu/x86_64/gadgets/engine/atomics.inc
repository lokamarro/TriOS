/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compare/exchange and exchange/add scalar operations. */



.macro compare_exchange_register width, load, store, host_register, store_register
gadget cmpxchg_\width\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()24, [_vcpu, x9]
	\load \host_register\()26, [_vcpu, x10]
	\load \host_register\()11, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	cmp \host_register\()11, \host_register\()24
	b.ne Lx86_cmpxchg_register_unequal\@
	\store \store_register\()26, [_vcpu, x9]
	b Lx86_cmpxchg_register_flags\@
Lx86_cmpxchg_register_unequal\@:
	\store \store_register\()24, [_vcpu, #UML_X86_64_CPU_REGISTERS]
Lx86_cmpxchg_register_flags\@:
	mov x12, x24
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	record_deferred 2, \width
	retire_and_next
.endm

.macro compare_exchange_memory width, load, store, host_register, accumulator_store_register
gadget cmpxchg_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()26, [_vcpu, x8]
	locked_aligned \width, Lx86_cmpxchg_split\@
Lx86_cmpxchg_exclusive\@:
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\load \host_register\()27, [_vcpu, #UML_X86_64_CPU_REGISTERS]
1:
	load_exclusive \width, 24, x7
	cmp \host_register\()27, \host_register\()24
	b.ne 2f
	store_exclusive \width, 9, 26, x7
	cbnz w9, 1b
	mov x11, x27
	mov x12, x24
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	record_deferred 2, \width
	finish_store (\width / 8)
2:
	clrex
	\store \accumulator_store_register\()24, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	mov x11, x27
	mov x12, x24
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	record_deferred 2, \width
	retire_and_next
Lx86_cmpxchg_split\@:
	split_operand (\width / 8), Lx86_cmpxchg_exclusive\@
	read_operand (\width / 8), \load, \host_register, 24
	\load \host_register\()27, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	cmp \host_register\()27, \host_register\()24
	b.ne Lx86_cmpxchg_memory_unequal\@
	mov x7, x25
	page_cross_check (\width / 8), Lx86_cmpxchg_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()26, [x7]
	b Lx86_cmpxchg_memory_written\@
Lx86_cmpxchg_write_cross\@:
	cross_page_store x26, (\width / 8)
Lx86_cmpxchg_memory_written\@:
	split_lock_leave
	mov x11, x27
	mov x12, x24
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	record_deferred 2, \width
	finish_store (\width / 8)
Lx86_cmpxchg_memory_unequal\@:
	split_lock_leave
	\store \accumulator_store_register\()24, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	mov x11, x27
	mov x12, x24
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	record_deferred 2, \width
	retire_and_next
.endm

.macro compare_exchange_width width, load, store, host_register, store_register
	compare_exchange_register \width, \load, \store, \host_register, \store_register
	compare_exchange_memory \width, \load, \store, \host_register, \store_register
.endm

compare_exchange_width 8, ldrb, strb, w, w
compare_exchange_width 16, ldrh, strh, w, w
compare_exchange_width 32, ldr, str, w, x
compare_exchange_width 64, ldr, str, x, x

.purgem compare_exchange_width
.purgem compare_exchange_memory
.purgem compare_exchange_register

.macro exchange_add_register width, load, store, host_register, store_register
gadget xadd_\width\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()24, [_vcpu, x9]
	\load \host_register\()26, [_vcpu, x10]
	add \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	\store \store_register\()24, [_vcpu, x10]
	\store \store_register\()27, [_vcpu, x9]
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred 1, \width
	retire_and_next
.endm

.macro exchange_add_memory width, load, store, host_register, source_store_register
gadget xadd_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()26, [_vcpu, x8]
	locked_aligned \width, Lx86_xadd_split\@
Lx86_xadd_exclusive\@:
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
1:
	load_exclusive \width, 24, x7
	add \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	store_exclusive \width, 9, 27, x7
	cbnz w9, 1b
	and x8, x23, #0xffff
	\store \source_store_register\()24, [_vcpu, x8]
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred 1, \width
	finish_store (\width / 8)
Lx86_xadd_split\@:
	split_operand (\width / 8), Lx86_xadd_exclusive\@
	read_operand (\width / 8), \load, \host_register, 24
	add \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	mov x7, x25
	page_cross_check (\width / 8), Lx86_xadd_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_xadd_written\@
Lx86_xadd_write_cross\@:
	cross_page_store x27, (\width / 8)
Lx86_xadd_written\@:
	split_lock_leave
	and x8, x23, #0xffff
	\store \source_store_register\()24, [_vcpu, x8]
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred 1, \width
	finish_store (\width / 8)
.endm

.macro exchange_add_width width, load, store, host_register, store_register
	exchange_add_register \width, \load, \store, \host_register, \store_register
	exchange_add_memory \width, \load, \store, \host_register, \store_register
.endm

exchange_add_width 8, ldrb, strb, w, w
exchange_add_width 16, ldrh, strh, w, w
exchange_add_width 32, ldr, str, w, x
exchange_add_width 64, ldr, str, x, x

.purgem exchange_add_width
.purgem exchange_add_memory
.purgem exchange_add_register

.macro compare_exchange_pair_flags
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	bic x8, x8, #0x40
	cbz w19, Lx86_cmpxchg_pair_flags_done\@
	orr x8, x8, #0x40
Lx86_cmpxchg_pair_flags_done\@:
	orr x8, x8, #0x2
	str x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
.endm

gadget cmpxchg_pair_8
	effective_address
	mov x25, x7
	tst x7, #7
	b.ne Lx86_cmpxchg_pair_8_split
Lx86_cmpxchg_pair_8_exclusive:
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 8
	ldr w10, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr w11, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	orr x10, x10, x11, lsl #32
	ldr w27, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldr w16, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	orr x27, x27, x16, lsl #32
1:
	ldaxr x24, [x7]
	cmp x24, x10
	b.ne 2f
	stlxr w12, x27, [x7]
	cbnz w12, 1b
	mov w19, #1
	compare_exchange_pair_flags
	finish_store 8
2:
	clrex
	mov w19, wzr
	mov w8, w24
	lsr x9, x24, #32
	mov w9, w9
	str x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	compare_exchange_pair_flags
	retire_and_next
Lx86_cmpxchg_pair_8_split:
	split_operand 8, Lx86_cmpxchg_pair_8_exclusive
	read_operand 8, ldr, x, 24
	ldr w8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr w9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	cmp w8, w24
	b.ne Lx86_cmpxchg_pair_8_unequal
	lsr x10, x24, #32
	cmp w9, w10
	b.ne Lx86_cmpxchg_pair_8_unequal
	mov w19, #1
	ldr w27, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldr w16, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	orr x27, x27, x16, lsl #32
	mov x7, x25
	page_cross_check 8, Lx86_cmpxchg_pair_8_write_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 8
	str x27, [x7]
	b Lx86_cmpxchg_pair_8_written
Lx86_cmpxchg_pair_8_write_cross:
	cross_page_store x27, 8
Lx86_cmpxchg_pair_8_written:
	split_lock_leave
	compare_exchange_pair_flags
	finish_store 8
Lx86_cmpxchg_pair_8_unequal:
	split_lock_leave
	mov w19, wzr
	mov w8, w24
	lsr x9, x24, #32
	mov w9, w9
	str x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	compare_exchange_pair_flags
	retire_and_next

/* CMPXCHG16B raises #GP for an operand that is not 16-byte aligned, before
 * it reads anything. */
gadget cmpxchg_pair_16
	effective_address
	mov x25, x7
	tst x7, #15
	b.ne Lx86_general_protection
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 16
	ldr x10, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr x11, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	ldr x27, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldr x16, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
1:
	ldaxp x24, x26, [x7]
	cmp x24, x10
	b.ne 2f
	cmp x26, x11
	b.ne 2f
	stlxp w12, x27, x16, [x7]
	cbnz w12, 1b
	mov w19, #1
	compare_exchange_pair_flags
	finish_store 16
2:
	clrex
	mov w19, wzr
	str x24, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x26, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	compare_exchange_pair_flags
	retire_and_next

.purgem compare_exchange_pair_flags
