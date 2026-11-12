/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The in-place baked-displacement shape, extended beyond ADD/SUB-immediate.
 *
 * inplace_immediate.inc states the mechanism. The families here are the rest
 * of the shapes where one guest register index serves every CpuState access a
 * gadget makes, which is what bounds a baked family at one entry per register
 * instead of one per operand combination:
 *
 *   logical immediate   "and/orr/eor/ands Rd, Rd, #imm"  one source
 *   move-wide keep      "movk Rd, #imm, lsl #s"          inherently in place
 *   add/sub register    "add/sub Rd, Rd, Rm"             Rm stays indexed
 *   fused multiply-add  "fmla Vd, Vn, Vm"                Vd is the accumulator
 *
 * A second source that stays register-indexed does not spoil the baked pair:
 * baking only the in-place operand keeps most of the gain.
 *
 * FMLA is the vector family worth baking: its destination is also its third
 * source, so it is in place by construction. The wider SIMD families are not
 * here on purpose -- 343 three-same entries at one copy per vector register is
 * about 1.7 MB of bank, which is the cost G194 found is charged to every
 * workload whether or not it forwards.
 */

/* ---- logical immediate, Rd == Rn, both ordinary registers ---------------- */
/* One operand word, the immediate Builder already decoded: the register
 * fields the baked entry no longer reads are not in the stream. */
.macro logical_inplace_entry operation, bits64, reg, suffix, forward
	.set Lipl\@, 0x8000 | (\reg * 8)
