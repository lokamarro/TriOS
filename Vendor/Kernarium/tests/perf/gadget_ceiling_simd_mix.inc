/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Realistic FP/SIMD loop body shared by the ideal gadget ceiling bench and the
 * guest_simd_mix_loop.S probe.
 *
 * The mix follows the FP-heavy Geekbench workloads: six vector transfers (a
 * 128-bit pair load and store, unsigned- and register-offset 128-bit loads, a
 * 128-bit store, a 32-bit scalar load and store), sixteen vector and scalar
 * arithmetic words (indexed and vector FMLA, FMUL, FADD, FSUB, FMAX, FABS,
 * scalar FADD and FMADD, an FP immediate, a broadcast, an integer-to-float and
 * two float-to-integer converts), an FP compare with a data-dependent branch, a
 * counter-driven skip, a null check that is never taken, and the scalar
 * address, cursor and loop control around them. Every accumulator decays by
 * one half each iteration, so all values stay bounded and finite for any
 * iteration count.
 *
 * Contract with the harnesses: x19 iterations, x20 checksum, x21 a zeroed
 * 64 KiB page-aligned buffer, x22 cursor, x24/x25 count the fall-throughs of
 * the two skipping branches, which skip three and two instructions, and
 * v16-v18 start at zero. Thirty-nine instructions; only x0-x13, x19-x25, v0-v7
 * and v16-v21 are written.
 */

.macro uml_gadget_ceiling_simd_mix_body loop, fail
\loop:
	add	x9, x21, x22
	eor	x10, x22, #0x40
	ldp	q0, q1, [x9]
	ldr	q2, [x9, #32]
	ldr	q3, [x21, x10]
	ldr	s4, [x21, #52]
	ubfx	w11, w19, #3, #8
	ucvtf	s5, w11
	fmov	v20.4s, #0.5
	dup	v5.4s, v5.s[0]
	fmul	v6.4s, v0.4s, v20.4s
	fadd	v6.4s, v6.4s, v5.4s
	fmul	v7.4s, v1.4s, v2.4s
	fmul	v16.4s, v16.4s, v20.4s
	fmla	v16.4s, v7.4s, v3.s[2]
	fsub	v17.4s, v6.4s, v3.4s
	fmax	v17.4s, v17.4s, v2.4s
	fmul	v18.4s, v18.4s, v20.4s
	fmla	v18.4s, v17.4s, v1.4s
	fadd	s21, s16, s18
	fmadd	s21, s4, s5, s21
	fcmp	s17, s6
	b.gt	1f
	add	x24, x24, #1
	fcvtzs	w12, s21
	add	x20, x20, x12
1:	stp	q6, q17, [x9]
	str	q6, [x9, #32]
	fabs	v19.4s, v18.4s
	fcvtzs	w13, s19
	tbz	x19, #4, 2f
	add	x25, x25, #1
	str	s19, [x21, #52]
2:	add	x20, x20, x13
	cbz	x21, \fail
	add	x22, x22, #64
	and	x22, x22, #0x7fc0
	subs	x19, x19, #1
	b.ne	\loop
.endm
