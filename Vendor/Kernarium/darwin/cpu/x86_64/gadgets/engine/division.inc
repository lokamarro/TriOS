/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar DIV and IDIV forms. */

.macro divide_register name, width, load, host_register, target
gadget \name\()_\width\()_r
	ldr x23, [_stream], #8
	and x8, x23, #0xffff
	\load \host_register\()15, [_vcpu, x8]
	b \target
.endm

.macro divide_memory name, width, load, host_register, target
gadget \name\()_\width\()_m
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), Lx86_divide_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()15, [x7]
	/* The shared tail belongs to the register form and carries no barrier. */
	x86_memory_order_boundary
	b \target
Lx86_divide_read_cross\@:
	cross_page_load (\width / 8)
	mov x15, x0
	x86_memory_order_boundary
	b \target
.endm

.macro divide_forms name, width, load, host_register, target
	divide_register \name, \width, \load, \host_register, \target
	divide_memory \name, \width, \load, \host_register, \target
.endm

divide_forms div, 8, ldrb, w, Lx86_divide_unsigned_8
divide_forms div, 16, ldrh, w, Lx86_divide_unsigned_16
divide_forms div, 32, ldr, w, Lx86_divide_unsigned_32
divide_forms div, 64, ldr, x, Lx86_divide_unsigned_64
divide_forms idiv, 8, ldrb, w, Lx86_divide_signed_8
divide_forms idiv, 16, ldrh, w, Lx86_divide_signed_16
divide_forms idiv, 32, ldr, w, Lx86_divide_signed_32
divide_forms idiv, 64, ldr, x, Lx86_divide_signed_64

.purgem divide_forms
.purgem divide_memory
.purgem divide_register

.p2align 4
Lx86_divide_unsigned_8:
	cbz w15, Lx86_divide_trap
	ldrh w13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	udiv w16, w13, w15
	cmp w16, #0xff
	b.hi Lx86_divide_trap
	msub w17, w16, w15, w13
	orr w16, w16, w17, lsl #8
	strh w16, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

.p2align 4
Lx86_divide_unsigned_16:
	cbz w15, Lx86_divide_trap
	ldrh w13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldrh w14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	orr w13, w13, w14, lsl #16
	udiv w16, w13, w15
	lsr w8, w16, #16
	cbnz w8, Lx86_divide_trap
	msub w17, w16, w15, w13
	strh w16, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	strh w17, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.p2align 4
Lx86_divide_unsigned_32:
	cbz w15, Lx86_divide_trap
	ldr w13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr w14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	orr x13, x13, x14, lsl #32
	mov w15, w15
	udiv x16, x13, x15
	lsr x8, x16, #32
	cbnz x8, Lx86_divide_trap
	msub x17, x16, x15, x13
	mov w16, w16
	mov w17, w17
	str x16, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x17, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

/* Unsigned [x14:x13] / x15. The caller checks x15 for zero. x12 reports
 * quotient overflow; otherwise x13 is the quotient and x14 the remainder.
 */
.p2align 4
Lx86_divide_unsigned_128_64:
	cmp x14, x15
	b.hs Lx86_divide_unsigned_128_overflow
	cbnz x14, Lx86_divide_unsigned_128_loop_setup
	udiv x16, x13, x15
	msub x14, x16, x15, x13
	mov x13, x16
	mov w12, wzr
	ret
Lx86_divide_unsigned_128_loop_setup:
	mov w11, #64
Lx86_divide_unsigned_128_loop:
	lsr x8, x13, #63
	lsl x13, x13, #1
	adds x14, x14, x14
	orr x14, x14, x8
	b.cs Lx86_divide_unsigned_128_subtract
	cmp x14, x15
	b.lo Lx86_divide_unsigned_128_next
Lx86_divide_unsigned_128_subtract:
	sub x14, x14, x15
	orr x13, x13, #1
Lx86_divide_unsigned_128_next:
	subs w11, w11, #1
	b.ne Lx86_divide_unsigned_128_loop
	mov w12, wzr
	ret
Lx86_divide_unsigned_128_overflow:
	mov w12, #1
	ret

.p2align 4
Lx86_divide_unsigned_64:
	cbz x15, Lx86_divide_trap
	ldr x13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr x14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	bl Lx86_divide_unsigned_128_64
	cbnz w12, Lx86_divide_trap
	str x13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.p2align 4
Lx86_divide_signed_8:
	sxtb w15, w15
	cbz w15, Lx86_divide_trap
	ldrsh w13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	sdiv w16, w13, w15
	sxtb w8, w16
	cmp w16, w8
	b.ne Lx86_divide_trap
	msub w17, w16, w15, w13
	and w16, w16, #0xff
	bfi w16, w17, #8, #8
	strh w16, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

.p2align 4
Lx86_divide_signed_16:
	sxth w15, w15
	cbz w15, Lx86_divide_trap
	ldrh w13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldrh w14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	orr w13, w13, w14, lsl #16
	sdiv w16, w13, w15
	sxth w8, w16
	cmp w16, w8
	b.ne Lx86_divide_trap
	msub w17, w16, w15, w13
	strh w16, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	strh w17, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.p2align 4
Lx86_divide_signed_32:
	sxtw x15, w15
	cbz x15, Lx86_divide_trap
	ldr w13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr w14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	orr x13, x13, x14, lsl #32
	sdiv x16, x13, x15
	sxtw x8, w16
	cmp x16, x8
	b.ne Lx86_divide_trap
	msub x17, x16, x15, x13
	mov w16, w16
	mov w17, w17
	str x16, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x17, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.p2align 4
Lx86_divide_signed_64:
	cbz x15, Lx86_divide_trap
	ldr x13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr x14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	lsr x24, x14, #63
	lsr x25, x15, #63
	cbz x24, Lx86_divide_signed_dividend_absolute
	mvn x13, x13
	mvn x14, x14
	adds x13, x13, #1
	adc x14, x14, xzr
Lx86_divide_signed_dividend_absolute:
	cbz x25, Lx86_divide_signed_divisor_absolute
	neg x15, x15
Lx86_divide_signed_divisor_absolute:
	bl Lx86_divide_unsigned_128_64
	cbnz w12, Lx86_divide_trap
	eor w25, w24, w25
	cbnz w25, Lx86_divide_signed_negative_quotient
	tbnz x13, #63, Lx86_divide_trap
	b Lx86_divide_signed_remainder
Lx86_divide_signed_negative_quotient:
	mov x8, #1
	lsl x8, x8, #63
	cmp x13, x8
	b.hi Lx86_divide_trap
	neg x13, x13
Lx86_divide_signed_remainder:
	cbz w24, Lx86_divide_signed_store
	neg x14, x14
Lx86_divide_signed_store:
	str x13, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x14, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.p2align 4
Lx86_divide_trap:
	instruction_rip x22
	str x22, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_FLOATING_POINT_TRAP
	b uml_x86_64_gadget_leave
