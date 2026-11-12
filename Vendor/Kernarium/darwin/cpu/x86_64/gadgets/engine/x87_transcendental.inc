/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Rare exact x87 transcendental and remainder operations. */

.macro x87_unary name, helper
gadget x87_\name
	sub sp, sp, #32
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	helper_call uml_x86_64_x87_\helper
	x87_leave_if_raised
	add x11, sp, #16
	x87_load_value x11, x9, w10
	add sp, sp, #32
	mov w13, #0
	x87_store_stack w13, x9, w10
	retire_and_next
.endm

x87_unary f2xm1, f2xm1
x87_unary sqrt, sqrt
x87_unary round, round
x87_unary sine, sine
x87_unary cosine, cosine

.macro x87_two_input name, helper, destination, pop, reverse=0
gadget x87_\name
	sub sp, sp, #48
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov w13, #1
	x87_load_stack w13, x9, w10
	add x11, sp, #16
	x87_store_value x11, x9, w10
	x87_binary_call \helper, \reverse
	mov w13, #\destination
	x87_store_stack w13, x9, w10
	.if \pop
	x87_pop
	.endif
	retire_and_next
.endm

x87_two_input fyl2x, fyl2x, 1, 1
x87_two_input fyl2xp1, fyl2xp1, 1, 1
x87_two_input atan, atan, 1, 1, 1
x87_two_input scale, scale, 0, 0

gadget x87_tangent
	x87_push_fault_check
	sub sp, sp, #48
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	add x3, sp, #32
	helper_call uml_x86_64_x87_tangent
	x87_leave_if_raised
	add x11, sp, #16
	x87_load_value x11, x9, w10
	mov w13, #0
	x87_store_stack w13, x9, w10
	add x11, sp, #32
	x87_load_value x11, x9, w10
	add sp, sp, #48
	/* C2: an operand too large to reduce, which stays as it was. */
	ldrh w11, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	tbnz w11, #10, Lx86_x87_tangent_unreduced
	x87_push x9, w10, 1
Lx86_x87_tangent_unreduced:
	retire_and_next

Lx86_x87_two_result_overflow:
	mov x9, #0xc000000000000000
	mov w10, #0xffff
	mov w13, #0
	x87_store_stack w13, x9, w10
	x87_push x9, w10
	retire_and_next


gadget x87_extract
	x87_push_fault_check
	sub sp, sp, #48
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	add x3, sp, #32
	helper_call uml_x86_64_x87_extract
	x87_leave_if_raised
	add x11, sp, #16
	x87_load_value x11, x9, w10
	mov w13, #0
	x87_store_stack w13, x9, w10
	add x11, sp, #32
	x87_load_value x11, x9, w10
	add sp, sp, #48
	x87_push x9, w10
	retire_and_next

gadget x87_sincos
	x87_push_fault_check
	sub sp, sp, #48
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	add x3, sp, #32
	helper_call uml_x86_64_x87_sincos
	x87_leave_if_raised
	add x11, sp, #16
	x87_load_value x11, x9, w10
	mov w13, #0
	x87_store_stack w13, x9, w10
	add x11, sp, #32
	x87_load_value x11, x9, w10
	add sp, sp, #48
	/* C2: an operand too large to reduce, which stays as it was. */
	ldrh w11, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	tbnz w11, #10, Lx86_x87_sincos_unreduced
	x87_push x9, w10, 1
Lx86_x87_sincos_unreduced:
	retire_and_next

/* FPREM and FPREM1, whose helper sets C0 to C3: C2 for a partial remainder,
 * else the quotient's low three bits in C0, C3 and C1. */
.macro x87_remainder name
	.globl CSYM(uml_x86_64_gadget_x87_\name\()_with_flags)
	.p2align 4
CSYM(uml_x86_64_gadget_x87_\name\()_with_flags):
	sub sp, sp, #48
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov w13, #1
	x87_load_stack w13, x9, w10
	add x11, sp, #16
	x87_store_value x11, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	add x3, sp, #32
	helper_call uml_x86_64_x87_\name
	x87_leave_if_raised
	add x11, sp, #32
	x87_load_value x11, x9, w10
	add sp, sp, #48
	mov w13, #0
	x87_store_stack w13, x9, w10
	retire_and_next
.endm

x87_remainder fprem
x87_remainder fprem1

.purgem x87_remainder
.purgem x87_two_input
.purgem x87_unary
.purgem x87_initialize_state
.purgem x87_push_fault_check
.purgem x87_bulk_store
.purgem x87_bulk_load
.purgem x87_scalar_store
.purgem x87_scalar_load
.purgem x87_set_integer_compare
.purgem x87_set_status_compare
.purgem x87_binary_call
.purgem x87_load_value
.purgem x87_store_value
.purgem x87_pop
.purgem x87_push
.purgem x87_store_stack
.purgem x87_store_physical
.purgem x87_set_tag
.purgem x87_load_stack
.purgem x87_stack_fault_check
.purgem x87_leave_if_raised
.purgem x87_physical
.purgem x87_load_u64
