/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Dependent ADD-immediate loop body shared by the ideal gadget ceiling bench
 * and the guest_add_immediate_loop.S probe.
 *
 * Sixteen dependent ADD-immediate words keep one result live while one linked
 * branch is amortized over them. This is the best-case scalar ceiling, not a
 * realistic mix. x19 counts iterations and x20 carries the chain; the fail
 * label is unused.
 */

.macro uml_gadget_ceiling_add_immediate_body loop, fail=0
\loop:
	add	x20, x20, #1
	add	x20, x20, #2
	add	x20, x20, #3
	add	x20, x20, #4
	add	x20, x20, #5
	add	x20, x20, #6
	add	x20, x20, #7
	add	x20, x20, #8
	add	x20, x20, #9
	add	x20, x20, #10
	add	x20, x20, #11
	add	x20, x20, #12
	add	x20, x20, #13
	add	x20, x20, #14
	add	x20, x20, #15
	add	x20, x20, #16
	subs	x19, x19, #1
	b.ne	\loop
.endm
