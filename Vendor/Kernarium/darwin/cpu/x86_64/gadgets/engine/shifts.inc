/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar shifts and rotates. */


/* x24 is the normalized input, w26 is a nonzero masked count. The rotate
 * count is reduced here because arm64 shifts do not wrap within byte and word
 * operands. x86 masks 64-bit through-carry rotates to 63 just like the other
 * 64-bit rotate forms.
 */
.macro shift_prepare_count operation, width
	.ifc \operation,rol
	and w26, w26, #(\width - 1)
	.endif
	.ifc \operation,ror
	and w26, w26, #(\width - 1)
	.endif
	.ifc \operation,rcl
	.if \width == 64
	and w26, w26, #63
	.else
	mov w8, #(\width + 1)
	udiv w9, w26, w8
	msub w26, w9, w8, w26
	.endif
	.endif
	.ifc \operation,rcr
	.if \width == 64
	and w26, w26, #63
	.else
	mov w8, #(\width + 1)
	udiv w9, w26, w8
	msub w26, w9, w8, w26
	.endif
	.endif
.endm

/* Compute the result in x27 and the last shifted-out bit in w19. */
.macro shift_compute operation, width, host_register
	/* SHL, SHR and SAR defer their flags: the record keeps the original
	 * value, the count and the result, and the materializer derives the
	 * shifted-out bit from them (shift_flags_body). */
	.ifc \operation,shl
	lslv \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	.endif

	.ifc \operation,shr
	lsrv \host_register\()27, \host_register\()24, \host_register\()26
	normalize_result \width, \host_register\()27
	.endif

	.ifc \operation,sar
	.if \width == 8
	sxtb w27, w24
	.elseif \width == 16
	sxth w27, w24
	.elseif \width == 32
	mov w27, w24
	.else
	mov x27, x24
	.endif
	asrv \host_register\()27, \host_register\()27, \host_register\()26
	normalize_result \width, \host_register\()27
	.endif

	.ifc \operation,rol
	mov \host_register\()8, #\width
	sub \host_register\()8, \host_register\()8, \host_register\()26
	lslv \host_register\()27, \host_register\()24, \host_register\()26
	lsrv \host_register\()9, \host_register\()24, \host_register\()8
	orr \host_register\()27, \host_register\()27, \host_register\()9
	normalize_result \width, \host_register\()27
	and w19, w27, #1
	.endif

	.ifc \operation,ror
	.if \width == 32
	rorv w27, w24, w26
	.elseif \width == 64
	rorv x27, x24, x26
	.else
	mov \host_register\()8, #\width
	sub \host_register\()8, \host_register\()8, \host_register\()26
	lsrv \host_register\()27, \host_register\()24, \host_register\()26
	lslv \host_register\()9, \host_register\()24, \host_register\()8
	orr \host_register\()27, \host_register\()27, \host_register\()9
	.endif
	normalize_result \width, \host_register\()27
	lsr \host_register\()19, \host_register\()27, #(\width - 1)
	and w19, w19, #1
	.endif

	.ifc \operation,rcl
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	and w19, w8, #1
	mov w16, w26
	mov \host_register\()27, \host_register\()24
Lx86_rotate_carry_loop\@:
	lsr \host_register\()8, \host_register\()27, #(\width - 1)
	lsl \host_register\()27, \host_register\()27, #1
	orr \host_register\()27, \host_register\()27, \host_register\()19
	normalize_result \width, \host_register\()27
	and w19, w8, #1
	subs w16, w16, #1
	b.ne Lx86_rotate_carry_loop\@
	.endif

	.ifc \operation,rcr
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	and w19, w8, #1
	mov w16, w26
	mov \host_register\()27, \host_register\()24
Lx86_rotate_carry_loop\@:
	and w8, w27, #1
	lsr \host_register\()27, \host_register\()27, #1
	lsl \host_register\()9, \host_register\()19, #(\width - 1)
	orr \host_register\()27, \host_register\()27, \host_register\()9
	normalize_result \width, \host_register\()27
	mov w19, w8
	subs w16, w16, #1
	b.ne Lx86_rotate_carry_loop\@
	.endif
.endm

.macro shift_set_szp width, host_register
	mov x9, #0xc4
	bic x8, x8, x9
	cbnz \host_register\()27, Lx86_shift_nonzero\@
	orr x8, x8, #0x40
Lx86_shift_nonzero\@:
	tbz \host_register\()27, #(\width - 1), Lx86_shift_not_sign\@
	orr x8, x8, #0x80
Lx86_shift_not_sign\@:
	and w10, w27, #0xff
	eor w10, w10, w10, lsr #4
	eor w10, w10, w10, lsr #2
	eor w10, w10, w10, lsr #1
	tbnz w10, #0, Lx86_shift_odd_parity\@
	orr x8, x8, #0x4