scalar_entry logical_immediate_inplace_\suffix\()_\reg, \forward, 12, Lipl\@, Lipl\@, Lscalar_none, Lscalar_none, 1
	ldr x14, [_stream], #8
	.if \forward & 1
	mov x15, x0
	.else
	ldr x15, [_vcpu, #(\reg * 8)]
	.endif
	.if \bits64
		.if \operation == 0
	and _scalar_x16, x15, x14
		.elseif \operation == 1
	orr _scalar_x16, x15, x14
		.elseif \operation == 2
	eor _scalar_x16, x15, x14
		.else
	ands _scalar_x16, x15, x14
		.endif
	.else
		.if \operation == 0
	and _scalar_w16, w15, w14
		.elseif \operation == 1
	orr _scalar_w16, w15, w14
		.elseif \operation == 2
	eor _scalar_w16, w15, w14
		.else
	ands _scalar_w16, w15, w14
		.endif
	.endif
	.if \operation == 3
	mrs x27, nzcv
	.endif
	str _scalar_x16, [_vcpu, #(\reg * 8)]
	next_gadget
.endm

.macro logical_inplace_register operation, bits64, reg, suffix
logical_inplace_entry \operation, \bits64, \reg, \suffix, 0
logical_inplace_entry \operation, \bits64, \reg, \suffix, 1
.endm

.macro logical_inplace_modes operation, bits64, prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
logical_inplace_register \operation, \bits64, \reg, \prefix
	.endr
.endm

logical_inplace_modes 0, 0, w_and
logical_inplace_modes 1, 0, w_orr
logical_inplace_modes 2, 0, w_eor
logical_inplace_modes 3, 0, w_ands
logical_inplace_modes 0, 1, x_and
logical_inplace_modes 1, 1, x_orr
logical_inplace_modes 2, 1, x_eor
logical_inplace_modes 3, 1, x_ands

/* ---- move-wide keep (MOVK), always in place ----------------------------- */
.macro move_keep_inplace_entry bits64, shift, reg, suffix, forward
	.set Lipm\@, 0x8000 | (\reg * 8)
scalar_entry move_wide_keep_inplace_\suffix\()_\reg, \forward, 0, Lipm\@, Lipm\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	ubfx x11, x8, #8, #16
	.if \forward & 1
	mov _scalar_x14, x0
	.else
	ldr _scalar_x14, [_vcpu, #(\reg * 8)]
	.endif
	.if \bits64
	bfi _scalar_x14, x11, #\shift, #16
	.else
	bfi _scalar_w14, w11, #\shift, #16
	.endif
	str _scalar_x14, [_vcpu, #(\reg * 8)]
	next_gadget
.endm

.macro move_keep_inplace_register bits64, shift, reg, suffix
move_keep_inplace_entry \bits64, \shift, \reg, \suffix, 0
move_keep_inplace_entry \bits64, \shift, \reg, \suffix, 1
.endm

.macro move_keep_inplace_modes bits64, shift, prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
move_keep_inplace_register \bits64, \shift, \reg, \prefix
	.endr
.endm

move_keep_inplace_modes 0, 0, w0
move_keep_inplace_modes 0, 16, w1
move_keep_inplace_modes 1, 0, x0
move_keep_inplace_modes 1, 16, x1
move_keep_inplace_modes 1, 32, x2
move_keep_inplace_modes 1, 48, x3

/* ---- add/sub shifted register, no shift, Rd == Rn ----------------------- */
/* This operand carries CpuState byte offsets, not register numbers: the
 * destination in bits 15:0, the left source in 31:16 and the right in 47:32.
 * Only the left pair is baked; the right source keeps its indexed load. */
.macro add_sub_reg_inplace_entry bits64, subtract, flags, reg, suffix, forward
	.set Lipr\@, 0x8000 | (\reg * 8)
scalar_entry add_sub_register_inplace_\suffix\()_\reg, \forward, 2, Lipr\@, Lipr\@, Lscalar_offset32, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \forward & 1
	mov x15, x0
	.else
	ldr x15, [_vcpu, #(\reg * 8)]
	.endif
	.if \forward & 2
	mov x16, x0
	.else
	ubfx x11, x8, #32, #16
	ldr x16, [_vcpu, x11]
	.endif
	.if \bits64
	add_sub_op \subtract, \flags, _scalar_x7, x15, x16
	.else
	add_sub_op \subtract, \flags, _scalar_w7, w15, w16
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	str _scalar_x7, [_vcpu, #(\reg * 8)]
	next_gadget
.endm

.macro add_sub_reg_inplace_register bits64, subtract, flags, reg, suffix
add_sub_reg_inplace_entry \bits64, \subtract, \flags, \reg, \suffix, 0
add_sub_reg_inplace_entry \bits64, \subtract, \flags, \reg, \suffix, 1
add_sub_reg_inplace_entry \bits64, \subtract, \flags, \reg, \suffix, 2
add_sub_reg_inplace_entry \bits64, \subtract, \flags, \reg, \suffix, 3
.endm

.macro add_sub_reg_inplace_modes bits64, subtract, flags, prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
add_sub_reg_inplace_register \bits64, \subtract, \flags, \reg, \prefix
	.endr
.endm

add_sub_reg_inplace_modes 0, 0, 0, w_add_n
add_sub_reg_inplace_modes 0, 0, 1, w_add_s
add_sub_reg_inplace_modes 0, 1, 0, w_sub_n
add_sub_reg_inplace_modes 0, 1, 1, w_sub_s
add_sub_reg_inplace_modes 1, 0, 0, x_add_n
add_sub_reg_inplace_modes 1, 0, 1, x_add_s
add_sub_reg_inplace_modes 1, 1, 0, x_sub_n
add_sub_reg_inplace_modes 1, 1, 1, x_sub_s

/* ---- fused multiply-add, baked accumulator ------------------------------ */
/* Vd is both the destination and the third source, so one baked displacement
 * serves the read and the write; Vn and Vm keep their indexed loads. Vector
 * slots are UML_GADGET_CPU_V0 + 16 * n for all 32 registers. */
.macro simd_fused_inplace_entry name, operation, arrangement, vec, forward
	.set Lipv\@, 0x8000 | (UML_GADGET_CPU_V0 + \vec * 16)
vector_entry simd_fused_inplace_\name\()_\vec, \forward, Lipv\@, Lscalar_offset16, Lscalar_offset32, Lipv\@, 1
	ensure_fp_environment
	ldr x11, [_stream], #8
	ubfx x9, x11, #16, #16
	lsr x10, x11, #32
	vector_read \forward, 1, q0, [_vcpu, x9]
	vector_read \forward, 2, q1, [_vcpu, x10]
	.if \forward & 4
	vector_read \forward, 4, q2, [_vcpu, #(UML_GADGET_CPU_V0 + \vec * 16)]
	.else
	ldr q2, [_vcpu, #(UML_GADGET_CPU_V0 + \vec * 16)]
	.endif
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	str q2, [_vcpu, #(UML_GADGET_CPU_V0 + \vec * 16)]
	next_gadget
.endm

.macro simd_fused_inplace_register name, operation, arrangement, vec
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 0
/* Three declared sources, so the contract names every non-empty mask. */
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 1
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 2
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 3
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 4
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 5
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 6
simd_fused_inplace_entry \name, \operation, \arrangement, \vec, 7
.endm

.macro simd_fused_inplace name, operation, arrangement
	.irp vec, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
simd_fused_inplace_register \name, \operation, \arrangement, \vec
	.endr
.endm

.macro simd_fused_inplace_shapes name, operation
	simd_fused_inplace \name\()_4h, \operation, 4h
	simd_fused_inplace \name\()_8h, \operation, 8h
	simd_fused_inplace \name\()_2s, \operation, 2s
	simd_fused_inplace \name\()_4s, \operation, 4s
	simd_fused_inplace \name\()_2d, \operation, 2d
.endm

simd_fused_inplace_shapes fmla, fmla
simd_fused_inplace_shapes fmls, fmls

/* ---- selection tables --------------------------------------------------- */
.macro inplace_row_31 stem
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	.quad CSYM(uml_arm64_gadget_\stem\()_\reg)
	.endr
.endm
.macro inplace_row_32 stem
	.irp vec, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad CSYM(uml_arm64_gadget_\stem\()_\vec)
	.endr
.endm

.pushsection __DATA_CONST,__gadget_inplace
.p2align 3

/* index = ((bits64 * 2 + operation) ... ) see the C++ selectors. */
.globl CSYM(uml_arm64_gadget_logical_immediate_inplace_table)
CSYM(uml_arm64_gadget_logical_immediate_inplace_table):
inplace_row_31 logical_immediate_inplace_w_and
inplace_row_31 logical_immediate_inplace_w_orr
inplace_row_31 logical_immediate_inplace_w_eor
inplace_row_31 logical_immediate_inplace_w_ands
inplace_row_31 logical_immediate_inplace_x_and
inplace_row_31 logical_immediate_inplace_x_orr
inplace_row_31 logical_immediate_inplace_x_eor
inplace_row_31 logical_immediate_inplace_x_ands

.globl CSYM(uml_arm64_gadget_move_wide_keep_inplace_table)
CSYM(uml_arm64_gadget_move_wide_keep_inplace_table):
inplace_row_31 move_wide_keep_inplace_w0
inplace_row_31 move_wide_keep_inplace_w1
inplace_row_31 move_wide_keep_inplace_x0
inplace_row_31 move_wide_keep_inplace_x1
inplace_row_31 move_wide_keep_inplace_x2
inplace_row_31 move_wide_keep_inplace_x3

.globl CSYM(uml_arm64_gadget_add_sub_register_inplace_table)
CSYM(uml_arm64_gadget_add_sub_register_inplace_table):
inplace_row_31 add_sub_register_inplace_w_add_n
inplace_row_31 add_sub_register_inplace_w_add_s
inplace_row_31 add_sub_register_inplace_w_sub_n
inplace_row_31 add_sub_register_inplace_w_sub_s
inplace_row_31 add_sub_register_inplace_x_add_n
inplace_row_31 add_sub_register_inplace_x_add_s
inplace_row_31 add_sub_register_inplace_x_sub_n
inplace_row_31 add_sub_register_inplace_x_sub_s

.globl CSYM(uml_arm64_gadget_simd_fused_inplace_table)
CSYM(uml_arm64_gadget_simd_fused_inplace_table):
inplace_row_32 simd_fused_inplace_fmla_4h
inplace_row_32 simd_fused_inplace_fmla_8h
inplace_row_32 simd_fused_inplace_fmla_2s
inplace_row_32 simd_fused_inplace_fmla_4s
inplace_row_32 simd_fused_inplace_fmla_2d
inplace_row_32 simd_fused_inplace_fmls_4h
inplace_row_32 simd_fused_inplace_fmls_8h
inplace_row_32 simd_fused_inplace_fmls_2s
inplace_row_32 simd_fused_inplace_fmls_4s
inplace_row_32 simd_fused_inplace_fmls_2d
.popsection

.purgem inplace_row_31
.purgem inplace_row_32
.purgem simd_fused_inplace_shapes
.purgem simd_fused_inplace
.purgem simd_fused_inplace_register
.purgem simd_fused_inplace_entry
.purgem add_sub_reg_inplace_modes
.purgem add_sub_reg_inplace_register
.purgem add_sub_reg_inplace_entry
.purgem add_sub_op
.purgem move_keep_inplace_modes
.purgem move_keep_inplace_register
.purgem move_keep_inplace_entry
.purgem logical_inplace_modes
.purgem logical_inplace_register
.purgem logical_inplace_entry

