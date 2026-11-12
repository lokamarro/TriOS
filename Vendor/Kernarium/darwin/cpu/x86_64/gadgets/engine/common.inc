/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../layout/assembly_offsets.h"
#include "../../../gadget_host_frame.inc"

.arch armv8.2-a

#define CSYM(symbol) _##symbol

_vcpu .req x20
_context .req x21
_stream .req x28
_executed .req x29

	.set Lx86_guest_memory_gadget, 0
	.set Lx86_simd_status_gadget, 0

.macro gadget name
	.globl CSYM(uml_x86_64_gadget_\name)
	.p2align 4
CSYM(uml_x86_64_gadget_\name):
	.set Lx86_guest_memory_gadget, 0
	.set Lx86_simd_status_gadget, 0
.endm

/* A call from the engine to a helper outside it branches to the helper's
 * veneer, one branch in the island gadget_veneers emits, so no other word of
 * the engine encodes a distance to code that moves with the rest of the
 * image. A helper the island does not list is an undefined temporary label. */
.macro helper_call symbol
	bl Lveneer_\symbol
.endm

.macro veneer symbol
Lveneer_\symbol:
	b CSYM(\symbol)
.endm

/* The island, in a fixed order: a new helper is appended. */
.macro gadget_veneers
	veneer uml_x86_64_gadget_tlb_miss
	veneer uml_x86_64_gadget_cross_page_load
	veneer uml_x86_64_gadget_cross_page_store
	veneer uml_x86_64_gadget_cross_page_vector_load
	veneer uml_x86_64_gadget_cross_page_vector_store
	veneer uml_x86_64_gadget_bulk_load
	veneer uml_x86_64_gadget_bulk_store
	veneer uml_x86_64_gadget_publish_code_write
	veneer uml_x86_64_gadget_invalidate_code
	veneer uml_x86_64_gadget_code_link_safe
	veneer uml_x86_64_gadget_split_lock_enter
	veneer uml_x86_64_gadget_split_lock_leave
	veneer uml_x86_64_gadget_segment_limit
	veneer uml_x86_64_gadget_random_value
	veneer uml_x86_64_x87_add
	veneer uml_x86_64_x87_subtract
	veneer uml_x86_64_x87_multiply
	veneer uml_x86_64_x87_divide
	veneer uml_x86_64_x87_compare
	veneer uml_x86_64_x87_sqrt
	veneer uml_x86_64_x87_round
	veneer uml_x86_64_x87_scale
	veneer uml_x86_64_x87_extract
	veneer uml_x86_64_x87_fprem
	veneer uml_x86_64_x87_fprem1
	veneer uml_x86_64_x87_from_i32
	veneer uml_x86_64_x87_from_i64
	veneer uml_x86_64_x87_from_f32
	veneer uml_x86_64_x87_from_f64
	veneer uml_x86_64_x87_from_bcd
	veneer uml_x86_64_x87_to_i16
	veneer uml_x86_64_x87_to_i32
	veneer uml_x86_64_x87_to_i64
	veneer uml_x86_64_x87_to_f32
	veneer uml_x86_64_x87_to_f64
	veneer uml_x86_64_x87_to_bcd
	veneer uml_x86_64_x87_image_tags
	veneer uml_x86_64_x87_sine
	veneer uml_x86_64_x87_cosine
	veneer uml_x86_64_x87_sincos
	veneer uml_x86_64_x87_tangent
	veneer uml_x86_64_x87_atan
	veneer uml_x86_64_x87_f2xm1
	veneer uml_x86_64_x87_fyl2x
	veneer uml_x86_64_x87_fyl2xp1
	veneer uml_x86_64_gadget_run_segment_instruction
.endm

/* Publish the guest's rounding mode as the host's.
 *
 * x86 keeps it in MXCSR and the host in FPCR, and the two number the directed
 * modes the other way round, so the two-bit field is reversed. Writing FPCR
 * can cost a pipeline flush, so it is written only when it would change --
 * which, for the guests that never leave round-to-nearest, is never.
 * `scratch*` must be free at the call site.
 */
