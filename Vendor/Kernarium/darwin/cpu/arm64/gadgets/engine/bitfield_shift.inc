/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * SBFM and UBFM as one or two variable shifts, appended after the
 * established bank so no existing entry moves or changes its 64-byte phase.
 *
 * With datasize d, the architecture's field extract and field insert are the
 * same operation: the result is (source << left) >> right, the right shift
 * logical for UBFM and arithmetic for SBFM, where
 *     left  = d - 1 - imms
 *     right = (immr - imms - 1) mod d.
 * For imms >= immr this keeps bits imms..immr, drops the rest and lands the
 * field at bit 0 (UBFX, SBFX, UXT*, SXT*); for imms < immr the field of
 * imms + 1 bits lands at d - immr (UBFIZ, SBFIZ). LSR and ASR are left = 0,
 * LSL is right = 0, and a W form's shifts run at 32 bits, so the result's
 * upper half is zero. Both amounts are below d, so a host register shift takes
 * them as they are.
 *
 * The builder precomputes both amounts and puts the first shift's at the
 * operand's bottom, where a register shift reads it without extracting it:
 * bits 5:0 hold left, or right for LSR and ASR, whose left is zero; bits 11:6
 * hold right (shifted down for a general form's second shift); the
 * destination's CpuState offset is in bits 31:16 and the source's in 47:32.
 * A discarded destination is a NOP and XZR reads the zero cell, so neither
 * needs an entry. BFM keeps bitfield_fast and its two mask words: its share
 * of the stream is a twentieth of these.
 *
 * Non-faulting, so the successor hoists into x14 as the ADD/SUB-immediate
 * family's does (Builder always emits a terminal after a translated
 * fallthrough). The SBFM and UBFM entries of bitfield_fast stay in the bank,
 * unselected, because removing them would move every later entry.
 */
.macro bitfield_shift_operate kind, bits64, source, source32
	.if \kind == 0
		.if \bits64
	lsl x0, \source, x8
		.else
	lsl w0, \source32, w8
		.endif
	.elseif \kind == 1
		.if \bits64
	lsr x0, \source, x8
		.else
	lsr w0, \source32, w8
		.endif
	.elseif \kind == 2
		.if \bits64
	asr x0, \source, x8
		.else
	asr w0, \source32, w8
		.endif
	.else
	lsr x12, x8, #6
		.if \bits64
	lsl x0, \source, x8
			.if \kind == 3
	lsr x0, x0, x12
			.else
	asr x0, x0, x12
			.endif
		.else
	lsl w0, \source32, w8
			.if \kind == 3
	lsr w0, w0, w12
			.else
	asr w0, w0, w12
			.endif
		.endif
	.endif
.endm

/* kind: 0 LSL, 1 LSR, 2 ASR, 3 UBFM, 4 SBFM; the last two take both shifts. */
.macro bitfield_shift_fast kind, bits64, suffix, forward=0
	.set Lsd\@, Lscalar_offset16
	.set Lsn\@, Lscalar_offset32
scalar_entry bitfield_shift_\suffix, \forward, 13, Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldp x8, x14, [_stream], #16
	.if \forward & 1
	bitfield_shift_operate \kind, \bits64, x0, w0
	.else
	ubfx x10, x8, #32, #16
	ldr x15, [_vcpu, x10]
	bitfield_shift_operate \kind, \bits64, x15, w15
	.endif
	ubfx x9, x8, #16, #16
	str x0, [_vcpu, x9]
	br x14
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, bitfield_shift_fast, \kind, \bits64, \suffix
.endm

bitfield_shift_fast 0, 0, w_lsl
bitfield_shift_fast 1, 0, w_lsr
bitfield_shift_fast 2, 0, w_asr
bitfield_shift_fast 3, 0, w_ubfm
bitfield_shift_fast 4, 0, w_sbfm
bitfield_shift_fast 0, 1, x_lsl
bitfield_shift_fast 1, 1, x_lsr
bitfield_shift_fast 2, 1, x_asr
bitfield_shift_fast 3, 1, x_ubfm
bitfield_shift_fast 4, 1, x_sbfm

.purgem bitfield_shift_fast
.purgem bitfield_shift_operate

/* The end of the family, for the table of entries a streaming program may
 * name. */
gadget bitfield_shift_end