Lx86_shift_odd_parity\@:
.endm

/* The count in w26 is the effective count. ROL/ROR use width as a marker
 * after a nonzero input count wraps to zero, so CF changes while OF remains
 * untouched. A zero count in the other paths means that no flags change.
 */
.macro shift_update_flags operation, width, host_register
	ldr x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	bic x8, x8, #1
	orr x8, x8, x19
	.ifc \operation,shl
	shift_set_szp \width, \host_register
	.endif
	.ifc \operation,shr
	shift_set_szp \width, \host_register
	.endif
	.ifc \operation,sar
	shift_set_szp \width, \host_register
	.endif
	.ifc \operation,shld
	shift_set_szp \width, \host_register
	.endif
	.ifc \operation,shrd
	shift_set_szp \width, \host_register
	.endif
	cmp w26, #1
	b.ne Lx86_shift_flags_done\@
	bic x8, x8, #0x800
	.ifc \operation,shl
	lsr \host_register\()9, \host_register\()27, #(\width - 1)
	eor w9, w9, w19
	.endif
	.ifc \operation,shr
	lsr \host_register\()9, \host_register\()24, #(\width - 1)
	.endif
	.ifc \operation,sar
	mov w9, wzr
	.endif
	.ifc \operation,shld
	lsr \host_register\()9, \host_register\()27, #(\width - 1)
	eor w9, w9, w19
	.endif
	.ifc \operation,shrd
	lsr \host_register\()9, \host_register\()24, #(\width - 1)
	lsr \host_register\()10, \host_register\()27, #(\width - 1)
	eor w9, w9, w10
	.endif
	.ifc \operation,rol
	lsr \host_register\()9, \host_register\()27, #(\width - 1)
	eor w9, w9, w19
	.endif
	.ifc \operation,ror
	lsr \host_register\()9, \host_register\()27, #(\width - 1)
	lsr \host_register\()10, \host_register\()27, #(\width - 2)
	eor w9, w9, w10
	.endif
	.ifc \operation,rcl
	lsr \host_register\()9, \host_register\()27, #(\width - 1)
	eor w9, w9, w19
	.endif
	.ifc \operation,rcr
	lsr \host_register\()9, \host_register\()27, #(\width - 1)
	lsr \host_register\()10, \host_register\()27, #(\width - 2)
	eor w9, w9, w10
	.endif
	tbz w9, #0, Lx86_shift_flags_done\@
	orr x8, x8, #0x800
