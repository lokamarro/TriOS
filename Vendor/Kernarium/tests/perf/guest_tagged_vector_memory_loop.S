/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux SIMD&FP transfers through a top-byte-tagged
 * pointer, as a process with the tagged-address ABI enabled (every Android
 * process with heap pointer tagging) makes them: the unsigned-offset,
 * unscaled, register-offset, pair and multiple-structure forms that memcpy
 * and memset use. The final counter is read back through the untagged alias.
 * The emulator ignores the top byte by default, as the Android product runs;
 * the prctl is only for kernels that gate the ABI per process.
 */

#include "probe_harness.inc"

	.equ NR_PRCTL,			167
	.equ PR_SET_TAGGED_ADDR_CTRL,	55
	.equ PR_TAGGED_ADDR_ENABLE,	1
	.equ ITERATIONS,		10000000

	probe_start
	mov	x0, #PR_SET_TAGGED_ADDR_CTRL
	mov	x1, #PR_TAGGED_ADDR_ENABLE
	mov	x2, xzr
	mov	x3, xzr
	mov	x4, xzr
	mov	x8, #NR_PRCTL
	svc	#0

	adrp	x22, data_page
	add	x22, x22, :lo12:data_page
	movz	x9, #0xb400, lsl #48
	orr	x0, x22, x9
	add	x5, x0, #256
	mov	x6, #160
	mov	x7, #192
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x9, #1
	dup	v1.2d, x9

.Lloop:
	ldr	q0, [x0, #16]
	add	v0.2d, v0.2d, v1.2d
	str	q0, [x0, #16]
	ldp	q2, q3, [x0, #32]
	stp	q3, q2, [x0, #64]
	ldur	q4, [x0, #97]
	stur	q4, [x0, #129]
	ldr	q5, [x0, x6]
	str	q5, [x0, x7]
	ld1	{v6.16b, v7.16b}, [x5]
	st1	{v6.16b, v7.16b}, [x5]
	subs	x19, x19, #1
	b.ne	.Lloop

	ldr	x20, [x22, #16]
	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish

	probe_end

	.section .data
	.p2align 12
data_page:
	.zero 512
