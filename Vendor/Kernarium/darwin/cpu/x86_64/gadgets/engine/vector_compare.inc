/* SPDX-License-Identifier: GPL-2.0-or-later */
/* COMISS/UCOMISS and COMISD/UCOMISD architectural flags. COMIS compares with
 * FCMPE, which like it raises invalid operation for any NaN; UCOMIS with FCMP,
 * which raises it only for a signalling one. */

/* Every arithmetic flag is defined below, and the only overrides a gadget
 * records lie inside that same set, so a pending record has nothing left to
 * contribute: it is discarded instead of materializing a value this gadget
 * overwrites. That happens only as the flags are stored, so a fault before
 * them, or a NaN that could raise an unmasked invalid operation, leaves the
 * guest's flags as they were. */
.macro vector_compare_set_flags
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	orr x9, x9, #2
	b.vs Lx86_vector_compare_unordered\@
	b.eq Lx86_vector_compare_equal\@
	b.mi Lx86_vector_compare_less\@
	b Lx86_vector_compare_done\@
Lx86_vector_compare_unordered\@:
	simd_leave_if_checking
	orr x9, x9, #1
	orr x9, x9, #4
	orr x9, x9, #0x40
	b Lx86_vector_compare_done\@
Lx86_vector_compare_equal\@:
	orr x9, x9, #0x40
	b Lx86_vector_compare_done\@
Lx86_vector_compare_less\@:
	orr x9, x9, #1
Lx86_vector_compare_done\@:
	str xzr, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

/* MXCSR.DAZ compares a denormal input as zero. */
.macro vector_compare_inputs element
	ldr w12, [_vcpu, #UML_X86_64_CPU_MXCSR]
	tbz w12, #6, Lx86_vector_compare_inputs_ready\@
	simd_flush_denormals_each \element, 0, 1
Lx86_vector_compare_inputs_ready\@:
.endm

.macro vector_compare_register name, compare, width, fp_register
gadget vector_\name\()_\width\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ldr \fp_register\()0, [_vcpu, x9]
	ldr \fp_register\()1, [_vcpu, x10]
	vector_compare_inputs \width
	\compare \fp_register\()0, \fp_register\()1
	vector_compare_set_flags
.endm

.macro vector_compare_memory name, compare, width, fp_register, load, host_register
gadget vector_\name\()_\width\()_rm
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), Lx86_vector_compare_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	ldr \fp_register\()1, [x7]
	b Lx86_vector_compare_loaded\@
Lx86_vector_compare_cross\@:
	cross_page_load (\width / 8)
	fmov \fp_register\()1, \host_register\()0
Lx86_vector_compare_loaded\@:
	and x8, x23, #0xffff
	ldr \fp_register\()0, [_vcpu, x8]
	vector_compare_inputs \width
	\compare \fp_register\()0, \fp_register\()1
	vector_compare_set_flags
.endm

vector_compare_register compare, fcmp, 32, s
vector_compare_register compare, fcmp, 64, d
vector_compare_memory compare, fcmp, 32, s, ldr, w
vector_compare_memory compare, fcmp, 64, d, ldr, x
vector_compare_register compare_signalling, fcmpe, 32, s
vector_compare_register compare_signalling, fcmpe, 64, d
vector_compare_memory compare_signalling, fcmpe, 32, s, ldr, w
vector_compare_memory compare_signalling, fcmpe, 64, d, ldr, x

.purgem vector_compare_memory
.purgem vector_compare_register
.purgem vector_compare_set_flags
.purgem vector_compare_inputs
