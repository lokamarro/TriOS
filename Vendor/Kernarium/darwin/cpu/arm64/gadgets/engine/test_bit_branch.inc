/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * TBZ and TBNZ with the bit number in the operand word instead of a mask word,
 * appended after the established bank so no existing entry moves or changes
 * its 64-byte phase.
 *
 * test_branch carries a one-bit mask beside the register's CpuState offset.
 * Here the bit number is in bits 5:0 of the offset's word, where a register
 * shift reads it without extracting it, and the offset is in bits 31:16. An
 * inlined branch's retire suffix is in bits 47:32, which the taken path alone
 * extracts. The register is shifted down by the bit and tested at bit 0, so a
 * TBZ or TBNZ is one stream word shorter than test_branch. The test_branch
 * entries stay in the bank, unselected, because removing them would move every
 * later entry.
 */
.macro test_bit_branch polarity, condition
gadget test_bit_branch_\polarity
	ldr x8, [_stream], #8
	ldp x19, x20, [_stream]
	lsr x9, x8, #16
	ldr x10, [_vcpu, x9]
	lsr x10, x10, x8
	tst x10, #1
	select_branch_target 0, \condition, x11

gadget test_bit_branch_\polarity\()_linked
	ldr x8, [_stream], #8
	ldp x19, x20, [_stream]
	lsr x9, x8, #16
	ldr x10, [_vcpu, x9]
	lsr x10, x10, x8
	tst x10, #1
	select_branch_target 1, \condition

/* Fall-through translated inline; see conditional_branch_inline. */
gadget test_bit_branch_\polarity\()_inline
	ldp x8, x19, [_stream], #16
	ubfx x9, x8, #16, #16
	ldr x10, [_vcpu, x9]
	lsr x10, x10, x8
	tb\polarity w10, #0, 99f
	next_gadget
99:
	sub x29, x29, x8, lsr #32
	follow_block_link x19
.endm

test_bit_branch z, eq
test_bit_branch nz, ne

.purgem test_bit_branch
.purgem select_branch_target

/* The end of the family, for the table of entries a streaming program may
 * name. */
gadget test_bit_branch_end
