/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux indirect branch/return throughput control.
 *
 * Each iteration executes one monomorphic BLR and one monomorphic RET. The
 * arithmetic and direct loop edge remain unchanged between emulator builds,
 * isolating data-only indirect successor-link performance.
 */

#include "probe_harness.inc"

	.equ ITERATIONS,	50000000

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	adr	x22, .Lcallee

.Lloop:
	blr	x22
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish

.Lcallee:
	add	x20, x20, #1
	ret

	probe_end