.macro publish_guest_rounding scratch0, scratch1, scratch2
	ldr w\scratch0, [_vcpu, #UML_X86_64_CPU_MXCSR]
	mrs x\scratch1, fpcr
	ubfx w\scratch0, w\scratch0, #13, #2
	rbit w\scratch2, w\scratch0
	lsr w\scratch2, w\scratch2, #30
	ubfx x\scratch0, x\scratch1, #22, #2
	cmp x\scratch0, x\scratch2
	b.eq Lx86_rounding_ready\@
	bfi x\scratch1, x\scratch2, #22, #2
	msr fpcr, x\scratch1
Lx86_rounding_ready\@:
.endm

.macro next_gadget
	ldr x8, [_stream], #8
	br x8
.endm

/* x86's MXCSR exception flags are sticky, and the host's cumulative FPSR
 * flags hold them while a run lasts: the SIMD floating-point gadgets
 * compute with host instructions whose IOC, DZC, OFC, UFC and IXC are
 * x86's IE, ZE, OE, UE and PE, and the run adds what they gathered to MXCSR
 * as it ends (program.cpp). An instruction that observes MXCSR adds them
 * first; one that loads it clears them. IDC is never DE: it reports an input
 * FPCR.FZ flushed, and the engine never sets FZ. Where an instruction's host
 * flags are not x86's, its gadget chooses another host instruction, keeps
 * FPSR around host work x86 does not do, or raises the missing flags in
 * software into MXCSR (`mxcsr_raise`), after any exception-check leave.
 *
 * Add FPSR's flags to the MXCSR value in `value`. Uses x`s0` and x`s1`. */
.macro fold_host_flags value, s0, s1
	mrs x\s0, fpsr
	and w\s1, w\s0, #1
	orr \value, \value, w\s1
	and w\s1, w\s0, #0x1e
	orr \value, \value, w\s1, lsl #1
.endm

/* Raise the MXCSR flags `bits`. Uses w`scratch`. */
.macro mxcsr_raise bits, scratch
	ldr w\scratch, [_vcpu, #UML_X86_64_CPU_MXCSR]
	orr w\scratch, w\scratch, #\bits
	str w\scratch, [_vcpu, #UML_X86_64_CPU_MXCSR]
.endm

/* Raise underflow and precision when a lane of v`mask` is set, as x86 does
 * for a tiny result MXCSR.FTZ flushes, exact or not. Uses v`temporary` and
 * x14. */
.macro mxcsr_raise_flushed mask, temporary
	umaxv s\temporary, v\mask\().4s
	fmov w14, s\temporary
	cbz w14, Lx86_mxcsr_flushed_none\@
	mxcsr_raise 0x30, 14
Lx86_mxcsr_flushed_none\@:
.endm

/* x86 promises total store order: the other CPUs see one CPU's stores in the
 * order it made them, and its loads see memory move forward in that same
 * order. Guest code written for x86 leans on that without saying so, and a
 * LOCK-prefixed instruction or an XCHG against memory is in addition a full
 * fence -- a work-stealing deque stores its tail, runs a locked instruction
 * and only then loads the head. The host orders nothing: it may run two
 * independent loads in either order, and an exclusive loop orders only its own
 * operand. With several vCPUs that is a task taken twice, a wild pointer or a
 * wakeup that never comes.
 *
 * A guest memory access is therefore followed by a barrier before the next
 * guest instruction can touch memory. `translate`, `stack_fast` and the few
 * private translation and bulk-helper macros mark the gadget they expand in,
 * and every retiring way out of a marked gadget carries the barrier, so a
 * gadget that works on registers alone pays nothing and a locked gadget is
 * fenced like any other store. The mark is textual: a memory form that joins
 * another gadget's tail runs the boundary before it branches. Accesses inside
 * one instruction keep the order the gadget gives them.
 */
.macro x86_memory_order_barrier
	dmb ish
.endm

.macro x86_marks_guest_memory
	.set Lx86_guest_memory_gadget, 1
.endm

.macro x86_memory_order_boundary
	.if Lx86_guest_memory_gadget
	x86_memory_order_barrier
	.endif
.endm

/* The address of the instruction whose stream word is in x23, or with
 * `length` the address of the one after it: the block's start, which the
 * context's program_begin names, plus the position in bits 45:36 (the baked
 * access word's is at `position`, 32) and the length in bits 59:56. A
 * compatibility-mode address wraps at 4 GiB. Cold exits only; x9 and x10 are
 * clobbered and `destination` is neither. */
.macro instruction_rip destination, length=0, position=UML_X86_64_STREAM_POSITION
	ubfx x9, x23, #\position, #UML_X86_64_STREAM_POSITION_BITS
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	add \destination, x10, x9
	.if \length
	ubfx x9, x23, #56, #4
	add \destination, \destination, x9
	.endif
	ldrb w9, [_vcpu, #UML_X86_64_CPU_EXECUTION_MODE]
	and x10, \destination, #0xffffffff
	cmp w9, #1
	csel \destination, x10, \destination, eq
.endm

.macro retire_and_next
	add _executed, _executed, #1
	x86_memory_order_boundary
	next_gadget
.endm

/* The engine leaves #MF for an x87 exception already pending, and #XM while
 * MXCSR unmasks underflow, precision or a denormal operand, to the runtime,
 * which runs the instructions that could raise them one at a time, checked;
 * the gadgets check other unmasked x87 and SIMD exceptions themselves while
 * the run's `exception_checks` says so. An instruction that loads MXCSR or
 * the x87 control word therefore retires and leaves when it could change what
 * a run checks, so the runtime sees the change before the next instruction
 * runs: when it unmasks an exception in a run that does not check that kind,
 * when it unmasks SIMD underflow, precision or a denormal operand, or when it
 * unmasks an x87 exception whose flag is set, which is then pending. For a gadget that computed its
 * effective address: the descriptor's position and length name the next
 * instruction. */
.macro retire_and_leave
	instruction_rip x8, 1
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

/* `value` is the MXCSR just loaded. */
.macro retire_unless_simd_unchecked value
	mvn w8, \value
	tst w8, #0x1f80
	b.ne Lx86_simd_unmasked\@
	retire_and_next
Lx86_simd_unmasked\@:
	mov w9, #0x1900
	tst w8, w9
	b.ne Lx86_simd_unchecked\@
	ldrb w8, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w8, #1, Lx86_simd_unchecked\@
	retire_and_next
Lx86_simd_unchecked\@:
	retire_and_leave
.endm

/* `control` is the x87 control word just loaded, and the status word is
 * already the one the instruction leaves. */
.macro retire_unless_x87_unchecked control
	mvn w8, \control
	tst w8, #0x3f
	b.ne Lx86_x87_unmasked\@
	retire_and_next
Lx86_x87_unmasked\@:
	ldrb w8, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w8, #0, Lx86_x87_unchecked\@
	ldrh w8, [_vcpu, #UML_X86_64_CPU_X87_STATUS]
	bic w8, w8, \control
	tst w8, #0x3f
	b.ne Lx86_x87_unchecked\@
	retire_and_next
Lx86_x87_unchecked\@:
	retire_and_leave
.endm

/* The same for an instruction that loads both. */
.macro retire_unless_any_unchecked
	ldr w8, [_vcpu, #UML_X86_64_CPU_MXCSR]
	mvn w8, w8
	tst w8, #0x1f80
	b.eq Lx86_simd_masked\@
	mov w9, #0x1900
	tst w8, w9
	b.ne Lx86_simd_unchecked\@
	ldrb w8, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w8, #1, Lx86_simd_unchecked\@
Lx86_simd_masked\@:
	ldrh w9, [_vcpu, #UML_X86_64_CPU_X87_CONTROL]
	retire_unless_x87_unchecked w9
Lx86_simd_unchecked\@:
	retire_and_leave
.endm

/* While MXCSR unmasks invalid operation, division by zero or overflow, and
 * masks underflow, precision and a denormal operand, the runtime sets
 * `exception_check_simd` in `exception_checks`. The SIMD floating-point
 * gadgets then test their values for anything that could have raised an
 * unmasked exception, before they store anything: a result whose exponent is
 * all ones or one below -- a NaN, an infinity, or the largest finite value an
 * overflow rounds to -- a NaN input to a comparison, minimum or maximum, and a
 * conversion to an integer out of its range. Such an instruction leaves, and
 * the runtime runs it again, checked, which raises #XM or completes it with
 * its exact flags. Where the gadget already takes a separate path for such a
 * value the test costs nothing more; elsewhere it costs a load and a branch
 * while the gadgets do not check. A gadget with an exact path tests there
 * (`simd_exact_branch`).
 *
 * Leaves while the gadgets check SIMD exceptions: on a path taken only for a
 * special value, or for an instruction whose intermediate results no test of
 * its result can see.
 *
 * The leave commits nothing, and the runtime adds the run's host flags to
 * MXCSR before it runs the instruction checked, so the host flags the
 * instruction raised so far must be ones x86 raises too: x86 computes no
 * lane of an instruction that faults before its computation, and the host
 * detects underflow before rounding and raises invalid operation for an FMA
 * of infinity times zero plus a quiet NaN. This leave is for a gadget whose
 * host instructions so far raised only x86's flags -- a test of the inputs,
 * a comparison's own flags, a scalar conversion; a gadget that computed
 * lanes x86 might not saves FPSR first (`simd_save_status_branch`), and its
 * leave (`simd_leave_computed_if_checking`) puts FPSR back. */
.macro simd_leave_if_checking
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_unchecked\@
	b uml_x86_64_gadget_exception_check
Lx86_simd_unchecked\@:
.endm

/* While the gadgets check SIMD exceptions, save FPSR for this gadget's
 * computed leave: branch to `save`, which `simd_save_status_path` places
 * off the gadget's straight path, and come back. `checks` names the
 * register holding `exception_checks`; by default it is loaded into w14. */
.macro simd_save_status_branch save, checks=-1
	.if \checks < 0
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbnz w14, #1, \save
	.else
	tbnz w\checks, #1, \save
	.endif
\save\()_back:
	.set Lx86_simd_status_gadget, 1
.endm

/* The save itself. Uses x15. */
.macro simd_save_status_path save
\save:
	mrs x15, fpsr
	str x15, [_context, #UML_X86_64_CONTEXT_SIMD_STATUS]
	b \save\()_back
.endm

.macro simd_require_status
	.if !Lx86_simd_status_gadget
	.error "a computed SIMD leave needs simd_save_status_branch earlier in its gadget"
	.endif
.endm

/* The leave for a gadget that saved FPSR, which it puts back. */
.macro simd_leave_computed_if_checking
	simd_require_status
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_unchecked\@
	b uml_x86_64_gadget_simd_exception_check
Lx86_simd_unchecked\@:
.endm

/* A SIMD floating-point gadget whose common path assumes MXCSR.DAZ and FTZ
 * clear and no SIMD check runs its exact path instead while
 * `exception_check_simd_exact` says so: one load and branch serve all three.
 * Applications commonly set DAZ and FTZ for good, so the exact path follows
 * inline and ends at `simd_exact_end` with a branch past the common path at
 * `common`; only its rare repairs go to the cold section. The exact path
 * reads MXCSR itself, and saves FPSR for its computed leaves. Uses w14 and
 * x15. */
.macro simd_exact_branch common
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #2, \common
	simd_save_status_branch \common\()_save, 14
.endm

/* The end of the exact path of `simd_exact_branch common`, which continues
 * at `done`. */
.macro simd_exact_end common, done
	b \done
	simd_save_status_path \common\()_save
.endm

/* Keep `exception_check_simd_exact` current after an instruction loads
 * MXCSR, now `value`: it follows DAZ, FTZ and `exception_check_simd`, which
 * stays as the run set it -- an instruction that changes what a run checks
 * leaves. Uses w`scratch0` and w`scratch1`. */
.macro refresh_simd_exact value, scratch0, scratch1
	ldrb w\scratch0, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	and w\scratch0, w\scratch0, #0xfffffffb
	mov w\scratch1, #0x8040
	tst \value, w\scratch1
	cset w\scratch1, ne
	orr w\scratch1, w\scratch1, w\scratch0, lsr #1
	and w\scratch1, w\scratch1, #1
	orr w\scratch0, w\scratch0, w\scratch1, lsl #2
	strb w\scratch0, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
.endm

/* w14 = nonzero when a lane of v`register` (`element` bits wide) has an
 * exponent of all ones or one below. Uses v17 and v18. */
.macro simd_special_lanes element, register
	.if \element == 16
	shl v17.8h, v\register\().8h, #1
	movi v18.8h, #0xf0, lsl #8
	cmhs v17.8h, v17.8h, v18.8h
	.elseif \element == 32
	shl v17.4s, v\register\().4s, #1
	movi v18.4s, #0xfe, lsl #24
	cmhs v17.4s, v17.4s, v18.4s
	.else
	shl v17.2d, v\register\().2d, #1
	mov x14, #0xffc0000000000000
	dup v18.2d, x14
	cmhs v17.2d, v17.2d, v18.2d
	.endif
	umaxv s17, v17.4s
	fmov w14, s17
.endm

/* Branches to `label` when lane 0 of v`register` has an exponent of all ones
 * or one below, or with `condition` lo when it does not. Uses x14. */
.macro simd_special_scalar element, register, label, condition=hs
	.if \element == 32
	fmov w14, s\register
	ubfx w14, w14, #23, #8
	cmp w14, #0xfe
	.else
	fmov x14, d\register
	ubfx x14, x14, #52, #11
	cmp x14, #0x7fe
	.endif
	b.\condition \label
.endm

/* w14 = all ones when no lane of v`left` or v`right` is a NaN, else zero.
 * Uses v17 and v18. */
.macro simd_ordered_lanes element, left, right
	.if \element == 32
	fcmeq v17.4s, v\left\().4s, v\left\().4s
	fcmeq v18.4s, v\right\().4s, v\right\().4s
	.else
	fcmeq v17.2d, v\left\().2d, v\left\().2d
	fcmeq v18.2d, v\right\().2d, v\right\().2d
	.endif
	and v17.16b, v17.16b, v18.16b
	uminv s17, v17.4s
	fmov w14, s17
.endm

/* Branches to `label` when a lane of v`first`, or of v`second` for a
 * 256-bit result, is a NaN. The results it tests are quiet, so the test
 * raises no host flag. Uses v17. */
.macro simd_nan_lanes element, label, first, second=-1
	.if \element == 32
	.if \second >= 0
	fmax v17.4s, v\first\().4s, v\second\().4s
	fmaxv s17, v17.4s
	.else
	fmaxv s17, v\first\().4s
	.endif
	fcmp s17, s17
	.else
	.if \second >= 0
	fmax v17.2d, v\first\().2d, v\second\().2d
	fmaxp d17, v17.2d
	.else
	fmaxp d17, v\first\().2d
	.endif
	fcmp d17, d17
	.endif
	b.vc Lx86_simd_no_nan\@
	b \label
Lx86_simd_no_nan\@:
.endm

/* MXCSR.DAZ and FTZ: replace each denormal lane of v`vector` (`element` bits
 * wide) with a zero of its sign. A lane whose exponent is zero takes its sign
 * bit alone, which leaves a zero as it was. The value passes through two
 * dependent instructions, as a scalar computation waits on it. Uses v16, v17
 * and x14. */
.macro simd_flush_denormals vector, element
	simd_flush_masks \element, 16, 17
	simd_flush_denormals_masked \vector, \element, 16, 17
.endm

/* The masks `simd_flush_denormals_masked` takes: the exponent in
 * v`exponent`, the sign in v`sign`. Several flushes share them. Uses x14. */
.macro simd_flush_masks element, exponent, sign
	.if \element == 32
	mov w14, #0x7f800000
	dup v\exponent\().4s, w14
	movi v\sign\().4s, #0x80, lsl #24
	.else
	mov x14, #0x7ff0000000000000
	dup v\exponent\().2d, x14
	mov x14, #0x8000000000000000
	dup v\sign\().2d, x14
	.endif
.endm

/* The flush itself, with the masks in v`exponent` and v`sign`, which may be
 * v16 and v17 for a single flush. Uses v16 and v17. */
.macro simd_flush_denormals_masked vector, element, exponent, sign
	and v17.16b, v\vector\().16b, v\sign\().16b
	.if \element == 32
	cmtst v16.4s, v\vector\().4s, v\exponent\().4s
	.else
	cmtst v16.2d, v\vector\().2d, v\exponent\().2d
	.endif
	bif v\vector\().16b, v17.16b, v16.16b
.endm

/* The same for two to four vectors, which share masks in v18 and v19. Uses
 * v16 to v19 and x14. */
.macro simd_flush_denormals_each element, first, second, third=-1, fourth=-1
	simd_flush_masks \element, 18, 19
	simd_flush_denormals_masked \first, \element, 18, 19
	simd_flush_denormals_masked \second, \element, 18, 19
	.if \third >= 0
	simd_flush_denormals_masked \third, \element, 18, 19
	.endif
	.if \fourth >= 0
	simd_flush_denormals_masked \fourth, \element, 18, 19
	.endif
.endm

/* v`mask` = all ones in the denormal lanes of v`vector`. Uses v`temporary`
 * and x14. */
.macro simd_denormal_lanes element, vector, mask, temporary
	.if \element == 32
	shl v\mask\().4s, v\vector\().4s, #1
	movi v\temporary\().16b, #0xff
	add v\mask\().4s, v\mask\().4s, v\temporary\().4s
	movi v\temporary\().4s, #0xff, msl #16
	cmhi v\mask\().4s, v\temporary\().4s, v\mask\().4s
	.else
	shl v\mask\().2d, v\vector\().2d, #1
	movi v\temporary\().16b, #0xff
	add v\mask\().2d, v\mask\().2d, v\temporary\().2d
	mov x14, #0x001fffffffffffff
	dup v\temporary\().2d, x14
	cmhi v\mask\().2d, v\temporary\().2d, v\mask\().2d
	.endif
.endm

/* MXCSR.FTZ for a result that is tiny only when it is denormal -- a sum or
 * difference, which is exact that small: replace each denormal lane of
 * v`first`, and of v`second`, with a zero of its sign, and raise underflow
 * and precision when there is one. The flush and the raise share one mask.
 * Uses v16 to v19 and x14. */
.macro simd_flush_denormal_results element, first, second=-1
	simd_denormal_lanes \element, \first, 16, 17
	.if \element == 32
	movi v17.4s, #0x80, lsl #24
	.else
	mov x14, #0x8000000000000000
	dup v17.2d, x14
	.endif
	and v18.16b, v\first\().16b, v17.16b
	bit v\first\().16b, v18.16b, v16.16b
	.if \second >= 0
	simd_denormal_lanes \element, \second, 19, 18
	and v18.16b, v\second\().16b, v17.16b
	bit v\second\().16b, v18.16b, v19.16b
	orr v16.16b, v16.16b, v19.16b
	.endif
	mxcsr_raise_flushed 16, 18
.endm

/* v`mask` = all ones in the lanes of v`output` that are not zero and at most
 * the smallest normal in magnitude. Uses v`temporary`. */
.macro simd_small_lanes element, output, mask, temporary
	.if \element == 32
	shl v\mask\().4s, v\output\().4s, #1
	movi v\temporary\().16b, #0xff
	add v\mask\().4s, v\mask\().4s, v\temporary\().4s
	movi v\temporary\().4s, #0x01, lsl #24
	cmhi v\mask\().4s, v\temporary\().4s, v\mask\().4s
	.else
	shl v\mask\().2d, v\output\().2d, #1
	movi v\temporary\().16b, #0xff
	add v\mask\().2d, v\mask\().2d, v\temporary\().2d
	mov x14, #0x0020000000000000
	dup v\temporary\().2d, x14
	cmhi v\mask\().2d, v\temporary\().2d, v\mask\().2d
	.endif
.endm

/* The scalar form of simd_small_lanes: Z is set when the result in s6 (an
 * `element` of 32) or d6 is not zero and at most the smallest normal in
 * magnitude. Uses x14. */
.macro simd_small_scalar element
	.if \element == 32
	fmov w14, s6
	lsl w14, w14, #1
	sub w14, w14, #1
	tst w14, #0xff000000
	.else
	fmov x14, d6
	lsl x14, x14, #1
	sub x14, x14, #1
	tst x14, #0xffe0000000000000
	.endif
.endm

/* x86 detects underflow after rounding: a result is tiny when, rounded to
 * full precision as if the exponent were unbounded, it lies below the
 * smallest normal. A denormal result always is. A product or quotient the
 * host rounded up to exactly the smallest normal is when the same operation,
 * with the first source scaled up into the normal range, rounds below the
 * threshold scaled alike. Results that small bound the first source, so the
 * scaling is exact, and the scaled operation's flags are discarded.
 *
 * v18 = all ones in the packed lanes whose scaled result is tiny. Uses v16,
 * x14 and x15. */
.macro simd_scaled_tiny operation, element, left, right
	mrs x15, fpsr
	.if \element == 32
	mov w14, #0x5f800000
	dup v18.4s, w14
	fmul v18.4s, v\left\().4s, v18.4s
	.ifc \operation,mul
	fmul v18.4s, v18.4s, v\right\().4s
	.else
	fdiv v18.4s, v18.4s, v\right\().4s
	.endif
	shl v18.4s, v18.4s, #1
	movi v16.4s, #0x41, lsl #24
	cmhi v18.4s, v16.4s, v18.4s
	.else
	mov x14, #0x5ff0000000000000
	dup v18.2d, x14
	fmul v18.2d, v\left\().2d, v18.2d
	.ifc \operation,mul
	fmul v18.2d, v18.2d, v\right\().2d
	.else
	fdiv v18.2d, v18.2d, v\right\().2d
	.endif
	shl v18.2d, v18.2d, #1
	mov x14, #0x4020000000000000
	dup v16.2d, x14
	cmhi v18.2d, v16.2d, v18.2d
	.endif
	msr fpsr, x15
.endm

/* MXCSR.FTZ: replace each tiny lane of the packed v`output` with a zero of
 * its sign, raising underflow and precision. A sum or difference that small
 * is exact, so only a denormal one is tiny. */
.macro simd_flush_tiny_packed operation, element, output, left, right
	.set Lx86_simd_rounded\@, 0
	.ifc \operation,mul
	.set Lx86_simd_rounded\@, 1
	.endif
	.ifc \operation,div
	.set Lx86_simd_rounded\@, 1
	.endif
	.if !Lx86_simd_rounded\@
	simd_flush_denormal_results \element, \output
	.else
	simd_small_lanes \element, \output, 17, 18
	simd_scaled_tiny \operation, \element, \left, \right
	and v17.16b, v17.16b, v18.16b
	mxcsr_raise_flushed 17, 18
	.if \element == 32
	movi v16.4s, #0x80, lsl #24
	.else
	mov x14, #0x8000000000000000
	dup v16.2d, x14
	.endif
	and v16.16b, v\output\().16b, v16.16b
	bsl v17.16b, v16.16b, v\output\().16b
	mov v\output\().16b, v17.16b
	.endif
.endm

/* Repair a NaN result in v`output` (lane 0 alone for a scalar one) from
 * v`left` and v`right`, the first and second operands of a two-operand
 * operation, or v`right` alone of a one-operand one. Uses v16 to v19 and x14.
 */
.macro simd_x86_nan element, output, left, right, binary, scalar
	/* x86 returns the first source quieted when it is a NaN, else the
	 * second, else -- for a NaN the operation generated -- the negative
	 * quiet-NaN indefinite. The host prefers a signaling NaN of either
	 * source and generates its positive default NaN, and its result is right
	 * only where just the second source is a NaN.
	 */
	.if \scalar
	.if \element == 32
	fcmp s\output, s\output
	b.vc Lx86_simd_nan_done\@
	.if \binary
	fcmp s\left, s\left
	b.vs Lx86_simd_first_nan\@
	.endif
	fcmp s\right, s\right
	b.vs Lx86_simd_nan_done\@
	mov w14, #0xffc00000
	fmov s\output, w14
	b Lx86_simd_nan_done\@
	.if \binary
Lx86_simd_first_nan\@:
	fmov w14, s\left
	orr w14, w14, #0x00400000
	fmov s\output, w14
	.endif
	.else
	fcmp d\output, d\output
	b.vc Lx86_simd_nan_done\@
	.if \binary
	fcmp d\left, d\left
	b.vs Lx86_simd_first_nan\@
	.endif
	fcmp d\right, d\right
	b.vs Lx86_simd_nan_done\@
	mov x14, #0xfff8000000000000
	fmov d\output, x14
	b Lx86_simd_nan_done\@
	.if \binary
Lx86_simd_first_nan\@:
	fmov x14, d\left
	orr x14, x14, #0x0008000000000000
	fmov d\output, x14
	.endif
	.endif
	.else
	.if \element == 32
	fcmeq v17.4s, v\output\().4s, v\output\().4s
	uminv s18, v17.4s
	fmov w14, s18
	cbnz w14, Lx86_simd_nan_done\@
	mov w14, #0xffc00000
	dup v16.4s, w14
	fcmeq v18.4s, v\right\().4s, v\right\().4s
	.if \binary
	fcmeq v19.4s, v\left\().4s, v\left\().4s
	.endif
	.else
	fcmeq v17.2d, v\output\().2d, v\output\().2d
	uminv s18, v17.4s
	fmov w14, s18
	cbnz w14, Lx86_simd_nan_done\@
	mov x14, #0xfff8000000000000
	dup v16.2d, x14
	fcmeq v18.2d, v\right\().2d, v\right\().2d
	.if \binary
	fcmeq v19.2d, v\left\().2d, v\left\().2d
	.endif
	.endif
	bsl v18.16b, v16.16b, v\output\().16b
	.if \binary
	.if \element == 32
	movi v16.4s, #0x40, lsl #16
	.else
	mov x14, #0x0008000000000000
	dup v16.2d, x14
	.endif
	orr v16.16b, v\left\().16b, v16.16b
	bsl v19.16b, v18.16b, v16.16b
	bsl v17.16b, v\output\().16b, v19.16b
	.else
	bsl v17.16b, v\output\().16b, v18.16b
	.endif
	mov v\output\().16b, v17.16b
	.endif
Lx86_simd_nan_done\@:
.endm

/* The leave of a result check: a computed one, which puts back the FPSR
 * `simd_save_status_branch` saved, unless `computed` is 0. */
.macro simd_check_leave computed
	.if \computed
	simd_require_status
	b uml_x86_64_gadget_simd_exception_check
	.else
	b uml_x86_64_gadget_exception_check
	.endif
.endm

/* While the gadgets check SIMD exceptions, leave when a lane of a packed
 * result -- v`first`, and v`second` for a 256-bit one -- is special. */
.macro simd_check_packed element, first, second=-1, computed=1
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_packed_ok\@
	simd_special_lanes \element, \first
	.if \second >= 0
	cbnz w14, Lx86_simd_packed_special\@
	simd_special_lanes \element, \second
	.endif
	cbz w14, Lx86_simd_packed_ok\@
Lx86_simd_packed_special\@:
	simd_check_leave \computed
Lx86_simd_packed_ok\@:
.endm

/* The same for a packed result in v4, and v5 for a 256-bit one. */
.macro simd_check_result element, width, computed=1
	.if \width == 256
	simd_check_packed \element, 4, 5, \computed
	.else
	simd_check_packed \element, 4, -1, \computed
	.endif
.endm

/* The same for the scalar result in lane 0 of v`register`. */
.macro simd_check_scalar element, register, computed=1
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_scalar_ok\@
	simd_special_scalar \element, \register, Lx86_simd_scalar_special\@
	b Lx86_simd_scalar_ok\@
Lx86_simd_scalar_special\@:
	simd_check_leave \computed
Lx86_simd_scalar_ok\@:
.endm

/* The same for a NaN in a lane of the packed inputs v`left` and v`right`, and
 * v`left2` and v`right2` for 256-bit ones. */
.macro simd_check_packed_nan element, left, right, left2=-1, right2=-1
	ldrb w14, [_vcpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_inputs_ok\@
	simd_ordered_lanes \element, \left, \right
	cbz w14, Lx86_simd_inputs_nan\@
	.if \left2 >= 0
	simd_ordered_lanes \element, \left2, \right2
	cbz w14, Lx86_simd_inputs_nan\@
	.endif
	b Lx86_simd_inputs_ok\@
Lx86_simd_inputs_nan\@:
	b uml_x86_64_gadget_exception_check
Lx86_simd_inputs_ok\@:
.endm

/* The same for a NaN in lane 0 of v`left` or v`right`, tested in any case:
 * the test costs no more than the gate would. */
.macro simd_check_scalar_nan element, left, right
	.if \element == 32
	fcmp s\left, s\right
	.else
	fcmp d\left, d\right
	.endif
	b.vc Lx86_simd_ordered\@
	simd_leave_if_checking
Lx86_simd_ordered\@:
.endm

/* Bytes 24..31 of the record hold operation, width, carry, one padding byte,
 * and the two override halves. Both varying fields are macro parameters, so
 * that whole tail is a single link-time constant and the record costs three
 * stores instead of six. Consumers that need a carry or an override write it
 * after this macro.
 */
.macro record_deferred operation, width
	add x15, _vcpu, #UML_X86_64_CPU_DEFERRED_LEFT
	stp x11, x12, [x15]
	mov x14, #((\operation) | ((\width / 8) << 8))
	stp x13, x14, [x15, #16]
.endm

/* The same record from any three registers. */
.macro record_deferred_from operation, width, left, right, result
	add x15, _vcpu, #UML_X86_64_CPU_DEFERRED_LEFT
	stp \left, \right, [x15]
	mov x14, #((\operation) | ((\width / 8) << 8))
	stp \result, x14, [x15, #16]
.endm

/* An INC or DEC record. It carries the pending bit its prologue left in the
 * record it displaces: set when the preserved carry lives in the carry slot.
 */
.macro record_deferred_pending operation, width
	add x15, _vcpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldrb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	stp x11, x12, [x15]
	mov x14, #((\operation) | ((\width / 8) << 8))
	orr x14, x14, x16, lsl #24
	stp x13, x14, [x15, #16]
.endm

/* INC and DEC preserve the carry flag, which the pending record may still
 * define. Rather than settle it -- a call, a table and the override overlay,
 * on an instruction that is two host instructions of work -- copy that record
 * into the carry slot and mark the pending bit, so the carry is computed only
 * when a consumer reads it through the INC or DEC record. A pending INC or
 * DEC record already names its carry source, so it is left alone; the one
 * with a carry override riding on it is settled the old way, which clears the
 * bit. The prologue writes nothing but the slot and the bit, so a memory form
 * that faults after it leaves the record exactly as it found it. x5, x7, x8,
 * x10 and x14 to x16 are clobbered.
 *
 * An empty record is settled, not pending: RFLAGS already holds the carry,
 * which is the state every gadget that materializes and republishes the flags
 * leaves -- ADCX and ADOX, the flag writers, and any consumer that had to
 * materialize. The slot answers only through the operation-and-width table,
 * where an empty record reads as a carry of zero, so parking one would
 * discard a settled carry. It takes the same way out as a pending INC or DEC
 * record: nothing to do but a carry override, which is settled as before.
 */
.macro lazy_carry_prologue
	ldr x14, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	ands w5, w14, #0xff
	sub w5, w5, #4
	/* `sub` keeps the flags `ands` defined, so an empty record forces the
	 * settled way out without a test of its own. */
	ccmp w5, #1, #4, ne
	b.ls Lx86_lazy_settled\@
	add x15, _vcpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x15]
	ldr x10, [x15, #16]
	add x15, _vcpu, #UML_X86_64_CPU_CARRY_SLOT
	stp x7, x8, [x15]
	stp x10, x14, [x15, #16]
	mov w16, #1
	strb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	b Lx86_lazy_ready\@
Lx86_lazy_settled\@:
	tbz x14, #32, Lx86_lazy_ready\@
	bl uml_x86_64_gadget_materialize_carry
Lx86_lazy_ready\@:
.endm

/* A guest access no wider than one byte lies inside one page by construction,
 * so the boundary test folds away entirely at assembly time. `size` is the
 * access width in bytes and must be an absolute expression.
 */
.macro page_cross_check size, label
	.if (\size) > 1
	add x9, x7, #((\size) - 1)
	eor x9, x9, x7
	tbnz x9, #12, \label
	.endif
.endm

/* A host W-form instruction writes the whole 64-bit register, so a result
 * computed at 32 or 64 bits already is the guest value. Only the narrow widths
 * still need masking. Sites whose producer is a narrow load or the zero-extending
 * cross-page helper need nothing at all and call neither macro.
 */
.macro normalize_result width, register
	.if \width == 8
	and \register, \register, #0xff
	.elseif \width == 16
	uxth \register, \register
	.endif
.endm

/* Stream descriptor:
 *   15:0   value-register byte offset
 *   25:16  base-register byte offset, or UML_X86_64_CPU_SEGMENT_ZERO when
 *          there is no base register
 *   35:26  index-register byte offset, or UML_X86_64_CPU_SEGMENT_ZERO when
 *          there is no index register
 *   45:36  the instruction's position in its block (`instruction_rip`)
 *   49:48  index scale shift
 *   51:50  segment (0 none, 1 FS, 2 GS), or with bit 55 the ES, CS, SS
 *          or DS whose base a stepped instruction adds
 *   52     32-bit effective-address arithmetic
 *   53     16-bit effective-address arithmetic
 *   54     none of the three above: the address is complete once the base,
 *          the scaled index and the displacement are summed
 *   55     a legacy segment in 51:50 (`segments.hpp`)
 *   59:56  instruction length
 *   60     a write translation proved this store publishes nothing:
 *          its page holds no cached code and no publication is pending
 * Followed by the folded displacement/absolute address. Every CpuState
 * offset a descriptor names is below 1024, which the builder checks.
 */
.macro effective_address
	ldp x23, x14, [_stream], #16
	ubfx x9, x23, #16, #10
	ubfx x11, x23, #26, #10
	ubfx x12, x23, #48, #2
	ldr x10, [_vcpu, x9]
	ldr x13, [_vcpu, x11]
	lsl x13, x13, x12
	add x7, x14, x10
	add x7, x7, x13
	/* A long-mode address with no segment override is finished here. The
	 * builder proves that from the operand, because what remains costs a
	 * load of the selected segment base through a computed address -- a form
	 * the host cannot disambiguate against the CpuState stores every gadget
	 * makes, and the most expensive instruction of the generic address path.
	 * Adding a zero segment base and masking nothing are what that work comes
	 * to in the plain case, so skipping it is the same address.
	 */
	tbnz x23, #UML_X86_64_MEMORY_PLAIN_ADDRESS, Lx86_segment_ready\@
	b Lx86_address_general\@
	/* What a segment override or a narrow address space still needs is two
	 * instructions in the entry and its whole body in the cold section, so a
	 * bank this size keeps only the plain form in the hot stream.
	 */
	.pushsection GADGET_SECTION_COLD
Lx86_address_general\@:
	/* Truncation distributes over addition and shifting, so masking the sum
	 * once is equivalent to masking every component and the sum.
	 */
	tbnz x23, #53, Lx86_address_16\@
	tbz x23, #52, Lx86_address_ready\@
	mov w7, w7
	b Lx86_address_ready\@
Lx86_address_16\@:
	uxth w7, w7
Lx86_address_ready\@:
	ubfx x9, x23, #50, #2
	cbnz x9, Lx86_address_segment\@
	/* Field 0 with bit 55 is a stepped instruction's ES. */
	tbnz x23, #UML_X86_64_MEMORY_LEGACY_SEGMENT, Lx86_address_legacy\@
	b Lx86_segment_ready\@
Lx86_address_segment\@:
	ldrb w11, [_vcpu, #UML_X86_64_CPU_EXECUTION_MODE]
	cmp w11, #1
	b.eq Lx86_address_compat\@
	/* Only compatibility mode steps instructions through the segment
	 * model, so long mode has FS or GS here, and nothing to do beyond what
	 * it always did: the selector indexes {segment_zero, fs_base, gs_base},
	 * and long mode keeps its full-width FS or GS base. */
	add x11, _vcpu, x9, lsl #3
	ldr x10, [x11, #UML_X86_64_CPU_SEGMENT_ZERO]
	add x7, x7, x10
	b Lx86_segment_ready\@
Lx86_address_compat\@:
	tbnz x23, #UML_X86_64_MEMORY_LEGACY_SEGMENT, Lx86_address_legacy\@
	/* FS or GS in compatibility mode: the offset passes the segment's
	 * inline check, or the instruction leaves for the runtime's exact one
	 * before it commits anything. Legacy protected mode has a 32-bit
	 * linear-address space, and Linux's i386 TLS sequences rely on a high
	 * base plus an offset wrapping through zero. */
	add x11, _vcpu, #(UML_X86_64_CPU_SEGMENTS + 3 * UML_X86_64_SEGMENT_CACHE_SIZE)
	add x11, x11, x9, lsl #5
	ldp x12, x13, [x11, #UML_X86_64_SEGMENT_CHECK_LOW]
	sub x12, x7, x12
	cmp x12, x13
	b.ls Lx86_address_checked\@
	b Lx86_segment_check
Lx86_address_checked\@:
	add x11, _vcpu, x9, lsl #3
	ldr x10, [x11, #UML_X86_64_CPU_SEGMENT_ZERO]
	add w7, w7, w10
	b Lx86_segment_ready\@
Lx86_address_legacy\@:
	/* A stepped instruction's ES, CS, SS or DS, whose accesses the runtime
	 * has checked: only the base to add, wrapping at 4 GiB. */
	add x11, _vcpu, x9, lsl #5
	ldr w10, [x11, #(UML_X86_64_CPU_SEGMENTS + UML_X86_64_SEGMENT_BASE)]
	add w7, w7, w10
	b Lx86_segment_ready\@
	.popsection
Lx86_segment_ready\@:
.endm

/* The stack's own entry, re-checked before any hashing: x25 holds the guest
 * address, and a tag equal to its page -- with no code bit set, which a
 * page holding a cached translation always has -- leaves the host address in
 * x7. Anything else takes \miss, which is the full translation with
 * `record_stack` so the next access finds the new entry here. A write also
 * requires that no earlier store left a publication pending, because the
 * ordinary translation would settle it here; with both conditions the store
 * owes no publication. It may still land in the block that is running, so a
 * store's caller joins its epilogue at the overlap test.
 */
.macro stack_fast access, miss
	x86_marks_guest_memory
	ldr x11, [_context, #UML_X86_64_CONTEXT_STACK_ENTRY]
	and x9, x25, #0xfffffffffffff000
	ldp x12, x13, [x11, #((\access) * 16)]
	eor x14, x9, x12
	.if \access == 1
	ldrb w15, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	orr x14, x14, x15
	.endif
	cbnz x14, \miss
	add x7, x25, x13
.endm

.macro translate access, entry_offset, size, record_stack=0, fault=Lx86_memory_fault
	x86_marks_guest_memory
	.if \access == 1
	/* Only a translation that reaches the decision below may skip the store's
	 * publication; the miss and page-crossing paths leave this clear.
	 */
	and x23, x23, #~(1 << 60)
	.endif
	and x9, x7, #0xfffffffffffff000
	lsr x10, x7, #12
	eor x10, x10, x10, lsr #15
	and w10, w10, #0x7fff
	ldr x11, [_context, #UML_X86_64_CONTEXT_TLB]
	add x11, x11, x10, lsl #5
	.if \record_stack
	str x11, [_context, #UML_X86_64_CONTEXT_STACK_ENTRY]
	.endif
	ldp x12, x13, [x11, #\entry_offset]
	.if \access == 1
	bic x14, x12, #(UML_X86_64_TLB_WRITE_CODE | UML_X86_64_TLB_WRITE_CACHED_CODE)
	cmp x9, x14
	.else
	cmp x9, x12
	.endif
	b.eq Lx86_tlb_hit\@
	mov x0, _context
	mov x1, x7
	mov w2, #\access
	mov w3, #\size
	helper_call uml_x86_64_gadget_tlb_miss
	cbz x0, \fault
	mov x7, x0
	b Lx86_tlb_done\@
Lx86_tlb_hit\@:
	.if \access == 1
	/* Reading the context after the guest store costs a memory-disambiguation
	 * stall, and calling out to publication costs far more. Publication has
	 * work only when a publish is still pending or this page holds cached
	 * code, and neither depends on what the store writes, so both are settled
	 * here. `write_code` is none, pending or published and only pending is
	 * work, so its low bit answers for it and joins the entry's cached-code
	 * bit in one test; the tag's own bits are clear on a hit, because the
	 * compare above proved the tag is a page-aligned address once they are
	 * removed.
	 */
	ldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	tbz x12, #1, Lx86_tlb_write_code_ready\@
	/* A test-and-branch reaches 32 KiB and the cold section is a bank away,
	 * so the rare arm is entered through an unconditional branch. */
	b Lx86_tlb_write_code_owed\@
Lx86_tlb_write_code_ready\@:
	orr w14, w14, w12, lsr #2
	tbnz w14, #0, Lx86_store_publishes\@
	orr x23, x23, #(1 << 60)
Lx86_store_publishes\@:
	.endif
	add x7, x13, x7
Lx86_tlb_done\@:
	.if \access == 1
	/* A store to a page some executor holds a translation of is the rare one,
	 * and it leaves the hot stream: a bank of thousands of write
	 * translations otherwise fetches this arm on every store that will never
	 * take it.
	 *
	 * Every such store owes a record of its own, even to a page this program
	 * has just published. The page stamp another vCPU validates against moves
	 * only with a publication, and a vCPU that revalidated a block of the page
	 * between two stores keeps the bytes it read there until the stamp moves
	 * again, so a store that published nothing would leave it running the
	 * code from before.
	 */
	.pushsection GADGET_SECTION_COLD
Lx86_tlb_write_code_owed\@:
	mov w14, #UML_X86_64_WRITE_CODE_PENDING
	strb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	b Lx86_tlb_write_code_ready\@
	.popsection
	.endif
.endm

/* A probe that only translates, for the gather and masked-memory loops,
 * which translate element by element: it neither records the stack entry
 * nor settles a store's publication, and a miss the MMU refuses branches to
 * `fault`. A write probe compares the write page with its tag bits intact,
 * so a store to a page holding code always takes the miss path, where the
 * helper publishes it. Its index is translate's. */
.macro translate_plain access, entry_offset, size, fault
	x86_marks_guest_memory
	and x9, x7, #0xfffffffffffff000
	lsr x10, x7, #12
	eor x10, x10, x10, lsr #15
	and w10, w10, #0x7fff
	ldr x11, [_context, #UML_X86_64_CONTEXT_TLB]
	add x11, x11, x10, lsl #5
	ldp x12, x13, [x11, #\entry_offset]
	cmp x9, x12
	b.eq Lx86_plain_tlb_hit\@
	mov x0, _context
	mov x1, x7
	mov w2, #\access
	mov w3, #\size
	helper_call uml_x86_64_gadget_tlb_miss
	cbz x0, \fault
	mov x7, x0
	b Lx86_plain_tlb_done\@
Lx86_plain_tlb_hit\@:
	add x7, x13, x7
Lx86_plain_tlb_done\@:
.endm

/* An access whose `size` bytes at guest address x25 cross a page goes
 * through the helpers, and a fault the MMU raises leaves through the shared
 * exit. A load returns the value zero-extended in x0. */
.macro cross_page_load size
	mov x0, _context
	mov x1, x25
	mov w2, #\size
	helper_call uml_x86_64_gadget_cross_page_load
	cbz x1, Lx86_memory_fault
.endm

.macro cross_page_store value, size, fault=Lx86_memory_fault
	mov x0, _context
	mov x1, x25
	mov x2, \value
	mov w3, #\size
	helper_call uml_x86_64_gadget_cross_page_store
	cbz w0, \fault
.endm

/* Read a `size`-byte memory operand into register `number`: x7 holds its
 * address and x25 the copy the cross-page helper reads from. `load` and
 * `register` give the in-page load and its register width, and `move` the
 * width at which the helper's result is copied. */
.macro read_operand size, load, register, number, move=x
	page_cross_check \size, Lx86_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \size
	\load \register\()\number, [x7]
	b Lx86_read_loaded\@
Lx86_read_cross\@:
	cross_page_load \size
	mov \move\()\number, \move\()0
Lx86_read_loaded\@:
.endm

/* Load a `size`-byte vector operand from `base` into v`low`, a 32-byte one
 * into v`low` and v`high`. A 1, 2, 4 or 8-byte operand loads b, h, s or d.
 * `zero_high` clears v`high` for a 256-bit form with a narrower source. */
.macro vector_source_load size, low, high, base, zero_high=0
	.if (\size) == 1
	ldr b\low, [\base]
	.elseif (\size) == 2
	ldr h\low, [\base]
	.elseif (\size) == 4
	ldr s\low, [\base]
	.elseif (\size) == 8
	ldr d\low, [\base]
	.else
	ldr q\low, [\base]
	.endif
	.if (\size) == 32
	ldr q\high, [\base, #16]
	.elseif \zero_high
	movi v\high\().16b, #0
	.endif
.endm

/* Read a `size`-byte vector memory operand at x7, with x25 the copy the
 * cross-page helper reads from, into v1, and v3 for 32 bytes, or the `low`
 * and `high` registers. An access across a page goes through a `frame`-byte
 * stack buffer, and a fault the MMU raises branches to `fault`, which the
 * gadget defines after its code with `vector_memory_source_fault`. `scalar`
 * sends an access of 8 bytes or less through `cross_page_load` and moves it
 * with an x register; `single` moves a 4-byte one with a w register. */
.macro vector_memory_source size, fault, low=1, high=3, frame=32, scalar=0, single=0, zero_high=0
	page_cross_check \size, Lx86_vector_source_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \size
	vector_source_load \size, \low, \high, x7, \zero_high
	b Lx86_vector_source_loaded\@
Lx86_vector_source_cross\@:
	.if ((\scalar) != 0) && ((\size) <= 8)
	cross_page_load \size
	.if ((\single) != 0) && ((\size) == 4)
	fmov s\low, w0
	.else
	fmov d\low, x0
	.endif
	.else
	sub sp, sp, #\frame
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #\size
	helper_call uml_x86_64_gadget_cross_page_vector_load
	cbz w0, \fault
	vector_source_load \size, \low, \high, sp, \zero_high
	add sp, sp, #\frame
	.endif
Lx86_vector_source_loaded\@:
.endm

/* The out-of-line tail a `vector_memory_source` branches to on a fault. */
.macro vector_memory_source_fault label, frame=32
\label:
	add sp, sp, #\frame
	b Lx86_memory_fault
.endm

/* x86 LOCK-prefixed read-modify-write.
 *
 * A locked gadget must be atomic against the other vCPU threads that execute
 * guest code concurrently, so it performs its update with an ARM64 exclusive
 * loop instead of a plain load and store.  x86 permits a locked operand at any
 * address.  A naturally aligned operand stays inside one page, so it goes
 * straight to the exclusive path, which needs no page-crossing check.
 *
 * Anything else reaches `split_operand`.  The host implements FEAT_LSE2, which
 * the runtime requires before it runs x86 code, so it takes an exclusive
 * access at any alignment inside one 16-byte granule, and an operand that fits
 * in one rejoins the exclusive path.  The rest -- an operand across a granule
 * or a page -- takes the gadget's ordinary load and store under the
 * process-wide split lock.  That keeps it atomic against every other operand
 * on that path, which covers the same instruction running on the other vCPUs,
 * but not against an exclusive loop or a plain store that overlaps it.
 *
 * Nothing may touch memory between the exclusive load and store or the monitor
 * clears and the loop cannot make progress, so operands are read into
 * registers before the loop and flags are recorded after it.
 */
.macro locked_aligned width, fallback
	.if \width != 8
	tst x7, #(\width / 8 - 1)
	b.ne \fallback
	.endif
.endm

/* The split lock brackets the ordinary load and store of an operand that left
 * the exclusive path.  Both ends are calls, so only callee-saved registers
 * survive them; x7 is restored from the guest address in x25.  A fault in
 * between reaches `Lx86_memory_fault`, which releases the lock.
 */
.macro split_lock_enter
	mov x0, _context
	helper_call uml_x86_64_gadget_split_lock_enter
	mov x7, x25
.endm

.macro split_lock_leave
	mov x0, _context
	helper_call uml_x86_64_gadget_split_lock_leave
.endm

/* The head of a locked gadget's fallback: x7 still holds the guest address,
 * and \exclusive is the gadget's exclusive path.  x9 is clobbered.
 */
.macro split_operand size, exclusive
	add x9, x7, #((\size) - 1)
	eor x9, x9, x7
	tst x9, #0xfffffffffffffff0
	b.eq \exclusive
	split_lock_enter
.endm

.macro load_exclusive width, number, address
	.if \width == 8
	ldaxrb w\number, [\address]
	.elseif \width == 16
	ldaxrh w\number, [\address]
	.elseif \width == 32
	ldaxr w\number, [\address]
	.else
	ldaxr x\number, [\address]
	.endif
.endm

.macro store_exclusive width, status, number, address
	.if \width == 8
	stlxrb w\status, w\number, [\address]
	.elseif \width == 16
	stlxrh w\status, w\number, [\address]
	.elseif \width == 32
	stlxr w\status, w\number, [\address]
	.else
	stlxr w\status, x\number, [\address]
	.endif
.endm

.macro publish_pending_code address, size
	tbnz x23, #60, Lx86_code_published\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, Lx86_code_published\@
	mov x0, _context
	mov x1, \address
	mov w2, #\size
	helper_call uml_x86_64_gadget_publish_code_write
Lx86_code_published\@:
.endm

.macro finish_store size
	publish_pending_code x25, \size
Lx86_store_check_overlap\@:
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x25, x8
	b.hs Lx86_store_done\@
	add x9, x25, #\size
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls Lx86_store_done\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbnz w8, Lx86_store_code_exit\@
	mov x0, _context
	mov x1, x25
	helper_call uml_x86_64_gadget_invalidate_code
Lx86_store_code_exit\@:
	b Lx86_store_code_exit
Lx86_store_done\@:
	retire_and_next
.endm

/* A BlockLink contains {stream, instruction_count, guest RIP}, and the two
 * words in front of the stream it names are the target's guest {begin, end}.
 * Refuse a target whose complete translated block would cross this native
 * run's exact budget. Every executable store resets cached targets whose
 * saved instruction bytes overlap the write. At the next branch, retain the
 * native chain only when the global code sequence contains exactly this run's
 * own publication, then accept that sequence without cooling the unrelated
 * links. An overwritten target already points at its link_exit fallback.
 * A reset record therefore needs no active-state branch and translated code
 * remains immutable.
 *
 * Keep this common to the load/store and control families: a direct CALL has
 * to finish its architectural stack write before it can enter its callee.
 */
.macro follow_block_link link
	mov x19, \link
	ldrb w9, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w9, Lx86_link_code_ready\@
	mov x0, _context
	helper_call uml_x86_64_gadget_code_link_safe
	cbz w0, Lx86_link_code_exit\@
	strb wzr, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
Lx86_link_code_exit\@:
	cbnz w0, Lx86_link_code_ready\@
	ldr x9, [x19, #16]
	str x9, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_link_code_ready\@:
	ldp _stream, x11, [x19]
	adds x12, _executed, x11
	b.le Lx86_link_follow\@
	ldr x9, [x19, #16]
	str x9, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_link_follow\@:
	ldp x12, x13, [_stream, #-16]
	stp x12, x13, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	next_gadget
.endm

/* Probe the address space's shared guest-RIP tier. The entry names the block
 * that last ran at that RIP; enter it only when the recorded RIP is the one
 * just computed, so an evicted or stale entry costs a fallback. `target` must
 * not be x11, x12 or x13.
 */
.macro follow_target_link target, miss
	ldr x11, [_context, #UML_X86_64_CONTEXT_TARGET_LINKS]
	eor x12, \target, \target, lsr #UML_X86_64_TARGET_LINK_FOLD
	and x12, x12, #UML_X86_64_TARGET_LINK_MASK
	add x11, x11, x12, lsl #4
	ldp x12, x13, [x11]
	cmp x12, \target
	b.ne \miss
	follow_block_link x13
.endm

/* An IndirectLink holds two {guest RIP, BlockLink pointer} pairs. A site that
 * stays monomorphic or bimorphic settles here in two compares. Beyond that the
 * shared tier still knows the target whenever its block has run recently, so
 * only a target neither tier holds is worth a C++ round trip; that exit
 * preserves the resolved target and identifies this exact data-only site for
 * cold training. Every hit enters the same budget-checked BlockLink path as a
 * direct edge. */
.macro follow_indirect_link target, site
	ldp x11, x12, [\site]
	cmp \target, x11
	b.eq Lx86_indirect_follow\@
	ldp x11, x12, [\site, #16]
	cmp \target, x11
	b.eq Lx86_indirect_follow\@
	follow_target_link \target, Lx86_indirect_train\@
Lx86_indirect_train\@:
	str \site, [_context, #UML_X86_64_CONTEXT_INDIRECT_MISS]
	str \target, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_indirect_follow\@:
	mov x8, x12
	follow_block_link x8
.endm

/* The entry frame's own slots, after the registers gadget_host_frame.inc
 * saves, from GADGET_HOST_FRAME_SAVED:
 *   0x00  spare
 *   0x08  the string gadgets' repeat budget, a word
 *   0x10  the callee-saved halves of v8-v15, which the gadgets overwrite along
 *         with the rest of the vector file and AAPCS64 makes the caller's
 * The budget keeps the offset the string gadgets have always encoded, so
 * their bytes stay as they were. */
#define UML_X86_64_FRAME_STRING_BUDGET (GADGET_HOST_FRAME_SAVED + 0x08)
#define UML_X86_64_FRAME_VECTORS (GADGET_HOST_FRAME_SAVED + 0x10)
#define UML_X86_64_FRAME_SLOTS 0x50

/* Runs a threaded stream until a gadget leaves the engine.
 *   in:  x0 stream, x1 CpuState, x2 execution context
 *   out: w0 the event, x1 the instructions retired
 */
.p2align 4
.globl CSYM(uml_x86_64_gadget_enter)
CSYM(uml_x86_64_gadget_enter):
	gadget_host_frame_push UML_X86_64_FRAME_SLOTS
	stp d8, d9, [sp, #(UML_X86_64_FRAME_VECTORS + 0x00)]
	stp d10, d11, [sp, #(UML_X86_64_FRAME_VECTORS + 0x10)]
	stp d12, d13, [sp, #(UML_X86_64_FRAME_VECTORS + 0x20)]
	stp d14, d15, [sp, #(UML_X86_64_FRAME_VECTORS + 0x30)]
	mov _stream, x0
	mov _vcpu, x1
	mov _context, x2
	mov x9, sp
	str x9, [_context, #UML_X86_64_CONTEXT_ENTRY_STACK]
	/* Run the dispatch in the guest's rounding mode. */
	publish_guest_rounding 9, 10, 11
	/* The retirement counter carries this run's budget as a bias, so every
	 * linked edge tests the sign of one addition rather than loading the
	 * limit and comparing against it. `leave` takes the bias back out. */
	ldr x9, [_context, #UML_X86_64_CONTEXT_INSTRUCTION_LIMIT]
	neg _executed, x9
	x86_memory_order_boundary
	next_gadget

/* The instruction running raised an exception the guest unmasked, which the
 * engine does not deliver itself, and left before it committed anything: the
 * runtime runs it again, checked. A gadget can leave from inside a frame of
 * its own, so the exit takes the stack back to the entry frame, and it hands
 * the runtime its stream pointer, which is past a stream word of that
 * instruction and so names it. A computed leave enters first, to put back
 * FPSR as the instruction found it (`simd_save_status_branch`). */
.p2align 4
uml_x86_64_gadget_simd_exception_check:
	ldr x9, [_context, #UML_X86_64_CONTEXT_SIMD_STATUS]
	msr fpsr, x9
uml_x86_64_gadget_exception_check:
	ldr x9, [_context, #UML_X86_64_CONTEXT_ENTRY_STACK]
	mov sp, x9
	str _stream, [_context, #UML_X86_64_CONTEXT_EXCEPTION_STREAM]
	mov w0, #UML_X86_64_EVENT_EXCEPTION_CHECK
	b uml_x86_64_gadget_leave

.p2align 4
uml_x86_64_gadget_leave:
	/* Some ways out of a gadget that has touched guest memory come straight
	 * here -- a repeated string that runs out of budget, a fallback after a
	 * link -- so leaving the engine always orders what came before it. */
	x86_memory_order_barrier
	/* Hand the caller back its own rounding mode, which is the host
	 * default, by clearing FPCR's mode field, so entry needs no slot for a
	 * saved copy.  Every path here has sp back at the entry frame: the
	 * callee-saved registers below are restored from it. */
	mrs x10, fpcr
	ubfx x11, x10, #22, #2
	cbz x11, Lx86_leave_rounding_ready
	bic x10, x10, #(3 << 22)
	msr fpcr, x10
Lx86_leave_rounding_ready:
	ldr x9, [_context, #UML_X86_64_CONTEXT_INSTRUCTION_LIMIT]
	add x1, _executed, x9
	ldp d8, d9, [sp, #(UML_X86_64_FRAME_VECTORS + 0x00)]
	ldp d10, d11, [sp, #(UML_X86_64_FRAME_VECTORS + 0x10)]
	ldp d12, d13, [sp, #(UML_X86_64_FRAME_VECTORS + 0x20)]
	ldp d14, d15, [sp, #(UML_X86_64_FRAME_VECTORS + 0x30)]
	gadget_host_frame_pop UML_X86_64_FRAME_SLOTS
	ret

/* Materialize the lazy x86 arithmetic flags without entering C++. Common
 * arithmetic gadgets record byte width plus masked inputs/result; consumers
 * pay this work only when they actually inspect flags.
 *
 * Operation and width are both constants at the recording site, so the two
 * run-time dispatch chains they used to drive are replaced by one bounded
 * table branch into a body that has them folded in. `flags_body` is the single
 * definition of the semantics: passing zero for either parameter selects the
 * run-time form, which is what the generic table slots use for a width the
 * table does not enumerate.
 */

/* Carry-flag input for the two operations that fold in an incoming carry. */
.macro flags_adjusted_carry width, compare_left
	.if (\width) == 0
	lsl x13, x12, #1
	sub x13, x13, #1
	add x14, x8, x16
	and x14, x14, x13
	.elseif (\width) == 8
	add x14, x8, x16
	.else
	add x14, x8, x16
	and x14, x14, #((1 << ((\width) * 8)) - 1)
	.endif
	cmp x14, x8
	cset w15, lo
	.ifc \compare_left,result
	cmp x10, x7
	.else
	cmp x7, x14
	.endif
	cinc w15, w15, lo
.endm

/* Test the width's sign bit in \reg and set OF when it is set. The caller
 * places \label, so the same macro serves named and numeric local labels.
 */
.macro flags_overflow_from width, reg, label
	.if (\width) == 0
	tst \reg, x12
	b.eq \label
	.else
	tbz \reg, #((\width) * 8 - 1), \label
	.endif
	orr x9, x9, #0x800
.endm

/* The last bit a shift moved out, in bit 0 of x14: x7 is the original
 * value, x8 the masked nonzero count. A count beyond the width shifts out
 * zeros for SHL (8) and SHR (9) and the sign for SAR (10). */
.macro shift_carry_bit operation, width
	.if (\operation) == 8
	mov x14, #((\width) * 8)
	cmp x8, x14
	b.hi 91f
	sub x14, x14, x8
	lsr x14, x7, x14
	b 92f
91:
	mov x14, xzr
92:
	.elseif (\operation) == 9
	cmp x8, #((\width) * 8)
	b.hi 91f
	sub x14, x8, #1
	lsr x14, x7, x14
	b 92f
91:
	mov x14, xzr
92:
	.else
	cmp x8, #((\width) * 8)
	b.ls 91f
	mov x14, #((\width) * 8 - 1)
	b 92f
91:
	sub x14, x8, #1
92:
	lsr x14, x7, x14
	.endif
.endm

/* SHL, SHR and SAR from their record: the result defines zero, sign and
 * parity, carry is the last bit shifted out, overflow is defined for a count
 * of one and otherwise left as it was, and the auxiliary flag is untouched --
 * exactly what the eager override used to leave. */
.macro shift_flags_body operation, width
	add x6, _vcpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x13, #0xc5
	bic x9, x9, x13
	cbnz x10, 1f
	orr x9, x9, #0x40
1:
	tbz x10, #((\width) * 8 - 1), 2f
	orr x9, x9, #0x80
2:
	and w13, w10, #0xff
	eor w13, w13, w13, lsr #4
	eor w13, w13, w13, lsr #2
	eor w13, w13, w13, lsr #1
	tbnz w13, #0, 3f
	orr x9, x9, #0x4
3:
	shift_carry_bit \operation, \width
	tbz w14, #0, 4f
	orr x9, x9, #0x1
4:
	cmp x8, #1
	b.ne 5f
	bic x9, x9, #0x800
	.if (\operation) == 8
	lsr x14, x10, #((\width) * 8 - 1)
	eor x14, x14, x9
	tbz x14, #0, 5f
	orr x9, x9, #0x800
	.elseif (\operation) == 9
	tbz x7, #((\width) * 8 - 1), 5f
	orr x9, x9, #0x800
	.endif
5:
	b Lx86_flags_apply_overrides
.endm

.macro shift_carry_body operation, width, base=UML_X86_64_CPU_DEFERRED_LEFT, exit=Lx86_carry_apply_overrides, load_flags=1
	add x6, _vcpu, #\base
	ldp x7, x8, [x6]
	.if \load_flags
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	.endif
	bic x9, x9, #0x1
	shift_carry_bit \operation, \width
	tbz w14, #0, 4f
	orr x9, x9, #0x1
4:
	b \exit
.endm

/* Narrow the deferred record's left, right and result, which x7, x8 and x10
 * hold, to the width that produced them. A width of zero is the run-time
 * form: it reads the width from the record at x6 and leaves its sign mask
 * in x12. */
.macro deferred_narrow width
	.if (\width) == 1
	and x7, x7, #0xff
	and x8, x8, #0xff
	and x10, x10, #0xff
	.elseif (\width) == 2
	uxth w7, w7
	uxth w8, w8
	uxth w10, w10
	.elseif (\width) == 4
	mov w7, w7
	mov w8, w8
	mov w10, w10
	.elseif (\width) == 0
	ldrb w11, [x6, #25]
	cmp w11, #1
	b.ne Lx86_narrow_half\@
	and x7, x7, #0xff
	and x8, x8, #0xff
	and x10, x10, #0xff
	mov x12, #0x80
	b Lx86_narrow_done\@
Lx86_narrow_half\@:
	cmp w11, #2
	b.ne Lx86_narrow_word\@
	uxth w7, w7
	uxth w8, w8
	uxth w10, w10
	mov x12, #0x8000
	b Lx86_narrow_done\@
Lx86_narrow_word\@:
	cmp w11, #4
	b.ne Lx86_narrow_quad\@
	mov w7, w7
	mov w8, w8
	mov w10, w10
	mov x12, #0x80000000
	b Lx86_narrow_done\@
Lx86_narrow_quad\@:
	mov x12, #1
	lsl x12, x12, #63
Lx86_narrow_done\@:
	.endif
.endm

.macro flags_body operation, width
	add x6, _vcpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	.if (\operation) == 0
	ldrb w16, [x6, #26]
	.elseif ((\operation) == 6) || ((\operation) == 7)
	ldrb w16, [x6, #26]
	.endif
	deferred_narrow \width
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	/* INC and DEC preserve CF; every other recorded operation defines it. */
	.if (\operation) == 0
	cmp w5, #4
	b.lo 41f
	cmp w5, #5
	b.hi 41f
	mov x13, #0x8d4
	b 42f
41:
	mov x13, #0x8d5
42:
	.elseif ((\operation) == 4) || ((\operation) == 5)
	mov x13, #0x8d4
	.else
	mov x13, #0x8d5
	.endif
	bic x9, x9, x13
	cbnz x10, 5f
	orr x9, x9, #0x40
5:
	.if (\width) == 0
	tst x10, x12
	b.eq 6f
	.else
	tbz x10, #((\width) * 8 - 1), 6f
	.endif
	orr x9, x9, #0x80
6:
	and w13, w10, #0xff
	eor w13, w13, w13, lsr #4
	eor w13, w13, w13, lsr #2
	eor w13, w13, w13, lsr #1
	tbnz w13, #0, 7f
	orr x9, x9, #0x4
7:
	.if (\operation) == 0
	cmp w5, #1
	b.eq Lx86_flags_add
	cmp w5, #4
	b.eq Lx86_flags_add_preserve_carry
	cmp w5, #6
	b.eq Lx86_flags_add_carry
	cmp w5, #2
	b.eq Lx86_flags_sub
	cmp w5, #5
	b.eq Lx86_flags_sub_preserve_carry
	cmp w5, #7
	b.eq Lx86_flags_sub_borrow
	b Lx86_flags_apply_overrides

Lx86_flags_add:
	cmp x10, x7
	b.hs Lx86_flags_add_preserve_carry
	orr x9, x9, #0x1
Lx86_flags_add_preserve_carry:
	eor x13, x7, x8
	mvn x13, x13
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from 0, x13, Lx86_flags_add_aux
Lx86_flags_add_aux:
	b Lx86_flags_auxiliary

Lx86_flags_add_carry:
	flags_adjusted_carry 0, result
	cbz w15, Lx86_flags_add_preserve_carry
	orr x9, x9, #0x1
	b Lx86_flags_add_preserve_carry

Lx86_flags_sub:
	cmp x7, x8
	b.hs Lx86_flags_sub_preserve_carry
	orr x9, x9, #0x1
Lx86_flags_sub_preserve_carry:
	eor x13, x7, x8
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from 0, x13, Lx86_flags_sub_aux
Lx86_flags_sub_aux:
	b Lx86_flags_auxiliary

Lx86_flags_sub_borrow:
	flags_adjusted_carry 0, right
	cbz w15, Lx86_flags_sub_preserve_carry
	orr x9, x9, #0x1
	b Lx86_flags_sub_preserve_carry

Lx86_flags_auxiliary:
	eor x13, x7, x8
	eor x13, x13, x10
	tbz x13, #4, Lx86_flags_auxiliary_done
	orr x9, x9, #0x10
Lx86_flags_auxiliary_done:
	sub w13, w5, #4
	cmp w13, #1
	b.hi Lx86_flags_apply_overrides
	ldrb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, Lx86_flags_apply_overrides
	adr x17, Lx86_flags_apply_overrides
	b Lx86_slot_carry
	.else
	/* Carry, overflow and auxiliary for one known operation. */
	.if (\operation) == 1
	cmp x10, x7
	b.hs 81f
	orr x9, x9, #0x1
81:
	.elseif (\operation) == 2
	cmp x7, x8
	b.hs 81f
	orr x9, x9, #0x1
81:
	.elseif (\operation) == 6
	flags_adjusted_carry \width, result
	cbz w15, 81f
	orr x9, x9, #0x1
81:
	.elseif (\operation) == 7
	flags_adjusted_carry \width, right
	cbz w15, 81f
	orr x9, x9, #0x1
81:
	.endif
	.if (\operation) != 3
	.if ((\operation) == 1) || ((\operation) == 4) || ((\operation) == 6)
	eor x13, x7, x8
	mvn x13, x13
	.else
	eor x13, x7, x8
	.endif
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from \width, x13, 82f
82:
	eor x13, x7, x8
	eor x13, x13, x10
	tbz x13, #4, 83f
	orr x9, x9, #0x10
83:
	.endif
	.if ((\operation) == 4) || ((\operation) == 5)
	/* The preserved carry may still be pending in the slot. */
	ldrb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, Lx86_flags_apply_overrides
	adr x17, Lx86_flags_apply_overrides
	b Lx86_slot_carry
	.endif
	b Lx86_flags_apply_overrides
	.endif
.endm

.p2align 4
uml_x86_64_gadget_materialize_flags:
	ldrb w5, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbnz w5, Lx86_flags_dispatch
	ldrh w13, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, 9f
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	b Lx86_flags_apply_overrides
Lx86_flags_dispatch:
	/* Masking both keys keeps the branch inside the table whatever the
	 * recorded bytes hold; the slots those values reach are the generic body.
	 */
	ldrb w11, [_vcpu, #UML_X86_64_CPU_DEFERRED_WIDTH]
	adr x14, Lx86_flags_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
Lx86_flags_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b Lx86_flags_o\op\()_w\w
	.else
	b Lx86_flags_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
Lx86_flags_o\op\()_w\w:
	flags_body \op, \w
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
Lx86_flags_o\op\()_w\w:
	shift_flags_body \op, \w
.endr
.endr

Lx86_flags_generic:
	flags_body 0, 0

Lx86_flags_apply_overrides:
	ldrh w13, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, Lx86_flags_store
	ldrh w14, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic x9, x9, x13
	and x14, x14, x13
	orr x9, x9, x14
Lx86_flags_store:
	orr x9, x9, #0x2
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	str xzr, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
9:
	ret

/* Carry-only materialization. INC and DEC preserve CF and define the other
 * five arithmetic flags from their own record; ADC and SBB read CF as an
 * operand and then define all six from theirs. Every one of them therefore
 * needs exactly one correct bit out of RFLAGS, and the full routine's zero,
 * sign, parity, auxiliary and overflow work is overwritten the moment the
 * caller records. This entry point computes carry alone from the same
 * pending record, through the same operation-and-width table, and leaves the
 * five dead bits as they are.
 *
 * It is only for callers that record before any guest memory access can
 * fault. Between this routine and the caller's own record the five stale bits
 * are architecturally visible, so a gadget that can leave in that window --
 * every read-modify-write form -- keeps the full routine.
 */
.macro carry_body operation, width, base=UML_X86_64_CPU_DEFERRED_LEFT, exit=Lx86_carry_apply_overrides, load_flags=1
	.if ((\operation) == 4) || ((\operation) == 5)
	/* INC and DEC preserve carry, which may still be pending in the slot.
	 * The slot itself never holds one of their records. */
	.if \load_flags
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	ldrb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, \exit
	adr x17, \exit
	b Lx86_slot_carry
	.else
	b \exit
	.endif
	.elseif (\operation) == 3
	/* Logical operations clear carry. */
	.if \load_flags
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	.endif
	bic x9, x9, #0x1
	b \exit
	.else
	add x6, _vcpu, #\base
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	.if ((\operation) == 0) || ((\operation) == 6) || ((\operation) == 7)
	ldrb w16, [x6, #26]
	.endif
	deferred_narrow \width
	.if \load_flags
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	.endif
	.if (\operation) == 0
	/* INC, DEC and an unrecognized operation reach this body with a
	 * run-time key; only the four defining operations clear carry. */
	cmp w5, #4
	b.lo 15f
	cmp w5, #5
	b.hi 15f
	.if \load_flags
	ldrb w16, [_vcpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, \exit
	adr x17, \exit
	b Lx86_slot_carry
	.else
	b \exit
	.endif
15:
	bic x9, x9, #0x1
	cmp w5, #1
	b.eq 16f
	cmp w5, #2
	b.eq 17f
	cmp w5, #6
	b.eq 18f
	cmp w5, #7
	b.eq 19f
	b \exit
16:
	cmp x10, x7
	b.hs \exit
	orr x9, x9, #0x1
	b \exit
17:
	cmp x7, x8
	b.hs \exit
	orr x9, x9, #0x1
	b \exit
18:
	flags_adjusted_carry 0, result
	cbz w15, \exit
	orr x9, x9, #0x1
	b \exit
19:
	flags_adjusted_carry 0, right
	cbz w15, \exit
	orr x9, x9, #0x1
	b \exit
	.else
	bic x9, x9, #0x1
	.if (\operation) == 1
	cmp x10, x7
	b.hs \exit
	orr x9, x9, #0x1
	.elseif (\operation) == 2
	cmp x7, x8
	b.hs \exit
	orr x9, x9, #0x1
	.elseif (\operation) == 6
	flags_adjusted_carry \width, result
	cbz w15, \exit
	orr x9, x9, #0x1
	.elseif (\operation) == 7
	flags_adjusted_carry \width, right
	cbz w15, \exit
	orr x9, x9, #0x1
	.endif
	b \exit
	.endif
	.endif
.endm

.p2align 4
uml_x86_64_gadget_materialize_carry:
	ldrb w5, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbnz w5, Lx86_carry_dispatch
	ldrh w13, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, 9f
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	b Lx86_carry_apply_overrides
Lx86_carry_dispatch:
	ldrb w11, [_vcpu, #UML_X86_64_CPU_DEFERRED_WIDTH]
	adr x14, Lx86_carry_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
Lx86_carry_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b Lx86_carry_o\op\()_w\w
	.else
	b Lx86_carry_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
Lx86_carry_o\op\()_w\w:
	carry_body \op, \w
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
Lx86_carry_o\op\()_w\w:
	shift_carry_body \op, \w
.endr
.endr

Lx86_carry_generic:
	carry_body 0, 0

Lx86_carry_apply_overrides:
	ldrh w13, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, Lx86_carry_store
	ldrh w14, [_vcpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic x9, x9, x13
	and x14, x14, x13
	orr x9, x9, x14
Lx86_carry_store:
	orr x9, x9, #0x2
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	str xzr, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
9:
	ret

/* The carry a pending INC or DEC record preserves, from the record in the
 * carry slot: the same operation-and-width table over the slot, computing
 * into the flags the caller is building in x9 and returning through x17. Only
 * the slot record's carry override applies; its other flags died when the
 * INC or DEC recorded. Both materializers reach this with x5 to x16 free.
 */
.p2align 4
Lx86_slot_carry:
	ldrb w5, [_vcpu, #(UML_X86_64_CPU_CARRY_SLOT + 24)]
	ldrb w11, [_vcpu, #(UML_X86_64_CPU_CARRY_SLOT + 25)]
	adr x14, Lx86_slot_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
Lx86_slot_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b Lx86_slot_o\op\()_w\w
	.else
	b Lx86_slot_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
Lx86_slot_o\op\()_w\w:
	carry_body \op, \w, UML_X86_64_CPU_CARRY_SLOT, Lx86_slot_apply_overrides, 0
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
Lx86_slot_o\op\()_w\w:
	shift_carry_body \op, \w, UML_X86_64_CPU_CARRY_SLOT, Lx86_slot_apply_overrides, 0
.endr
.endr

Lx86_slot_generic:
	carry_body 0, 0, UML_X86_64_CPU_CARRY_SLOT, Lx86_slot_apply_overrides, 0

Lx86_slot_apply_overrides:
	ldrh w13, [_vcpu, #(UML_X86_64_CPU_CARRY_SLOT + 28)]
	tbz w13, #0, 9f
	ldrh w14, [_vcpu, #(UML_X86_64_CPU_CARRY_SLOT + 30)]
	bfi x9, x14, #0, #1
9:
	br x17


gadget block_exit
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

/* The same block end, for a block the fetch window cut short rather than a
 * branch. Its successor is the next address, which is as statically known as a
 * direct jump's target, so it is followed the same way. Falling off the end of
 * a block retires no instruction, which is why this does not advance the
 * retirement count the way `jump_linked` does for the jump it executes.
 */
gadget block_exit_linked
	ldr x8, [_stream]
	follow_block_link x8

gadget nop
	retire_and_next

/* Read the vector register at state offset x10 into d0 for 64 bits, or into
 * q0, and q2 for the upper half of 256 bits. */
.macro vector_read width
	.if \width == 64
	ldr d0, [_vcpu, x10]
	.else
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	.endif
.endm

/* Read the two source registers that bits 31:16 and 47:32 of descriptor x8
 * address into q0 and q1, and their upper halves into q2 and q3 for 256
 * bits; x10 and x11 are left holding their addresses. */
.macro vector_read_pair width
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q2, [x10, #16]
	ldr q3, [x11, #16]
	.endif
.endm

/* The TBL indices in v6 for PSHUFD and VPERMILPS: destination dword n takes
 * the source dword that bits 2n+1:2n of the immediate in w13 name. */
.macro vector_shuffle_dword_indices
	mov w16, #0x0101
	movk w16, #0x0101, lsl #16
	mov w17, #0x0100
	movk w17, #0x0302, lsl #16
	.irp lane, 0, 1, 2, 3
	lsr w14, w13, #(\lane * 2)
	and w14, w14, #3
	lsl w14, w14, #2
	mul w14, w14, w16
	add w14, w14, w17
	mov v6.s[\lane], w14
	.endr
.endm

/* Write the vector result in q4 (and q5 for 256 bits) to the register the
 * descriptor's low half addresses. A 128-bit VEX write, which bit 63 marks,
 * zeroes the upper half; a legacy SSE write leaves it. */
.macro vector_write_xmm width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	tbz x\descriptor, #63, Lx86_vector_write_done\@
	movi v6.16b, #0
	str q6, [x9, #16]
Lx86_vector_write_done\@:
	.endif
.endm

/* An MMX write, as on x86, also stores `value` into the x87 register the MMX
 * register at state offset `offset` aliases, with an all-ones exponent, and
 * clears the x87 tag word and top. `index`, `entry` and `tag` are scratch. */
.macro synchronize_mmx offset, value, index=x10, entry=x11, tag=w12
	sub \index, \offset, #UML_X86_64_CPU_MMX
	lsr \index, \index, #3
	add \entry, _vcpu, #UML_X86_64_CPU_X87
	add \entry, \entry, \index, lsl #4
	str \value, [\entry]
	mov \tag, #0xffff
	strh \tag, [\entry, #8]
	strh wzr, [_vcpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_vcpu, #UML_X86_64_CPU_X87_TOP]
.endm

/* vector_write_xmm, or for a 64-bit width an MMX write of d4. */
.macro vector_write width, descriptor
	.if \width == 64
	and x9, x\descriptor, #0xffff
	str d4, [_vcpu, x9]
	synchronize_mmx x9, d4
	.else
	vector_write_xmm \width, \descriptor
	.endif
.endm

/* Lane helpers of the three-operand map gadgets: the byte index sequence
 * 0..15, and a lane mask from bits `first` upward of w13 or x13. */
.macro vector_map3_index_sequence output
	mov x14, #0x0100
	movk x14, #0x0302, lsl #16
	movk x14, #0x0504, lsl #32
	movk x14, #0x0706, lsl #48
	fmov d\output, x14
	mov x14, #0x0908
	movk x14, #0x0b0a, lsl #16
	movk x14, #0x0d0c, lsl #32
	movk x14, #0x0f0e, lsl #48
	mov v\output\().d[1], x14
.endm
.macro vector_map3_mask_s output, first
	movi v\output\().16b, #0
	.irp lane, 0, 1, 2, 3
	ubfx w14, w13, #(\first + \lane), #1
	neg w14, w14
	mov v\output\().s[\lane], w14
	.endr
.endm
.macro vector_map3_mask_d output, first
	movi v\output\().16b, #0
	.irp lane, 0, 1
	ubfx x14, x13, #(\first + \lane), #1
	neg x14, x14
	mov v\output\().d[\lane], x14
	.endr
.endm
