/* SPDX-License-Identifier: GPL-2.0-or-later */
gadget svc
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_SYSCALL
	b uml_arm64_gadget_leave

gadget interrupt
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_INTERRUPT
	b uml_arm64_gadget_leave

gadget illegal
	ldr x8, [_stream]
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_ILLEGAL
	b uml_arm64_gadget_leave

gadget breakpoint
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_BREAKPOINT
	b uml_arm64_gadget_leave

/* A BlockLink contains {stream, instruction_count, guest_pc}. An unlinked
 * target points at the one-word {link_exit} stream in front of it, so the hot
 * path needs no linked/unlinked test. Resolved edges defer the architectural PC store
 * until an actual exit. Bound chains at 4,096 guest instructions; the
 * executor still observes pending shootdowns/IPIs sixteen times within its
 * 64K fairness limit. */
.macro follow_block_link link
	ldp _stream, x11, [\link]
	add x29, x29, x11
	/* x29 is biased by -4,096 at entry, so a negative total remains below
	 * the retirement limit. Unlike testing bit 12 directly, this stays correct
	 * when callers enter with counts above 8,191. */
	tbnz x29, #63, 98f
	ldr x9, [\link, #16]
	str x9, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
98:
	next_gadget
.endm

/* The stream an unlinked BlockLink names is the link_exit word right in front
 * of that BlockLink, so the guest PC is the BlockLink's own. */
gadget link_exit
	ldr x9, [_stream, #16]
	str x9, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/*
 * A block that accesses FP/SIMD state keeps this entry word in front of its
 * stream, and links to the block name that word, so the entry falls through
 * into the block. Most entries need one predicted loaded-state branch and one
 * dirty-byte store. Only a freshly deferred kernel frame calls the cold
 * materialization helper.
 */
gadget fp_link_entry
	ldrb w8, [_vcpu, #UML_GADGET_CPU_FP_LOADED]
	cbz w8, 97f
	mov w8, #1
	strb w8, [_vcpu, #UML_GADGET_CPU_FP_DIRTY]
98:
	next_gadget
97:
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	helper_call uml_arm64_gadget_access_fp_state
	ldp x1, x2, [sp], #16
	b 98b

gadget branch
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_linked
	ldr x8, [_stream]
	follow_block_link x8

gadget branch_link
	ldr x8, [_stream], #8
	ldr x9, [_stream]
	str x9, [_vcpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* Return predictor: a linked call pushes {return pc, return BlockLink} on a
 * sixteen-record ring in CpuState. Seven instructions per call; the ring is
 * private engine state that a cached return consults only after its site
 * cells miss, so a wrong or stale record costs nothing but the old exit. */
.macro push_return_record pc, link
	ldrb w12, [_vcpu, #UML_GADGET_CPU_RETURN_TOP]
	add x13, _vcpu, #UML_GADGET_CPU_RETURN_STACK
	add x13, x13, x12, lsl #4
	stp \pc, \link, [x13]
	add w12, w12, #1
	and w12, w12, #UML_GADGET_CPU_RETURN_MASK
	strb w12, [_vcpu, #UML_GADGET_CPU_RETURN_TOP]
.endm

/* The call's second operand is the return block's link rather than the
 * return address, which that BlockLink holds. */
gadget branch_link_linked
	ldr x8, [_stream], #8
	ldr x11, [_stream]
	ldr x9, [x11, #16]
	str x9, [_vcpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	push_return_record x9, x11
	follow_block_link x8

gadget branch_register
	ldr x8, [_stream]
	ldr x10, [_vcpu, x8]
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_link_register
	ldr x8, [_stream], #8
	ldr x11, [_stream]
	ldr x10, [_vcpu, x8]
	str x11, [_vcpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget return
	ldr x8, [_stream]
	ldr x10, [_vcpu, x8]
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* Store the target in x10 and its BlockLink in x12 into the first empty cell
 * of the site whose cells x9 addresses, if it has one. Every executor running
 * the address space shares the site, and retraining a polymorphic site at
 * each ring or table hit made their vCPUs fight over its cache line, so
 * translated code fills a cell only once per reset; the cold helper still
 * trains a miss round-robin. An empty cell's PC is all ones. */
.macro train_indirect_cell
	ldr x13, [x9]
	cmn x13, #1
	b.eq 1f
	ldr x13, [x9, #16]
	cmn x13, #1
	b.ne 2f
	stp x10, x12, [x9, #16]
	b 2f
1:
	stp x10, x12, [x9]
2:
.endm

/* Two-entry data-only indirect target caches. Translation fixes the source
 * register offset and cache-cell address. A hit immediately follows the
 * already-safe BlockLink; only a miss commits the target and identifies the
 * cell for cold C++ training. */
.macro follow_indirect_link ras=0
	ldp x11, x12, [x9]
	cmp x10, x11
	b.eq 98f
	ldp x11, x12, [x9, #16]
	cmp x10, x11
	b.eq 98f
	.if \ras
	/* The newest return record is popped only when it names the live link
	 * register; anything else leaves the ring alone and exits as before. */
	ldrb w13, [_vcpu, #UML_GADGET_CPU_RETURN_TOP]
	sub w13, w13, #1
	and w13, w13, #UML_GADGET_CPU_RETURN_MASK
	add x14, _vcpu, #UML_GADGET_CPU_RETURN_STACK
	add x14, x14, x13, lsl #4
	ldp x11, x12, [x14]
	cmp x10, x11
	b.ne 97f
	strb w13, [_vcpu, #UML_GADGET_CPU_RETURN_TOP]
	/* Train the site's next cell as the cold helper would, so the ordinary
	 * cell path hits this target from now on and the ring is consulted
	 * only when the site is more polymorphic than its cells. */
	train_indirect_cell
	b 98f
97:
	.endif
	/* The target table is consulted out of line. */
	b Ltarget_tier\@
98:
	follow_block_link x12
	/* Every site of the address space shares one direct-mapped table of
	 * {target, BlockLink}, filled by the cold helper at each miss, so a
	 * target any site has missed on before is followed without a round
	 * trip -- which is what a site with more targets than cells needs.
	 * A hit trains the site's next cell as the ring does, so a site that
	 * settles on few targets goes back to its cells. */
	.pushsection GADGET_SECTION_COLD
Ltarget_tier\@:
	ldr x13, [_vcpu, #UML_GADGET_CPU_TARGET_TIER]
	cbz x13, Ltarget_tier_miss\@
	eor x14, x10, x10, lsr #10
	ubfx x14, x14, #2, #UML_GADGET_TARGET_TIER_BITS
	add x13, x13, x14, lsl #4
	ldp x11, x12, [x13]
	cmp x10, x11
	b.ne Ltarget_tier_miss\@
	train_indirect_cell
	follow_block_link x12
Ltarget_tier_miss\@:
	str x9, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x10, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
	.popsection
.endm

gadget branch_register_cached
	ldp x8, x9, [_stream]
	ldr x10, [_vcpu, x8]
	follow_indirect_link

/* As branch_link_linked, the return address comes out of the return block's
 * link. */
gadget branch_link_register_cached
	ldp x8, x9, [_stream], #16
	ldr x14, [_stream]
	ldr x10, [_vcpu, x8]
	ldr x11, [x14, #16]
	str x11, [_vcpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	push_return_record x11, x14
	follow_indirect_link

gadget return_cached
	ldp x8, x9, [_stream]
	ldr x10, [_vcpu, x8]
	follow_indirect_link 1

/* The exit of a fused or conditional branch: x19 and x20 hold the two
 * successors and the flags the condition reads are set. The linked form
 * follows the chosen BlockLink; the unlinked form stores the chosen guest PC
 * in `scratch` and leaves. */
.macro select_branch_target linked, condition, scratch=x9
	.if \linked
	csel x8, x19, x20, \condition
	follow_block_link x8
	.else
	csel \scratch, x19, x20, \condition
	str \scratch, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
	.endif
.endm

/* The exit of an inline-fall-through branch: x19 is the taken link and x20 the
 * number of instructions translated after the branch. Not taken continues
 * with the next gadget; taken gives that suffix back, which entering the
 * block precharged, and follows the link. */
.macro inline_branch_tail condition
	b.\condition 99f
	next_gadget
99:
	sub x29, x29, x20
	follow_block_link x19
.endm

/* B.cond's condition is fixed when its data-only program is built.  Selecting
 * one of these precompiled gadgets avoids a C++ helper call at every loop
 * edge.  EL0 may write NZCV, so CSEL can evaluate the original guest flags
 * directly without synthesizing a runtime condition-code switch.
 */
.macro conditional_branch name, condition
gadget branch_\name
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	msr nzcv, x27
	select_branch_target 0, \condition
.endm

.macro conditional_branch_linked name, condition
gadget branch_\name\()_linked
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	msr nzcv, x27
	select_branch_target 1, \condition
.endm

/* A conditional branch whose fall-through is translated inline behind it.
 *
 * A block otherwise ends at its first branch, which on a browser guest means a
 * mean block of five instructions -- too short for the per-block costs to
 * amortize against. Continuing through the fall-through keeps the guest
 * addresses contiguous, so nothing about validation or fault accounting
 * changes, and the not-taken case stops being an indirect jump through a link
 * at all.
 *
 * The stream holds the taken link and the number of instructions translated
 * after this one, in the word where the unfused form keeps the fall-through
 * link. Entering a block precharges its whole length, so a taken branch
 * leaving from the middle has to give that suffix back before following its
 * link.
 */
.macro conditional_branch_inline name, condition
gadget branch_\name\()_inline
	ldr x19, [_stream], #8
	ldr x20, [_stream], #8
	msr nzcv, x27
	inline_branch_tail \condition
.endm

/* G-2 superinstruction: guest CMP of two registers followed by B.cond. The
 * builder's selection (program_builder_data_processing_impl.ipp) says why.
 *
 * Stream operand is the compare's own encoding, whose left and right register
 * byte offsets sit at bits 16 and 32, followed by the taken and fall-through
 * words. Only the unshifted register-register form with an XZR destination is
 * fused; a shift, an extension or a real destination falls through to the pair.
 */
.macro compare_register_branch bits64, width, name, condition, linked
	.if \linked
gadget compare_register_branch_\width\()_\name\()_linked
	.else
gadget compare_register_branch_\width\()_\name
	.endif
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ubfx x10, x8, #16, #16
	ldr x15, [_vcpu, x10]
	ubfx x11, x8, #32, #16
	ldr x16, [_vcpu, x11]
	.if \bits64
	subs xzr, x15, x16
	.else
	subs wzr, w15, w16
	.endif
	mrs x27, nzcv
	select_branch_target \linked, \condition
.endm

/* Fall-through translated inline; see conditional_branch_inline. */
.macro compare_register_branch_inline bits64, width, name, condition
gadget compare_register_branch_\width\()_\name\()_inline
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #48
	ubfx x10, x8, #16, #16
	ldr x15, [_vcpu, x10]
	ubfx x11, x8, #32, #16
	ldr x16, [_vcpu, x11]
	.if \bits64
	subs xzr, x15, x16
	.else
	subs wzr, w15, w16
	.endif
	mrs x27, nzcv
	inline_branch_tail \condition
.endm

.macro compare_register_branch_pair name, condition
compare_register_branch 1, x, \name, \condition, 0
compare_register_branch 0, w, \name, \condition, 0
compare_register_branch 1, x, \name, \condition, 1
compare_register_branch 0, w, \name, \condition, 1
compare_register_branch_inline 1, x, \name, \condition
compare_register_branch_inline 0, w, \name, \condition
.endm

compare_register_branch_pair eq, eq
compare_register_branch_pair ne, ne
compare_register_branch_pair hs, hs
compare_register_branch_pair lo, lo
compare_register_branch_pair mi, mi
compare_register_branch_pair pl, pl
compare_register_branch_pair vs, vs
compare_register_branch_pair vc, vc
compare_register_branch_pair hi, hi
compare_register_branch_pair ls, ls
compare_register_branch_pair ge, ge
compare_register_branch_pair lt, lt
compare_register_branch_pair gt, gt
compare_register_branch_pair le, le

.purgem compare_register_branch_pair
.purgem compare_register_branch
.purgem compare_register_branch_inline

/* G-2 superinstruction: guest CMP-immediate immediately followed by B.cond.
 *
 * That pair is the most common control-flow shape there is, and unfused it
 * costs a full threaded dispatch between two gadgets that have nothing to say
 * to each other except the flags. Worse, the flags make the round trip through
 * a system register twice - the compare ends with `mrs x27, nzcv` and the
 * branch begins with `msr nzcv, x27` - purely because the two halves are
 * separate entries.
 *
 * Fusing removes the dispatch and the `msr`. The `mrs` stays: the guest's NZCV
 * is architecturally live after the branch, so the block must leave it in x27
 * whichever way the branch goes. MRS does not disturb PSTATE, so it can sit
 * between the SUBS and the CSEL that reads its flags.
 *
 * Stream operand is the compare's own encoding - Rn at bits 8..12, immediate
 * from bit 16 - followed by the taken and fall-through words the conditional
 * branch already took. Rn == 31 means SP for this instruction class and is left
 * to the unfused path rather than given a variant.
 */
.macro compare_immediate_branch bits64, width, name, condition, linked
	.if \linked
gadget compare_immediate_branch_\width\()_\name\()_linked
	.else
gadget compare_immediate_branch_\width\()_\name
	.endif
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ubfx w10, w8, #8, #5
	ldr x15, [_vcpu, x10, lsl #3]
	lsr x11, x8, #16
	.if \bits64
	subs xzr, x15, x11
	.else
	subs wzr, w15, w11
	.endif
	mrs x27, nzcv
	select_branch_target \linked, \condition
.endm

/* Fall-through translated inline; see conditional_branch_inline. */
.macro compare_immediate_branch_inline bits64, width, name, condition
gadget compare_immediate_branch_\width\()_\name\()_inline
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #40
	ubfx w10, w8, #8, #5
	ldr x15, [_vcpu, x10, lsl #3]
	ubfx x11, x8, #16, #24
	.if \bits64
	subs xzr, x15, x11
	.else
	subs wzr, w15, w11
	.endif
	mrs x27, nzcv
	inline_branch_tail \condition
.endm

.macro compare_immediate_branch_pair name, condition
compare_immediate_branch 1, x, \name, \condition, 0
compare_immediate_branch 0, w, \name, \condition, 0
compare_immediate_branch 1, x, \name, \condition, 1
compare_immediate_branch 0, w, \name, \condition, 1
compare_immediate_branch_inline 1, x, \name, \condition
compare_immediate_branch_inline 0, w, \name, \condition
.endm

compare_immediate_branch_pair eq, eq
compare_immediate_branch_pair ne, ne
compare_immediate_branch_pair hs, hs
compare_immediate_branch_pair lo, lo
compare_immediate_branch_pair mi, mi
compare_immediate_branch_pair pl, pl
compare_immediate_branch_pair vs, vs
compare_immediate_branch_pair vc, vc
compare_immediate_branch_pair hi, hi
compare_immediate_branch_pair ls, ls
compare_immediate_branch_pair ge, ge
compare_immediate_branch_pair lt, lt
compare_immediate_branch_pair gt, gt
compare_immediate_branch_pair le, le

.purgem compare_immediate_branch_pair
.purgem compare_immediate_branch
.purgem compare_immediate_branch_inline

conditional_branch eq, eq
conditional_branch ne, ne
conditional_branch hs, hs
conditional_branch lo, lo
conditional_branch mi, mi
conditional_branch pl, pl
conditional_branch vs, vs
conditional_branch vc, vc
conditional_branch hi, hi
conditional_branch ls, ls
conditional_branch ge, ge
conditional_branch lt, lt
conditional_branch gt, gt
conditional_branch le, le

conditional_branch_linked eq, eq
conditional_branch_linked ne, ne
conditional_branch_linked hs, hs
conditional_branch_linked lo, lo
conditional_branch_linked mi, mi
conditional_branch_linked pl, pl
conditional_branch_linked vs, vs
conditional_branch_linked vc, vc
conditional_branch_linked hi, hi
conditional_branch_linked ls, ls
conditional_branch_linked ge, ge
conditional_branch_linked lt, lt
conditional_branch_linked gt, gt
conditional_branch_linked le, le
conditional_branch_inline eq, eq
conditional_branch_inline ne, ne
conditional_branch_inline hs, hs
conditional_branch_inline lo, lo
conditional_branch_inline mi, mi
conditional_branch_inline pl, pl
conditional_branch_inline vs, vs
conditional_branch_inline vc, vc
conditional_branch_inline hi, hi
conditional_branch_inline ls, ls
conditional_branch_inline ge, ge
conditional_branch_inline lt, lt
conditional_branch_inline gt, gt
conditional_branch_inline le, le

/* A64 treats both condition encodings 14 and 15 as always true. */
gadget branch_always
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_always_linked
	ldr x8, [_stream]
	follow_block_link x8

/* CBZ/CBNZ width, polarity, source offset, and both successors are fixed by
 * translation.  The valid runtime path is straight-line and contains no
 * opcode extraction, register-number test, or conditional branch. */
.macro compare_branch width_name, width, polarity, condition
gadget compare_branch_\width_name\()_\polarity
	ldr x8, [_stream], #8
	ldp x19, x20, [_stream]
	ldr \width\()9, [_vcpu, x8]
	cmp \width\()9, #0
	select_branch_target 0, \condition, x10

gadget compare_branch_\width_name\()_\polarity\()_linked
	ldr x8, [_stream], #8
	ldp x19, x20, [_stream]
	ldr \width\()9, [_vcpu, x8]
	cmp \width\()9, #0
	select_branch_target 1, \condition

/* Fall-through translated inline; see conditional_branch_inline. */
gadget compare_branch_\width_name\()_\polarity\()_inline
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #16
	and x8, x8, #0xffff
	ldr \width\()9, [_vcpu, x8]
	cmp \width\()9, #0
	inline_branch_tail \condition
.endm

compare_branch w, w, z, eq
compare_branch w, w, nz, ne
compare_branch x, x, z, eq
compare_branch x, x, nz, ne

/* TBZ/TBNZ use a translation-computed one-bit mask.  This removes all
 * instruction-field extraction and the variable shift from the hot path. */
.macro test_branch polarity, condition
gadget test_branch_\polarity
	ldp x8, x9, [_stream], #16
	ldp x19, x20, [_stream]
	ldr x10, [_vcpu, x8]
	tst x10, x9
	select_branch_target 0, \condition, x11

gadget test_branch_\polarity\()_linked
	ldp x8, x9, [_stream], #16
	ldp x19, x20, [_stream]
	ldr x10, [_vcpu, x8]
	tst x10, x9
	select_branch_target 1, \condition

/* Fall-through translated inline; see conditional_branch_inline. */
gadget test_branch_\polarity\()_inline
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #16
	and x8, x8, #0xffff
	ldr x10, [_vcpu, x8]
	tst x10, x9
	inline_branch_tail \condition
.endm

test_branch z, eq
test_branch nz, ne

.purgem test_branch
.purgem compare_branch

.purgem conditional_branch_linked
.purgem inline_branch_tail
/* select_branch_target stays defined for test_bit_branch.inc, which purges it.
 * follow_block_link, follow_indirect_link, train_indirect_cell and
 * push_return_record stay defined for test_bit_branch.inc, return_x30.inc and
 * a32.inc. return_x30.inc purges follow_indirect_link; a32.inc, the last to
 * use the other three, purges them. */