Lx86_shift_flags_done\@:
	orr x8, x8, #0x2
	str x8, [_vcpu, #UML_X86_64_CPU_RFLAGS]
.endm

/* Shifts and rotates usually feed another arithmetic operation before any
 * flag consumer. Preserve the prior lazy flag producer and overlay only the
 * flags this instruction defines; a later full arithmetic producer can then
 * discard both without materializing either one.
 */
.macro shift_record_flags operation, width, host_register
	.ifc \operation,shl
	record_deferred_from 8, \width, x24, x26, x27
	.exitm
	.endif
	.ifc \operation,shr
	record_deferred_from 9, \width, x24, x26, x27
	.exitm
	.endif
	.ifc \operation,sar
	record_deferred_from 10, \width, x24, x26, x27
	.exitm
	.endif
	.ifc \operation,rol
	mov w8, #1
	and w9, w19, #1
	.else
	.ifc \operation,ror
	mov w8, #1
	and w9, w19, #1
	.else
	mov w8, #0xc5
	and w9, w19, #1
	cbnz \host_register\()27, Lx86_shift_override_nonzero\@
	orr w9, w9, #0x40
Lx86_shift_override_nonzero\@:
	tbz \host_register\()27, #(\width - 1), Lx86_shift_override_not_sign\@
	orr w9, w9, #0x80
Lx86_shift_override_not_sign\@:
	and w10, w27, #0xff
	eor w10, w10, w10, lsr #4
	eor w10, w10, w10, lsr #2
	eor w10, w10, w10, lsr #1
	tbnz w10, #0, Lx86_shift_override_szp_done\@
	orr w9, w9, #0x4
Lx86_shift_override_szp_done\@:
	.endif
	.endif
	cmp w26, #1
	b.ne Lx86_shift_override_ready\@
	orr w8, w8, #0x800
	.ifc \operation,shl
	lsr \host_register\()10, \host_register\()27, #(\width - 1)
	eor w10, w10, w19
	.endif
	.ifc \operation,shr
	lsr \host_register\()10, \host_register\()24, #(\width - 1)
	.endif
	.ifc \operation,sar
	mov w10, wzr
	.endif
	.ifc \operation,shld
	lsr \host_register\()10, \host_register\()27, #(\width - 1)
	eor w10, w10, w19
	.endif
	.ifc \operation,shrd
	lsr \host_register\()10, \host_register\()24, #(\width - 1)
	lsr \host_register\()12, \host_register\()27, #(\width - 1)
	eor w10, w10, w12
	.endif
	.ifc \operation,rol
	lsr \host_register\()10, \host_register\()27, #(\width - 1)
	eor w10, w10, w19
	.endif
	.ifc \operation,ror
	lsr \host_register\()10, \host_register\()27, #(\width - 1)
	lsr \host_register\()12, \host_register\()27, #(\width - 2)
	eor w10, w10, w12
	.endif
	tbz w10, #0, Lx86_shift_override_ready\@
	orr w9, w9, #0x800
Lx86_shift_override_ready\@:
	ldrh w10, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	ldrh w11, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic w11, w11, w8
	orr w11, w11, w9
	orr w10, w10, w8
	strh w10, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	strh w11, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
.endm

/* The part of a shift or rotate of the value in `host_register`24 by the count
 * in w26 that its register and memory forms share: mask the count and compute
 * the result into `host_register`27. A zero count branches to `store`, which
 * keeps the value, and the result is ready at `write`; both are labels this
 * defines. */
.macro shift_apply operation, width, host_register, store, write
	.if \width == 64
	and w26, w26, #0x3f
	.else
	and w26, w26, #0x1f
	.endif
	cbz w26, \store
	.ifc \operation,rcl
	bl uml_x86_64_gadget_materialize_flags
	.endif
	.ifc \operation,rcr
	bl uml_x86_64_gadget_materialize_flags
	.endif
	shift_prepare_count \operation, \width
	.ifc \operation,rcl
	cbz w26, \store
	.endif
	.ifc \operation,rcr
	cbz w26, \store
	.endif
	shift_compute \operation, \width, \host_register
	.ifc \operation,rol
	cbnz w26, \write
	mov w26, #\width
	.endif
	.ifc \operation,ror
	cbnz w26, \write
	mov w26, #\width
	.endif
	b \write
\store:
	mov \host_register\()27, \host_register\()24
\write:
.endm

/* Record the flags a shift or rotate defines. */
.macro shift_flags operation, width, host_register
	.ifc \operation,rcl
	shift_update_flags \operation, \width, \host_register
	.else
	.ifc \operation,rcr
	shift_update_flags \operation, \width, \host_register
	.else
	shift_record_flags \operation, \width, \host_register
	.endif
	.endif
.endm

.macro shift_register operation, width, load, store, host_register, store_register, form
gadget \operation\()_\width\()_r\form
	.ifc \form,i
	ldp x25, x26, [_stream], #16
	.else
	ldr x8, [_stream], #8
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	ldrb w26, [_vcpu, x9]
	.endif
	\load \host_register\()24, [_vcpu, x25]
	shift_apply \operation, \width, \host_register, Lx86_shift_register_store\@, Lx86_shift_register_write\@
	\store \store_register\()27, [_vcpu, x25]
	cbz w26, Lx86_shift_register_done\@
	shift_flags \operation, \width, \host_register
Lx86_shift_register_done\@:
	retire_and_next
.endm

.macro shift_memory operation, width, load, store, host_register, form
gadget \operation\()_\width\()_m\form
	effective_address
	mov x25, x7
	.ifc \form,r
	and x8, x23, #0xffff
	ldrb w26, [_vcpu, x8]
	.else
	ldr x26, [_stream], #8
	.endif
	read_operand (\width / 8), \load, \host_register, 24
	shift_apply \operation, \width, \host_register, Lx86_shift_memory_store\@, Lx86_shift_memory_write\@
	mov x7, x25
	page_cross_check (\width / 8), Lx86_shift_memory_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_shift_memory_written\@
Lx86_shift_memory_write_cross\@:
	cross_page_store x27, (\width / 8)
Lx86_shift_memory_written\@:
	cbz w26, Lx86_shift_memory_done\@
	shift_flags \operation, \width, \host_register
Lx86_shift_memory_done\@:
	finish_store (\width / 8)
.endm

.macro shift_width operation, width, load, store, host_register, store_register
	shift_register \operation, \width, \load, \store, \host_register, \store_register, r
	shift_register \operation, \width, \load, \store, \host_register, \store_register, i
	shift_memory \operation, \width, \load, \store, \host_register, r
	shift_memory \operation, \width, \load, \store, \host_register, i
.endm

.macro shift_family operation
	shift_width \operation, 8, ldrb, strb, w, w
	shift_width \operation, 16, ldrh, strh, w, w
	shift_width \operation, 32, ldr, str, w, x
	shift_width \operation, 64, ldr, str, x, x
.endm

shift_family shl
shift_family shr
shift_family sar
shift_family rol
shift_family ror
shift_family rcl
shift_family rcr

/* SHLD/SHRD use x24 for the destination, x15 for the source, w26 for the
 * masked count, x27 for the result, and w19 for CF. Only a 16-bit operation
 * can have a masked count larger than its operand width.
 */
.macro double_shift_compute operation, width, host_register
	cmp w26, #\width
	b.hi Lx86_double_shift_large_count\@
	mov \host_register\()8, #\width
	sub \host_register\()8, \host_register\()8, \host_register\()26
	.ifc \operation,shld
	lslv \host_register\()27, \host_register\()24, \host_register\()26
	lsrv \host_register\()9, \host_register\()15, \host_register\()8
	orr \host_register\()27, \host_register\()27, \host_register\()9
	lsrv \host_register\()19, \host_register\()24, \host_register\()8
	.endif
	.ifc \operation,shrd
	lsrv \host_register\()27, \host_register\()24, \host_register\()26
	lslv \host_register\()9, \host_register\()15, \host_register\()8
	orr \host_register\()27, \host_register\()27, \host_register\()9
	sub \host_register\()8, \host_register\()26, #1
	lsrv \host_register\()19, \host_register\()24, \host_register\()8
	.endif
	b Lx86_double_shift_computed\@
Lx86_double_shift_large_count\@:
	sub \host_register\()8, \host_register\()26, #\width
	.ifc \operation,shld
	lslv \host_register\()27, \host_register\()15, \host_register\()8
	mov \host_register\()9, #\width
	sub \host_register\()9, \host_register\()9, \host_register\()8
	lsrv \host_register\()19, \host_register\()15, \host_register\()9
	.endif
	.ifc \operation,shrd
	lsrv \host_register\()27, \host_register\()15, \host_register\()8
	sub \host_register\()8, \host_register\()8, #1
	lsrv \host_register\()19, \host_register\()15, \host_register\()8
	.endif
Lx86_double_shift_computed\@:
	normalize_result \width, \host_register\()27
	and w19, w19, #1
.endm

.macro double_shift_register operation, width, load, store, host_register, store_register, form
gadget \operation\()_\width\()_rr\form
	ldp x8, x26, [_stream], #16
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_vcpu, x25]
	\load \host_register\()15, [_vcpu, x9]
	.ifc \form,r
	ldrb w26, [_vcpu, x26]
	.endif
	.if \width == 64
	and w26, w26, #0x3f
	.else
	and w26, w26, #0x1f
	.endif
	cbz w26, Lx86_double_shift_register_zero\@
	double_shift_compute \operation, \width, \host_register
	\store \store_register\()27, [_vcpu, x25]
	shift_record_flags \operation, \width, \host_register
	retire_and_next
