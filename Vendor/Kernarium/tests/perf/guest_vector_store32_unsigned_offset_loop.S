/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fixed-work arm64/Linux 32-bit FP/SIMD unsigned-offset store probe.
 *
 * STR S0,[Xn] selects vector_store32_unsigned_fast. Sixteen base registers
 * spread the stores across sixteen cache lines while keeping every address on
 * one aligned guest page and every translated access on the cached fast path.
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
	str	s0, [x13]
	str	s0, [x3]
	str	s0, [x4]
	str	s0, [x5]
	str	s0, [x6]
	str	s0, [x7]
	str	s0, [x14]
	str	s0, [x15]
	str	s0, [x16]
	str	s0, [x17]
	str	s0, [x18]
	str	s0, [x24]
	str	s0, [x25]
	str	s0, [x26]
	str	s0, [x27]
	str	s0, [x29]
	str	s0, [x13]
	str	s0, [x3]
	str	s0, [x4]
	str	s0, [x5]
	str	s0, [x6]
	str	s0, [x7]
	str	s0, [x14]
	str	s0, [x15]
	str	s0, [x16]
	str	s0, [x17]
	str	s0, [x18]
	str	s0, [x24]
	str	s0, [x25]
	str	s0, [x26]
	str	s0, [x27]
	str	s0, [x29]
	.endm

#include "vector_store32_probe.inc"

	.section .data
success_message:
	.ascii "UML_PERF_PROBE name=vector_store32_unsigned_fast sample="
sample_digit:
	.ascii "0"
	.ascii " stores=160000000 instructions=170000000 elapsed_ns=0x"
elapsed_hex:
	.ascii "0000000000000000\n"
success_message_end:
