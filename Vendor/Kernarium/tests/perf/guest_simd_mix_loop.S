/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux realistic FP/SIMD-mix control.
 *
 * The loop body is the shared gadget_ceiling_simd_mix.inc, so this real
 * guest run, the host native control and the ideal gadget stream execute the
 * same instruction words. The printed iteration count, checksum, path counts
 * and dynamic instruction count must equal the host bench's native result for
 * the same iteration count.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ BODY_INSTRUCTIONS,	39
	.equ PATH0_SKIPPED,	3
	.equ PATH1_SKIPPED,	2

#include "gadget_ceiling_simd_mix.inc"

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	adrp	x21, buffer
	add	x21, x21, :lo12:buffer
	mov	x20, xzr
	mov	x22, xzr
	mov	x23, xzr
	mov	x24, xzr
	mov	x25, xzr
	movi	v16.16b, #0
	movi	v17.16b, #0
	movi	v18.16b, #0

	uml_gadget_ceiling_simd_mix_body .Lloop, .Lfailure

	/* Retired instructions: every body word per iteration, less the words
	 * each data-dependent branch skipped whenever it was taken. */
	movz	x0, #(ITERATIONS & 0xffff)
	movk	x0, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x1, #BODY_INSTRUCTIONS
	mul	x2, x0, x1
	sub	x3, x0, x24
	mov	x4, #PATH0_SKIPPED
	msub	x2, x3, x4, x2
	sub	x3, x0, x25
	mov	x4, #PATH1_SKIPPED
	msub	x2, x3, x4, x2

	adrp	x9, iterations_hex
	add	x9, x9, :lo12:iterations_hex
	bl	format_hex
	adrp	x9, checksum_hex
	add	x9, x9, :lo12:checksum_hex
	mov	x0, x20
	bl	format_hex
	adrp	x9, path0_hex
	add	x9, x9, :lo12:path0_hex
	mov	x0, x24
	bl	format_hex
	adrp	x9, path1_hex
	add	x9, x9, :lo12:path1_hex
	mov	x0, x25
	bl	format_hex
	adrp	x9, instructions_hex
	add	x9, x9, :lo12:instructions_hex
	mov	x0, x2
	bl	format_hex

	probe_finish	length=(success_message_end - success_message)

	/* x0 = value, x9 = sixteen-byte destination; clobbers x10-x12. */
format_hex:
	mov	w11, #16
7:	lsr	x12, x0, #60
	add	w12, w12, #48
	cmp	w12, #58
	b.lo	8f
	add	w12, w12, #39
8:	strb	w12, [x9], #1
	lsl	x0, x0, #4
	subs	w11, w11, #1
	b.ne	7b
	ret

	.size _start, .-_start

	.section .data
success_message:
	.ascii "UML_PERF_PROBE workload=simd-mix iterations=0x"
iterations_hex:
	.ascii "0000000000000000"
	.ascii " checksum=0x"
checksum_hex:
	.ascii "0000000000000000"
	.ascii " paths=0x"
path0_hex:
	.ascii "0000000000000000"
	.ascii ",0x"
path1_hex:
	.ascii "0000000000000000"
	.ascii " instructions=0x"
instructions_hex:
	.ascii "0000000000000000"
	.ascii "\nOK\n"
success_message_end:

	.section .bss
	.balign 16384
buffer:
	.space 65536

	.section .note.GNU-stack, "", %progbits
