/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fixed-work arm64/Linux 32-bit FP/SIMD register-offset store probe.
 *
 * STR S0,[Xn,X28] with a zero X28 selects vector_other_store32_x_0. Sixteen
 * base registers spread the stores across the same sixteen cache lines as the
 * paired unsigned-offset probe while retaining exactly the same timed count.
 */

	.global _start

	.macro target_store_setup
	adrp	x13, data_page
	add	x13, x13, :lo12:data_page
	add	x3, x13, #64
	add	x4, x13, #128
	add	x5, x13, #192
	add	x6, x13, #256
	add	x7, x13, #320
	add	x14, x13, #384
	add	x15, x13, #448
	add	x16, x13, #512
	add	x17, x13, #576
	add	x18, x13, #640
	add	x24, x13, #704
	add	x25, x13, #768
	add	x26, x13, #832
	add	x27, x13, #896
	add	x29, x13, #960
	mov	x28, xzr
	movz	w23, #0x5a5a
	movk	w23, #0xa5a5, lsl #16
	fmov	s0, w23
	.endm

	.macro target_store_body
	str	s0, [x13, x28]
	str	s0, [x3, x28]
	str	s0, [x4, x28]
	str	s0, [x5, x28]
	str	s0, [x6, x28]
	str	s0, [x7, x28]
	str	s0, [x14, x28]
	str	s0, [x15, x28]
	str	s0, [x16, x28]
	str	s0, [x17, x28]
	str	s0, [x18, x28]
	str	s0, [x24, x28]
	str	s0, [x25, x28]
	str	s0, [x26, x28]
	str	s0, [x27, x28]
	str	s0, [x29, x28]
	str	s0, [x13, x28]
	str	s0, [x3, x28]
	str	s0, [x4, x28]
	str	s0, [x5, x28]
	str	s0, [x6, x28]
	str	s0, [x7, x28]
	str	s0, [x14, x28]
	str	s0, [x15, x28]
	str	s0, [x16, x28]
	str	s0, [x17, x28]
	str	s0, [x18, x28]
	str	s0, [x24, x28]
	str	s0, [x25, x28]
	str	s0, [x26, x28]
	str	s0, [x27, x28]
	str	s0, [x29, x28]
	.endm

#include "vector_store32_probe.inc"

	.section .data
success_message:
	.ascii "UML_PERF_PROBE name=vector_other_store32_x_0 sample="
sample_digit:
	.ascii "0"
	.ascii " stores=160000000 instructions=170000000 elapsed_ns=0x"
elapsed_hex:
	.ascii "0000000000000000\n"
success_message_end:
