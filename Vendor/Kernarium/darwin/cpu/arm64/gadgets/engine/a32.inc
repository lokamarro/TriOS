/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The AArch32 part of the gadget region: A32 and T32 instructions as the
 * shared decoder (cpu/arm64/aarch32/decode.hpp) reads them, lowered onto
 * general registers. It has a bank, a cold section and an island of its own
 * after the SME part, so no earlier part moves for it, and it holds no SVE or
 * SME instruction: sme.inc has turned those extensions off.
 *
 * State: R0-R14 are CpuState::registers[0..14], each a 64-bit slot that
 * holds the register zero-extended: the runtime loads it so, and a write is
 * always a 32-bit store. registers[15] and registers[16] are the part's own
 * scratch slots, where the builder puts the value of PC read as an operand
 * (PC is not a register here) and the address of a T32 exclusive transfer
 * with an offset; every entry clears them, because the A64 part's exclusive
 * and ordered gadgets read an address slot whole (a32_layout.h). NZCV is the guest's, in x27, as everywhere in the engine; Q, GE and
 * ITSTATE are in CpuState (a32_q, a32_ge_mask, a32_itstate).
 *
 * The PC of a translated stream is a key (cpu/arm64/aarch32/guest_key.hpp):
 * every PC word a gadget stores, and every guest PC in a BlockLink, is the
 * key of an A32 or T32 instruction. The runtime converts it on the way out.
 * A gadget that can leave the block with the instruction not retired (an
 * undefined instruction, VLDM, VSTM, a structure transfer) has the key of
 * that instruction in the stream word after its operands, which
 * uml_arm64_gadget_memory_fault and uml_arm64_gadget_illegal read through
 * _stream. The other memory gadgets carry the instruction's position in the
 * block in their operand instead (a32_layout.h; the exclusive and ordered
 * transfers, which are the A64 part's gadgets, in its top byte), and
 * finish_a32_block turns the position a fault reports into the key.
 *
 * Instruction conditions: a conditional instruction is preceded by an
 * a32_skip_<cc> gadget whose operand is the byte distance to skip over the
 * instruction's stream words when the condition fails, so the instruction
 * itself is the unconditional gadget. A run of instructions under one
 * condition that set no flags share one skip. A run of two to nine words
 * takes a32_skip_<cc>_<words> instead, which holds the distance and has no
 * operand. An instruction that leaves the block while others are translated
 * behind it in the same block is preceded by a32_credit, which gives back the
 * count of instructions behind it.
 *
 * The part's gadgets are generated at build time by engine/generate_a32.py
 * (a32_generated.inc) beside this file's hand-written control and system
 * gadgets. */

.pushsection GADGET_SECTION_A32_BANK

/* sme.inc named its gadgets for the SME part; these are ordinary entries. */
.purgem gadget
.macro gadget name
	.globl CSYM(uml_arm64_gadget_\name)
	.p2align 4
CSYM(uml_arm64_gadget_\name):
.endm

/* The part's helper calls: a helper the base parts already call is reached
 * through the base island's veneer, whose address is fixed; one they do not
 * gets a veneer at the end of this part's own island, so the first island
 * never grows. */
.purgem helper_call
.macro helper_call symbol, cpu=0
	.ifdef Lveneer_\symbol
	bl Lveneer_\symbol
	.else
	.ifndef La32veneer_\symbol
	.pushsection GADGET_SECTION_A32_VENEER
La32veneer_\symbol:
	b _\symbol
	.popsection
	.endif
	bl La32veneer_\symbol
	.endif
.endm

#include "../a32_layout.h"

/* The part's TLB probe: transfer_translate and its cold half, which key a
 * multi-byte access by the page of its final byte, so a crossing transfer
 * only misses and the cold half recognizes it. */
.macro a32_translate access, page_offset, bytes, address, cold, resume, cross, forward=0, fault=uml_arm64_gadget_memory_fault
	transfer_translate \access, \page_offset, \bytes, \address, \cold, \resume, \cross, \forward, \fault
.endm

.macro a32_translate_cold access, page_offset, bytes, address, cold, resume, cross, forward=0, outofline=0, fault=uml_arm64_gadget_memory_fault
	transfer_translate_cold \access, \page_offset, \bytes, \address, \cold, \resume, \cross, \forward, \outofline, \fault
.endm

/* The native floating-point environment holds the guest's FPCR, or the
 * Standard FPSCR value that Advanced SIMD floating point runs under (flush to
 * zero, default NaN, round to nearest; AHP and FZ16 from the guest);
 * CpuState::a32_fp_standard says which. A gadget that needs one switches to
 * it only when the environment is not in it already, so a run of
 * instructions of one kind, across blocks, writes the FPCR once. Each macro
 * activates the environment first when it is not active, and clobbers only
 * x5, x6 and lr, as ensure_fp_environment does. */
.macro a32_fp_guest
	ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w6, La32fpinactive\@
	ldrb w6, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]
	cbz w6, La32fpdone\@
	bl La32_fp_to_guest
	b La32fpdone\@
La32fpinactive\@:
	bl uml_arm64_gadget_activate_native_fp_environment
	strb wzr, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]
La32fpdone\@:
.endm

.macro a32_fp_standard
	ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w6, La32fpinactive\@
	ldrb w6, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]
	cbnz w6, La32fpdone\@
	b La32fpswitch\@
La32fpinactive\@:
	bl uml_arm64_gadget_activate_native_fp_environment
La32fpswitch\@:
	bl La32_fp_to_standard
La32fpdone\@:
.endm

/* Runs one instruction through the AArch32 semantic library
 * (uml_arm64_gadget_a32_execute): x21 is the instruction's key, or a block
 * transfer's position as a32_position_pc forms it, x8 the library operand
 * (a32_layout.h) and _stream is past the instruction's stream words. The
 * guest's NZCV, kept in x27, goes to CpuState before the call and comes back
 * after it; everything else the base profile touches is in CpuState already.
 * A fault leaves through the memory-fault exit with the fault in the TLB, an
 * UNDEFINED instruction through the illegal-instruction exit, an internal
 * error as the fatal event. */
.macro a32_library_call
	str w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	/* The live FPSR is the host register while the native environment is
	 * active: commit it to CpuState for the library, which merges the flags
	 * it raises into CpuState, and reinstall it after (fpsr_model_helper). */
	ldrb w16, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w16, 96f
	mrs x17, fpsr
	str w17, [_vcpu, #UML_GADGET_CPU_FPSR]
96:
	mov x19, x1
	mov x20, x2
	mov x0, x1
	mov x1, x2
	mov x2, x8
	mov x3, x21
	helper_call uml_arm64_gadget_a32_execute
	mov x1, x19
	mov x2, x20
	ldrb w16, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w16, 95f
	ldr w17, [_vcpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x17
95:
	ldr w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	cbz w0, 97f
	cmp w0, #UML_GADGET_A32_LIBRARY_FAULT
	b.eq La32_memory_fault
	cmp w0, #UML_GADGET_A32_LIBRARY_UNDEFINED
	b.eq La32_undefined
	sub x9, _stream, #8
	str x9, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x21, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #-1
	b uml_arm64_gadget_leave
97:
.endm

/* x10 is the target key and x9 the site's cells: follow the cell that holds
 * the key, else (for a return) the newest return record, else the address
 * space's target table, else leave with the key as the PC and the site in
 * the exit scratch for the executor to train. The key of a PC-misaligned A32
 * target has bit 1 set and matches no cell. */
.macro a32_follow_indirect ras=0
	ldp x11, x12, [x9]
	cmp x10, x11
	b.eq 98f
	ldp x11, x12, [x9, #16]
	cmp x10, x11
	b.eq 98f
	.if \ras
	ldrb w13, [_vcpu, #UML_GADGET_CPU_RETURN_TOP]
	sub w13, w13, #1
	and w13, w13, #UML_GADGET_CPU_RETURN_MASK
	add x14, _vcpu, #UML_GADGET_CPU_RETURN_STACK
	add x14, x14, x13, lsl #4
	ldp x11, x12, [x14]
	cmp x10, x11
	b.ne 97f
	strb w13, [_vcpu, #UML_GADGET_CPU_RETURN_TOP]
	train_indirect_cell
	b 98f
97:
	.endif
	b La32_target_tier\@
98:
	follow_block_link x12
	.pushsection GADGET_SECTION_A32_COLD
La32_target_tier\@:
	ldr x13, [_vcpu, #UML_GADGET_CPU_TARGET_TIER]
	cbz x13, La32_target_tier_miss\@
	eor x14, x10, x10, lsr #10
	ubfx x14, x14, #2, #UML_GADGET_TARGET_TIER_BITS
	add x13, x13, x14, lsl #4
	ldp x11, x12, [x13]
	cmp x10, x11
	b.ne La32_target_tier_miss\@
	train_indirect_cell
	follow_block_link x12
La32_target_tier_miss\@:
	str x9, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
	.popsection
.endm

/* x10 := the key a BX-style branch to the 32-bit value in w10 reaches: bit 0
 * selects T32 (BXWritePC), the address is the value without it. An A32
 * target keeps bit 1, so a misaligned one stays misaligned. */
.macro a32_key_interworking
	lsl x13, x10, #1
	and x13, x13, #0x1fffffffc
	orr x13, x13, #0xc000000000000000
	orr x14, x10, #0x8000000000000000
	tst w10, #1
	csel x10, x13, x14, ne
.endm

/* x10 := the T32 key of the halfword-aligned address in w10, bit 0 ignored
 * (BranchWritePC in Thumb state). */
.macro a32_key_thumb
	lsl x13, x10, #1
	and x13, x13, #0x1fffffffc
	orr x10, x13, #0xc000000000000000
.endm

/* A condition, as an NZCV test of x27: skip the number of stream bytes its
 * operand holds when it does not hold. */
.macro a32_skip name, inverse
gadget a32_skip_\name
	ldr x8, [_stream], #8
	msr nzcv, x27
	b.\inverse 1f
	next_gadget
1:
	add _stream, _stream, x8
	next_gadget
	/* The same with the distance in the gadget, for the runs of two to
	 * nine words (Translator::close_run), which carry no operand. */
	.irp words, 2, 3, 4, 5, 6, 7, 8, 9
gadget a32_skip_\name\()_\words
	msr nzcv, x27
	b.\inverse 1f
	next_gadget
1:
	add _stream, _stream, #(8 * \words)
	next_gadget
	.endr
.endm

a32_skip eq, ne
a32_skip ne, eq
a32_skip cs, cc
a32_skip cc, cs
a32_skip mi, pl
a32_skip pl, mi
a32_skip vs, vc
a32_skip vc, vs
a32_skip hi, ls
a32_skip ls, hi
a32_skip ge, lt
a32_skip lt, ge
a32_skip gt, le
a32_skip le, gt

.purgem a32_skip

/* The instructions translated behind a leaving instruction in the same block
 * were precharged and will not run: give them back. */
gadget a32_credit
	ldr x8, [_stream], #8
	sub x29, x29, x8
	next_gadget

/* A block that ends inside an IT block records where it stopped, so the
 * next entry runs the rest an instruction at a time. */
gadget a32_set_itstate
	ldr x8, [_stream], #8
	strb w8, [_vcpu, #UML_ARM64_CPU_A32_ITSTATE]
	next_gadget

/* \value := the value LR gets for a call that returns to \key: the return
 * address with the Thumb bit. An A32 key's low word is the address, and a
 * T32 key holds the halfword address shifted left one bit under bit 62
 * (aarch32/guest_key.hpp). Scratch x13. */
.macro a32_return_value value, valuex, key
	ubfx x13, \key, #62, #1
	lsr \valuex, \key, x13
	orr \value, \value, w13
.endm

/* BL and BLX (immediate): [target BlockLink][return BlockLink]. LR comes
 * from the return link's key, so the stream carries no LR value. */
gadget a32_bl_linked
	ldp x8, x11, [_stream]
	ldr x9, [x11, #16]
	a32_return_value w12, x12, x9
	str w12, [_vcpu, #(UML_GADGET_CPU_X0 + 14 * 8)]
	push_return_record x9, x11
	follow_block_link x8

/* The unlinked form, for a translation with no link table: the target key,
 * the LR value. */
gadget a32_bl
	ldr x8, [_stream], #8
	ldr x12, [_stream]
	str w12, [_vcpu, #(UML_GADGET_CPU_X0 + 14 * 8)]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* BX Rm and its relatives: [source register offset][site] for a cached
 * branch, [source register offset] for an uncached one. The _return forms
 * also consult the return ring. */
.macro a32_indirect name, key, ras, cached
gadget a32_\name
	ldr x8, [_stream], #8
	.if \cached
	ldr x9, [_stream], #8
	.endif
	ldr w10, [_vcpu, x8]
	\key
	.if \cached
	a32_follow_indirect \ras
	.else
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
	.endif
.endm

a32_indirect bx_cached, a32_key_interworking, 0, 1
a32_indirect bx_return_cached, a32_key_interworking, 1, 1
a32_indirect bx, a32_key_interworking, 0, 0
a32_indirect bw_cached, a32_key_thumb, 0, 1
a32_indirect bw_return_cached, a32_key_thumb, 1, 1
a32_indirect bw, a32_key_thumb, 0, 0

.purgem a32_indirect

/* BLX Rm: the target is read before LR is written. [source offset][site]
 * [return BlockLink] cached, LR coming from the return link's key as for
 * BL; [source offset][LR value] not. */
gadget a32_blx_cached
	ldp x8, x9, [_stream], #16
	ldr w10, [_vcpu, x8]
	a32_key_interworking
	ldr x14, [_stream]
	ldr x11, [x14, #16]
	a32_return_value w15, x15, x11
	str w15, [_vcpu, #(UML_GADGET_CPU_X0 + 14 * 8)]
	push_return_record x11, x14
	a32_follow_indirect 0

gadget a32_blx
	ldr x8, [_stream], #8
	ldr x15, [_stream]
	ldr w10, [_vcpu, x8]
	a32_key_interworking
	str w15, [_vcpu, #(UML_GADGET_CPU_X0 + 14 * 8)]
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* A PC value as an operand: the builder computes it, and a gadget that reads
 * PC reads it from the scratch slot it names. [slot offset][value] */
gadget a32_set_scratch
	ldp x8, x9, [_stream], #16
	str w9, [_vcpu, x8]
	next_gadget

/* SVC and BKPT leave with the key stream word as the exit scratch too, so
 * the C++ boundary can find the block and, in a T32 IT block, recover
 * ITSTATE. [key] */
gadget a32_svc
	ldr x8, [_stream]
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_SYSCALL
	b uml_arm64_gadget_leave

gadget a32_bkpt
	ldr x8, [_stream]
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_BREAKPOINT
	b uml_arm64_gadget_leave

/* CBZ and CBNZ: [rn offset][taken BlockLink][suffix] with the fall-through
 * translated inline, and [rn offset][taken][fall-through] when the block
 * ends here. */
.macro a32_compare_branch name, condition
gadget a32_\name\()_inline
	ldp x8, x19, [_stream], #16
	ldr x20, [_stream], #8
	ldr w9, [_vcpu, x8]
	cmp w9, #0
	b.\condition 1f
	next_gadget
1:
	sub x29, x29, x20
	follow_block_link x19

gadget a32_\name\()_linked
	ldp x8, x19, [_stream], #16
	ldr x20, [_stream]
	ldr w9, [_vcpu, x8]
	cmp w9, #0
	csel x8, x19, x20, \condition
	follow_block_link x8

gadget a32_\name\()_plain
	ldp x8, x19, [_stream], #16
	ldr x20, [_stream]
	ldr w9, [_vcpu, x8]
	cmp w9, #0
	csel x10, x19, x20, \condition
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

a32_compare_branch cbz, eq
a32_compare_branch cbnz, ne

.purgem a32_compare_branch

/* TBB and TBH: [rn[0:8] rm[8:16] credit[16:24] position[24:32] PC value
 * [32:64]][site]. The target is the PC value plus twice the table entry. The
 * credit gives back the instructions translated behind a conditional one, as
 * a32_credit does for the other leaving instructions, but only once the table
 * entry is loaded: a fault leaves them precharged, which the executor's fault
 * path expects. */
.macro a32_table_branch name, load, shift, bytes
gadget a32_\name
	ldr x19, [_stream], #8
	and x9, x19, #0xff
	ldr w9, [_vcpu, x9]
	ubfx x10, x19, #8, #8
	ldr w10, [_vcpu, x10]
	add w7, w9, w10, lsl #\shift
	a32_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, La32tb_cold_\name, La32tb_resume_\name, La32tb_cross_\name, 0, fault=La32_position_fault
	\load w10, [x7]
	ubfx x11, x19, #16, #8
	sub x29, x29, x11
	lsr x11, x19, #32
	add w10, w11, w10, lsl #1
	a32_key_thumb
	ldr x9, [_stream], #8
	a32_follow_indirect 0
	a32_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, La32tb_cold_\name, La32tb_resume_\name, La32tb_cross_\name, 0, fault=La32_position_fault
	.if \shift
La32tb_cross_\name:
	transfer_slow 0, x7, 2, fault=La32_position_fault
	umov w10, v0.h[0]
	ubfx x11, x19, #16, #8
	sub x29, x29, x11
	lsr x11, x19, #32
	add w10, w11, w10, lsl #1
	a32_key_thumb
	ldr x9, [_stream], #8
	a32_follow_indirect 0
	.endif
.endm

a32_table_branch tbb, ldrb, 0, 1
a32_table_branch tbh, ldrh, 1, 2

.purgem a32_table_branch

/* An instruction the engine does not lower: the semantic library runs it.
 * [key][library operand] */
gadget a32_library
	ldp x21, x8, [_stream], #16
	a32_library_call
	next_gadget

/* Switch the active native environment to the Standard FPSCR value, skipping
 * the MSR when the guest's FPCR already is that value, and to the guest's
 * FPCR. x5 and x6 only. */
.p2align 4
La32_fp_to_standard:
	ldr w6, [_vcpu, #UML_GADGET_CPU_FPCR]
	movz w5, #0x0408, lsl #16
	and w5, w6, w5
	orr w5, w5, #0x03000000
	cmp w5, w6
	b.eq 1f
	msr fpcr, x5
1:	mov w5, #1
	strb w5, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]
	ret

.p2align 4
La32_fp_to_guest:
	ldr w6, [_vcpu, #UML_GADGET_CPU_FPCR]
	msr fpcr, x6
	strb wzr, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]
	ret

/* Faults the memory gadgets share: the key of the instruction is in x21 and
 * the stream is past the operand. A gadget without a key enters at the
 * La32_position_ labels with its operand word in x19, whose position
 * (a32_layout.h) becomes the PC: shifted left by two with bit 0 set, which
 * no key has. */
.macro a32_position_pc
	ubfx x21, x19, #UML_GADGET_A32_POSITION_SHIFT, #8
	lsl x21, x21, #2
	orr x21, x21, #1
.endm

.p2align 4
La32_position_fault:
	a32_position_pc
La32_memory_fault:
	sub x9, _stream, #8
	str x9, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x21, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave

La32_undefined:
	sub x9, _stream, #8
	str x9, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x21, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_ILLEGAL
	b uml_arm64_gadget_leave

.p2align 4
La32_position_alignment_fault_read:
	a32_position_pc
La32_alignment_fault_read:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #0
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b La32_memory_fault

.p2align 4
La32_position_alignment_fault_write:
	a32_position_pc
La32_alignment_fault_write:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #1
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b La32_memory_fault

/* MRS Rd, APSR: N, Z, C and V from x27, Q, GE[3:0] gathered from the
 * byte-lane mask, and PSTATE.M, which reads 0b10000 (User) at EL0.
 * [rd offset] */
gadget a32_mrs
	ldr x8, [_stream], #8
	and w9, w27, #0xf0000000
	ldrb w10, [_vcpu, #UML_ARM64_CPU_A32_Q]
	orr w9, w9, w10, lsl #27
	ldr w10, [_vcpu, #UML_ARM64_CPU_A32_GE_MASK]
	and w11, w10, #1
	ubfx w12, w10, #8, #1
	orr w11, w11, w12, lsl #1
	ubfx w12, w10, #16, #1
	orr w11, w11, w12, lsl #2
	ubfx w12, w10, #24, #1
	orr w11, w11, w12, lsl #3
	orr w9, w9, w11, lsl #16
	orr w9, w9, #0x10
	str w9, [_vcpu, x8]
	next_gadget

/* MSR APSR_nzcvq and APSR_g: [rn offset[0:8], bit 8 for N Z C V and Q,
 * bit 9 for GE]. */
gadget a32_msr
	ldr x8, [_stream], #8
	and x9, x8, #0xff
	ldr w9, [_vcpu, x9]
	tbz x8, #8, 1f
	and w27, w9, #0xf0000000
	ubfx w10, w9, #27, #1
	strb w10, [_vcpu, #UML_ARM64_CPU_A32_Q]
1:
	tbz x8, #9, 2f
	ubfx w10, w9, #16, #1
	neg w10, w10
	and w11, w10, #0xff
	ubfx w10, w9, #17, #1
	neg w10, w10
	and w10, w10, #0xff
	orr w11, w11, w10, lsl #8
	ubfx w10, w9, #18, #1
	neg w10, w10
	and w10, w10, #0xff
	orr w11, w11, w10, lsl #16
	ubfx w10, w9, #19, #1
	neg w10, w10
	and w10, w10, #0xff
	orr w11, w11, w10, lsl #24
	str w11, [_vcpu, #UML_ARM64_CPU_A32_GE_MASK]
2:
	next_gadget

/* The coprocessor 15 and generic timer registers EL0 can reach; each takes
 * the register offset(s) as its operand. */
gadget a32_read_tpidruro
	ldr x8, [_stream], #8
	ldr w9, [_vcpu, #UML_ARM64_CPU_A32_TPIDRURO]
	str w9, [_vcpu, x8]
	next_gadget

gadget a32_read_tpidrurw
	ldr x8, [_stream], #8
	ldr w9, [_vcpu, #UML_GADGET_CPU_TLS]
	str w9, [_vcpu, x8]
	next_gadget

gadget a32_write_tpidrurw
	ldr x8, [_stream], #8
	ldr w9, [_vcpu, x8]
	str x9, [_vcpu, #UML_GADGET_CPU_TLS]
	next_gadget

gadget a32_read_cntfrq
	ldr x8, [_stream], #8
	mrs x9, cntfrq_el0
	str w9, [_vcpu, x8]
	next_gadget

/* MRRC CNTVCT: [rt offset[0:8], rt2 offset[8:16]] */
gadget a32_read_cntvct
	ldr x8, [_stream], #8
	mrs x9, cntvct_el0
	and x10, x8, #0xff
	str w9, [_vcpu, x10]
	ubfx x10, x8, #8, #8
	lsr x9, x9, #32
	str w9, [_vcpu, x10]
	next_gadget

/* The CP15 barriers run only under SCTLR.CP15BEN; otherwise the instruction
 * is UNDEFINED. [key] */
.macro a32_cp15_barrier name, instruction
gadget a32_cp15_\name
	ldr x21, [_stream], #8
	ldrb w8, [_vcpu, #UML_ARM64_CPU_A32_CP15BEN]
	cbz w8, La32_undefined
	\instruction
	next_gadget
.endm

a32_cp15_barrier dsb, "dsb sy"
a32_cp15_barrier dmb, "dmb sy"
a32_cp15_barrier isb, "isb"

.purgem a32_cp15_barrier

gadget a32_isb
	isb
	next_gadget

/* The fall-through of a block that ends at an ISB, the ARMv8 or the CP15
 * one: leave when the address space's code changed since this program was
 * validated, as the A64 isb gadget does (engine/system_atomic.inc). The
 * block ends there, so everything it precharged has retired. Stream: [next
 * key]. */
gadget a32_code_sequence_check
	ldr x8, [_vcpu, #UML_ARM64_CPU_CODE_SEQUENCE]
	ldr x9, [_vcpu, #UML_ARM64_CPU_CODE_SEQUENCE_SEEN]
	ldr x8, [x8]
	cmp x8, x9
	b.ne 1f
	add _stream, _stream, #8
	next_gadget
1:
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

#include "a32_generated.inc"

.purgem a32_return_value
.purgem a32_follow_indirect
.purgem train_indirect_cell
.purgem push_return_record
.purgem follow_block_link
.purgem a32_key_interworking
.purgem a32_key_thumb

.popsection
