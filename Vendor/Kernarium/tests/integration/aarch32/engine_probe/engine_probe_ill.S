// SPDX-License-Identifier: GPL-2.0-or-later
/* Group ill: one instruction per routine, first in the routine, then bx lr.
 * The handler resumes after the instruction by skipping its size. */
	.syntax	unified
	.text

	.macro	ill_arm name, insn
	.arm
	.balign	4
	.globl	\name
	.type	\name, %function
\name:
	\insn
	bx	lr
	.endm

	.macro	ill_word name, word
	.arm
	.balign	4
	.globl	\name
	.type	\name, %function
\name:
	.inst	\word
	bx	lr
	.endm

	.macro	ill_thumb name, insn
	.thumb
	.balign	4
	.thumb_func
	.globl	\name
	.type	\name, %function
\name:
	\insn
	bx	lr
	.endm

	.macro	ill_half name, half
	.thumb
	.balign	4
	.thumb_func
	.globl	\name
	.type	\name, %function
\name:
	.inst.n	\half
	bx	lr
	.endm

	.macro	ill_wide name, first, second
	.thumb
	.balign	4
	.thumb_func
	.globl	\name
	.type	\name, %function
\name:
	.inst.w	(\first << 16) | \second
	bx	lr
	.endm

@ ---- UNDEFINED in A32: SIGILL ----
	ill_arm	ill_udf0_arm, "udf #0"
	ill_arm	ill_udf17_arm, "udf #17"
	ill_arm	ill_udf_ffff_arm, "udf #0xffff"
	ill_word ill_media_arm, 0xe6000010
	ill_word ill_cdp_arm, 0xee000300
	ill_word ill_ldc_cp3_arm, 0xed900300
	ill_arm	ill_mrc_midr_arm, "mrc p15, 0, r0, c0, c0, 0"
	ill_arm	ill_mcr_sctlr_arm, "mcr p15, 0, r0, c1, c0, 0"
	ill_arm	ill_mcr_iciallu_arm, "mcr p15, 0, r0, c7, c5, 0"
	ill_arm	ill_mcr_dccimvac_arm, "mcr p15, 0, r0, c7, c14, 1"
	ill_arm	ill_mcr_ttbr0_arm, "mcr p15, 0, r0, c2, c0, 0"
	ill_word ill_smc_arm, 0xe1600070
	ill_word ill_hvc_arm, 0xe1400070
	ill_word ill_eret_arm, 0xe160006e
	ill_word ill_swp_arm, 0xe1012090
	ill_word ill_swpb_arm, 0xe1412090
	ill_word ill_setend_arm, 0xf1010200

@ ---- UNDEFINED in T32: SIGILL ----
	ill_half ill_udf0_thumb, 0xde00
	ill_half ill_udf2_thumb, 0xde02
	ill_wide ill_udfw1_thumb, 0xf7f0, 0xa001
	ill_wide ill_mrc_midr_thumb, 0xee10, 0x0f10
	ill_wide ill_mcr_sctlr_thumb, 0xee01, 0x0f10
	ill_half ill_setend_thumb, 0xb658
	ill_wide ill_smc_thumb, 0xf7f0, 0x8000

@ ---- breakpoints: SIGTRAP ----
	ill_arm	brk_bkpt_arm, "bkpt #0x12"
	ill_word brk_udf16_arm, 0xe7f001f0
	ill_thumb brk_bkpt_thumb, "bkpt #0x34"
	ill_half brk_udf1_thumb, 0xde01
	ill_wide brk_udfw0_thumb, 0xf7f0, 0xa000

@ ---- the CP15 barriers: allowed unless abi.cp15_barrier is 0 ----
	ill_word cp_dmb_arm, 0xee070fba
	ill_word cp_dsb_arm, 0xee070f9a
	ill_word cp_isb_arm, 0xee070f95
	ill_wide cp_dmb_thumb, 0xee07, 0x0fba
	ill_wide cp_dsb_thumb, 0xee07, 0x0f9a
	ill_wide cp_isb_thumb, 0xee07, 0x0f95

@ ---- instructions that must run: hints, barriers, exclusives, prefetches ----
@ long ok_hints_{arm,thumb}(void *readable): returns r0 = 1 after all ran.
	.arm
	.balign	4
	.globl	ok_hints_arm
	.type	ok_hints_arm, %function
ok_hints_arm:
	nop
	yield
	wfe
	sev
	dmb	ish
	dmb	sy
	dsb	ish
	dsb	sy
	isb	sy
	clrex
	pld	[r0]
	pld	[r0, #64]
	pli	[r0]
	ldrex	r1, [r0]
	clrex
	mov	r0, #1
	bx	lr

	.thumb
	.balign	4
	.thumb_func
	.globl	ok_hints_thumb
	.type	ok_hints_thumb, %function
ok_hints_thumb:
	nop
	yield
	wfe
	sev
	dmb	ish
	dmb	sy
	dsb	ish
	dsb	sy
	isb	sy
	clrex
	pld	[r0]
	pld	[r0, #64]
	pli	[r0]
	ldrex	r1, [r0]
	clrex
	movs	r0, #1
	bx	lr
