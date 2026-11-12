/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FXSAVE/FXRSTOR and the x87/SSE/AVX subset of XSAVE/XRSTOR. */

.set Lx86_extended_legacy_size, 512
.set Lx86_extended_header, 512
.set Lx86_extended_ymm_high, 576
.set Lx86_extended_size, 832

.macro extended_state_load size
	x86_marks_guest_memory
	sub sp, sp, #Lx86_extended_size
	mov x19, sp
	mov x0, _context
	mov x1, x25
	mov x2, x19
	mov w3, #\size
	helper_call uml_x86_64_gadget_bulk_load
	cbnz w0, Lx86_extended_load_done\@
	add sp, sp, #Lx86_extended_size
	b Lx86_memory_fault
Lx86_extended_load_done\@:
.endm

/* XSAVE and XRSTOR touch the legacy area and the header, and the AVX
 * component only when RFBM selects it, so an area that ends before the AVX
 * component, as the SSE-only layout does, is accessed in full. x24 holds RFBM
 * and the macros leave the extent in w26. */
.macro extended_state_extent
	and w26, w24, #4
	lsl w26, w26, #6
	add w26, w26, #Lx86_extended_ymm_high
.endm

.macro extended_state_load_extent
	x86_marks_guest_memory
	extended_state_extent
	sub sp, sp, #Lx86_extended_size
	mov x19, sp
	mov x0, _context
	mov x1, x25
	mov x2, x19
	mov w3, w26
	helper_call uml_x86_64_gadget_bulk_load
	cbnz w0, Lx86_extended_load_done\@
	add sp, sp, #Lx86_extended_size
	b Lx86_memory_fault
Lx86_extended_load_done\@:
.endm

.macro extended_state_requested
	ldr w24, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr w9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 16)]
	bfi x24, x9, #32, #32
	and x24, x24, #7
.endm

