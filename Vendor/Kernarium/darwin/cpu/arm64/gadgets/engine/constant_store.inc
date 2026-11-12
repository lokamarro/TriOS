/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * A constant stored to a register in one stream word, appended after the
 * established bank so no existing entry moves or changes its 64-byte phase.
 *
 * move_wide_constant carries the destination offset and the 64-bit value in
 * two words. A value that fits 48 bits, or whose top sixteen bits are all
 * ones, leaves room for the destination's CpuState offset in bits 63:48 of the
 * same word: MOVZ with a shift below 48, MOVN, and the page or PC-relative
 * address ADR and ADRP fold to. The word holds the value's low 48 bits, and
 * the entry supplies bits 63:48 itself. Builder chooses by value, so a value
 * with any other top sixteen bits keeps move_wide_constant and its two words.
 *
 * Non-faulting, so the successor hoists into x14 as the ADD/SUB-immediate
 * family's does (Builder always emits a terminal after a translated
 * fallthrough). The result stays in x0 for the next entry's forwarding, as
 * move_wide_constant's does.
 */
.if UML_GADGET_CPU_X0 != 0
.error "constant_store.inc addresses registers from the CpuState base"
.endif

.macro constant_store_fast suffix, fill
scalar_entry move_wide_packed_\suffix, 0, 0, Lscalar_offset48, Lscalar_none, Lscalar_none, Lscalar_none, 1
	ldp x8, x14, [_stream], #16
	.if \fill
	orr _scalar_x11, x8, #0xffff000000000000
	.else
	and _scalar_x11, x8, #0xffffffffffff
	.endif
	lsr x9, x8, #48
	str _scalar_x11, [_vcpu, x9]
	br x14
.endm

constant_store_fast zero, 0
constant_store_fast ones, 1

.purgem constant_store_fast

/* The end of the family, for the table of entries a streaming program may
 * name. */
gadget constant_store_end
