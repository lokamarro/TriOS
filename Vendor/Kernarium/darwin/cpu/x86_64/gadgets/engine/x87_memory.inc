/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x87 scalar conversion, packed state, and control-word memory forms. */

.macro x87_load_converted name, size, helper, signed16=0, has_cpu=1
gadget x87_load_\name
	effective_address
	x87_scalar_load \size, x9
	sub sp, sp, #16
	.if \has_cpu
	mov x0, _vcpu
		.if \size == 4
	mov w1, w9
		.else
	mov x1, x9
		.endif
	mov x2, sp
	.else
		.if \signed16
	sxth w0, w9
		.elseif \size == 4
	mov w0, w9
		.else
	mov x0, x9
		.endif
	mov x1, sp
	.endif
	helper_call uml_x86_64_x87_\helper
	.if \has_cpu
	x87_leave_if_raised
	.endif
	x87_load_value sp, x9, w10
	add sp, sp, #16
	x87_push x9, w10
	retire_and_next
.endm

x87_load_converted f32, 4, from_f32, 0, 1
x87_load_converted f64, 8, from_f64, 0, 1
x87_load_converted i16, 2, from_i32, 1, 0
x87_load_converted i32, 4, from_i32, 0, 0
x87_load_converted i64, 8, from_i64, 0, 0

gadget x87_load_f80
	effective_address
	mov x25, x7
	sub sp, sp, #16
	x87_bulk_load 10, sp, Lx86_x87_load_f80_fault
	x87_load_value sp, x9, w10
	add sp, sp, #16
	x87_push x9, w10
	retire_and_next
Lx86_x87_load_f80_fault:
	add sp, sp, #16
	b Lx86_memory_fault

gadget x87_load_bcd
	effective_address
	mov x25, x7
	sub sp, sp, #32
	x87_bulk_load 10, sp, Lx86_x87_load_bcd_fault
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	helper_call uml_x86_64_x87_from_bcd
	add x11, sp, #16
	x87_load_value x11, x9, w10
	add sp, sp, #32
	x87_push x9, w10
	retire_and_next
Lx86_x87_load_bcd_fault:
	add sp, sp, #32
	b Lx86_memory_fault

/* Store ST(0) to memory through the SoftFloat conversion `helper`. The integer
 * helpers take `truncate`, which FISTTP sets. */
.macro x87_store_memory name, size, helper, pop, truncate=
gadget x87_store_\name
	effective_address
	mov x25, x7
	sub sp, sp, #16
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _vcpu
	mov x1, sp
	.ifnb \truncate
	mov w2, #\truncate
	.endif
	helper_call uml_x86_64_x87_to_\helper
	x87_leave_if_raised
	mov x9, x0
	add sp, sp, #16
	x87_scalar_store \size, x9
	.if \pop
	x87_pop
	.endif
	finish_store \size
.endm

x87_store_memory f32_keep, 4, f32, 0
x87_store_memory f32_pop, 4, f32, 1
x87_store_memory f64_keep, 8, f64, 0
x87_store_memory f64_pop, 8, f64, 1

x87_store_memory i16_truncate_pop, 2, i16, 1, 1
x87_store_memory i16_keep, 2, i16, 0, 0
x87_store_memory i16_pop, 2, i16, 1, 0
x87_store_memory i32_truncate_pop, 4, i32, 1, 1
x87_store_memory i32_keep, 4, i32, 0, 0
x87_store_memory i32_pop, 4, i32, 1, 0
x87_store_memory i64_truncate_pop, 8, i64, 1, 1
x87_store_memory i64_pop, 8, i64, 1, 0

gadget x87_store_f80_pop
	effective_address
	mov x25, x7
	sub sp, sp, #16
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	x87_bulk_store 10, sp, Lx86_x87_store_f80_fault
	add sp, sp, #16
	x87_pop
	x87_clear_c1
	finish_store 10
Lx86_x87_store_f80_fault:
	add sp, sp, #16
	b Lx86_memory_fault

gadget x87_store_bcd_pop
	effective_address
	mov x25, x7
	sub sp, sp, #32
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _vcpu
	mov x1, sp
	add x2, sp, #16
	helper_call uml_x86_64_x87_to_bcd
	x87_leave_if_raised
	add x11, sp, #16
	x87_bulk_store 10, x11, Lx86_x87_store_bcd_fault
	add sp, sp, #32
	x87_pop
	finish_store 10
Lx86_x87_store_bcd_fault:
	add sp, sp, #32
	b Lx86_memory_fault

gadget x87_load_control
	effective_address
	x87_scalar_load 2, x9
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	retire_unless_x87_unchecked w9

gadget x87_store_control
	effective_address
	mov x25, x7
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	x87_scalar_store 2, x9
	finish_store 2

gadget x87_store_status
	effective_address
	mov x25, x7
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #0x3800
	ldrb w10, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	orr w9, w9, w10, lsl #11
	x87_scalar_store 2, x9
	finish_store 2

