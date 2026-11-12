/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar BMI1/BMI2 operations. */


.macro bmi_flags width, host_register
	/* CF, ZF, SF and OF are defined below and AF and PF are undefined, and the only overrides a gadget
	 * records lie inside that same set, so a pending record has nothing left
	 * to contribute: discard it instead of materializing a value this
	 * gadget immediately overwrites. */
	str xzr, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x9, #0x8c1
	bic x8, x8, x9
	cbz w19, Lx86_bmi_no_carry\@
	orr x8, x8, #1
Lx86_bmi_no_carry\@:
	cbnz \host_register\()27, Lx86_bmi_nonzero\@
	orr x8, x8, #0x40
Lx86_bmi_nonzero\@:
	tbz \host_register\()27, #(\width - 1), Lx86_bmi_not_sign\@
	orr x8, x8, #0x80
Lx86_bmi_not_sign\@:
	orr x8, x8, #0x2
	str x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
.endm

.macro bmi_unary_compute operation, width, host_register
	.ifc \operation,blsr
	sub \host_register\()8, \host_register\()24, #1
	and \host_register\()27, \host_register\()24, \host_register\()8
	cmp \host_register\()24, #0
	cset w19, eq
	.endif
	.ifc \operation,blsmsk
	sub \host_register\()8, \host_register\()24, #1
	eor \host_register\()27, \host_register\()24, \host_register\()8
	cmp \host_register\()24, #0
	cset w19, eq
	.endif
	.ifc \operation,blsi
	neg \host_register\()8, \host_register\()24
	and \host_register\()27, \host_register\()24, \host_register\()8
	cmp \host_register\()24, #0
	cset w19, ne
	.endif
.endm

.macro bmi_unary_register operation, width, load, host_register, store_register
gadget \operation\()_\width\()_rr
	ldr x8, [_stream], #8
	and x22, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_vcpu, x9]
	bmi_unary_compute \operation, \width, \host_register
	bmi_flags \width, \host_register
	str \store_register\()27, [_vcpu, x22]
	retire_and_next
.endm

.macro bmi_unary_memory operation, width, load, host_register, store_register
gadget \operation\()_\width\()_rm
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 24
	and x22, x23, #0xffff
	bmi_unary_compute \operation, \width, \host_register
	bmi_flags \width, \host_register
	str \store_register\()27, [_vcpu, x22]
	retire_and_next
.endm

.macro bmi_unary_width operation, width, load, host_register, store_register
	bmi_unary_register \operation, \width, \load, \host_register, \store_register
	bmi_unary_memory \operation, \width, \load, \host_register, \store_register
.endm

.macro bmi_unary_family operation
	bmi_unary_width \operation, 32, ldr, w, x
	bmi_unary_width \operation, 64, ldr, x, x
.endm

bmi_unary_family blsr
bmi_unary_family blsmsk
bmi_unary_family blsi

.purgem bmi_unary_family
.purgem bmi_unary_width
.purgem bmi_unary_memory
.purgem bmi_unary_register
.purgem bmi_unary_compute

/* x24 is the first VEX source/control and x26 is the r/m source. x22 is the
 * architectural destination offset and x25 is the first-register offset,
 * which is also MULX's low destination.
 */
.macro bmi_ternary_compute operation, width, host_register, store_register
	.ifc \operation,andn
	bic \host_register\()27, \host_register\()26, \host_register\()24
	mov w19, wzr
	bmi_flags \width, \host_register
	.endif

	.ifc \operation,bzhi
	and w8, w24, #0xff
	cmp w8, #\width
	b.hs Lx86_bzhi_large\@
	cbz w8, Lx86_bzhi_zero\@
	mov \host_register\()9, #1
	lslv \host_register\()9, \host_register\()9, \host_register\()8
	sub \host_register\()9, \host_register\()9, #1
	and \host_register\()27, \host_register\()26, \host_register\()9
	mov w19, wzr
	b Lx86_bzhi_ready\@
Lx86_bzhi_large\@:
	mov \host_register\()27, \host_register\()26
	mov w19, #1
	b Lx86_bzhi_ready\@
Lx86_bzhi_zero\@:
	eor \host_register\()27, \host_register\()27, \host_register\()27
	mov w19, wzr
Lx86_bzhi_ready\@:
	bmi_flags \width, \host_register
	.endif

	.ifc \operation,bextr
	and w8, w24, #0xff
	ubfx w9, w24, #8, #8
	cmp w8, #\width
	b.hs Lx86_bextr_zero\@
	cbz w9, Lx86_bextr_zero\@
	lsrv \host_register\()27, \host_register\()26, \host_register\()8
	mov w10, #\width
	sub w10, w10, w8
	cmp w9, w10
	b.hs Lx86_bextr_ready\@
	mov \host_register\()10, #1
	lslv \host_register\()10, \host_register\()10, \host_register\()9
	sub \host_register\()10, \host_register\()10, #1
	and \host_register\()27, \host_register\()27, \host_register\()10
	b Lx86_bextr_ready\@
Lx86_bextr_zero\@:
	eor \host_register\()27, \host_register\()27, \host_register\()27
