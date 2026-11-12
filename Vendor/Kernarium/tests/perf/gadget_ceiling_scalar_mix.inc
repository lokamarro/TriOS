/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Realistic scalar loop body shared by the ideal gadget ceiling bench and the
 * guest_scalar_mix_loop.S probe.
 *
 * The mix follows the gadget families a browser and a Geekbench guest execute:
 * seven loads (64-, 32-, 16- and 8-bit, register and unsigned offsets, one
 * pair), three stores (one pair), five branches (the loop, a bounds check and
 * a null check that are never taken, two data-dependent skips) and twenty-six
 * ALU words covering shifted and extended operands, logical immediates,
 * move-wide, multiply, multiply-add, bitfield extract, compare and conditional
 * select. The hash chain in x23 depends only on the loop index, so the
 * cursor-addressed loads sit off the loop-carried path, as in streaming code,
 * while their values and the hash feed the stores, the second branch and the
 * checksum.
 *
 * Contract with the harnesses: x19 iterations, x20 checksum, x21 a zeroed
 * 64 KiB page-aligned buffer, x22 cursor, x23 hash, and x24/x25 count the
 * fall-throughs of the two data-dependent branches, each of which skips three
 * instructions. Forty-one instructions; only x0-x12 and x19-x25 are written.
 */

.macro uml_gadget_ceiling_scalar_mix_body loop, fail
\loop:
	ldr	x0, [x21, x22]
	ldr	w1, [x21, #64]
	ldp	x2, x3, [x21, #128]
	ldrb	w4, [x21, #7]
	ldrh	w11, [x21, #10]
	eor	x5, x23, x19
	movz	w6, #0xc2b2
	movk	w6, #0xae35, lsl #16
	mul	x23, x5, x6
	eor	x23, x23, x23, lsr #29
	and	x8, x23, #0x7ff8
	cmp	x8, #0x8000
	b.hs	\fail
	ldr	w12, [x21, x8]
	add	w1, w1, w4, uxtb
	add	w1, w1, w12
	orr	x7, x1, x23, lsl #32
	add	x7, x7, x0
	cmp	x8, x22
	csel	x22, x8, x22, lo
	str	x7, [x21, x22]
	tbz	x23, #3, 1f
	add	x24, x24, #1
	stp	x0, x7, [x21, #256]
	sub	x2, x3, x0, lsr #3
1:	ldr	x9, [x21, #512]
	cmp	x9, x0
	b.lo	2f
	add	x25, x25, #1
	sub	x20, x20, x9
	str	x7, [x21, #512]
2:	ubfx	x10, x23, #12, #12
	madd	w10, w10, w6, w1
	cbz	x21, \fail
	add	x20, x20, x10
	add	x20, x20, x11, lsl #3
	eor	x20, x20, x2
	add	x22, x22, #8
	and	x22, x22, #0x7ff8
	subs	x19, x19, #1
	b.ne	\loop
.endm