Lx86_double_shift_register_zero\@:
	\store \store_register\()24, [_vcpu, x25]
	retire_and_next
.endm

.macro double_shift_memory operation, width, load, store, host_register, form
gadget \operation\()_\width\()_mr\form
	effective_address
	mov x25, x7
	ldr x26, [_stream], #8
	read_operand (\width / 8), \load, \host_register, 24
	and x8, x23, #0xffff
	\load \host_register\()15, [_vcpu, x8]
	.ifc \form,r
	ldrb w26, [_vcpu, x26]
	.endif
	.if \width == 64
	and w26, w26, #0x3f
	.else
	and w26, w26, #0x1f
	.endif
	cbz w26, Lx86_double_shift_memory_zero\@
	double_shift_compute \operation, \width, \host_register
	mov x7, x25
	page_cross_check (\width / 8), Lx86_double_shift_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_double_shift_written\@
Lx86_double_shift_write_cross\@:
	cross_page_store x27, (\width / 8)
Lx86_double_shift_written\@:
	shift_record_flags \operation, \width, \host_register
	finish_store (\width / 8)
Lx86_double_shift_memory_zero\@:
	retire_and_next
.endm

.macro double_shift_width operation, width, load, store, host_register, store_register
	double_shift_register \operation, \width, \load, \store, \host_register, \store_register, i
	double_shift_register \operation, \width, \load, \store, \host_register, \store_register, r
	double_shift_memory \operation, \width, \load, \store, \host_register, i
	double_shift_memory \operation, \width, \load, \store, \host_register, r
.endm

.macro double_shift_family operation
	double_shift_width \operation, 16, ldrh, strh, w, w
	double_shift_width \operation, 32, ldr, str, w, x
	double_shift_width \operation, 64, ldr, str, x, x
.endm

double_shift_family shld
double_shift_family shrd

.purgem double_shift_family
.purgem double_shift_width
.purgem double_shift_memory
.purgem double_shift_register
.purgem double_shift_compute

.purgem shift_family
.purgem shift_width
.purgem shift_memory
.purgem shift_register
.purgem shift_flags
.purgem shift_apply
.purgem shift_record_flags
.purgem shift_update_flags
.purgem shift_set_szp
.purgem shift_compute
.purgem shift_prepare_count
