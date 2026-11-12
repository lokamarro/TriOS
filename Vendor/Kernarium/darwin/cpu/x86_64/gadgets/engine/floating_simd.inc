/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native x86 floating-point and SIMD gadgets. */

.macro vector_move_rr name, width, zero_upper
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	.if \width == 16
	ldr q0, [x10]
	str q0, [x9]
	.if \zero_upper
	movi v1.16b, #0
	str q1, [x9, #16]
	.endif
	.else
	ldp q0, q1, [x10]
	stp q0, q1, [x9]
	.endif
	retire_and_next
.endm

vector_move_rr vector_move_128_rr, 16, 0
vector_move_rr vector_move_128_rr_vex, 16, 1
vector_move_rr vector_move_256_rr, 32, 0

.purgem vector_move_rr

.macro vector_move_64_128_rr name, zero_upper
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	ldr d0, [x10]
	str q0, [x9]
	.if \zero_upper
	movi v1.16b, #0
	str q1, [x9, #16]
	.endif
	retire_and_next
.endm

vector_move_64_128_rr vector_move_64_128_rr, 0
vector_move_64_128_rr vector_move_64_128_rr_vex, 1

.purgem vector_move_64_128_rr

.macro vector_move_load name, width, zero_upper
gadget \name
	effective_address
	mov x25, x7
	page_cross_check (\width), Lx86_vector_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \width
	.if \width == 16
	ldr q0, [x7]
	.else
	ldp q0, q1, [x7]
	.endif
Lx86_vector_load_write\@:
	and x8, x23, #0xffff
	add x8, _vcpu, x8
	.if \width == 16
	str q0, [x8]
	.if \zero_upper
	movi v1.16b, #0
	str q1, [x8, #16]
	.endif
	.else
	stp q0, q1, [x8]
	.endif
	retire_and_next
Lx86_vector_load_cross\@:
	mov x0, _context
	mov x1, x25
	and x8, x23, #0xffff
	add x2, _vcpu, x8
	mov w3, #\width
	helper_call uml_x86_64_gadget_cross_page_vector_load
	cbz w0, Lx86_memory_fault
	.if \zero_upper
	and x8, x23, #0xffff
	add x8, _vcpu, x8
	movi v1.16b, #0
	str q1, [x8, #16]
	.endif
	retire_and_next
.endm

vector_move_load vector_move_128_rm, 16, 0
vector_move_load vector_move_128_rm_vex, 16, 1
vector_move_load vector_move_256_rm, 32, 0

.purgem vector_move_load

.macro vector_move_store name, width
gadget \name
	effective_address
	mov x25, x7
	page_cross_check (\width), Lx86_vector_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, \width
	and x8, x23, #0xffff
	add x8, _vcpu, x8
	.if \width == 16
	ldr q0, [x8]
	str q0, [x7]
	.else
	ldp q0, q1, [x8]
	stp q0, q1, [x7]
	.endif
	finish_store \width
Lx86_vector_store_cross\@:
	mov x0, _context
	mov x1, x25
	and x8, x23, #0xffff
	add x2, _vcpu, x8
	mov w3, #\width
	helper_call uml_x86_64_gadget_cross_page_vector_store
	cbz w0, Lx86_memory_fault
	finish_store \width
.endm

vector_move_store vector_move_128_mr, 16
vector_move_store vector_move_256_mr, 32

.purgem vector_move_store

.macro zero_vector_upper index
	str q0, [_vcpu, #(UML_X86_64_CPU_VECTORS + \index * 32 + 16)]
.endm

.macro zero_vector index
	str q0, [_vcpu, #(UML_X86_64_CPU_VECTORS + \index * 32)]
	zero_vector_upper \index
.endm

gadget vector_zero_upper
	movi v0.16b, #0
	zero_vector_upper 0
	zero_vector_upper 1
	zero_vector_upper 2
	zero_vector_upper 3
	zero_vector_upper 4
	zero_vector_upper 5
	zero_vector_upper 6
	zero_vector_upper 7
	zero_vector_upper 8
	zero_vector_upper 9
	zero_vector_upper 10
	zero_vector_upper 11
	zero_vector_upper 12
	zero_vector_upper 13
	zero_vector_upper 14
	zero_vector_upper 15
	retire_and_next

gadget vector_zero_all
	movi v0.16b, #0
	zero_vector 0
	zero_vector 1
	zero_vector 2
	zero_vector 3
	zero_vector 4
	zero_vector 5
	zero_vector 6
	zero_vector 7
	zero_vector 8
	zero_vector 9
	zero_vector 10
	zero_vector 11
	zero_vector 12
	zero_vector 13
	zero_vector 14
	zero_vector 15
	retire_and_next

.purgem zero_vector
.purgem zero_vector_upper

.macro vector_bitwise_compute operation, destination, left, right
	.ifc \operation,and
	and \destination\().16b, \left\().16b, \right\().16b
	.endif
	.ifc \operation,andnot
	bic \destination\().16b, \right\().16b, \left\().16b
	.endif
	.ifc \operation,or
	orr \destination\().16b, \left\().16b, \right\().16b
	.endif
	.ifc \operation,xor
	eor \destination\().16b, \left\().16b, \right\().16b
	.endif
.endm

.macro vector_bitwise_rr name, operation, width, zero_upper
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x10]
	ldr q2, [x11]
	vector_bitwise_compute \operation, v0, v0, v2
	str q0, [x9]
	.if \width == 32
	ldr q1, [x10, #16]
	ldr q3, [x11, #16]
	vector_bitwise_compute \operation, v1, v1, v3
	str q1, [x9, #16]
	.elseif \zero_upper
	movi v1.16b, #0
	str q1, [x9, #16]
	.endif
	retire_and_next
.endm

.macro vector_bitwise_register_family operation
	vector_bitwise_rr vector_\operation\()_128_rr, \operation, 16, 0
	vector_bitwise_rr vector_\operation\()_128_rr_vex, \operation, 16, 1
	vector_bitwise_rr vector_\operation\()_256_rr, \operation, 32, 0
.endm

vector_bitwise_register_family and
vector_bitwise_register_family andnot
vector_bitwise_register_family or
vector_bitwise_register_family xor

.purgem vector_bitwise_register_family
.purgem vector_bitwise_rr

.macro vector_bitwise_rm name, operation, width, zero_upper
gadget \name
	effective_address
	mov x25, x7
	ldr x24, [_stream], #8
	page_cross_check (\width), Lx86_vector_bitwise_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \width
	ldr q2, [x7]
	.if \width == 32
	ldr q3, [x7, #16]
	.endif
	b Lx86_vector_bitwise_compute\@
Lx86_vector_bitwise_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #\width
	helper_call uml_x86_64_gadget_cross_page_vector_load
	cbnz w0, Lx86_vector_bitwise_cross_loaded\@
	add sp, sp, #32
	b Lx86_memory_fault
Lx86_vector_bitwise_cross_loaded\@:
	ldp q2, q3, [sp]
	add sp, sp, #32
Lx86_vector_bitwise_compute\@:
	and x8, x23, #0xffff
	and x9, x24, #0xffff
	add x8, _vcpu, x8
	add x9, _vcpu, x9
	ldr q0, [x9]
	vector_bitwise_compute \operation, v0, v0, v2
	str q0, [x8]
	.if \width == 32
	ldr q1, [x9, #16]
	vector_bitwise_compute \operation, v1, v1, v3
	str q1, [x8, #16]
	.elseif \zero_upper
	movi v1.16b, #0
	str q1, [x8, #16]
	.endif
	retire_and_next
.endm

.macro vector_bitwise_memory_family operation
	vector_bitwise_rm vector_\operation\()_128_rm, \operation, 16, 0
	vector_bitwise_rm vector_\operation\()_128_rm_vex, \operation, 16, 1
	vector_bitwise_rm vector_\operation\()_256_rm, \operation, 32, 0
.endm

vector_bitwise_memory_family and
vector_bitwise_memory_family andnot
vector_bitwise_memory_family or
vector_bitwise_memory_family xor

.purgem vector_bitwise_memory_family
.purgem vector_bitwise_rm
.purgem vector_bitwise_compute