.macro extended_state_save_x87
	tbz x24, #0, Lx86_extended_save_x87_done\@
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	strh w9, [x19]
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #0x3800
	ldrb w10, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	and w10, w10, #7
	orr w9, w9, w10, lsl #11
	strh w9, [x19, #2]

	/* Convert the full two-bit tag word to FXSAVE's physical bitmap. */
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	mov w10, #0
	mov w11, #0
Lx86_extended_save_tag_loop\@:
	and w12, w9, #3
	cmp w12, #3
	cset w12, ne
	lsl w12, w12, w11
	orr w10, w10, w12
	lsr w9, w9, #2
	add w11, w11, #1
	cmp w11, #8
	b.lo Lx86_extended_save_tag_loop\@
	strb w10, [x19, #4]
	strh wzr, [x19, #6]
	str xzr, [x19, #8]
	str xzr, [x19, #16]

	/* FXSAVE stores ST(i) in logical stack order, not physical order. */
	add x10, _vcpu, #UML_X86_64_CPU_X87
	add x11, x19, #32
	ldrb w13, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	mov w12, #0
Lx86_extended_save_x87_loop\@:
	add w14, w13, w12
	and w14, w14, #7
	add x15, x10, x14, lsl #4
	ldr x16, [x15]
	ldrh w17, [x15, #8]
	str x16, [x11]
	strh w17, [x11, #8]
	add x11, x11, #16
	add w12, w12, #1
	cmp w12, #8
	b.lo Lx86_extended_save_x87_loop\@
Lx86_extended_save_x87_done\@:
.endm

/* MXCSR and MXCSR_MASK belong to the SSE and the AVX components alike: XSAVE
 * stores them when RFBM selects either one. */
.macro extended_state_save_mxcsr
	tst x24, #6
	b.eq Lx86_extended_save_mxcsr_done\@
	ldr w9, [_vcpu, #UML_X86_64_CPU_MXCSR]
	fold_host_flags w9, 10, 11
	str w9, [_vcpu, #UML_X86_64_CPU_MXCSR]
	str w9, [x19, #24]
	mov w9, #0xffff
	str w9, [x19, #28]
Lx86_extended_save_mxcsr_done\@:
.endm

.macro extended_state_save_sse
	tbz x24, #1, Lx86_extended_save_sse_done\@
	add x10, _vcpu, #UML_X86_64_CPU_VECTORS
	add x11, x19, #160
	mov w12, #16
Lx86_extended_save_sse_loop\@:
	ldr q0, [x10], #32
	str q0, [x11], #16
	subs w12, w12, #1
	b.ne Lx86_extended_save_sse_loop\@
Lx86_extended_save_sse_done\@:
.endm

.macro extended_state_save_avx
	tbz x24, #2, Lx86_extended_save_avx_done\@
	add x10, _vcpu, #(UML_X86_64_CPU_VECTORS + 16)
	add x11, x19, #Lx86_extended_ymm_high
	mov w12, #16
Lx86_extended_save_avx_loop\@:
	ldr q0, [x10], #32
	str q0, [x11], #16
	subs w12, w12, #1
	b.ne Lx86_extended_save_avx_loop\@
Lx86_extended_save_avx_done\@:
.endm

.macro extended_state_write_extent
	x86_marks_guest_memory
	extended_state_extent
	mov x0, _context
	mov x1, x25
	mov x2, x19
	mov w3, w26
	helper_call uml_x86_64_gadget_bulk_store
	cbnz w0, Lx86_extended_write_done\@
	add sp, sp, #Lx86_extended_size
	b Lx86_memory_fault
Lx86_extended_write_done\@:
	tbnz x23, #60, Lx86_extended_published\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, Lx86_extended_published\@
	mov x0, _context
	mov x1, x25
	mov w2, w26
	helper_call uml_x86_64_gadget_publish_code_write
Lx86_extended_published\@:
	add sp, sp, #Lx86_extended_size
	instruction_rip x8, 1
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro extended_state_write size
	x86_marks_guest_memory
	mov x0, _context
	mov x1, x25
	mov x2, x19
	mov w3, #\size
	helper_call uml_x86_64_gadget_bulk_store
	cbnz w0, Lx86_extended_write_done\@
	add sp, sp, #Lx86_extended_size
	b Lx86_memory_fault
Lx86_extended_write_done\@:
	publish_pending_code x25, \size
	add sp, sp, #Lx86_extended_size
	instruction_rip x8, 1
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro extended_state_restore_x87
	tbz x24, #0, Lx86_extended_restore_x87_done\@
	tbnz x25, #0, Lx86_extended_restore_x87_present\@
	mov w9, #0x037f
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	strh wzr, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	mov w9, #0xffff
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	add x10, _vcpu, #UML_X86_64_CPU_MMX
	mov w11, #8
Lx86_extended_clear_mmx_loop\@:
	str xzr, [x10], #8
	subs w11, w11, #1
	b.ne Lx86_extended_clear_mmx_loop\@
	add x10, _vcpu, #UML_X86_64_CPU_X87
	mov w11, #8
Lx86_extended_clear_x87_loop\@:
	stp xzr, xzr, [x10], #16
	subs w11, w11, #1
	b.ne Lx86_extended_clear_x87_loop\@
	b Lx86_extended_restore_x87_done\@

Lx86_extended_restore_x87_present\@:
	ldrh w9, [x19]
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	ldrh w9, [x19, #2]
	ubfx w13, w9, #11, #3
	strb w13, [_vcpu, #UML_X86_64_CPU_X87_TOP]
	bic w9, w9, #0x3800
	strh w9, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	ldrb w26, [x19, #4]
	mov w27, #0xffff
	add x10, _vcpu, #UML_X86_64_CPU_X87
	add x11, x19, #32
	mov w12, #0
Lx86_extended_restore_x87_loop\@:
	add w14, w13, w12
	and w14, w14, #7
	add x15, x10, x14, lsl #4
	ldr x16, [x11]
	ldrh w17, [x11, #8]
	str x16, [x15]
	strh w17, [x15, #8]
	add x15, _vcpu, #UML_X86_64_CPU_MMX
	str x16, [x15, x14, lsl #3]
	lsrv w9, w26, w14
	tbz w9, #0, Lx86_extended_restore_x87_next\@

	/* Reconstruct the two-bit tag from the restored 80-bit payload. */
	and w9, w17, #0x7fff
	mov w17, #2
	cbz w9, Lx86_extended_restore_x87_zero_exp\@
	mov w15, #0x7fff
	cmp w9, w15
	b.eq Lx86_extended_restore_x87_classified\@
	tbz x16, #63, Lx86_extended_restore_x87_classified\@
	mov w17, #0
	b Lx86_extended_restore_x87_classified\@
Lx86_extended_restore_x87_zero_exp\@:
	cbnz x16, Lx86_extended_restore_x87_classified\@
	mov w17, #1
Lx86_extended_restore_x87_classified\@:
	lsl w9, w14, #1
	mov w15, #3
	lsl w15, w15, w9
	bic w27, w27, w15
	lsl w17, w17, w9
	orr w27, w27, w17
Lx86_extended_restore_x87_next\@:
	add x11, x11, #16
	add w12, w12, #1
	cmp w12, #8
	b.lo Lx86_extended_restore_x87_loop\@
	strh w27, [_vcpu, #UML_X86_64_CPU_X87_TAG]
Lx86_extended_restore_x87_done\@:
.endm

/* XRSTOR and FXRSTOR check the image's MXCSR against MXCSR_MASK, 0xffff,
 * before they change any state, whenever RFBM selects the SSE or the AVX
 * component, and raise #GP(0) for a reserved bit; XSTATE_BV does not matter. */
.macro extended_state_check_mxcsr
	tst x24, #6
	b.eq Lx86_extended_check_mxcsr_done\@
	ldr w9, [x19, #24]
	tst w9, #0xffff0000
	b.ne Lx86_extended_restore_invalid
Lx86_extended_check_mxcsr_done\@:
.endm

/* The same condition loads MXCSR from the image, XSTATE_BV or not. */
.macro extended_state_restore_mxcsr
	tst x24, #6
	b.eq Lx86_extended_restore_mxcsr_done\@
	ldr w9, [x19, #24]
	str w9, [_vcpu, #UML_X86_64_CPU_MXCSR]
	msr fpsr, xzr
	refresh_simd_exact w9, 10, 11
	publish_guest_rounding 9, 10, 11
Lx86_extended_restore_mxcsr_done\@:
.endm

.macro extended_state_restore_sse
	tbz x24, #1, Lx86_extended_restore_sse_done\@
	tbnz x25, #1, Lx86_extended_restore_sse_present\@
	add x10, _vcpu, #UML_X86_64_CPU_VECTORS
	movi v0.16b, #0
	mov w11, #16
Lx86_extended_clear_sse_loop\@:
	str q0, [x10], #32
	subs w11, w11, #1
	b.ne Lx86_extended_clear_sse_loop\@
	b Lx86_extended_restore_sse_done\@
Lx86_extended_restore_sse_present\@:
	add x10, _vcpu, #UML_X86_64_CPU_VECTORS
	add x11, x19, #160
	mov w12, #16
Lx86_extended_restore_sse_loop\@:
	ldr q0, [x11], #16
	str q0, [x10], #32
	subs w12, w12, #1
	b.ne Lx86_extended_restore_sse_loop\@
Lx86_extended_restore_sse_done\@:
.endm

.macro extended_state_restore_avx
	tbz x24, #2, Lx86_extended_restore_avx_done\@
	tbnz x25, #2, Lx86_extended_restore_avx_present\@
	add x10, _vcpu, #(UML_X86_64_CPU_VECTORS + 16)
	movi v0.16b, #0
	mov w11, #16
Lx86_extended_clear_avx_loop\@:
	str q0, [x10], #32
	subs w11, w11, #1
	b.ne Lx86_extended_clear_avx_loop\@
	b Lx86_extended_restore_avx_done\@
Lx86_extended_restore_avx_present\@:
	add x10, _vcpu, #(UML_X86_64_CPU_VECTORS + 16)
	add x11, x19, #Lx86_extended_ymm_high
	mov w12, #16
Lx86_extended_restore_avx_loop\@:
	ldr q0, [x11], #16
	str q0, [x10], #32
	subs w12, w12, #1
	b.ne Lx86_extended_restore_avx_loop\@
Lx86_extended_restore_avx_done\@:
.endm

/* FXSAVE and FXRSTOR require their image aligned to 16 bytes, XSAVE and
 * XRSTOR to 64, and raise #GP(0) at the instruction for one that is not,
 * before any access. effective_address leaves the instruction's stream word
 * in x23, which the #GP exit finds its address from. */
.macro extended_state_aligned mask
	tst x7, #\mask
	b.ne Lx86_extended_misaligned
.endm

gadget extended_state_fxsave
	effective_address
	extended_state_aligned 15
	mov x25, x7
	extended_state_load Lx86_extended_legacy_size
	mov x24, #7
	extended_state_save_x87
	extended_state_save_mxcsr
	extended_state_save_sse
	extended_state_write Lx86_extended_legacy_size

gadget extended_state_fxrstor
	effective_address
	extended_state_aligned 15
	mov x25, x7
	extended_state_load Lx86_extended_legacy_size
	mov x24, #7
	mov x25, #7
	extended_state_check_mxcsr
	extended_state_restore_x87
	extended_state_restore_mxcsr
	extended_state_restore_sse
	add sp, sp, #Lx86_extended_size
	retire_unless_any_unchecked

/* XSAVE sets the XSTATE_BV bits RFBM selects, for the components it stores,
 * and leaves the rest of the header as it found it. */
gadget extended_state_xsave
	effective_address
	extended_state_aligned 63
	mov x25, x7
	extended_state_requested
	extended_state_load_extent
	ldr x9, [x19, #Lx86_extended_header]
	orr x9, x9, x24
	str x9, [x19, #Lx86_extended_header]
	extended_state_save_x87
	extended_state_save_mxcsr
	extended_state_save_sse
	extended_state_save_avx
	extended_state_write_extent

gadget extended_state_xrstor
	effective_address
	extended_state_aligned 63
	mov x25, x7
	extended_state_requested
	extended_state_load_extent
	ldr x25, [x19, #Lx86_extended_header]
	and x25, x25, #7
	extended_state_check_mxcsr
	extended_state_restore_x87
	extended_state_restore_mxcsr
	extended_state_restore_sse
	extended_state_restore_avx
	add sp, sp, #Lx86_extended_size
	retire_unless_any_unchecked

/* The image is loaded and nothing has changed: free it and fault. */
Lx86_extended_restore_invalid:
	add sp, sp, #Lx86_extended_size
	b Lx86_general_protection

Lx86_extended_misaligned:
	b Lx86_general_protection

.purgem extended_state_aligned
.purgem extended_state_restore_avx
.purgem extended_state_restore_sse
.purgem extended_state_restore_mxcsr
.purgem extended_state_check_mxcsr
.purgem extended_state_restore_x87
.purgem extended_state_write
.purgem extended_state_save_avx
.purgem extended_state_save_sse
.purgem extended_state_save_mxcsr
.purgem extended_state_save_x87
.purgem extended_state_requested
.purgem extended_state_load
.purgem extended_state_write_extent
.purgem extended_state_load_extent
.purgem extended_state_extent
