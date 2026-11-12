/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared x87 stack, tag, memory, and exact-math call machinery. */

.set Lx86_x87_tag_empty, 3
.set Lx86_x87_status_c0, 0x0100
.set Lx86_x87_status_c1, 0x0200
.set Lx86_x87_status_c2, 0x0400
.set Lx86_x87_status_c3, 0x4000

.macro x87_load_u64 destination, value
	movz \destination, #((\value) & 0xffff)
	movk \destination, #(((\value) >> 16) & 0xffff), lsl #16
	movk \destination, #(((\value) >> 32) & 0xffff), lsl #32
	movk \destination, #(((\value) >> 48) & 0xffff), lsl #48
.endm

/* While the guest has an x87 exception unmasked the runtime sets
 * `exception_checks`, and an instruction that raises one leaves before it
 * commits anything; the runtime runs it again, checked, which leaves the
 * exception pending for the next waiting instruction as x87 does. A helper
 * reports one in `exception_raised`, which its caller tests before it stores a
 * result: a load and a branch after each helper call, whether or not the guest
 * unmasks anything. */
.p2align 4
Lx86_x87_raised:
	b uml_x86_64_gadget_exception_check

.macro x87_leave_if_raised
	ldrb w9, [_vcpu, #UML_X86_64_CPU_EXCEPTION_RAISED]
	cbnz w9, Lx86_x87_raised
.endm

/* A stack fault is an invalid operation: an unmasked one leaves the same way,
 * before the fault touches the status word or the stack. */
.macro x87_stack_fault_check
	ldrb w17, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w17, #0, Lx86_x87_stack_fault_masked\@
	ldrh w17, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	tbnz w17, #0, Lx86_x87_stack_fault_masked\@
	b uml_x86_64_gadget_exception_check
Lx86_x87_stack_fault_masked\@:
.endm

.macro x87_physical logical, physical
	ldrb \physical, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	add \physical, \physical, \logical
	and \physical, \physical, #7
.endm

/* A load of an empty register is a stack underflow: it reads as the QNaN
 * indefinite. With `empty` the instruction goes on there with it loaded,
 * where the present value would have been changed. */
.macro x87_load_stack logical, significand, exponent, empty=
	x87_physical \logical, w14
	ldrh w15, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	lsl w16, w14, #1
	lsr w15, w15, w16
	and w15, w15, #3
	cmp w15, #Lx86_x87_tag_empty
	b.ne Lx86_x87_load_present\@
	x87_stack_fault_check
	ldrh w15, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	mov w17, #0x41
	orr w15, w15, w17
	strh w15, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	mov \significand, #0xc000000000000000
	mov \exponent, #0xffff
	.ifb \empty
	b Lx86_x87_load_done\@
	.else
	b \empty
	.endif
Lx86_x87_load_present\@:
	add x15, _vcpu, #UML_X86_64_CPU_X87
	add x15, x15, w14, uxtw #4
	ldr \significand, [x15]
	ldrh \exponent, [x15, #8]
Lx86_x87_load_done\@:
.endm

/* Write the low two bits of `tag` as `physical`'s tag; uses w15 and w16.
 * Rotating the entry to bit 0 and back needs no mask register. */
.macro x87_set_tag physical, tag
	ldrh w15, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	lsl w16, \physical, #1
	ror w15, w15, w16
	bfxil w15, \tag, #0, #2
	neg w16, w16
	ror w15, w15, w16
	strh w15, [_vcpu, #UML_X86_64_CPU_X87_TAG]
.endm

.macro x87_store_physical physical, significand, exponent
	add x15, _vcpu, #UML_X86_64_CPU_X87
	add x15, x15, \physical, uxtw #4
	str \significand, [x15]
	strh \exponent, [x15, #8]
	add x15, _vcpu, #UML_X86_64_CPU_MMX
	str \significand, [x15, \physical, uxtw #3]

	/* Recreate the x87 two-bit classification tag. */
	mov w17, #2
	and w15, \exponent, #0x7fff
	cbz w15, Lx86_x87_store_zero_exp\@
	mov w16, #0x7fff
	cmp w15, w16
	b.eq Lx86_x87_store_classified\@
	tbz \significand, #63, Lx86_x87_store_classified\@
	mov w17, #0
	b Lx86_x87_store_classified\@
Lx86_x87_store_zero_exp\@:
	cbnz \significand, Lx86_x87_store_classified\@
	mov w17, #1
Lx86_x87_store_classified\@:
	x87_set_tag \physical, w17
.endm

.macro x87_store_stack logical, significand, exponent
	x87_physical \logical, w14
	x87_store_physical w14, \significand, \exponent
.endm

/* An instruction that rounds nothing clears C1: a store to a register or
 * to an 80-bit operand, FCHS, FABS, FFREE and FFREEP, as the loads, FXCH,
 * the compares into the status word and the stack-pointer instructions
 * do. FCOMI, FUCOMI and their popping forms keep it. */
.macro x87_clear_c1
	ldrh w15, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w15, w15, #Lx86_x87_status_c1
	strh w15, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
.endm

/* A push clears C1 unless `keep_c1` says the value pushed is a rounded
 * result whose C1 the instruction reports, as FPTAN's and FSINCOS's are. */
.macro x87_push significand, exponent, keep_c1=0
	ldrh w15, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	.if \keep_c1 == 0
	bic w15, w15, #Lx86_x87_status_c1
	.endif
	ldrb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	sub w14, w14, #1
	and w14, w14, #7
	ldrh w16, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	lsl w17, w14, #1
	lsr w16, w16, w17
	and w16, w16, #3
	cmp w16, #Lx86_x87_tag_empty
	b.eq Lx86_x87_push_slot_ready\@
	/* A stack overflow, masked, pushes the QNaN indefinite. */
	x87_stack_fault_check
	mov w16, #0x241
	orr w15, w15, w16
	mov \significand, #0xc000000000000000
	mov \exponent, #0xffff
Lx86_x87_push_slot_ready\@:
	strh w15, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	strb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	x87_store_physical w14, \significand, \exponent
.endm

/* FPTAN, FSINCOS and FXTRACT check the register of their second result
 * first: on a full stack x86 computes nothing, and masked, leaves the QNaN
 * indefinite in ST0 and pushes another, raising only the stack overflow.
 * While the gadgets check x87 exceptions an unmasked one leaves here. */
.macro x87_push_fault_check
	ldrb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	sub w14, w14, #1
	and w14, w14, #7
	ldrh w16, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	lsl w17, w14, #1
	lsr w16, w16, w17
	and w16, w16, #3
	cmp w16, #Lx86_x87_tag_empty
	b.eq Lx86_x87_push_fault_none\@
	x87_stack_fault_check
	b Lx86_x87_two_result_overflow
Lx86_x87_push_fault_none\@:
.endm

.macro x87_pop
	ldrb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	mov w17, #Lx86_x87_tag_empty
	x87_set_tag w14, w17
	add w14, w14, #1
	and w14, w14, #7
	strb w14, [_vcpu, #UML_X86_64_CPU_X87_TOP]
.endm

.macro x87_store_value pointer, significand, exponent
	str \significand, [\pointer]
	strh \exponent, [\pointer, #8]
.endm

.macro x87_load_value pointer, significand, exponent
	ldr \significand, [\pointer]
	ldrh \exponent, [\pointer, #8]
.endm

/* Call the SoftFloat helper that combines the operands the caller stored at
 * sp and sp+16 and leaves its result at sp+32, `reverse` swapping them, then
 * load the result and release the 48-byte frame. */
.macro x87_binary_call helper, reverse
	mov x0, _vcpu
	.if \reverse
	add x1, sp, #16
	mov x2, sp
	.else
	mov x1, sp
	add x2, sp, #16
	.endif
	add x3, sp, #32
	helper_call uml_x86_64_x87_\helper
	x87_leave_if_raised
	add x11, sp, #32
	x87_load_value x11, x9, w10
	add sp, sp, #48
.endm

.macro x87_set_status_compare result
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	mov w10, #0x4700
	bic w9, w9, w10
	cmp \result, #1
	b.ne Lx86_x87_compare_not_less\@
	orr w9, w9, #Lx86_x87_status_c0
	b Lx86_x87_compare_done\@
Lx86_x87_compare_not_less\@:
	cmp \result, #2
	b.ne Lx86_x87_compare_not_equal\@
	orr w9, w9, #Lx86_x87_status_c3
	b Lx86_x87_compare_done\@
Lx86_x87_compare_not_equal\@:
	cmp \result, #3
	b.ne Lx86_x87_compare_done\@
	mov w10, #0x4500
	orr w9, w9, w10
Lx86_x87_compare_done\@:
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
.endm

.macro x87_set_integer_compare result
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	cmp \result, #1
	b.ne Lx86_x87_integer_not_less\@
	orr x9, x9, #0x1
	b Lx86_x87_integer_compare_done\@
Lx86_x87_integer_not_less\@:
	cmp \result, #2
	b.ne Lx86_x87_integer_not_equal\@
	orr x9, x9, #0x40
	b Lx86_x87_integer_compare_done\@
Lx86_x87_integer_not_equal\@:
	cmp \result, #3
	b.ne Lx86_x87_integer_compare_done\@
	mov x10, #0x45
	orr x9, x9, x10
Lx86_x87_integer_compare_done\@:
	orr x9, x9, #0x2
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
.endm

.macro x87_scalar_load size, destination
	page_cross_check (\size), Lx86_x87_scalar_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \size
	.if \size == 2
	ldrh w9, [x7]
	.elseif \size == 4
	ldr w9, [x7]
	.else
	ldr x9, [x7]
	.endif
	b Lx86_x87_scalar_load_done\@
Lx86_x87_scalar_load_cross\@:
	mov x0, _context
	mov x1, x7
	mov w2, #\size
	helper_call uml_x86_64_gadget_cross_page_load
	cbz x1, Lx86_memory_fault
	mov \destination, x0
Lx86_x87_scalar_load_done\@:
.endm

.macro x87_scalar_store size, source
	mov x24, \source
	mov x7, x25
	page_cross_check (\size), Lx86_x87_scalar_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, \size
	.if \size == 2
	strh w24, [x7]
	.elseif \size == 4
	str w24, [x7]
	.else
	str x24, [x7]
	.endif
	b Lx86_x87_scalar_store_done\@
Lx86_x87_scalar_store_cross\@:
	cross_page_store x24, \size
Lx86_x87_scalar_store_done\@:
.endm

.macro x87_bulk_load size, buffer, fault
	x86_marks_guest_memory
	mov x0, _context
	mov x1, x25
	mov x2, \buffer
	mov w3, #\size
	helper_call uml_x86_64_gadget_bulk_load
	cbz w0, \fault
.endm

.macro x87_bulk_store size, buffer, fault
	x86_marks_guest_memory
	mov x0, _context
	mov x1, x25
	mov x2, \buffer
	mov w3, #\size
	helper_call uml_x86_64_gadget_bulk_store
	cbz w0, \fault
.endm
