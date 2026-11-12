/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact 80-bit arithmetic with native x87 stack and flag handling. */

.macro x87_register_binary opcode, group, helper, left_index, reverse, pop
gadget x87_register_\opcode\()_\group
	ldr w24, [_stream], #8
	sub sp, sp, #48
	.if \left_index
	x87_load_stack w24, x9, w10
	x87_store_value sp, x9, w10
	mov w13, #0
	x87_load_stack w13, x9, w10
	add x11, sp, #16
	x87_store_value x11, x9, w10
	.else
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	x87_load_stack w24, x9, w10
	add x11, sp, #16
	x87_store_value x11, x9, w10
	.endif
	x87_binary_call \helper, \reverse
	.if \left_index
	x87_store_stack w24, x9, w10
	.else
	mov w13, #0
	x87_store_stack w13, x9, w10
	.endif
	.if \pop
	x87_pop
	.endif
	retire_and_next
.endm

x87_register_binary d8, 0, add, 0, 0, 0
x87_register_binary d8, 1, multiply, 0, 0, 0
x87_register_binary d8, 4, subtract, 0, 0, 0
x87_register_binary d8, 5, subtract, 0, 1, 0
x87_register_binary d8, 6, divide, 0, 0, 0
x87_register_binary d8, 7, divide, 0, 1, 0
x87_register_binary dc, 0, add, 1, 0, 0
x87_register_binary dc, 1, multiply, 1, 0, 0
x87_register_binary dc, 4, subtract, 1, 1, 0
x87_register_binary dc, 5, subtract, 1, 0, 0
x87_register_binary dc, 6, divide, 1, 1, 0
x87_register_binary dc, 7, divide, 1, 0, 0
x87_register_binary de, 0, add, 1, 0, 1
x87_register_binary de, 1, multiply, 1, 0, 1
x87_register_binary de, 4, subtract, 1, 1, 1
x87_register_binary de, 5, subtract, 1, 0, 1
x87_register_binary de, 6, divide, 1, 1, 1
x87_register_binary de, 7, divide, 1, 0, 1

.macro x87_compare_register name, integer, pops
gadget x87_compare_\name
	ldr w24, [_stream], #8
	.if \integer
	bl uml_x86_64_gadget_materialize_flags
	.endif
	sub sp, sp, #32
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	x87_load_stack w24, x9, w10
	add x11, sp, #16
	x87_store_value x11, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	/* Bit 8 of the register word marks FUCOM and FUCOMI; bit 1 of the
	 * helper's flags FCOMI and FUCOMI, which keep C1. */
	lsr w3, w24, #8
	.if \integer
	orr w3, w3, #2
	.endif
	helper_call uml_x86_64_x87_compare
	x87_leave_if_raised
	add sp, sp, #32
	.if \integer
	x87_set_integer_compare w0
	.else
	x87_set_status_compare w0
	.endif
	.rept \pops
	x87_pop
	.endr
	retire_and_next
.endm

x87_compare_register status_keep, 0, 0
x87_compare_register status_pop, 0, 1
x87_compare_register status_pop2, 0, 2
x87_compare_register integer_keep, 1, 0
x87_compare_register integer_pop, 1, 1

/* FCMOVcc: an empty ST0 or ST(i) is a stack fault whichever way the
 * condition goes, and masked, leaves the QNaN indefinite in ST0. */
.macro x87_conditional_move condition
gadget x87_conditional_move_\condition
	ldr w24, [_stream], #8
	bl uml_x86_64_gadget_materialize_flags
	x87_load_stack w24, x11, w12, empty=Lx86_x87_conditional_fault\@
	mov w13, #0
	x87_load_stack w13, x9, w10, empty=Lx86_x87_conditional_destination_fault\@
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	.if \condition == 0
	tbz x9, #0, Lx86_x87_conditional_done\@
	.elseif \condition == 1
	tbz x9, #6, Lx86_x87_conditional_done\@
	.elseif \condition == 2
	tbnz x9, #0, Lx86_x87_conditional_take\@
	tbz x9, #6, Lx86_x87_conditional_done\@
	.elseif \condition == 3
	tbz x9, #2, Lx86_x87_conditional_done\@
	.elseif \condition == 4
	tbnz x9, #0, Lx86_x87_conditional_done\@
	.elseif \condition == 5
	tbnz x9, #6, Lx86_x87_conditional_done\@
	.elseif \condition == 6
	tbnz x9, #0, Lx86_x87_conditional_done\@
	tbnz x9, #6, Lx86_x87_conditional_done\@
	.else
	tbnz x9, #2, Lx86_x87_conditional_done\@
	.endif
