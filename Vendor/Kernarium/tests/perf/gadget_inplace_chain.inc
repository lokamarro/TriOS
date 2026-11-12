/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Interleaved in-place ADD-immediate chains, shared by the ideal gadget
 * ceiling bench and the guest_inplace_chain_loop.S probe.
 *
 * gadget_ceiling_add_immediate.inc keeps one result live across sixteen
 * adjacent words, which adjacent forwarding carries in a register, so it
 * measures dispatch rather than the cost of reaching a guest register. This
 * body interleaves three independent "add Rd, Rd, #imm" chains instead. No two
 * consecutive words share a register, so no adjacent forward is ever selected
 * and every word reads its operand back out of CpuState -- the write-through
 * hop a real workload pays whenever a producer and its consumer are not
 * neighbours.
 *
 * Three chains rather than one because a single chain measures only latency:
 * with three the host has independent work to overlap, which is the condition
 * under which the addressing form of the hop decides throughput.
 *
 * Follows the ceiling harness register contract: x19 counts iterations and x20
 * is the returned checksum, so the other two chains run in x22 and x23 and are
 * folded into x20 after the loop. The fail label is unused. Eighteen
 * instructions per iteration.
 */

.macro uml_gadget_ceiling_inplace_chain_body loop, fail=0
\loop:
	add	x20, x20, #1
	add	x22, x22, #2
	add	x23, x23, #3
	add	x20, x20, #4
	add	x22, x22, #5
	add	x23, x23, #6
	add	x20, x20, #7
	add	x22, x22, #8
	add	x23, x23, #9
	add	x20, x20, #10
	add	x22, x22, #11
	add	x23, x23, #12
	add	x20, x20, #13
	add	x22, x22, #14
	add	x23, x23, #15
	add	x20, x20, #16
	subs	x19, x19, #1
	b.ne	\loop
	eor	x20, x20, x22
	eor	x20, x20, x23
.endm
