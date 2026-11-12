/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar-width vector transfers and MMX architectural synchronization. */

.macro vector_scalar_store width, load, host_register
gadget vector_move_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()24, [_vcpu, x8]
	page_cross_check (\width / 8), Lx86_vector_scalar_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	.if \width == 32
	str w24, [x7]
	.else
	str x24, [x7]
	.endif
	finish_store (\width / 8)
Lx86_vector_scalar_store_cross\@:
	cross_page_store x24, (\width / 8)
	finish_store (\width / 8)
.endm

vector_scalar_store 32, ldr, w
vector_scalar_store 64, ldr, x

.purgem vector_scalar_store

gadget vector_move_64_128_rm
	effective_address
	mov x25, x7
	page_cross_check 8, Lx86_vector_move_64_128_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr d0, [x7]
	b Lx86_vector_move_64_128_write
Lx86_vector_move_64_128_cross:
	cross_page_load 8
	fmov d0, x0
Lx86_vector_move_64_128_write:
	and x8, x23, #0xffff
	add x8, _vcpu, x8
	str q0, [x8]
	retire_and_next

gadget vector_move_64_128_rm_vex
	effective_address
	mov x25, x7
	page_cross_check 8, Lx86_vector_move_64_128_vex_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr d0, [x7]
	b Lx86_vector_move_64_128_vex_write
Lx86_vector_move_64_128_vex_cross:
	cross_page_load 8
	fmov d0, x0
Lx86_vector_move_64_128_vex_write:
	and x8, x23, #0xffff
	add x8, _vcpu, x8
	str q0, [x8]
	movi v1.16b, #0
	str q1, [x8, #16]
	retire_and_next

gadget vector_move_64_mmx_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ldr x11, [_vcpu, x10]
	str x11, [_vcpu, x9]
	synchronize_mmx x9, x11, x12, x13, w14
	retire_and_next

gadget vector_move_64_mmx_rm
	effective_address
	mov x25, x7
	read_operand 8, ldr, x, 24
	and x9, x23, #0xffff
	str x24, [_vcpu, x9]
	synchronize_mmx x9, x24, x12, x13, w14
	retire_and_next

.macro vector_gpr_to_xmm width, load, host_register, vex
gadget vector_gpr_to_xmm_\width\()\vex
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_vcpu, x10]
	add x9, _vcpu, x9
	movi v0.16b, #0
	str q0, [x9]
	.if \width == 32
	str w11, [x9]
	.else
	str x11, [x9]
	.endif
	.ifc \vex,_vex
	str q0, [x9, #16]
	.endif
	retire_and_next
.endm

vector_gpr_to_xmm 32, ldr, w,
vector_gpr_to_xmm 64, ldr, x,
vector_gpr_to_xmm 32, ldr, w, _vex
vector_gpr_to_xmm 64, ldr, x, _vex

.purgem vector_gpr_to_xmm

.macro vector_gpr_to_mmx width, load, host_register
gadget vector_gpr_to_mmx_\width
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_vcpu, x10]
	.if \width == 32
	mov w11, w11
	.endif
	str x11, [_vcpu, x9]
	synchronize_mmx x9, x11, x12, x13, w14
	retire_and_next
.endm

vector_gpr_to_mmx 32, ldr, w
vector_gpr_to_mmx 64, ldr, x

.purgem vector_gpr_to_mmx

gadget vector_memory_32_to_mmx
	effective_address
	mov x25, x7
	read_operand 4, ldr, w, 24, w
	and x9, x23, #0xffff
	str x24, [_vcpu, x9]
	synchronize_mmx x9, x24, x12, x13, w14
	retire_and_next

.macro vector_scalar_register width, load, store, host_register, vex
gadget vector_scalar_move_\width\()_rr\vex
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_vcpu, x10]
	.ifc \vex,_vex
	ubfx x12, x8, #32, #16
	add x12, _vcpu, x12
	ldr q0, [x12]
	add x9, _vcpu, x9
	str q0, [x9]
	\store \host_register\()11, [x9]
	movi v1.16b, #0
	str q1, [x9, #16]
	.else
	\store \host_register\()11, [_vcpu, x9]
	.endif
	retire_and_next
.endm

vector_scalar_register 32, ldr, str, w,
vector_scalar_register 64, ldr, str, x,
vector_scalar_register 32, ldr, str, w, _vex
vector_scalar_register 64, ldr, str, x, _vex

.purgem vector_scalar_register

.macro vector_scalar_load width, load, host_register, vex
gadget vector_scalar_move_\width\()_rm\vex
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 24
	and x8, x23, #0xffff
	add x8, _vcpu, x8
	movi v0.16b, #0
	str q0, [x8]
	.if \width == 32
	str w24, [x8]
	.else
	str x24, [x8]
	.endif
	.ifc \vex,_vex
	str q0, [x8, #16]
	.endif
	retire_and_next
.endm

vector_scalar_load 32, ldr, w,
vector_scalar_load 64, ldr, x,
vector_scalar_load 32, ldr, w, _vex
vector_scalar_load 64, ldr, x, _vex

.purgem vector_scalar_load