Lx86_x87_conditional_take\@:
	mov w13, #0
	x87_store_stack w13, x11, w12
Lx86_x87_conditional_done\@:
	retire_and_next
Lx86_x87_conditional_fault\@:
	/* ST(i) is empty, so ST0 takes the indefinite it read as. */
	mov w13, #0
	x87_store_stack w13, x11, w12
	retire_and_next
Lx86_x87_conditional_destination_fault\@:
	mov w13, #0
	x87_store_stack w13, x9, w10
	retire_and_next
.endm

.irp condition, 0, 1, 2, 3, 4, 5, 6, 7
x87_conditional_move \condition
.endr

gadget x87_test_zero
	sub sp, sp, #32
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	stp xzr, xzr, [sp, #16]
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	mov w3, #0
	helper_call uml_x86_64_x87_compare
	x87_leave_if_raised
	add sp, sp, #32
	x87_set_status_compare w0
	retire_and_next

.macro x87_memory_convert size, helper, signed16, has_cpu, output
	.if \has_cpu
	mov x0, _vcpu
		.if \size == 4
	mov w1, w9
		.else
	mov x1, x9
		.endif
	mov x2, \output
	.else
		.if \signed16
	sxth w0, w9
		.elseif \size == 4
	mov w0, w9
		.else
	mov x0, x9
		.endif
	mov x1, \output
	.endif
	helper_call uml_x86_64_x87_\helper
	.if \has_cpu
	x87_leave_if_raised
	.endif
.endm

.macro x87_memory_binary type, size, converter, signed16, has_cpu, name, helper, reverse
gadget x87_memory_\type\()_\name
	effective_address
	x87_scalar_load \size, x9
	sub sp, sp, #48
	mov x19, sp
	add x11, sp, #16
	x87_memory_convert \size, \converter, \signed16, \has_cpu, x11
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value x19, x9, w10
	x87_binary_call \helper, \reverse
	mov w13, #0
	x87_store_stack w13, x9, w10
	retire_and_next
.endm

.macro x87_memory_binary_family type, size, converter, signed16, has_cpu
	x87_memory_binary \type, \size, \converter, \signed16, \has_cpu, add, add, 0
	x87_memory_binary \type, \size, \converter, \signed16, \has_cpu, multiply, multiply, 0
	x87_memory_binary \type, \size, \converter, \signed16, \has_cpu, subtract, subtract, 0
	x87_memory_binary \type, \size, \converter, \signed16, \has_cpu, subtract_reverse, subtract, 1
	x87_memory_binary \type, \size, \converter, \signed16, \has_cpu, divide, divide, 0
	x87_memory_binary \type, \size, \converter, \signed16, \has_cpu, divide_reverse, divide, 1
.endm

x87_memory_binary_family f32, 4, from_f32, 0, 1
x87_memory_binary_family i32, 4, from_i32, 0, 0
x87_memory_binary_family f64, 8, from_f64, 0, 1
x87_memory_binary_family i16, 2, from_i32, 1, 0

.macro x87_memory_compare type, size, converter, signed16, has_cpu, pop
gadget x87_memory_\type\()_compare_\pop
	effective_address
	x87_scalar_load \size, x9
	sub sp, sp, #32
	mov x19, sp
	add x11, sp, #16
	x87_memory_convert \size, \converter, \signed16, \has_cpu, x11
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value x19, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	mov w3, #0
	helper_call uml_x86_64_x87_compare
	x87_leave_if_raised
	add sp, sp, #32
	x87_set_status_compare w0
	.if \pop
	x87_pop
	.endif
	retire_and_next
.endm

.macro x87_memory_compare_family type, size, converter, signed16, has_cpu
	x87_memory_compare \type, \size, \converter, \signed16, \has_cpu, 0
	x87_memory_compare \type, \size, \converter, \signed16, \has_cpu, 1
.endm

x87_memory_compare_family f32, 4, from_f32, 0, 1
x87_memory_compare_family i32, 4, from_i32, 0, 0
x87_memory_compare_family f64, 8, from_f64, 0, 1
x87_memory_compare_family i16, 2, from_i32, 1, 0

.purgem x87_memory_compare_family
.purgem x87_memory_compare
.purgem x87_memory_binary_family
.purgem x87_memory_binary
.purgem x87_memory_convert
.purgem x87_conditional_move
.purgem x87_compare_register
.purgem x87_register_binary
