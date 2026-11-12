/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * RET through X30, the register compilers return through, appended after the
 * established bank so no existing entry moves or changes its 64-byte phase.
 *
 * return_cached names its source register by a CpuState offset in a stream
 * word, and every translated RET pays for that word's fetch and its load.
 * With X30 baked in, the stream is the gadget and the indirect site, the
 * target comes from one load at a fixed displacement, and RET through any
 * other register keeps return_cached. The target tiers, the return ring and
 * the miss exit are return_cached's own, from follow_indirect_link.
 *
 * Included after the LSE2 stubs, so their cold part does not move either.
 */
gadget return_x30_cached
	ldr x9, [_stream]
	ldr x10, [_vcpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	follow_indirect_link 1

.purgem follow_indirect_link