Lx86_bextr_ready\@:
	mov w19, wzr
	bmi_flags \width, \host_register
	.endif

	.ifc \operation,shlx
	and w8, w24, #(\width - 1)
	lslv \host_register\()27, \host_register\()26, \host_register\()8
	.endif
	.ifc \operation,sarx
	and w8, w24, #(\width - 1)
	asrv \host_register\()27, \host_register\()26, \host_register\()8
	.endif
	.ifc \operation,shrx
	and w8, w24, #(\width - 1)
	lsrv \host_register\()27, \host_register\()26, \host_register\()8
	.endif

	.ifc \operation,pdep
	eor \host_register\()27, \host_register\()27, \host_register\()27
	mov \host_register\()8, #1
	mov \host_register\()9, #1
Lx86_pdep_loop\@:
	tst \host_register\()26, \host_register\()9
	b.eq Lx86_pdep_next\@
	tst \host_register\()24, \host_register\()8
	b.eq Lx86_pdep_source_clear\@
	orr \host_register\()27, \host_register\()27, \host_register\()9
Lx86_pdep_source_clear\@:
	lsl \host_register\()8, \host_register\()8, #1
Lx86_pdep_next\@:
	lsl \host_register\()9, \host_register\()9, #1
	cbnz \host_register\()9, Lx86_pdep_loop\@
	.endif

	.ifc \operation,pext
	eor \host_register\()27, \host_register\()27, \host_register\()27
	mov \host_register\()8, #1
	mov \host_register\()9, #1
Lx86_pext_loop\@:
	tst \host_register\()26, \host_register\()9
	b.eq Lx86_pext_next\@
	tst \host_register\()24, \host_register\()9
	b.eq Lx86_pext_source_clear\@
	orr \host_register\()27, \host_register\()27, \host_register\()8
Lx86_pext_source_clear\@:
	lsl \host_register\()8, \host_register\()8, #1
Lx86_pext_next\@:
	lsl \host_register\()9, \host_register\()9, #1
	cbnz \host_register\()9, Lx86_pext_loop\@
	.endif

	.ifc \operation,mulx
	.if \width == 32
	ldr w8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	umull x27, w8, w26
	mov w16, w27
	lsr x27, x27, #32
	mov w27, w27
	.else
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	mul x16, x8, x26
	umulh x27, x8, x26
	.endif
	str \store_register\()16, [_vcpu, x25]
	.endif
.endm

.macro bmi_ternary_register operation, width, load, host_register, store_register
gadget \operation\()_\width\()_rrr
	ldr x8, [_stream], #8
	and x22, x8, #0xffff
	ubfx x25, x8, #16, #16
	ubfx x9, x8, #32, #16
	\load \host_register\()24, [_vcpu, x25]
	\load \host_register\()26, [_vcpu, x9]
	bmi_ternary_compute \operation, \width, \host_register, \store_register
	str \store_register\()27, [_vcpu, x22]
	retire_and_next
.endm

.macro bmi_ternary_memory operation, width, load, host_register, store_register
gadget \operation\()_\width\()_rrm
	effective_address
	mov x27, x7
	ldr x25, [_stream], #8
	page_cross_check (\width / 8), Lx86_bmi_ternary_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()26, [x7]
	b Lx86_bmi_ternary_loaded\@
Lx86_bmi_ternary_read_cross\@:
	mov x0, _context
	mov x1, x27
	mov w2, #(\width / 8)
	helper_call uml_x86_64_gadget_cross_page_load
	cbz x1, Lx86_memory_fault
	mov x26, x0
Lx86_bmi_ternary_loaded\@:
	and x22, x23, #0xffff
	\load \host_register\()24, [_vcpu, x25]
	bmi_ternary_compute \operation, \width, \host_register, \store_register
	str \store_register\()27, [_vcpu, x22]
	retire_and_next
.endm

.macro bmi_ternary_width operation, width, load, host_register, store_register
	bmi_ternary_register \operation, \width, \load, \host_register, \store_register
	bmi_ternary_memory \operation, \width, \load, \host_register, \store_register
.endm

.macro bmi_ternary_family operation
	bmi_ternary_width \operation, 32, ldr, w, x
	bmi_ternary_width \operation, 64, ldr, x, x
.endm

bmi_ternary_family andn
bmi_ternary_family bzhi
bmi_ternary_family bextr
bmi_ternary_family shlx
bmi_ternary_family sarx
bmi_ternary_family shrx
bmi_ternary_family pdep
bmi_ternary_family pext
bmi_ternary_family mulx

.purgem bmi_ternary_family
.purgem bmi_ternary_width
.purgem bmi_ternary_memory
.purgem bmi_ternary_register
.purgem bmi_ternary_compute

.macro rorx_register width, load, host_register, store_register
gadget rorx_\width\()_rri
	ldp x8, x26, [_stream], #16
	and x22, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_vcpu, x9]
	and w26, w26, #(\width - 1)
	rorv \host_register\()27, \host_register\()24, \host_register\()26
	str \store_register\()27, [_vcpu, x22]
	retire_and_next
.endm

.macro rorx_memory width, load, host_register, store_register
gadget rorx_\width\()_rmi
	effective_address
	mov x25, x7
	ldr x26, [_stream], #8
	read_operand (\width / 8), \load, \host_register, 24
	and x22, x23, #0xffff
	and w26, w26, #(\width - 1)
	rorv \host_register\()27, \host_register\()24, \host_register\()26
	str \store_register\()27, [_vcpu, x22]
	retire_and_next
.endm

rorx_register 32, ldr, w, x
rorx_register 64, ldr, x, x
rorx_memory 32, ldr, w, x
rorx_memory 64, ldr, x, x

.purgem rorx_memory
.purgem rorx_register
.purgem bmi_flags
