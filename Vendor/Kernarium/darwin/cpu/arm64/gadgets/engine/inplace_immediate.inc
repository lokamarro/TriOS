/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * In-place ADD/SUB-immediate entries with the CpuState displacement baked in.
 *
 * The ordinary family addresses its operand through a register index decoded
 * from the stream word: "ldr x15, [x1, x10, lsl #3]" feeding "str x0, [x1, x9,
 * lsl #3]". A dependent store-to-load pair in that form is slow, because the
 * core's memory-dependence prediction forwards at rename only when both
 * accesses name a base register and a constant displacement. Giving both sides
 * the immediate form makes the pair about as cheap as the register move that
 * adjacent forwarding achieves (G196), and unlike a forwarded register it
 * survives a block link, an intervening gadget and an executor round trip,
 * because the value is in memory the whole time.
 *
 * Baking a displacement means one entry per guest register, so the general
 * shape would be one entry per operand combination. The in-place shape --
 * "add Rd, Rd, #imm" with Rd an ordinary register -- needs only one index for
 * both accesses, which bounds the family at 31 entries per operation and
 * covers what actually waits: induction variables and accumulators, whose
 * producer and consumer sit on opposite sides of a loop branch.
 *
 * Decoding the two register fields also disappears, but that is incidental;
 * G17 and G36 removed the same instructions from this family without moving
 * its runtime.
 *
 * Appended after the established bank so no existing entry moves or changes
 * its 64-byte phase.
 */

/* Rd == Rn == \reg, both ordinary registers. The stream word keeps the
 * ordinary compact operand so the translator's encoding is unchanged; only
 * the two register fields are now dead, and the immediate still arrives in
 * bits 39:16. */
.macro add_sub_immediate_inplace_entry bits64, subtract, flags, reg, suffix, forward
	.set Lipd\@, 0x8000 | (\reg * 8)
scalar_entry add_sub_immediate_inplace_\suffix\()_\reg, \forward, 1, Lipd\@, Lipd\@, Lscalar_none, Lscalar_none, 1
	/* Non-faulting, and x14 is free: the same bounded hoist the ordinary
	 * family uses. Builder always emits a terminal after a translated
	 * fallthrough, so the successor word exists. */
	ldp x8, x14, [_stream], #16
	.if \forward & 1
	mov x15, x0
	.else
	ldr x15, [_vcpu, #(\reg * 8)]
	.endif
	lsr x11, x8, #16
	.if \bits64
	add_sub_op \subtract, \flags, _scalar_x16, x15, x11
	.else
	add_sub_op \subtract, \flags, _scalar_w16, w15, w11
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	str _scalar_x16, [_vcpu, #(\reg * 8)]
	br x14
.endm

.macro add_sub_immediate_inplace_register bits64, subtract, flags, reg, suffix
add_sub_immediate_inplace_entry \bits64, \subtract, \flags, \reg, \suffix, 0
	/* Keep the adjacent-forwarding arm the ordinary family has, so
	 * selecting the baked entry never costs an existing forward. */
add_sub_immediate_inplace_entry \bits64, \subtract, \flags, \reg, \suffix, 1
.endm

.macro add_sub_immediate_inplace_modes bits64, subtract, flags, prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
add_sub_immediate_inplace_register \bits64, \subtract, \flags, \reg, \prefix
	.endr
.endm

add_sub_immediate_inplace_modes 0, 0, 0, w_add_n
add_sub_immediate_inplace_modes 0, 0, 1, w_add_s
add_sub_immediate_inplace_modes 0, 1, 0, w_sub_n
add_sub_immediate_inplace_modes 0, 1, 1, w_sub_s
add_sub_immediate_inplace_modes 1, 0, 0, x_add_n
add_sub_immediate_inplace_modes 1, 0, 1, x_add_s
add_sub_immediate_inplace_modes 1, 1, 0, x_sub_n
add_sub_immediate_inplace_modes 1, 1, 1, x_sub_s

/* One flat selection table so the translator indexes rather than switches:
 * index = ((bits64 * 2 + subtract) * 2 + flags) * 31 + register. Entries are
 * the base gadgets; adjacent forwarding replaces a selection afterwards
 * through the ordinary scalar contract. */
.macro add_sub_immediate_inplace_row prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	.quad CSYM(uml_arm64_gadget_add_sub_immediate_inplace_\prefix\()_\reg)
	.endr
.endm

.pushsection __DATA_CONST,__gadget_inplace
.p2align 3
.globl CSYM(uml_arm64_gadget_add_sub_immediate_inplace_table)
CSYM(uml_arm64_gadget_add_sub_immediate_inplace_table):
add_sub_immediate_inplace_row w_add_n
add_sub_immediate_inplace_row w_add_s
add_sub_immediate_inplace_row w_sub_n
add_sub_immediate_inplace_row w_sub_s
add_sub_immediate_inplace_row x_add_n
add_sub_immediate_inplace_row x_add_s
add_sub_immediate_inplace_row x_sub_n
add_sub_immediate_inplace_row x_sub_s
.popsection
.purgem add_sub_immediate_inplace_row

.purgem add_sub_immediate_inplace_modes
.purgem add_sub_immediate_inplace_register
.purgem add_sub_immediate_inplace_entry