.macro x87_environment_restore name, environment_size, full
gadget x87_environment_restore_\name
	effective_address
	mov x25, x7
	.set Lx86_x87_environment_total\@, (\environment_size + (\full * 80))
	.if Lx86_x87_environment_total\@ <= 96
	.set Lx86_x87_environment_stack\@, 96
	.else
	.set Lx86_x87_environment_stack\@, 112
	.endif
	sub sp, sp, #Lx86_x87_environment_stack\@
	x87_bulk_load Lx86_x87_environment_total\@, sp, \
		Lx86_x87_environment_restore_fault\@
	ldrh w9, [sp]
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	.if \environment_size == 28
	ldrh w9, [sp, #4]
	ldrh w10, [sp, #8]
	.else
	ldrh w9, [sp, #2]
	ldrh w10, [sp, #4]
	.endif
	ubfx w11, w9, #11, #3
	strb w11, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	bic w9, w9, #0x3800
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	strh w10, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	.if \full
	/* The image holds ST(0) first: ST(i) is physical register TOP + i. */
	add x9, sp, #\environment_size
	mov w12, #0
Lx86_x87_environment_restore_loop\@:
	add w15, w11, w12
	and w15, w15, #7
	ldr x13, [x9]
	ldrh w14, [x9, #8]
	add x10, _vcpu, #UML_X86_64_CPU_X87
	add x10, x10, x15, lsl #4
	str x13, [x10]
	strh w14, [x10, #8]
	add x10, _vcpu, #UML_X86_64_CPU_MMX
	str x13, [x10, x15, lsl #3]
	add x9, x9, #10
	add w12, w12, #1
	cmp w12, #8
	b.lo Lx86_x87_environment_restore_loop\@
	.endif
	add sp, sp, #Lx86_x87_environment_stack\@
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	retire_unless_x87_unchecked w9
Lx86_x87_environment_restore_fault\@:
	add sp, sp, #Lx86_x87_environment_stack\@
	b Lx86_memory_fault
.endm

.macro x87_environment_save name, environment_size, full
gadget x87_environment_save_\name
	effective_address
	mov x25, x7
	.set Lx86_x87_environment_total\@, (\environment_size + (\full * 80))
	.if Lx86_x87_environment_total\@ <= 96
	.set Lx86_x87_environment_stack\@, 96
	.else
	.set Lx86_x87_environment_stack\@, 112
	.endif
	sub sp, sp, #Lx86_x87_environment_stack\@
	movi v0.16b, #0
	mov x9, sp
	.set Lx86_x87_environment_clear_count\@, \
		(Lx86_x87_environment_stack\@ / 16)
	mov w10, #Lx86_x87_environment_clear_count\@
Lx86_x87_environment_clear_loop\@:
	str q0, [x9], #16
	subs w10, w10, #1
	b.ne Lx86_x87_environment_clear_loop\@
	/* The image tags each register that is not empty by what it
	 * holds, which an MMX instruction's all-valid tags do not say. */
	mov x0, _vcpu
	helper_call uml_x86_64_x87_image_tags
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	strh w9, [sp]
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #0x3800
	ldrb w10, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	orr w9, w9, w10, lsl #11
	.if \environment_size == 28
	strh w9, [sp, #4]
	strh w0, [sp, #8]
	.else
	strh w9, [sp, #2]
	strh w0, [sp, #4]
	.endif
	.if \full
	/* ST(0) first: ST(i) is physical register TOP + i. */
	add x9, sp, #\environment_size
	mov w11, #0
Lx86_x87_environment_save_loop\@:
	add w14, w10, w11
	and w14, w14, #7
	add x15, _vcpu, #UML_X86_64_CPU_X87
	add x15, x15, x14, lsl #4
	ldr x12, [x15]
	ldrh w13, [x15, #8]
	str x12, [x9]
	strh w13, [x9, #8]
	add x9, x9, #10
	add w11, w11, #1
	cmp w11, #8
	b.lo Lx86_x87_environment_save_loop\@
	.endif
	x87_bulk_store Lx86_x87_environment_total\@, sp, \
		Lx86_x87_environment_save_fault\@
	add sp, sp, #Lx86_x87_environment_stack\@
	.if \full
	x87_initialize_state
	.else
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	orr w9, w9, #0x3f
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	.endif
	finish_store Lx86_x87_environment_total\@
Lx86_x87_environment_save_fault\@:
	add sp, sp, #Lx86_x87_environment_stack\@
	b Lx86_memory_fault
.endm

x87_environment_restore env14, 14, 0
x87_environment_restore env28, 28, 0
x87_environment_restore state94, 14, 1
x87_environment_restore state108, 28, 1
x87_environment_save env14, 14, 0
x87_environment_save env28, 28, 0
x87_environment_save state94, 14, 1
x87_environment_save state108, 28, 1

.purgem x87_environment_save
.purgem x87_environment_restore
.purgem x87_store_memory
.purgem x87_load_converted
