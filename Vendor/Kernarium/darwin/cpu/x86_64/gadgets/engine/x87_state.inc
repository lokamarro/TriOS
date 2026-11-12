/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register-stack and control-only x87 instructions. */

.macro x87_initialize_state
	mov w9, #0x037f
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	strh wzr, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	mov w9, #0xffff
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_vcpu, #UML_X86_64_CPU_X87_TOP]
.endm

gadget x87_emms
	mov w9, #0xffff
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	retire_and_next

gadget x87_nop
	retire_and_next

gadget x87_load_register
	ldr w13, [_stream], #8
	x87_load_stack w13, x9, w10
	x87_push x9, w10
	retire_and_next

gadget x87_exchange_register
	ldr w13, [_stream], #8
	ldrb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	add w15, w14, w13
	and w15, w15, #7
	ldrh w16, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	lsl w17, w14, #1
	lsr w9, w16, w17
	and w24, w9, #3
	lsl w17, w15, #1
	lsr w9, w16, w17
	and w25, w9, #3
	cmp w24, #Lx86_x87_tag_empty
	ccmp w25, #Lx86_x87_tag_empty, #4, ne
	b.eq Lx86_x87_exchange_empty
	add x16, _vcpu, #UML_X86_64_CPU_X87
	add x17, x16, w14, uxtw #4
	add x16, x16, w15, uxtw #4
	ldr x9, [x17]
	ldrh w10, [x17, #8]
	ldr x11, [x16]
	ldrh w12, [x16, #8]
	str x11, [x17]
	strh w12, [x17, #8]
	str x9, [x16]
	strh w10, [x16, #8]
	add x16, _vcpu, #UML_X86_64_CPU_MMX
	ldr x9, [x16, w14, uxtw #3]
	ldr x10, [x16, w15, uxtw #3]
	str x10, [x16, w14, uxtw #3]
	str x9, [x16, w15, uxtw #3]
	/* x87_set_tag uses w15 itself. */
	mov w13, w15
	x87_set_tag w14, w25
	x87_set_tag w13, w24
Lx86_x87_exchange_done:
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #Lx86_x87_status_c1
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	retire_and_next
/* A stack underflow: masked, an empty register reads as the QNaN
 * indefinite and the exchange goes on. */
Lx86_x87_exchange_empty:
	x87_stack_fault_check
	mov w12, #0
	x87_load_stack w12, x9, w10
	x87_load_stack w13, x11, w12
	x87_store_stack w13, x9, w10
	mov w13, #0
	x87_store_stack w13, x11, w12
	b Lx86_x87_exchange_done

/* FCHS and FABS of an empty ST(0) leave the indefinite the underflow made. */
gadget x87_change_sign
	mov w13, #0
	x87_load_stack w13, x9, w10, Lx86_x87_sign_underflow
	eor w10, w10, #0x8000
	x87_store_stack w13, x9, w10
	x87_clear_c1
	retire_and_next

gadget x87_absolute
	mov w13, #0
	x87_load_stack w13, x9, w10, Lx86_x87_sign_underflow
	and w10, w10, #0x7fff
	x87_store_stack w13, x9, w10
	x87_clear_c1
	retire_and_next
Lx86_x87_sign_underflow:
	x87_store_stack w13, x9, w10
	x87_clear_c1
	retire_and_next

/* FXAM's class in C3, C2 and C0: 000 unsupported, 001 NaN, 010 normal,
 * 011 infinity, 100 zero, 101 empty, 110 denormal; its sign in C1. */
gadget x87_examine
	ldrb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	add x15, _vcpu, #UML_X86_64_CPU_X87
	add x15, x15, w14, uxtw #4
	ldr x11, [x15]
	ldrh w16, [x15, #8]
	ldrh w15, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	lsl w17, w14, #1
	lsr w15, w15, w17
	and w15, w15, #3
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	mov w10, #0x4700
	bic w9, w9, w10
	tbz w16, #15, Lx86_x87_examine_positive
	orr w9, w9, #Lx86_x87_status_c1
Lx86_x87_examine_positive:
	cmp w15, #Lx86_x87_tag_empty
	mov w10, #0x4100
	b.eq Lx86_x87_examine_done
	and w16, w16, #0x7fff
	cbz w16, Lx86_x87_examine_tiny
	mov w10, #0
	tbz x11, #63, Lx86_x87_examine_done
	mov w15, #0x7fff
	cmp w16, w15
	mov w10, #0x0400
	b.ne Lx86_x87_examine_done
	lsl x11, x11, #1
	cmp x11, #0
	mov w10, #0x0500
	mov w15, #0x0100
	csel w10, w10, w15, eq
	b Lx86_x87_examine_done
Lx86_x87_examine_tiny:
	cmp x11, #0
	mov w10, #0x4000
	mov w15, #0x4400
	csel w10, w10, w15, eq
Lx86_x87_examine_done:
	orr w9, w9, w10
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	retire_and_next

.macro x87_constant name, significand, exponent
gadget x87_constant_\name
	x87_load_u64 x9, \significand
	mov w10, #\exponent
	x87_push x9, w10
	retire_and_next
.endm

x87_constant one, 0x8000000000000000, 0x3fff
x87_constant log2_10, 0xd49a784bcd1b8afe, 0x4000
x87_constant log2_e, 0xb8aa3b295c17f0bc, 0x3fff
x87_constant pi, 0xc90fdaa22168c235, 0x4000
x87_constant log10_2, 0x9a209a84fbcff799, 0x3ffd
x87_constant ln_2, 0xb17217f7d1cf79ac, 0x3ffe
x87_constant zero, 0x0000000000000000, 0x0000

gadget x87_decrement_top
	ldrb w9, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	sub w9, w9, #1
	and w9, w9, #7
	strb w9, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #Lx86_x87_status_c1
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	retire_and_next

gadget x87_increment_top
	ldrb w9, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	add w9, w9, #1
	and w9, w9, #7
	strb w9, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #Lx86_x87_status_c1
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	retire_and_next

.macro x87_free_entry name, pop
gadget x87_free_\name
	ldr w13, [_stream], #8
	x87_physical w13, w14
	mov w17, #Lx86_x87_tag_empty
	x87_set_tag w14, w17
	.if \pop
	x87_pop
	.endif
	x87_clear_c1
	retire_and_next
.endm
x87_free_entry register, 0
x87_free_entry register_pop, 1

.macro x87_store_register_entry name, pop
gadget x87_store_register_\name
	ldr w13, [_stream], #8
	mov w12, #0
	x87_load_stack w12, x9, w10
	x87_store_stack w13, x9, w10
	.if \pop
	x87_pop
	.endif
	x87_clear_c1
	retire_and_next
.endm
x87_store_register_entry keep, 0
x87_store_register_entry pop, 1

gadget x87_store_status_ax
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #0x3800
	ldrb w10, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	orr w9, w9, w10, lsl #11
	ldr x10, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	bfxil x10, x9, #0, #16
	str x10, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget x87_clear_exceptions
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	and w9, w9, #0xff00
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	retire_and_next

gadget x87_initialize
	x87_initialize_state
	retire_and_next

.purgem x87_store_register_entry
.purgem x87_free_entry
.purgem x87_constant
